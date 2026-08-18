// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "dialog_ai_proofread.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "base_grid.h"
#include "ai_client.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "theme.h"
#include "video_controller.h"

#include <libaegisub/character_count.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/option.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

namespace {

wxDEFINE_EVENT(EVT_AI_PROOFREAD_DONE, wxThreadEvent);

struct ProofreadOutcome {
	bool cancelled = false;
	std::string error;
	ai::ProofreadResult result;
};

constexpr int decision_pending = -1;
constexpr int decision_skipped = -2;

struct ProofreadSession {
	agi::Context const *context = nullptr;
	ai::ProofreadResult result;
	std::vector<ai::SubtitleLine> context_lines;
	std::vector<int> decisions;
};

ProofreadSession latest_session;

std::unordered_map<int, AssDialogue *> current_lines_by_id(agi::Context const *context) {
	std::unordered_map<int, AssDialogue *> lines;
	if (!context || !context->ass) return lines;
	for (auto& line : context->ass->Events)
		lines[line.Id] = &line;
	return lines;
}

bool latest_session_applies_to(agi::Context const *context) {
	if (latest_session.context != context || latest_session.context_lines.empty()) return false;
	auto lines = current_lines_by_id(context);
	if (latest_session.result.issues.empty())
		return lines.count(latest_session.context_lines.front().id) != 0;
	return std::all_of(latest_session.result.issues.begin(), latest_session.result.issues.end(),
		[&](ai::ProofreadIssue const& issue) { return lines.count(issue.line_id) != 0; });
}

void remember_session(agi::Context const *context, ai::ProofreadResult const& result,
	std::vector<ai::SubtitleLine> const& context_lines, std::vector<int> const& decisions) {
	latest_session.context = context;
	latest_session.result = result;
	latest_session.context_lines = context_lines;
	latest_session.decisions = decisions;
}

wxTextCtrl *make_readonly(wxWindow *parent, int height) {
	auto text = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition,
		wxSize(-1, height), wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
	text->SetBackgroundColour(parent->GetBackgroundColour());
	return text;
}

wxString format_timestamp(int time_ms) {
	time_ms = std::max(0, time_ms);
	return wxString::Format("%d:%02d.%03d", time_ms / 60000,
		(time_ms / 1000) % 60, time_ms % 1000);
}

bool is_editorial_note_warning(ai::ProofreadIssue const& issue) {
	auto const& text = issue.explanation;
	for (auto phrase : {"szerkesztői jegyzet", "Szerkesztői jegyzet",
		"szerkesztői megjegyzés", "Szerkesztői megjegyzés", "editorial note", "Editor note"})
		if (text.find(phrase) != std::string::npos) return true;
	return false;
}

struct AssSegment {
	bool marker = false;
	std::string text;
};

std::vector<AssSegment> split_ass_segments(std::string const& text) {
	std::vector<AssSegment> segments;
	std::string plain;
	auto flush_plain = [&] {
		if (!plain.empty()) {
			segments.push_back({false, std::move(plain)});
			plain.clear();
		}
	};
	for (size_t i = 0; i < text.size();) {
		if (text[i] == '{') {
			auto end = text.find('}', i + 1);
			if (end != std::string::npos) {
				flush_plain();
				segments.push_back({true, text.substr(i, end - i + 1)});
				i = end + 1;
				continue;
			}
		}
		if (text[i] == '\\' && i + 1 < text.size() &&
			(text[i + 1] == 'N' || text[i + 1] == 'n' || text[i + 1] == 'h')) {
			flush_plain();
			segments.push_back({true, text.substr(i, 2)});
			i += 2;
			continue;
		}
		plain += text[i++];
	}
	flush_plain();
	return segments;
}

std::vector<std::string> ass_markers(std::string const& text) {
	std::vector<std::string> markers;
	for (auto const& segment : split_ass_segments(text))
		if (segment.marker) markers.push_back(segment.text);
	return markers;
}

size_t nearest_word_boundary(std::string const& text, size_t desired, size_t minimum) {
	if (desired >= text.size()) return text.size();
	auto right = text.find(' ', desired);
	auto left = text.rfind(' ', desired);
	if (left != std::string::npos && left < minimum) left = std::string::npos;
	if (right == std::string::npos) return left == std::string::npos ? text.size() : left + 1;
	if (left == std::string::npos) return right + 1;
	return desired - left <= right - desired ? left + 1 : right + 1;
}

std::string preserve_ass_markup(std::string const& original, std::string const& suggestion) {
	if (ass_markers(original) == ass_markers(suggestion)) return suggestion;

	// Models occasionally omit otherwise valid ASS tags from one or more
	// alternatives. Transfer the original markers locally instead of throwing
	// away the entire completed review. Text is distributed proportionally
	// between the original styled spans, keeping marker order and line breaks.
	auto segments = split_ass_segments(original);
	auto replacement = agi::util::clean_ass_text(suggestion);
	std::vector<size_t> weights(segments.size());
	size_t total_weight = 0;
	for (size_t i = 0; i < segments.size(); ++i) {
		if (segments[i].marker) continue;
		weights[i] = agi::CharacterCount(segments[i].text,
			agi::IGNORE_BLOCKS | agi::IGNORE_WHITESPACE);
		total_weight += weights[i];
	}
	if (!total_weight) return original;

	std::string rebuilt;
	size_t consumed_weight = 0;
	size_t replacement_start = 0;
	for (size_t i = 0; i < segments.size(); ++i) {
		if (segments[i].marker) {
			rebuilt += segments[i].text;
			continue;
		}
		if (!weights[i]) {
			rebuilt += segments[i].text;
			continue;
		}
		consumed_weight += weights[i];
		size_t desired = replacement.size() * consumed_weight / total_weight;
		size_t end = consumed_weight == total_weight
			? replacement.size()
			: nearest_word_boundary(replacement, desired, replacement_start);
		rebuilt += replacement.substr(replacement_start, end - replacement_start);
		replacement_start = end;
	}
	return rebuilt;
}

wxString category_name(std::string const& category) {
	if (category == "spelling") return _("Spelling / typo");
	if (category == "punctuation") return _("Punctuation");
	if (category == "grammar") return _("Grammar");
	if (category == "style") return _("Wording / style");
	if (category == "repetition") return _("Word repetition");
	if (category == "consistency") return _("Subtitle consistency");
	if (category == "source_mismatch") return _("Source-line mismatch");
	return to_wx(category);
}

class AIProofreadDialog final : public wxDialog {
	agi::Context *context;
	std::vector<AssDialogue *> target_lines;
	std::vector<ai::SubtitleLine> context_lines;
	std::unordered_map<int, AssDialogue *> line_by_id;

	wxStaticText *status;
	wxGauge *progress;
	wxPanel *review_panel;
	wxStaticText *line_heading;
	wxStaticText *category_label;
	wxTextCtrl *explanation;
	wxTextCtrl *current_text;
	wxTextCtrl *source_text;
	wxListBox *alternatives;
	wxButton *play_line_button;
	wxButton *play_scene_button;
	wxButton *stop_playback_button;
	wxButton *apply_button;
	wxButton *skip_button;
	wxButton *back_button;
	wxButton *cancel_button;

	wxTimer pulse_timer;
	std::atomic_bool cancelled{false};
	ai::ProofreadResult result;
	std::unordered_map<int, std::string> original_text_by_id;
	std::vector<int> decisions;
	size_t issue_index = 0;
	int accepted = 0;
	int skipped = 0;
	bool busy = false;
	bool reviewing = false;
	bool close_when_idle = false;
	bool replaying = false;
	std::vector<int> initial_decisions;

	std::string ApiKey() const { return ai::GetApiKey(); }
	std::string Model() const { return OPT_GET("AI/OpenAI/Model")->GetString(); }
	std::string TranscriptionModel() const { return "gpt-transcribe"; }
	AssDialogue *CurrentLine() const {
		if (issue_index >= result.issues.size()) return nullptr;
		auto it = line_by_id.find(result.issues[issue_index].line_id);
		return it == line_by_id.end() ? nullptr : it->second;
	}

	void StopPlayback() {
		if (context->videoController) context->videoController->Stop();
	}

	void FocusLineInGrid(AssDialogue *line) {
		if (!line) return;
		context->selectionController->SetSelectionAndActive({line}, line);
		auto grid = context->subsGrid;
		if (!grid) return;
		grid->MakeDialogueVisible(line);
		// Video seeking and modal-window processing can schedule another grid
		// refresh. Reapply the position after the click handler has completed.
		grid->CallAfter([grid, line] { grid->MakeDialogueVisible(line); });
	}

	void PlayLine() {
		auto line = CurrentLine();
		if (!line || !context->project->VideoProvider()) return;
		FocusLineInGrid(line);
		context->videoController->PlayRange(static_cast<int>(line->Start), static_cast<int>(line->End));
	}

	void PlayScene() {
		auto line = CurrentLine();
		if (!line || !context->project->VideoProvider()) return;
		FocusLineInGrid(line);
		auto current = std::find_if(context_lines.begin(), context_lines.end(),
			[line](ai::SubtitleLine const& candidate) { return candidate.id == line->Id; });
		if (current == context_lines.end()) return;

		int start = std::max(0, current->start_ms - 2000);
		int end = current->end_ms + 2000;
		if (current != context_lines.begin()) {
			auto const& previous = *(current - 1);
			if (previous.end_ms >= current->start_ms - 5000)
				start = std::max(0, previous.start_ms);
		}
		if (current + 1 != context_lines.end()) {
			auto const& next = *(current + 1);
			if (next.start_ms <= current->end_ms + 5000)
				end = next.end_ms;
		}
		context->videoController->PlayRange(start, end);
	}

	void StartRequest() {
		busy = true;
		reviewing = false;
		cancelled.store(false);
		status->SetLabel(_("AI analysis in progress"));
		progress->SetRange(100);
		progress->SetValue(0);
		pulse_timer.Start(120);
		review_panel->Hide();
		apply_button->Hide();
		skip_button->Hide();
		back_button->Hide();
		cancel_button->SetLabel(_("Cancel request"));
		cancel_button->Show();
		SetMinSize(FromDIP(wxSize(560, 185)));
		SetSize(FromDIP(wxSize(680, 195)));
		CenterOnParent();
		Layout();

		auto key = ApiKey();
		auto model = Model();
		auto transcription_model = TranscriptionModel();
		auto lines = context_lines;
		agi::dispatch::Background().Async([this, key = std::move(key), model = std::move(model),
			transcription_model = std::move(transcription_model),
			lines = std::move(lines)]() mutable {
			auto outcome = std::make_shared<ProofreadOutcome>();
			try {
				ai::OpenAIClient client(std::move(key), std::move(model),
					std::move(transcription_model), {}, &cancelled);
				outcome->result = client.Proofread(lines);
			}
			catch (std::exception const& error) {
				outcome->cancelled = cancelled.load();
				outcome->error = error.what();
			}
			auto event = new wxThreadEvent(EVT_AI_PROOFREAD_DONE);
			event->SetPayload(std::move(outcome));
			wxQueueEvent(this, event);
		});
	}

	void StartReplay() {
		busy = false;
		status->SetLabel(_("Replaying the latest AI post-check without an AI connection."));
		progress->SetRange(std::max(1, static_cast<int>(result.issues.size())));
		progress->SetValue(0);
		if (result.issues.empty()) {
			progress->SetValue(1);
			wxMessageBox(result.message.empty()
				? _("No clear spelling, style or consistency issue was found in the checked lines.")
				: to_wx(result.message),
				_("AI post-check"), wxOK | wxICON_INFORMATION, this);
			EndModal(wxID_OK);
			return;
		}
		reviewing = true;
		ShowIssue();
	}

	void Fail(wxString const& message) {
		busy = false;
		reviewing = false;
		pulse_timer.Stop();
		progress->SetValue(0);
		status->SetLabel(message);
		cancel_button->SetLabel(_("Close"));
		cancel_button->Show();
		Layout();
	}

	void OnRequestDone(wxThreadEvent& event) {
		auto outcome = event.GetPayload<std::shared_ptr<ProofreadOutcome>>();
		busy = false;
		pulse_timer.Stop();
		if (close_when_idle) {
			EndModal(wxID_CANCEL);
			return;
		}
		if (!outcome->error.empty()) {
			if (!outcome->cancelled)
				wxMessageBox(to_wx(outcome->error), _("AI post-check failed"),
					wxOK | wxICON_ERROR, this);
			Fail(outcome->cancelled ? _("Request cancelled.") : _("The AI post-check failed."));
			return;
		}

		result = std::move(outcome->result);
		std::set<int> seen;
		std::vector<ai::ProofreadIssue> valid;
		for (auto& issue : result.issues) {
			if (is_editorial_note_warning(issue)) continue;
			auto line_it = line_by_id.find(issue.line_id);
			if (line_it == line_by_id.end() || !seen.insert(issue.line_id).second) {
				Fail(_("The AI response referred to an invalid or duplicate subtitle line."));
				wxMessageBox(_("No subtitle text was changed. Please run the post-check again."),
					_("AI post-check failed"), wxOK | wxICON_ERROR, this);
				return;
			}
			auto const& original = line_it->second->Text.get();
			for (auto& suggestion : issue.suggestions)
				suggestion = preserve_ass_markup(original, suggestion);
			valid.push_back(std::move(issue));
		}
		result.issues = std::move(valid);
		decisions.assign(result.issues.size(), decision_pending);
		initial_decisions = decisions;
		remember_session(context, result, context_lines, decisions);

		if (result.issues.empty()) {
			progress->SetRange(1);
			progress->SetValue(1);
			wxMessageBox(result.message.empty()
			? _("No clear spelling, style or consistency issue was found in the checked lines.")
				: to_wx(result.message),
				_("AI post-check"), wxOK | wxICON_INFORMATION, this);
			EndModal(wxID_OK);
			return;
		}

		reviewing = true;
		ShowIssue();
	}

	void ShowIssue() {
		if (issue_index >= result.issues.size()) {
			Finish();
			return;
		}
		auto& issue = result.issues[issue_index];
		auto line = CurrentLine();
		if (!line) {
			++issue_index;
			ShowIssue();
			return;
		}

		progress->SetRange(static_cast<int>(result.issues.size()));
		progress->SetValue(static_cast<int>(issue_index));
		status->SetLabel(wxString::Format(_("Suggestion %d of %d - line %d"),
			static_cast<int>(issue_index + 1), static_cast<int>(result.issues.size()), line->Row + 1));
		auto start = format_timestamp(static_cast<int>(line->Start));
		auto end = format_timestamp(static_cast<int>(line->End));
		line_heading->SetLabel(wxString::Format(_("Line %d   %s - %s"),
			line->Row + 1, start.c_str(), end.c_str()));
		bool previously_accepted = replaying && initial_decisions[issue_index] >= 0;
		SetMinSize(FromDIP(wxSize(700, 600)));
		SetSize(FromDIP(wxSize(900, 700)));
		review_panel->Show();
		back_button->Show();
		back_button->Enable(issue_index > 0);
		skip_button->Show();
		apply_button->SetLabel(_("Approve selected correction"));
		apply_button->Show(!previously_accepted);
		cancel_button->SetLabel(_("Cancel review"));
		cancel_button->Show();

		wxString categories;
		for (auto const& category : issue.categories) {
			if (!categories.empty()) categories += "  |  ";
			categories += category_name(category);
		}
		if (previously_accepted) {
			if (!categories.empty()) categories += "  |  ";
			categories += _("Previously accepted");
			category_label->SetForegroundColour(wxColour(35, 135, 70));
		}
		else {
			category_label->SetForegroundColour(wxColour(190, 85, 20));
		}
		category_label->SetLabel(categories);
		explanation->SetValue(to_wx(issue.explanation));
		current_text->SetValue(to_wx(agi::util::clean_ass_text(line->Text.get())));
		source_text->SetValue(to_wx(agi::util::clean_ass_text(line->SourceLineText.get())));

		alternatives->Clear();
		for (size_t i = 0; i < issue.suggestions.size(); ++i) {
			auto display = agi::util::clean_ass_text(issue.suggestions[i]);
			alternatives->Append(wxString::Format("%d. ", static_cast<int>(i + 1)) + to_wx(display));
		}
		int selection = previously_accepted ? initial_decisions[issue_index] : 0;
		if (selection < 0 || selection >= static_cast<int>(issue.suggestions.size()))
			selection = wxNOT_FOUND;
		alternatives->SetSelection(selection);
		alternatives->Enable(!previously_accepted);
		UpdateSelection();
		Layout();
	}

	void UpdateSelection() {
		if (issue_index >= result.issues.size()) return;
		if (replaying && initial_decisions[issue_index] >= 0) {
			apply_button->Disable();
			return;
		}
		auto selection = alternatives->GetSelection();
		if (selection == wxNOT_FOUND) {
			apply_button->Disable();
			return;
		}
		apply_button->Enable();
	}

	void Apply() {
		StopPlayback();
		auto line = CurrentLine();
		auto selection = alternatives->GetSelection();
		if (!line || selection == wxNOT_FOUND) return;
		line->Text = result.issues[issue_index].suggestions[selection];
		decisions[issue_index] = selection;
		++accepted;
		++issue_index;
		ShowIssue();
	}

	void Skip() {
		StopPlayback();
		if (!(replaying && initial_decisions[issue_index] >= 0)) {
			decisions[issue_index] = decision_skipped;
			++skipped;
		}
		++issue_index;
		ShowIssue();
	}

	void Back() {
		if (!issue_index) return;
		StopPlayback();
		--issue_index;
		auto decision = decisions[issue_index];
		auto initial_decision = initial_decisions[issue_index];
		auto const& issue = result.issues[issue_index];
		auto line_it = line_by_id.find(issue.line_id);
		if (decision >= 0 && initial_decision < 0) {
			if (line_it != line_by_id.end())
				line_it->second->Text = original_text_by_id.at(issue.line_id);
			--accepted;
		}
		else if (decision == decision_skipped && initial_decision == decision_pending) {
			--skipped;
		}
		decisions[issue_index] = initial_decision;
		ShowIssue();
	}

	void Finish() {
		StopPlayback();
		reviewing = false;
		progress->SetValue(progress->GetRange());
		if (accepted > 0)
			context->ass->Commit(_("apply AI post-check corrections"), AssFile::COMMIT_DIAG_TEXT);
		remember_session(context, result, context_lines, decisions);
		EndModal(wxID_OK);
	}

	void AbortReview() {
		StopPlayback();
		for (size_t i = 0; i < decisions.size(); ++i) {
			if (decisions[i] < 0 || initial_decisions[i] >= 0) continue;
			auto line_it = line_by_id.find(result.issues[i].line_id);
			if (line_it != line_by_id.end())
				line_it->second->Text = original_text_by_id.at(result.issues[i].line_id);
		}
		decisions = initial_decisions;
		remember_session(context, result, context_lines, decisions);
		reviewing = false;
		EndModal(wxID_CANCEL);
	}

	void CancelOrClose() {
		if (busy) {
			close_when_idle = true;
			cancelled.store(true);
			status->SetLabel(_("Cancelling the request..."));
			cancel_button->Disable();
			return;
		}
		if (reviewing) {
			AbortReview();
			return;
		}
		StopPlayback();
		EndModal(wxID_CANCEL);
	}

	void OnClose(wxCloseEvent& event) {
		if (busy) {
			CancelOrClose();
			event.Veto();
			return;
		}
		CancelOrClose();
	}

public:
	AIProofreadDialog(agi::Context *context, std::vector<AssDialogue *> target_lines,
		std::vector<ai::SubtitleLine> context_lines, bool replaying = false,
		ai::ProofreadResult replay_result = {}, std::vector<int> replay_decisions = {})
	: wxDialog(context->parent, wxID_ANY, _("AI post-check"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context)
	, target_lines(std::move(target_lines))
	, context_lines(std::move(context_lines))
	, pulse_timer(this)
	, result(std::move(replay_result))
	, decisions(std::move(replay_decisions))
	, replaying(replaying) {
		if (this->replaying) {
			if (decisions.size() != result.issues.size())
				decisions.assign(result.issues.size(), decision_pending);
			initial_decisions = decisions;
		}
		for (auto line : this->target_lines) {
			line_by_id[line->Id] = line;
			original_text_by_id[line->Id] = line->Text.get();
		}

		auto main = new wxBoxSizer(wxVERTICAL);
		auto analysis = new wxBoxSizer(wxVERTICAL);
		// Name the language being checked, since it is now configurable.
		auto language = ai::GetCheckLanguage();
		auto title = new wxStaticText(this, wxID_ANY, language.empty() ?
			_("Subtitle post-check") :
			agi::wxformat(_("%s subtitle post-check"), to_wx(language)));
		auto title_font = title->GetFont();
		title_font.SetWeight(wxFONTWEIGHT_BOLD);
		title_font.SetPointSize(title_font.GetPointSize() + 2);
		title->SetFont(title_font);
		analysis->Add(title, wxSizerFlags().Border(wxBOTTOM, 6));

		status = new wxStaticText(this, wxID_ANY, _("Preparing AI analysis..."));
		analysis->Add(status, wxSizerFlags().Expand().Border(wxBOTTOM, 6));
		progress = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(-1, 12)));
		app_theme::StyleProgress(progress);
		analysis->Add(progress, wxSizerFlags().Expand());
		main->Add(analysis, wxSizerFlags().Expand().Border(wxALL, 12));

		review_panel = new wxPanel(this);
		auto review = new wxBoxSizer(wxVERTICAL);
		review_panel->SetSizer(review);
		line_heading = new wxStaticText(review_panel, wxID_ANY, "");
		auto heading_font = line_heading->GetFont();
		heading_font.SetWeight(wxFONTWEIGHT_BOLD);
		line_heading->SetFont(heading_font);
		review->Add(line_heading, wxSizerFlags().Expand().Border(wxBOTTOM, 5));
		auto playback_buttons = new wxBoxSizer(wxHORIZONTAL);
		play_line_button = new wxButton(review_panel, wxID_ANY, _("Play line"));
		play_scene_button = new wxButton(review_panel, wxID_ANY, _("Play full scene"));
		stop_playback_button = new wxButton(review_panel, wxID_ANY, _("Stop playback"));
		playback_buttons->Add(play_line_button);
		playback_buttons->Add(play_scene_button, wxSizerFlags().Border(wxLEFT, 6));
		playback_buttons->Add(stop_playback_button, wxSizerFlags().Border(wxLEFT, 6));
		review->Add(playback_buttons, wxSizerFlags().Border(wxBOTTOM, 5));
		category_label = new wxStaticText(review_panel, wxID_ANY, "");
		category_label->SetForegroundColour(wxColour(190, 85, 20));
		review->Add(category_label, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
		explanation = make_readonly(review_panel, 62);
		review->Add(explanation, wxSizerFlags().Expand().Border(wxBOTTOM, 8));

		auto texts = new wxBoxSizer(wxHORIZONTAL);
		auto current_box = new wxStaticBoxSizer(wxVERTICAL, review_panel, _("Current subtitle"));
		current_text = make_readonly(current_box->GetStaticBox(), 82);
		current_box->Add(current_text, wxSizerFlags(1).Expand().Border(wxALL, 5));
		texts->Add(current_box, wxSizerFlags(1).Expand().Border(wxRIGHT, 5));
		auto source_box = new wxStaticBoxSizer(wxVERTICAL, review_panel, _("Source line"));
		source_text = make_readonly(source_box->GetStaticBox(), 82);
		source_box->Add(source_text, wxSizerFlags(1).Expand().Border(wxALL, 5));
		texts->Add(source_box, wxSizerFlags(1).Expand().Border(wxLEFT, 5));
		review->Add(texts, wxSizerFlags().Expand().Border(wxBOTTOM, 8));

		auto alternatives_box = new wxStaticBoxSizer(wxVERTICAL, review_panel,
			_("Choose a proposed correction"));
		alternatives = new wxListBox(alternatives_box->GetStaticBox(), wxID_ANY,
			wxDefaultPosition, wxSize(-1, 155), 0, nullptr, wxLB_SINGLE | wxLB_HSCROLL);
		alternatives_box->Add(alternatives, wxSizerFlags().Expand().Border(wxALL, 5));
		review->Add(alternatives_box, wxSizerFlags(1).Expand());
		main->Add(review_panel, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT, 14));

		main->Add(new wxStaticLine(this), wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT, 12));
		auto buttons = new wxBoxSizer(wxHORIZONTAL);
		cancel_button = new wxButton(this, wxID_ANY, _("Cancel request"));
		buttons->Add(cancel_button);
		buttons->AddStretchSpacer();
		back_button = new wxButton(this, wxID_ANY, _("Back"));
		buttons->Add(back_button);
		skip_button = new wxButton(this, wxID_ANY, _("Skip"));
		buttons->Add(skip_button, wxSizerFlags().Border(wxLEFT, 8));
		apply_button = new wxButton(this, wxID_ANY, _("Approve selected correction"));
		buttons->Add(apply_button, wxSizerFlags().Border(wxLEFT, 8));
		main->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 12));

		review_panel->Hide();
		apply_button->Hide();
		skip_button->Hide();
		back_button->Hide();
		SetSizer(main);
		SetSize(FromDIP(wxSize(680, 195)));
		SetMinSize(FromDIP(wxSize(560, 185)));
		CenterOnParent();

		Bind(EVT_AI_PROOFREAD_DONE, &AIProofreadDialog::OnRequestDone, this);
		Bind(wxEVT_CLOSE_WINDOW, &AIProofreadDialog::OnClose, this);
		Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
			if ((reviewing || busy) && event.GetKeyCode() == WXK_ESCAPE) {
				CancelOrClose();
				return;
			}
			event.Skip();
		});
		Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (busy) progress->Pulse(); });
		cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CancelOrClose(); });
		play_line_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PlayLine(); });
		play_scene_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PlayScene(); });
		stop_playback_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StopPlayback(); });
		back_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Back(); });
		bool video_open = context->project->VideoProvider() != nullptr;
		play_line_button->Enable(video_open);
		play_scene_button->Enable(video_open);
		stop_playback_button->Enable(video_open);
		skip_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Skip(); });
		apply_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Apply(); });
		alternatives->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { UpdateSelection(); });
		CallAfter([this] {
			if (this->replaying) StartReplay();
			else StartRequest();
		});
	}
};

} // namespace

void ShowAIProofreadDialog(agi::Context *context,
	std::vector<AssDialogue *> target_lines,
	std::vector<ai::SubtitleLine> context_lines) {
	AIProofreadDialog dialog(context, std::move(target_lines), std::move(context_lines));
	dialog.ShowModal();
}

bool HasLatestAIProofread(agi::Context const *context) {
	return latest_session_applies_to(context);
}

void ShowLatestAIProofreadDialog(agi::Context *context) {
	if (!latest_session_applies_to(context)) {
		wxMessageBox(_("There is no latest AI post-check available for this subtitle."),
			_("AI post-check"), wxOK | wxICON_INFORMATION, context->parent);
		return;
	}

	auto lines_by_id = current_lines_by_id(context);
	std::vector<AssDialogue *> target_lines;
	auto context_lines = latest_session.context_lines;
	for (auto& cached : context_lines) {
		auto current = lines_by_id.find(cached.id);
		if (current == lines_by_id.end()) continue;
		auto line = current->second;
		target_lines.push_back(line);
		cached.start_ms = static_cast<int>(line->Start);
		cached.end_ms = static_cast<int>(line->End);
		cached.source_text = agi::util::clean_ass_text(line->SourceLineText.get());
		cached.current_text = agi::util::clean_ass_text(line->GetStrippedText());
		cached.actor = line->Actor.get();
		cached.style = line->Style.get();
		cached.ass_text = line->Text.get();
	}

	AIProofreadDialog dialog(context, std::move(target_lines), std::move(context_lines), true,
		latest_session.result, latest_session.decisions);
	dialog.ShowModal();
}
