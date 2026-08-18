// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "dialog_ai_translate.h"

#include "ass_dialogue.h"
#include "audio_controller.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "time_range.h"
#include "theme.h"
#include "video_controller.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/option.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {

wxDEFINE_EVENT(EVT_AI_REQUEST_DONE, wxThreadEvent);

struct RequestOutcome {
	bool initial = false;
	bool cancelled = false;
	std::string error;
	ai::ReviewResult result;
};

wxTextCtrl *make_heading(wxWindow *parent, wxString const& value) {
	auto label = new wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition,
		wxDefaultSize, wxTE_READONLY | wxBORDER_NONE);
	auto font = label->GetFont();
	font.SetWeight(wxFONTWEIGHT_BOLD);
	label->SetFont(font);
	label->SetBackgroundColour(parent->GetBackgroundColour());
	label->SetForegroundColour(parent->GetForegroundColour());
	auto height = label->GetCharHeight() + parent->FromDIP(2);
	label->SetMinSize(wxSize(-1, height));
	label->SetMaxSize(wxSize(-1, height));
	label->SetSelection(0, 0);
	return label;
}

wxStaticText *make_title(wxWindow *parent, wxString const& value) {
	auto title = new wxStaticText(parent, wxID_ANY, value);
	auto font = title->GetFont();
	font.SetWeight(wxFONTWEIGHT_BOLD);
	font.SetPointSize(font.GetPointSize() + 2);
	title->SetFont(font);
	return title;
}

wxTextCtrl *make_selectable_text(wxWindow *parent, wxString const& value,
	size_t characters_per_line = 82) {
	auto text = new wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition,
		wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_NO_VSCROLL | wxBORDER_NONE);
	text->SetBackgroundColour(parent->GetBackgroundColour());
	text->SetForegroundColour(parent->GetForegroundColour());

	// Read-only text controls allow selection and copying. Give them enough
	// height to behave like normal labels inside the outer scrolling chat view.
	size_t visual_lines = 0;
	size_t line_start = 0;
	for (size_t i = 0; i <= value.length(); ++i) {
		if (i != value.length() && value[i] != '\n') continue;
		auto length = i - line_start;
		visual_lines += std::max<size_t>(1, (length + characters_per_line - 1) / characters_per_line);
		line_start = i + 1;
	}
	auto height = static_cast<int>(visual_lines) * text->GetCharHeight() + parent->FromDIP(2);
	text->SetMinSize(wxSize(-1, height));
	text->SetMaxSize(wxSize(-1, height));
	text->SetSelection(0, 0);
	return text;
}

wxString format_timestamp(int time_ms) {
	time_ms = std::max(0, time_ms);
	return wxString::Format("%d:%02d.%03d", time_ms / 60000,
		(time_ms / 1000) % 60, time_ms % 1000);
}

wxString verdict_text(std::string const& verdict) {
	if (verdict == "ok") return _("OK - no change needed");
	if (verdict == "minor_issue") return _("Minor issue");
	if (verdict == "major_issue") return _("Major issue");
	return _("Review result");
}

wxColour verdict_colour(std::string const& verdict) {
	if (verdict == "ok") return wxColour(35, 125, 65);
	if (verdict == "minor_issue") return wxColour(190, 115, 0);
	if (verdict == "major_issue") return wxColour(190, 45, 45);
	return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
}

class AIReviewDialog final : public wxDialog {
	agi::Context *context;
	std::vector<AssDialogue *> subtitle_lines;
	std::vector<ai::SubtitleLine> input_lines;
	agi::fs::path audio_file;

	wxStaticText *status;
	wxGauge *progress;
	wxScrolledWindow *conversation_panel;
	wxBoxSizer *conversation_sizer;
	wxTextCtrl *chat_input;
	wxButton *send_button;
	wxButton *play_scene_button;
	wxButton *stop_audio_button;
	wxButton *cancel_request_button;
	wxButton *close_button;

	ai::ReviewResult current_result;
	std::atomic_bool cancelled{false};
	bool busy = false;
	bool close_when_idle = false;
	int wheel_remainder = 0;
	std::unordered_set<wxWindow *> wheel_forwarding_controls;

	std::string ApiKey() const { return ai::GetApiKey(); }
	std::string Model() const { return OPT_GET("AI/OpenAI/Model")->GetString(); }
	std::string TranscriptionModel() const { return "gpt-transcribe"; }
	void SetBusy(bool value, wxString const& message = {}) {
		busy = value;
		if (value && context->audioController && context->audioController->IsPlaying())
			context->audioController->Stop();
		status->SetLabel(message);
		progress->Show(value);
		if (value) progress->Pulse();
		conversation_panel->Enable(!value);
		play_scene_button->Enable(!value);
		stop_audio_button->Enable(!value);
		chat_input->Enable(!value && !current_result.lines.empty());
		send_button->Enable(!value && !current_result.lines.empty());
		close_button->Enable(!value);
		cancel_request_button->Show(value);
		cancel_request_button->Enable(value);
		Layout();
	}

	void PostOutcome(std::shared_ptr<RequestOutcome> outcome) {
		auto event = new wxThreadEvent(EVT_AI_REQUEST_DONE);
		event->SetPayload(std::move(outcome));
		wxQueueEvent(this, event);
	}

	void PlayRange(int start_ms, int end_ms) {
		if (!context->audioController || end_ms <= start_ms) return;
		if (context->videoController) context->videoController->Stop();
		context->audioController->PlayRange(TimeRange(start_ms, end_ms));
	}

	void PlayScene() {
		int start = INT_MAX;
		int end = 0;
		for (auto line : subtitle_lines) {
			start = std::min(start, static_cast<int>(line->Start));
			end = std::max(end, static_cast<int>(line->End));
		}
		if (start != INT_MAX) PlayRange(start, end);
	}

	void PlayLine(size_t index) {
		if (index >= subtitle_lines.size()) return;
		PlayRange(static_cast<int>(subtitle_lines[index]->Start),
			static_cast<int>(subtitle_lines[index]->End));
	}

	void StopAudio() {
		if (context->audioController) context->audioController->Stop();
	}

	void CancelRequest() {
		if (!busy) return;
		cancelled.store(true);
		status->SetLabel(_("Cancelling the request..."));
		cancel_request_button->Disable();
	}

	void RefreshConversation() {
		EnableConversationScrolling(conversation_panel);
		conversation_panel->FitInside();
		conversation_panel->Layout();
		Layout();
		CallAfter([this] {
			conversation_panel->Scroll(-1, conversation_panel->GetScrollRange(wxVERTICAL));
		});
	}

	void OnConversationMouseWheel(wxMouseEvent& event) {
		if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL || !event.GetWheelDelta()) {
			event.Skip();
			return;
		}

		wheel_remainder += event.GetWheelRotation();
		auto steps = wheel_remainder / event.GetWheelDelta();
		wheel_remainder %= event.GetWheelDelta();
		if (!steps) return;

		if (event.IsPageScroll())
			conversation_panel->ScrollPages(-steps);
		else
			conversation_panel->ScrollLines(-steps * std::max(1, event.GetLinesPerAction()));
	}

	void EnableConversationScrolling(wxWindow *window) {
		for (auto child : window->GetChildren()) {
			if (wheel_forwarding_controls.insert(child).second)
				child->Bind(wxEVT_MOUSEWHEEL, &AIReviewDialog::OnConversationMouseWheel, this);
			EnableConversationScrolling(child);
		}
	}

	void AddMessage(wxString const& author, wxString const& message) {
		auto message_panel = new wxPanel(conversation_panel, wxID_ANY,
			wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
		auto box = new wxBoxSizer(wxVERTICAL);
		message_panel->SetSizer(box);
		box->Add(make_heading(message_panel, author),
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP, 6));
		auto text = make_selectable_text(message_panel, message, 92);
		box->Add(text, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
		conversation_sizer->Add(message_panel,
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
		RefreshConversation();
	}

	void AddSection(wxBoxSizer *box, wxWindow *parent,
		wxString const& heading, wxString const& value) {
		box->Add(make_heading(parent, heading),
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP, 6));
		auto text = make_selectable_text(parent, value.empty() ? _("(empty)") : value);
		auto value_row = new wxBoxSizer(wxHORIZONTAL);
		value_row->AddSpacer(parent->FromDIP(14));
		value_row->Add(text, wxSizerFlags(1).Expand());
		value_row->AddSpacer(parent->FromDIP(6));
		box->Add(value_row, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	}

	void AddReview(ai::ReviewResult const& result) {
		auto response_parent = new wxPanel(conversation_panel, wxID_ANY,
			wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
		auto response = new wxBoxSizer(wxVERTICAL);
		response_parent->SetSizer(response);
		response->Add(make_heading(response_parent, _("AI review")),
			wxSizerFlags().Expand().Border(wxALL, 6));

		if (!result.message.empty()) {
			auto summary = make_selectable_text(response_parent, to_wx(result.message), 90);
			response->Add(summary, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
		}

		std::unordered_map<int, ai::LineReview const *> reviews;
		for (auto const& review : result.lines)
			reviews[review.id] = &review;

		for (size_t index = 0; index < input_lines.size(); ++index) {
			auto const& input = input_lines[index];
			auto review_it = reviews.find(input.id);
			auto review = review_it == reviews.end() ? nullptr : review_it->second;

			auto line_title = to_wx(agi::format(_("Line %d"), static_cast<int>(index + 1)));
			line_title += "   ";
			line_title += format_timestamp(static_cast<int>(subtitle_lines[index]->Start));
			line_title += L" \u2013 ";
			line_title += format_timestamp(static_cast<int>(subtitle_lines[index]->End));
			auto line_parent = new wxPanel(response_parent, wxID_ANY,
				wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
			auto line_box = new wxBoxSizer(wxVERTICAL);
			line_parent->SetSizer(line_box);
			line_box->Add(make_heading(line_parent, line_title),
				wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxTOP, 6));

			auto top = new wxBoxSizer(wxHORIZONTAL);
			auto verdict = make_heading(line_parent,
				review ? verdict_text(review->verdict) : _("No review returned"));
			if (review) verdict->SetForegroundColour(verdict_colour(review->verdict));
			top->Add(verdict, wxSizerFlags(1).CenterVertical().Border(wxRIGHT, 6));
			auto play = new wxButton(line_parent, wxID_ANY, _("Play this line"));
			play->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) { PlayLine(index); });
			top->Add(play);
			line_box->Add(top, wxSizerFlags().Expand().Border(wxALL, 6));

			AddSection(line_box, line_parent, _("Current subtitle"), to_wx(input.current_text));
			AddSection(line_box, line_parent, _("English source"), to_wx(input.source_text));

			wxString japanese = review ? to_wx(review->japanese) : wxString{};
			if (review && !review->romaji.empty()) {
				if (!japanese.empty()) japanese += "\n";
				japanese += _("Romaji: ") + to_wx(review->romaji);
			}
			AddSection(line_box, line_parent, _("Japanese / Romaji"), japanese);

			if (review) {
				AddSection(line_box, line_parent, _("Assessment"), to_wx(review->assessment));
				if (!review->issues.empty()) {
					wxString issues;
					for (auto const& issue : review->issues)
						issues += L"\u2022 " + to_wx(issue) + "\n";
					issues.Trim();
					AddSection(line_box, line_parent, _("Issues found"), issues);
				}
				if (!review->suggested_text.empty())
					AddSection(line_box, line_parent, _("Suggested correction"), to_wx(review->suggested_text));
			}

			response->Add(line_parent,
				wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
		}

		conversation_sizer->Add(response_parent,
			wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
		RefreshConversation();
	}

	void StartInitialRequest() {
		cancelled.store(false);
		SetBusy(true, _("Transcribing the Japanese audio and reviewing the subtitles..."));
		auto key = ApiKey();
		auto model = Model();
		auto transcription_model = TranscriptionModel();
		auto audio = audio_file;
		auto lines = input_lines;

		agi::dispatch::Background().Async([this, key = std::move(key), model = std::move(model),
			transcription_model = std::move(transcription_model),
			audio = std::move(audio), lines = std::move(lines)]() mutable {
			auto outcome = std::make_shared<RequestOutcome>();
			outcome->initial = true;
			try {
				ai::OpenAIClient client(std::move(key), std::move(model),
					std::move(transcription_model), {}, &cancelled);
				auto transcript = client.Transcribe(audio);
				if (cancelled.load()) throw ai::Error("A kérés megszakítva.");
				outcome->result = client.Review(lines, transcript);
			}
			catch (std::exception const& error) {
				outcome->cancelled = cancelled.load();
				outcome->error = error.what();
			}
			PostOutcome(std::move(outcome));
		});
	}

	void StartChatRequest() {
		auto message = from_wx(chat_input->GetValue());
		if (message.empty() || busy) return;
		chat_input->Clear();
		AddMessage(_("You"), to_wx(message));
		SetBusy(true, _("The AI is reviewing your follow-up question..."));
		cancelled.store(false);

		auto key = ApiKey();
		auto model = Model();
		auto transcription_model = TranscriptionModel();
		auto previous = current_result;

		agi::dispatch::Background().Async([this, key = std::move(key), model = std::move(model),
			transcription_model = std::move(transcription_model),
			previous = std::move(previous), message = std::move(message)]() mutable {
			auto outcome = std::make_shared<RequestOutcome>();
			try {
				ai::OpenAIClient client(std::move(key), std::move(model),
					std::move(transcription_model), {}, &cancelled);
				outcome->result = client.Continue(previous, message);
			}
			catch (std::exception const& error) {
				outcome->cancelled = cancelled.load();
				outcome->error = error.what();
			}
			PostOutcome(std::move(outcome));
		});
	}

	void OnRequestDone(wxThreadEvent& event) {
		auto outcome = event.GetPayload<std::shared_ptr<RequestOutcome>>();
		if (close_when_idle) {
			SetBusy(false, {});
			EndModal(wxID_CANCEL);
			return;
		}

		if (!outcome->error.empty()) {
			SetBusy(false, {});
			if (!outcome->cancelled)
				wxMessageBox(to_wx(outcome->error), _("AI request failed"),
					wxOK | wxICON_ERROR, this);
			status->SetLabel(outcome->cancelled ? _("Request cancelled.") :
				_("The request failed. You can close the dialog or retry by reopening it."));
			return;
		}

		current_result = std::move(outcome->result);
		SetBusy(false, {});
		if (outcome->initial)
			AddReview(current_result);
		else
			AddMessage(_("AI"), to_wx(current_result.message));
		status->SetLabel(outcome->initial ? _("Subtitle review is ready.") : _("Follow-up review is ready."));
		chat_input->SetFocus();
	}

	void RequestClose() {
		if (!busy) {
			EndModal(wxID_CANCEL);
			return;
		}
		close_when_idle = true;
		CancelRequest();
	}

	void OnCloseWindow(wxCloseEvent& event) {
		if (busy) {
			RequestClose();
			event.Veto();
		}
		else {
			EndModal(wxID_CANCEL);
		}
	}

public:
	AIReviewDialog(agi::Context *context,
		std::vector<AssDialogue *> subtitle_lines,
		std::vector<ai::SubtitleLine> input_lines,
		agi::fs::path audio_file)
	: wxDialog(context->parent, wxID_ANY, _("AI subtitle review"),
		wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context)
	, subtitle_lines(std::move(subtitle_lines))
	, input_lines(std::move(input_lines))
	, audio_file(std::move(audio_file)) {
		auto main = new wxBoxSizer(wxVERTICAL);

		auto header = new wxBoxSizer(wxHORIZONTAL);
		auto header_text = new wxBoxSizer(wxVERTICAL);
		auto title = make_title(this, _("AI subtitle quality review"));
		header_text->Add(title);
		auto summary = new wxStaticText(this, wxID_ANY,
			to_wx(agi::format(_("Selected lines: %d"), static_cast<int>(this->input_lines.size()))));
		header_text->Add(summary, wxSizerFlags().Border(wxTOP, 4));
		header->Add(header_text, wxSizerFlags(1).CenterVertical());
		play_scene_button = new wxButton(this, wxID_ANY, _("Play full scene"));
		header->Add(play_scene_button, wxSizerFlags().CenterVertical().Border(wxLEFT, 12));
		stop_audio_button = new wxButton(this, wxID_ANY, _("Stop playback"));
		header->Add(stop_audio_button, wxSizerFlags().CenterVertical().Border(wxLEFT, 8));
		main->Add(header, wxSizerFlags().Expand().Border(wxALL, 12));

		auto status_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Request status"));
		auto status_row = new wxBoxSizer(wxHORIZONTAL);
		status = new wxStaticText(status_box->GetStaticBox(), wxID_ANY, _("Preparing AI request..."));
		status_row->Add(status, wxSizerFlags(1).CenterVertical());
		cancel_request_button = new wxButton(status_box->GetStaticBox(), wxID_ANY, _("Cancel request"));
		cancel_request_button->Hide();
		status_row->Add(cancel_request_button, wxSizerFlags().CenterVertical().Border(wxLEFT, 8));
		status_box->Add(status_row, wxSizerFlags().Expand().Border(wxALL, 6));
		progress = new wxGauge(status_box->GetStaticBox(), wxID_ANY, 100);
		app_theme::StyleProgress(progress);
		progress->Hide();
		status_box->Add(progress, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
		main->Add(status_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 12));

		conversation_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition,
			wxSize(-1, 600), wxVSCROLL | wxBORDER_NONE);
		conversation_panel->SetScrollRate(0, 12);
		conversation_sizer = new wxBoxSizer(wxVERTICAL);
		conversation_sizer->Add(make_heading(conversation_panel, _("Conversation")),
			wxSizerFlags().Border(wxALL, 10));
		conversation_panel->SetSizer(conversation_sizer);
		main->Add(conversation_panel, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT, 12));

		auto input_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Ask a follow-up question"));
		auto input_row = new wxBoxSizer(wxHORIZONTAL);
		chat_input = new wxTextCtrl(input_box->GetStaticBox(), wxID_ANY, "",
			wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
		chat_input->SetHint(_("Ask about meaning, tone, wording, or a specific line..."));
		send_button = new wxButton(input_box->GetStaticBox(), wxID_ANY, _("Send"));
		input_row->Add(chat_input, wxSizerFlags(1).Expand());
		input_row->Add(send_button, wxSizerFlags().Border(wxLEFT, 8));
		input_box->Add(input_row, wxSizerFlags().Expand().Border(wxALL, 8));
		main->Add(input_box, wxSizerFlags().Expand().Border(wxALL, 12));

		auto buttons = new wxBoxSizer(wxHORIZONTAL);
		buttons->AddStretchSpacer();
		close_button = new wxButton(this, wxID_CANCEL, _("Close"));
		buttons->Add(close_button);
		main->Add(buttons, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 12));

		SetSizer(main);
		SetSize(FromDIP(wxSize(1020, 900)));
		SetMinSize(FromDIP(wxSize(760, 640)));
		CenterOnParent();

		Bind(EVT_AI_REQUEST_DONE, &AIReviewDialog::OnRequestDone, this);
		Bind(wxEVT_CLOSE_WINDOW, &AIReviewDialog::OnCloseWindow, this);
		close_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RequestClose(); });
		play_scene_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PlayScene(); });
		stop_audio_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StopAudio(); });
		cancel_request_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CancelRequest(); });
		send_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartChatRequest(); });
		chat_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { StartChatRequest(); });

		chat_input->Disable();
		send_button->Disable();
		CallAfter([this] { StartInitialRequest(); });
	}

	~AIReviewDialog() override {
		if (context->audioController) context->audioController->Stop();
	}
};

} // namespace

void ShowAIReviewDialog(agi::Context *context,
	std::vector<AssDialogue *> lines,
	std::vector<ai::SubtitleLine> input,
	agi::fs::path audio_file) {
	AIReviewDialog dialog(context, std::move(lines), std::move(input),
		std::move(audio_file));
	dialog.ShowModal();
}
