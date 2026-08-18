// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "command.h"

#include "../ai_client.h"
#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../compat.h"
#include "../dialog_ai_connection.h"
#include "../dialog_ai_karaoke.h"
#include "../dialog_ai_proofread.h"
#include "../dialog_ai_translate.h"
#include "../format.h"
#include "../include/aegisub/context.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/character_count.h>
#include <libaegisub/option.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <climits>
#include <utility>
#include <vector>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/choicdlg.h>
#include <wx/msgdlg.h>

namespace {

constexpr int max_scene_duration_ms = 120000;
constexpr size_t max_review_lines = 100;
constexpr int audio_padding_ms = 200;

bool is_default_dialogue_style(std::string const& style) {
	return style == "Default" || style.compare(0, 10, "Default - ") == 0;
}

bool has_subtitle_text(AssDialogue const *line) {
	return line && !line->Comment && !agi::util::clean_ass_text(line->GetStrippedText()).empty();
}

struct TemporaryFile final {
	wxString path;
	~TemporaryFile() {
		if (!path.empty() && wxFileExists(path))
			wxRemoveFile(path);
	}
};

/// Ask which language the review and the post-check should work in, once, and
/// remember the answer. Both commands need it before they can build their prompts.
bool EnsureCheckLanguage(wxWindow *parent) {
	if (!ai::GetCheckLanguage().empty()) return true;
	auto languages = ai::CheckLanguageChoices();
	wxArrayString choices;
	choices.reserve(languages.size());
	for (auto const& language : languages) choices.push_back(to_wx(language));
	auto found = std::find(languages.begin(), languages.end(), std::string("English"));
	int preselect = found == languages.end() ? 0 :
		static_cast<int>(std::distance(languages.begin(), found));
	wxSingleChoiceDialog dialog(parent,
		_("Which language are the subtitles you are checking written in?\n\nYou can change this later in Preferences, under General."),
		_("AI check language"), choices);
	dialog.SetSelection(preselect);
	if (dialog.ShowModal() != wxID_OK) return false;
	int selected = dialog.GetSelection();
	if (selected < 0 || selected >= static_cast<int>(languages.size())) return false;
	ai::SetCheckLanguage(languages[selected]);
	return true;
}

struct ai_configure final : public cmd::Command {
	CMD_NAME("ai/configure")
	STR_MENU("Configure AI connection...")
	STR_DISP("Configure AI connection")
	STR_HELP("Configure the per-user OpenAI and Cloudinary API connections")

	void operator()(agi::Context *c) override {
		ShowAIConnectionDialog(c->parent, false);
	}
};

struct ai_review final : public cmd::Command {
	CMD_NAME("ai/review")
	STR_MENU("Review selected lines with AI...")
	STR_DISP("Review selected lines with AI")
	STR_HELP("Check up to 100 lines and two minutes of subtitles against Japanese audio and the source lines")
	CMD_TYPE(cmd::COMMAND_VALIDATE)

	bool Validate(agi::Context const *c) override {
		return c->project->AudioProvider() && !c->selectionController->GetSelectedSet().empty();
	}

	void operator()(agi::Context *c) override {
		if (ai::GetApiKey().empty() && !ShowAIConnectionDialog(c->parent, true))
			return;
		if (!EnsureCheckLanguage(c->parent)) return;

		auto lines = c->selectionController->GetSortedSelection();
		if (lines.empty() || !c->project->AudioProvider()) return;
		if (lines.size() > max_review_lines) {
			wxMessageBox(fmt_tl(
				"At most 100 subtitle lines can be reviewed at once. The current selection contains %d lines. Select a smaller scene.",
				static_cast<int>(lines.size())),
				_("AI subtitle review"), wxOK | wxICON_WARNING, c->parent);
			return;
		}

		int start = INT_MAX;
		int end = 0;
		for (auto line : lines) {
			start = std::min(start, static_cast<int>(line->Start));
			end = std::max(end, static_cast<int>(line->End));
			if (agi::util::clean_ass_text(line->SourceLineText.get()).empty()) {
				wxMessageBox(fmt_tl("Selected line %d has no English source text.", line->Row + 1),
					_("AI subtitle review"), wxOK | wxICON_WARNING, c->parent);
				return;
			}
		}

		if (end - start > max_scene_duration_ms) {
			auto duration = end - start;
			wxMessageBox(fmt_tl(
				"The selected scene is %d:%02d long. AI subtitle review can process scenes up to 2:00. Select a shorter scene.",
				duration / 60000, (duration / 1000) % 60),
				_("AI subtitle review"), wxOK | wxICON_WARNING, c->parent);
			return;
		}

		int clip_start = std::max(0, start - audio_padding_ms);
		int clip_end = end + audio_padding_ms;
		TemporaryFile temporary;
		auto base = wxFileName::CreateTempFileName("aegisub-ai-");
		if (base.empty()) {
			wxMessageBox(_("A temporary audio file could not be created."),
				_("AI subtitle review"), wxOK | wxICON_ERROR, c->parent);
			return;
		}
		temporary.path = base + ".wav";
		if (!wxRenameFile(base, temporary.path, true)) {
			wxRemoveFile(base);
			wxMessageBox(_("The temporary audio file could not be prepared."),
				_("AI subtitle review"), wxOK | wxICON_ERROR, c->parent);
			return;
		}

		try {
			agi::SaveAudioClip(*c->project->AudioProvider(),
				agi::fs::path(from_wx(temporary.path)), clip_start, clip_end);
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("The audio clip could not be created"),
				wxOK | wxICON_ERROR, c->parent);
			return;
		}

		std::vector<ai::SubtitleLine> input;
		input.reserve(lines.size());
		for (auto line : lines) {
			input.push_back({
				line->Id,
				static_cast<int>(line->Start) - clip_start,
				static_cast<int>(line->End) - clip_start,
				agi::util::clean_ass_text(line->SourceLineText.get()),
				agi::util::clean_ass_text(line->GetStrippedText()),
				line->Actor.get(),
				line->Style.get()
			});
		}

		// ShowModal inside this function locks the rest of Aegisub for the whole
		// conversation. Therefore the captured line pointers and selection cannot
		// drift before the user explicitly applies the final suggestions.
		ShowAIReviewDialog(c, std::move(lines), std::move(input),
			agi::fs::path(from_wx(temporary.path)));
	}
};

struct ai_proofread final : public cmd::Command {
	CMD_NAME("ai/proofread")
	STR_MENU("New")
	STR_DISP("New AI post-check")
	STR_HELP("Start a new AI post-check and approve corrections one by one")
	CMD_TYPE(cmd::COMMAND_VALIDATE)

	bool Validate(agi::Context const *c) override {
		if (!c->ass) return false;

		auto const& selected = c->selectionController->GetSelectedSet();
		if (selected.size() >= 100)
			return std::any_of(selected.begin(), selected.end(), has_subtitle_text);

		return std::any_of(c->ass->Events.begin(), c->ass->Events.end(),
			[](AssDialogue const& line) {
				return is_default_dialogue_style(line.Style.get()) && has_subtitle_text(&line);
			});
	}

	void operator()(agi::Context *c) override {
		if (ai::GetApiKey().empty() && !ShowAIConnectionDialog(c->parent, true))
			return;
		if (!EnsureCheckLanguage(c->parent)) return;

		auto selected = c->selectionController->GetSortedSelection();
		bool use_selection = selected.size() >= 100;
		selected.erase(std::remove_if(selected.begin(), selected.end(),
			[](AssDialogue *line) { return line->Comment; }), selected.end());

		std::vector<AssDialogue *> default_lines;
		for (auto& line : c->ass->Events) {
			if (!line.Comment && is_default_dialogue_style(line.Style.get()))
				default_lines.push_back(&line);
		}

		auto target_lines = use_selection ? selected : default_lines;
		if (target_lines.empty() ||
			!std::any_of(target_lines.begin(), target_lines.end(), has_subtitle_text)) {
			wxMessageBox(_("The chosen scope contains no subtitle text to check."),
				_("AI post-check"), wxOK | wxICON_INFORMATION, c->parent);
			return;
		}

		std::vector<ai::SubtitleLine> context_lines;
		context_lines.reserve(target_lines.size());
		for (auto line : target_lines) {
			ai::SubtitleLine input;
			input.id = line->Id;
			input.start_ms = static_cast<int>(line->Start);
			input.end_ms = static_cast<int>(line->End);
			input.source_text = agi::util::clean_ass_text(line->SourceLineText.get());
			input.current_text = agi::util::clean_ass_text(line->GetStrippedText());
			input.actor = line->Actor.get();
			input.style = line->Style.get();
			input.ass_text = line->Text.get();
			input.target = true;
			context_lines.push_back(std::move(input));
		}

		// The modal dialog keeps line pointers stable and blocks all other Aegisub
		// editing while analysis and the complete approve/skip walk-through run.
		ShowAIProofreadDialog(c, std::move(target_lines), std::move(context_lines));
	}
};

struct ai_proofread_latest final : public cmd::Command {
	CMD_NAME("ai/proofread/latest")
	STR_MENU("Latest")
	STR_DISP("Latest AI post-check")
	STR_HELP("Review the latest AI post-check again without an active AI connection")
	CMD_TYPE(cmd::COMMAND_VALIDATE)

	bool Validate(agi::Context const *c) override {
		return HasLatestAIProofread(c);
	}

	void operator()(agi::Context *c) override {
		ShowLatestAIProofreadDialog(c);
	}
};

template<ai::KaraokeMode Mode>
struct ai_karaoke : public cmd::Command {
	CMD_TYPE(cmd::COMMAND_VALIDATE)
	bool Validate(agi::Context const *c) override {
		return !c->selectionController->GetSelectedSet().empty() &&
			(Mode == ai::KaraokeMode::KanjiGeneration || c->project->AudioProvider());
	}
	void operator()(agi::Context *c) override {
		if (ai::GetApiKey().empty() && !ShowAIConnectionDialog(c->parent, true)) return;
		ShowAIKaraokeDialog(c, Mode, c->selectionController->GetSortedSelection());
	}
};

struct ai_karaoke_recognition final : public ai_karaoke<ai::KaraokeMode::AudioRecognition> {
	CMD_NAME("ai/karaoke/recognition")
	STR_MENU("Fill from audio recognition...")
	STR_DISP("Fill from audio recognition")
	STR_HELP("Fill the selected lines with audio-recognized timed romaji karaoke")
};

struct ai_karaoke_syllables final : public ai_karaoke<ai::KaraokeMode::SyllableTiming> {
	CMD_NAME("ai/karaoke/syllables")
	STR_MENU("Time karaoke lines...")
	STR_DISP("Time karaoke lines")
	STR_HELP("Add audio-aligned karaoke tags to the selected lines")
};

struct ai_karaoke_kanji final : public ai_karaoke<ai::KaraokeMode::KanjiGeneration> {
	CMD_NAME("ai/karaoke/kanji")
	STR_MENU("Create kanji lines...")
	STR_DISP("Create kanji lines")
	STR_HELP("Create plain kanji lines below the selected romaji lines without karaoke timing")
};

} // namespace

namespace cmd {
void init_ai() {
	reg(std::make_unique<ai_configure>());
	reg(std::make_unique<ai_review>());
	reg(std::make_unique<ai_proofread>());
	reg(std::make_unique<ai_proofread_latest>());
	reg(std::make_unique<ai_karaoke_recognition>());
	reg(std::make_unique<ai_karaoke_syllables>());
	reg(std::make_unique<ai_karaoke_kanji>());
}
}
