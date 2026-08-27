// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../ass_karaoke.h"
#include "../ass_parsed_line.h"
#include "../ass_style.h"
#include "../compat.h"
#include "../dialog_folder_search.h"
#include "../dialog_search_replace.h"
#include "../dialogs.h"
#include "../font_size_object.h"
#include "../format.h"
#include "../subtitle_line_combiner.h"
#include "../include/aegisub/context.h"
#include "../initial_line_state.h"
#include "../libresrc/libresrc.h"
#include "../line_change_flags.h"
#include "../options.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../subs_controller.h"
#include "../text_selection_controller.h"
#include "../typesetting_gradient.h"
#include "../typesetting_glitch.h"
#include "../typesetting_animated_text.h"
#include "../typesetting_textbox.h"
#include "../utils.h"
#include "../video_controller.h"

#include <libaegisub/address_of_adaptor.h>
#include <libaegisub/ass/karaoke.h>
#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/split.h>
#include <libaegisub/string.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/indirected.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/sliced.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/regex.hpp>
#include <boost/tokenizer.hpp>

#include <wx/clipbrd.h>
#include <wx/fontdlg.h>
#include <wx/textentry.h>

const std::string foldStartMarker = "{:Foldstart}";
const std::string foldEndMarker = "{:Foldend}";

void EditChangeText(agi::Context *c);

namespace {
	using namespace boost::adaptors;
	using cmd::Command;

struct validate_sel_nonempty : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->selectionController->GetSelectedSet().size() > 0;
	}
};

struct validate_video_and_sel_nonempty : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->project->VideoProvider() && !c->selectionController->GetSelectedSet().empty();
	}
};

struct validate_sel_multiple : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->selectionController->GetSelectedSet().size() > 1;
	}
};

struct validate_no_imagemask : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		const auto& sel = c->selectionController->GetSelectedSet();

		for (auto* line : sel)
			if (c->imageMask && c->imageMask->IsGroupStart(line))
				return false;

		return true;
	}
};

struct validate_sel_multiple_no_imagemask : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		const auto& sel = c->selectionController->GetSelectedSet();

		if (sel.size() <= 1)
			return false;

		for (auto* line : sel)
			if (c->imageMask && c->imageMask->IsGroupStart(line))
				return false;

		return true;
	}
};

struct validate_sel_nonempty_no_imagemask : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		const auto& sel = c->selectionController->GetSelectedSet();

		if (sel.size() <= 0)
			return false;

		for (auto* line : sel)
			if (c->imageMask && c->imageMask->IsGroupStart(line))
				return false;

		return true;
	}
};

struct validate_video_and_sel_nonempty_no_imagemask : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		const auto& sel = c->selectionController->GetSelectedSet();

		if (!c->project->VideoProvider() || sel.size() <= 0)
			return false;

		for (auto* line : sel)
			if (c->imageMask && c->imageMask->IsGroupStart(line))
				return false;

		return true;
	}
};

template<typename String>
std::unique_ptr<AssDialogue> get_dialogue(String data) {
	boost::trim(data);
	try {
		// Try to interpret the line as an ASS line
		return std::make_unique<AssDialogue>(data);
	}
	catch (...) {
		// Line didn't parse correctly, assume it's plain text that
		// should be pasted in the Text field only
		auto d = std::make_unique<AssDialogue>();
		d->End = 0;
		d->Text = data;
		return d;
	}
}

template<typename Paster>
void paste_lines(agi::Context *c, bool paste_over, Paster&& paste_line) {
	std::string data = GetClipboard();
	if (data.empty()) return;

	AssDialogue *first = nullptr;
	Selection newsel;

	std::vector<AssDialogue *> pendingFoldStarts;
	std::vector<std::pair<AssDialogue*, AssDialogue*>> foldsToAdd;
	bool restored_gradient_metadata = false;
	bool restored_glitch_metadata = false;
	bool restored_animated_text_metadata = false;
	bool restored_textbox_metadata = false;
	typesetting::animated_text::ClipboardPasteState animated_text_paste_state;

	boost::char_separator<char> sep("\r\n");
	for (auto curdata : boost::tokenizer<boost::char_separator<char>>(data, sep)) {
		AssDialogue *inserted = paste_line(get_dialogue(curdata));
		if (!inserted)
			break;

		std::string text = inserted->Text.get();
		size_t startPos = text.find(foldStartMarker);
		size_t endPos = text.find(foldEndMarker);

		if (startPos != std::string::npos) {
			text.replace(startPos, foldStartMarker.length(), "");
			pendingFoldStarts.push_back(inserted);
		}

		if (endPos != std::string::npos) {
			text.replace(endPos, foldEndMarker.length(), "");

			if (!pendingFoldStarts.empty()) {
				foldsToAdd.emplace_back(pendingFoldStarts.back(), inserted);
				pendingFoldStarts.pop_back();
			}
		}

		inserted->Text = text;
		restored_gradient_metadata |=
			typesetting::gradient::RestoreClipboardMetadata(*c->ass, *inserted);
		restored_glitch_metadata |=
			typesetting::glitch::RestoreClipboardMetadata(*c->ass, *inserted);
		restored_animated_text_metadata |=
			typesetting::animated_text::RestoreClipboardMetadata(*c->ass, *inserted,
				&animated_text_paste_state);
		restored_textbox_metadata |=
			typesetting::textbox::RestoreClipboardMetadata(*c->ass, *inserted);

		newsel.insert(inserted);
		if (!first)
			first = inserted;
	}

	if (first) {
		int commit_type = paste_over ? AssFile::COMMIT_DIAG_FULL : AssFile::COMMIT_DIAG_ADDREM;
		if (restored_gradient_metadata || restored_glitch_metadata ||
			restored_animated_text_metadata || restored_textbox_metadata)
			commit_type |= AssFile::COMMIT_EXTRADATA;
		int commitId = c->ass->Commit(_("paste"), commit_type);

		// Inserting lines assigns their row numbers during the commit above. FoldController
		// needs those row numbers to validate and recreate the copied folds.
		for (auto const& fold : foldsToAdd) {
			int foldCommitId = c->foldController->AddFold(*fold.first, *fold.second, true, commitId);
			if (foldCommitId >= 0)
				commitId = foldCommitId;
		}

		if (!paste_over)
			c->selectionController->SetSelectionAndActive(std::move(newsel), first);
	}
}

bool paste_over(wxWindow *parent, std::vector<bool>& pasteOverOptions, AssDialogue &new_line, AssDialogue &old_line) {
	if (pasteOverOptions.empty()) {
		if (!ShowPasteOverDialog(parent)) return false;
		pasteOverOptions = OPT_GET("Tool/Paste Lines Over/Fields")->GetListBool();
	}

	if (pasteOverOptions[0])  old_line.Comment   = new_line.Comment;
	if (pasteOverOptions[1])  old_line.Layer     = new_line.Layer;
	if (pasteOverOptions[2])  old_line.Start     = new_line.Start;
	if (pasteOverOptions[3])  old_line.End       = new_line.End;
	if (pasteOverOptions[4])  old_line.Style     = new_line.Style;
	if (pasteOverOptions[5])  old_line.Actor     = new_line.Actor;
	if (pasteOverOptions[6])  old_line.Margin[0] = new_line.Margin[0];
	if (pasteOverOptions[7])  old_line.Margin[1] = new_line.Margin[1];
	if (pasteOverOptions[8])  old_line.Margin[2] = new_line.Margin[2];
	if (pasteOverOptions[9])  old_line.Effect    = new_line.Effect;
	if (pasteOverOptions[10]) {
		std::string text = new_line.Text.get();

		size_t pos = text.find(foldStartMarker);
		if (pos != std::string::npos) {
			text.replace(pos, foldStartMarker.length(), "");
		} else {
			pos = text.find(foldEndMarker);
			if (pos != std::string::npos)
				text.replace(pos, foldEndMarker.length(), "");
		}

		old_line.Text = text;
		if (!new_line.SourceLineText.get().empty())
			old_line.SourceLineText = new_line.SourceLineText;
	}

	return true;
}


template<typename Func>
void update_lines(const agi::Context *c, wxString const& undo_msg, Func&& f) {
	const auto active_line = c->selectionController->GetActiveLine();
	const int sel_start = c->textSelectionController->GetSelectionStart();
	const int sel_end = c->textSelectionController->GetSelectionEnd();
	const int norm_sel_start = normalize_pos(active_line->Text, sel_start);
	const int norm_sel_end = normalize_pos(active_line->Text, sel_end);
	int active_sel_shift = 0;

	for (const auto line : c->selectionController->GetSelectedSet()) {
		int shift = f(line, sel_start, sel_end, norm_sel_start, norm_sel_end);
		if (line == active_line)
			active_sel_shift = shift;
	}

	auto const& sel = c->selectionController->GetSelectedSet();
	c->ass->Commit(undo_msg, AssFile::COMMIT_DIAG_TEXT, -1, sel.size() == 1 ? *sel.begin() : nullptr);
	if (active_sel_shift != 0)
		c->textSelectionController->SetSelection(sel_start + active_sel_shift, sel_end + active_sel_shift);
}

void toggle_override_tag(const agi::Context *c, bool (AssStyle::*field), const char *tag, wxString const& undo_msg) {
	update_lines(c, undo_msg, [&](AssDialogue *line, int sel_start, int sel_end, int norm_sel_start, int norm_sel_end) {
		AssStyle const* const style = c->ass->GetStyle(line->Style);
		bool state = style ? style->*field : AssStyle().*field;

		parsed_line parsed(line);
		int blockn = parsed.block_at_pos(norm_sel_start);

		state = parsed.get_value(blockn, state, tag);

		int shift = parsed.set_tag(tag, state ? "0" : "1", norm_sel_start, sel_start);
		if (sel_start != sel_end)
			parsed.set_tag(tag, state ? "1" : "0", norm_sel_end, sel_end + shift);
		return shift;
	});
}

void show_color_picker(const agi::Context *c, agi::Color (AssStyle::*field), const char *tag, const char *alt, const char *alpha) {
	agi::Color initial_color;
	const auto active_line = c->selectionController->GetActiveLine();
	const int sel_start = c->textSelectionController->GetSelectionStart();
	const int sel_end = c->textSelectionController->GetSelectionStart();
	const int norm_sel_start = normalize_pos(active_line->Text, sel_start);

	auto const& sel = c->selectionController->GetSelectedSet();
	using line_info = std::pair<agi::Color, parsed_line>;
	std::vector<line_info> lines;
	for (auto line : sel) {
		AssStyle const* const style = c->ass->GetStyle(line->Style);
		agi::Color color = (style ? style->*field : AssStyle().*field);

		parsed_line parsed(line);
		int blockn = parsed.block_at_pos(norm_sel_start);

		int a = parsed.get_value(blockn, (int)color.a, alpha, "\\alpha");
		color = parsed.get_value(blockn, color, tag, alt);
		color.a = a;

		if (line == active_line)
			initial_color = color;

		lines.emplace_back(color, std::move(parsed));
	}

	int active_shift = 0;
	int commit_id = -1;
	bool ok = GetColorFromUser(c->parent, initial_color, true, [&](agi::Color new_color) {
		for (auto& line : lines) {
			int shift = line.second.set_tag(tag, new_color.GetAssOverrideFormatted(), norm_sel_start, sel_start);
			if (new_color.a != line.first.a) {
				shift += line.second.set_tag(alpha, agi::format("&H%02X&", (int)new_color.a), norm_sel_start, sel_start + shift);
				line.first.a = new_color.a;
			}

			if (line.second.line == active_line)
				active_shift = shift;
		}

		commit_id = c->ass->Commit(_("set color"), AssFile::COMMIT_DIAG_TEXT, commit_id, sel.size() == 1 ? *sel.begin() : nullptr);
		if (active_shift)
			c->textSelectionController->SetSelection(sel_start + active_shift, sel_start + active_shift);
	});

	if (!ok && commit_id != -1) {
		c->subsController->Undo();
		c->textSelectionController->SetSelection(sel_start, sel_end);
	}
}

static void cleanup_override_block(parsed_line& parsed, int blockn, AssStyle const* style, LineChangeFlags const& flags)
{
	if (blockn < 0 || blockn >= (int)parsed.blocks.size() || !flags.Any())
		return;

	auto* block = parsed.blocks[blockn].get();
	if (block->GetType() != AssBlockType::OVERRIDE)
		return;

	auto* ovr = static_cast<AssDialogueBlockOverride*>(block);
	int cur_b;
	int cur_i;
	int cur_u;
	int cur_s;
	std::string cur_fn;
	double cur_fs;
	double cur_fscx;
	double cur_fscy;
	double cur_fsp;

	if (flags.b)
		cur_b = parsed.get_value(blockn - 1, style->bold, "\\b");
	if (flags.i)
		cur_i = parsed.get_value(blockn - 1, style->italic, "\\i");
	if (flags.u)
		cur_u = parsed.get_value(blockn - 1, style->underline, "\\u");
	if (flags.s)
		cur_s = parsed.get_value(blockn - 1, style->strikeout, "\\s");
	if (flags.fn)
		cur_fn = parsed.get_value(blockn - 1, style->font, "\\fn");

	if (flags.fs)
		cur_fs   = parsed.get_value(blockn - 1, (double)style->fontsize, "\\fs");
	if (flags.fscx)
		cur_fscx = parsed.get_value(blockn - 1, style->scalex, "\\fscx");
	if (flags.fscy)
		cur_fscy = parsed.get_value(blockn - 1, style->scaley, "\\fscy");
	if (flags.fsp)
		cur_fsp  = parsed.get_value(blockn - 1, style->spacing, "\\fsp");

	for (size_t i = 0; i < ovr->Tags.size(); ) {
		auto& tag = ovr->Tags[i];
		bool erase = false;

		if (flags.b && tag.Name == "\\b") {
			int v = tag.Params[0].Get<int>(cur_b);
			if (v == cur_b) erase = true;
			else cur_b = v;
		}
		else if (flags.i && tag.Name == "\\i") {
			int v = tag.Params[0].Get<int>(cur_i);
			if (v == cur_i) erase = true;
			else cur_i = v;
		}
		else if (flags.u && tag.Name == "\\u") {
			int v = tag.Params[0].Get<int>(cur_u);
			if (v == cur_u) erase = true;
			else cur_u = v;
		}
		else if (flags.s && tag.Name == "\\s") {
			int v = tag.Params[0].Get<int>(cur_s);
			if (v == cur_s) erase = true;
			else cur_s = v;
		}
		else if (flags.fn && tag.Name == "\\fn") {
			std::string v = tag.Params[0].Get<std::string>(cur_fn);
			if (v == cur_fn) erase = true;
			else cur_fn = v;
		}
		else if (flags.fs && tag.Name == "\\fs") {
			double v = tag.Params[0].Get<double>(cur_fs);
			if (v == cur_fs) erase = true;
			else cur_fs = v;
		}
		else if (flags.fscx && tag.Name == "\\fscx") {
			double v = tag.Params[0].Get<double>(cur_fscx);
			if (v == cur_fscx) erase = true;
			else cur_fscx = v;
		}
		else if (flags.fscy && tag.Name == "\\fscy") {
			double v = tag.Params[0].Get<double>(cur_fscy);
			if (v == cur_fscy) erase = true;
			else cur_fscy = v;
		}
		else if (flags.fsp && tag.Name == "\\fsp") {
			double v = tag.Params[0].Get<double>(cur_fsp);
			if (v == cur_fsp) erase = true;
			else cur_fsp = v;
		}

		if (erase)
			ovr->Tags.erase(ovr->Tags.begin() + i);
		else
			++i;
	}

	parsed.line->UpdateText(parsed.blocks);
	parsed.blocks = parsed.line->ParseTags();

	if (ovr->Tags.empty()) {
		std::string text = parsed.line->Text.get();

		size_t pos = text.find("{}");
		if (pos != std::string::npos)
			text.erase(pos, 2);

		parsed.line->Text = text;
		parsed.blocks = parsed.line->ParseTags();
		return;
	}
}

struct edit_color_primary final : public Command {
	CMD_NAME("edit/color/primary")
	CMD_ICON(button_color_one)
	STR_MENU("Primary Color...")
	STR_DISP("Primary Color")
	STR_HELP("Set the primary fill color (\\c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::primary, "\\c", "\\1c", "\\1a");
	}
};

struct edit_color_secondary final : public Command {
	CMD_NAME("edit/color/secondary")
	CMD_ICON(button_color_two)
	STR_MENU("Secondary Color...")
	STR_DISP("Secondary Color")
	STR_HELP("Set the secondary (karaoke) fill color (\\2c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::secondary, "\\2c", "", "\\2a");
	}
};

struct edit_color_outline final : public Command {
	CMD_NAME("edit/color/outline")
	CMD_ICON(button_color_three)
	STR_MENU("Outline Color...")
	STR_DISP("Outline Color")
	STR_HELP("Set the outline color (\\3c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::outline, "\\3c", "", "\\3a");
	}
};

struct edit_color_shadow final : public Command {
	CMD_NAME("edit/color/shadow")
	CMD_ICON(button_color_four)
	STR_MENU("Shadow Color...")
	STR_DISP("Shadow Color")
	STR_HELP("Set the shadow color (\\4c) at the cursor position")

	void operator()(agi::Context *c) override {
		show_color_picker(c, &AssStyle::shadow, "\\4c", "", "\\4a");
	}
};

struct edit_style_bold final : public Command {
	CMD_NAME("edit/style/bold")
	CMD_ICON(button_bold)
	STR_MENU("Toggle Bold")
	STR_DISP("Toggle Bold")
	STR_HELP("Toggle bold (\\b) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::bold, "\\b", _("toggle bold"));
	}
};

struct edit_style_italic final : public Command {
	CMD_NAME("edit/style/italic")
	CMD_ICON(button_italics)
	STR_MENU("Toggle Italics")
	STR_DISP("Toggle Italics")
	STR_HELP("Toggle italics (\\i) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::italic, "\\i", _("toggle italic"));
	}
};

struct edit_style_underline final : public Command {
	CMD_NAME("edit/style/underline")
	CMD_ICON(button_underline)
	STR_MENU("Toggle Underline")
	STR_DISP("Toggle Underline")
	STR_HELP("Toggle underline (\\u) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::underline, "\\u", _("toggle underline"));
	}
};

struct edit_style_strikeout final : public Command {
	CMD_NAME("edit/style/strikeout")
	CMD_ICON(button_strikeout)
	STR_MENU("Toggle Strikeout")
	STR_DISP("Toggle Strikeout")
	STR_HELP("Toggle strikeout (\\s) for the current selection or at the current cursor position")

	void operator()(agi::Context *c) override {
		toggle_override_tag(c, &AssStyle::strikeout, "\\s", _("toggle strikeout"));
	}
};

struct edit_font final : public Command {
	CMD_NAME("edit/font")
	CMD_ICON(button_fontname)
	STR_MENU("Font Face...")
	STR_DISP("Font Face")
	STR_HELP("Select a font face and size")

	void operator()(agi::Context *c) override {
		const parsed_line active(c->selectionController->GetActiveLine());

		int orig_pos = c->textSelectionController->GetInsertionPoint();
		int insertion_point = normalize_pos(active.line->Text, orig_pos);
		auto const& sel = c->selectionController->GetSelectedSet();

		if (sel.size() > 1) {
			insertion_point = 0;
			orig_pos = 0;
		}

		auto font_for_line = [&](parsed_line const& line) -> wxFont {
			const int blockn = line.block_at_pos(insertion_point);

			const AssStyle *style = c->ass->GetStyle(line.line->Style);
			const AssStyle default_style;
			if (!style) {
				style = &default_style;
			}

			wxFont result = wxFont(
				line.get_value(blockn, (int)style->fontsize, "\\fs"),
				wxFONTFAMILY_DEFAULT,
				line.get_value(blockn, style->italic, "\\i") ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
				line.get_value(blockn, style->bold, "\\b") ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
				line.get_value(blockn, style->underline, "\\u"),
				to_wx(line.get_value(blockn, style->font, "\\fn")));

			if (line.get_value(blockn, style->underline, "\\s"))
				result.SetStrikethrough(true);

			return result;
		};

		auto face_for_line = [&](parsed_line const& line) -> wxString {
			const int blockn = line.block_at_pos(insertion_point);

			const AssStyle *style = c->ass->GetStyle(line.line->Style);
			const AssStyle default_style;
			if (!style)
				style = &default_style;

			return to_wx(line.get_value(blockn, style->font, "\\fn"));
		};


		const wxFont initial = font_for_line(active);
		const wxString initialFace = face_for_line(active);

		FontSizeObject curSizeObj;
		{
			int blockn = active.block_at_pos(insertion_point);

			const AssStyle* style = c->ass->GetStyle(active.line->Style);
			const AssStyle default_style;
			if (!style)
				style = &default_style;

			curSizeObj.fs   = active.get_value(blockn, (double)style->fontsize, "\\fs");
			curSizeObj.fscx = active.get_value(blockn, style->scalex, "\\fscx");
			curSizeObj.fscy = active.get_value(blockn, style->scaley, "\\fscy");
			curSizeObj.fsp  = active.get_value(blockn,style->spacing, "\\fsp");
		}

		using line_info = parsed_line;
		std::vector<line_info> lines;

		for (auto line : sel)
			lines.emplace_back(line);

		int active_shift = 0;
		int commit_id = -1;

		bool ok = GetFontFromUser(c->parent, initialFace, initial, curSizeObj, c->ass.get(), [&](wxString const& face, wxFont const& new_font, FontSizeObject const& size, LineChangeFlags const& flags) {
			if (!flags.Any())
				return;

			bool isChanged = false;

			for (auto& parsed : lines) {
				int total_shift = 0;

				std::string old_text = parsed.line->Text.get();
				int old_len = parsed.line->Text.get().size();

				const AssStyle* style = c->ass->GetStyle(parsed.line->Style);
				const AssStyle default_style;
				if (!style) style = &default_style;

				if (flags.fn) {
					int s = parsed.set_tag("\\fn", from_wx(face), insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.b) {
					int s = parsed.set_tag("\\b", new_font.GetWeight() == wxFONTWEIGHT_BOLD ? "1" : "0", insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.i) {
					int s = parsed.set_tag("\\i", new_font.GetStyle() == wxFONTSTYLE_ITALIC ? "1" : "0", insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.u) {
					int s = parsed.set_tag("\\u", new_font.GetUnderlined() ? "1" : "0", insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.s) {
					int s = parsed.set_tag("\\s", new_font.GetStrikethrough() ? "1" : "0", insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.fs) {
					int s = parsed.set_tag("\\fs", agi::format("%g", size.fs), insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.fscx) {
					int s = parsed.set_tag("\\fscx", agi::format("%g", size.fscx), insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.fscy) {
					int s = parsed.set_tag("\\fscy", agi::format("%g", size.fscy), insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				if (flags.fsp) {
					int s = parsed.set_tag("\\fsp", agi::format("%g", size.fsp), insertion_point, orig_pos + total_shift);
					total_shift += s;
				}

				cleanup_override_block(parsed, parsed.block_at_pos(insertion_point), style, flags);

				if (parsed.line == active.line) {
					int new_len = parsed.line->Text.get().size();
					active_shift = new_len - old_len;
				}

				if (parsed.line->Text.get() != old_text)
					isChanged = true;
			}

			if (isChanged) {
				commit_id = c->ass->Commit(_("set font"), AssFile::COMMIT_DIAG_TEXT, commit_id, sel.size() == 1 ? *sel.begin() : nullptr);
			}

			if (active_shift) {
				int new_pos = orig_pos + active_shift;
				c->textSelectionController->SetSelection(new_pos, new_pos);
			}
		});

		if (!ok && commit_id != -1) {
			c->subsController->Undo();
		}
	}
};

struct edit_find_replace final : public Command {
	CMD_NAME("edit/find_replace")
	CMD_ICON(find_replace_menu)
	STR_MENU("Find and R&eplace...")
	STR_DISP("Find and Replace")
	STR_HELP("Find and replace words in subtitles")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowSearchReplaceDialog(c, true);
	}
};

struct edit_find_in_folder final : public Command {
	CMD_NAME("edit/find_in_folder")
	STR_MENU("Find in &Folder...")
	STR_DISP("Find in Folder")
	STR_HELP("Find text in subtitle files under a folder")
	void operator()(agi::Context *c) override { ShowFolderSearchDialog(c); }
};

static void copy_lines(agi::Context *c) {
	auto selection = c->selectionController->GetSelectedSet();
	typesetting::animated_text::ExpandSelection(*c->ass, selection);
	if (c->imageMask) c->imageMask->ExpandTypesettingSelection(selection);
	std::vector<AssDialogue *> sorted(selection.begin(), selection.end());
	std::sort(sorted.begin(), sorted.end(), [](AssDialogue *a, AssDialogue *b) {
		return a->Row < b->Row;
	});
	std::string clipboard;
	for (auto d : sorted) {
		if (!clipboard.empty()) clipboard += "\r\n";
		std::string str = d->GetEntryData(false);

		if (d->Fold.hasFold() && !d->Fold.isEnd())
			str += "{:Foldstart}";

		if (d->Fold.hasFold() && d->Fold.isEnd())
			str += "{:Foldend}";

		str += typesetting::gradient::ClipboardMetadata(*c->ass, *d);
		str += typesetting::glitch::ClipboardMetadata(*c->ass, *d);
		str += typesetting::animated_text::ClipboardMetadata(*c->ass, *d);
		str += typesetting::textbox::ClipboardMetadata(*c->ass, *d);

		std::string source_line(agi::Trim(d->SourceLineText.get()));
		if (!source_line.empty()) {
			str += "{:Source Line: ";

			for (auto character : source_line)
				if (character != '\n' && character != '\r')
					str += character;

			str += "}";
		}

		clipboard += std::move(str);
	}
	SetClipboard(clipboard);
}

static void delete_lines(agi::Context *c, wxString const& commit_message) {
	auto sel = c->selectionController->GetSelectedSet();
	if (c->imageMask) c->imageMask->ExpandTypesettingSelection(sel);
	bool transferred_gradient_metadata = false;
	if (c->imageMask) {
		for (auto line : sel) {
			if (!c->imageMask->IsGroupStart(line) ||
				!c->imageMask->IsGradientGroup(line)) continue;
			auto const& group = c->imageMask->GetGroupLines(line);
			auto survivor = std::find_if(group.begin(), group.end(), [&](AssDialogue *candidate) {
				return !sel.count(candidate);
			});
			if (survivor != group.end())
				transferred_gradient_metadata |= typesetting::gradient::TransferGroupMetadata(
					*c->ass, *line, **survivor);
		}
	}

	// Find a line near the active line not being deleted to make the new active line
	AssDialogue *pre_sel = nullptr;
	AssDialogue *post_sel = nullptr;
	bool hit_selection = false;

	for (auto& diag : c->ass->Events) {
		if (sel.count(&diag))
			hit_selection = true;
		else if (hit_selection && !post_sel) {
			post_sel = &diag;
			break;
		}
		else
			pre_sel = &diag;
	}

	// Remove the selected lines, but defer the deletion until after we select
	// different lines. We can't just change the selection first because we may
	// need to create a new dialogue line for it, and we can't select dialogue
	// lines until after they're committed.
	std::vector<std::unique_ptr<AssDialogue>> to_delete;
	c->ass->Events.remove_and_dispose_if([&sel](AssDialogue const& e) {
		return sel.count(const_cast<AssDialogue *>(&e));
	}, [&](AssDialogue *e) {
		to_delete.emplace_back(e);
	});

	AssDialogue *new_active = post_sel;
	if (!new_active)
		new_active = pre_sel;
	// If we didn't get a new active line then we just deleted all the dialogue
	// lines, so make a new one
	if (!new_active) {
		new_active = new AssDialogue;
		c->ass->Events.push_back(*new_active);
	}

	int commit_type = AssFile::COMMIT_DIAG_ADDREM;
	if (transferred_gradient_metadata) commit_type |= AssFile::COMMIT_EXTRADATA;
	c->ass->Commit(commit_message, commit_type);
	c->selectionController->SetSelectionAndActive({ new_active }, new_active);
}

struct edit_line_copy final : public validate_sel_nonempty {
	CMD_NAME("edit/line/copy")
	CMD_ICON(copy_button)
	STR_MENU("&Copy Lines")
	STR_DISP("Copy Lines")
	STR_HELP("Copy subtitles to the clipboard")

	void operator()(agi::Context *c) override {
		// Ideally we'd let the control's keydown handler run and only deal
		// with the events not processed by it, but that doesn't seem to be
		// possible with how wx implements key event handling - the native
		// platform processing is evoked only if the wx event is unprocessed,
		// and there's no way to do something if the native platform code leaves
		// it unprocessed

		if (wxTextEntryBase *ctrl = dynamic_cast<wxTextEntryBase*>(c->parent->FindFocus()))
			ctrl->Copy();
		else {
			copy_lines(c);
		}
	}
};

struct edit_line_cut: public validate_sel_nonempty {
	CMD_NAME("edit/line/cut")
	CMD_ICON(cut_button)
	STR_MENU("Cu&t Lines")
	STR_DISP("Cut Lines")
	STR_HELP("Cut subtitles")

	void operator()(agi::Context *c) override {
		if (wxTextEntryBase *ctrl = dynamic_cast<wxTextEntryBase*>(c->parent->FindFocus()))
			ctrl->Cut();
		else {
			copy_lines(c);
			delete_lines(c, _("cut lines"));
		}
	}
};

struct edit_line_delete final : public validate_sel_nonempty {
	CMD_NAME("edit/line/delete")
	CMD_ICON(delete_button)
	STR_MENU("De&lete Lines")
	STR_DISP("Delete Lines")
	STR_HELP("Delete currently selected lines")

	void operator()(agi::Context *c) override {
		delete_lines(c, _("delete lines"));
	}
};

static void duplicate_lines(agi::Context *c, int shift) {
	Selection sel = c->selectionController->GetSelectedSet();
	// A collapsed gradient/textbox/image group is represented by its first row in
	// the grid. Duplicating only that row either loses the generated effect or
	// leaves a source marker attached to the following group's rows.
	if (!shift && c->imageMask) {
		std::vector<AssDialogue *> selected(sel.begin(), sel.end());
		for (auto line : selected) {
			auto const& group = c->imageMask->GetGroupLines(line);
			sel.insert(group.begin(), group.end());
		}
	}
	auto in_selection = [&](AssDialogue const& d) { return sel.count(const_cast<AssDialogue *>(&d)); };

	Selection new_sel;
	AssDialogue *new_active = nullptr;

	auto start = c->ass->Events.begin();
	auto end = c->ass->Events.end();
	while (start != end) {
		// Find the first line in the selection
		start = std::find_if(start, end, in_selection);
		if (start == end) break;

		// And the last line in this contiguous selection
		auto insert_pos = std::find_if_not(start, end, in_selection);
		auto last = std::prev(insert_pos);

		// Duplicate each of the selected lines, inserting them in a block
		// after the selected block
		do {
			auto old_diag = &*start;
			auto new_diag = new AssDialogue(*old_diag);

			c->ass->Events.insert(insert_pos, *new_diag);
			new_sel.insert(new_diag);
			if (!new_active)
				new_active = new_diag;

			if (shift) {
				int cur_frame = c->videoController->GetFrameN();
				int old_start = c->videoController->FrameAtTime(new_diag->Start, agi::vfr::START);
				int old_end = c->videoController->FrameAtTime(new_diag->End, agi::vfr::END);

				// If the current frame isn't within the range of the line then
				// splitting doesn't make any sense, so instead just duplicate
				// the line and set the new one to just this frame
				if (cur_frame < old_start || cur_frame > old_end) {
					new_diag->Start = c->videoController->TimeAtFrame(cur_frame, agi::vfr::START);
					new_diag->End = c->videoController->TimeAtFrame(cur_frame, agi::vfr::END);
				}
				/// @todo This does dumb things when old_start == old_end
				else if (shift < 0) {
					old_diag->End = c->videoController->TimeAtFrame(cur_frame - 1, agi::vfr::END);
					new_diag->Start = c->videoController->TimeAtFrame(cur_frame, agi::vfr::START);
				}
				else {
					old_diag->End = c->videoController->TimeAtFrame(cur_frame, agi::vfr::END);
					new_diag->Start = c->videoController->TimeAtFrame(cur_frame + 1, agi::vfr::START);
				}

				/// @todo also split \t and \move?
			}
		} while (start++ != last);

		// Skip over the lines we just made
		start = insert_pos;
	}

	if (new_sel.empty()) return;

	c->ass->Commit(shift ? _("split") : _("duplicate lines"), AssFile::COMMIT_DIAG_ADDREM);

	c->selectionController->SetSelectionAndActive(std::move(new_sel), new_active);
}

struct edit_line_duplicate final : public validate_sel_nonempty {
	CMD_NAME("edit/line/duplicate")
	STR_MENU("&Duplicate Lines")
	STR_DISP("Duplicate Lines")
	STR_HELP("Duplicate the selected lines")

	void operator()(agi::Context *c) override {
		duplicate_lines(c, 0);
	}
};

struct edit_line_duplicate_shift final : public validate_video_and_sel_nonempty_no_imagemask {
	CMD_NAME("edit/line/split/after")
	STR_MENU("Split lines after current frame")
	STR_DISP("Split lines after current frame")
	STR_HELP("Split the current line into a line which ends on the current frame and a line which starts on the next frame")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		duplicate_lines(c, 1);
	}
};

struct edit_line_duplicate_shift_back final : public validate_video_and_sel_nonempty_no_imagemask {
	CMD_NAME("edit/line/split/before")
	STR_MENU("Split lines before current frame")
	STR_DISP("Split lines before current frame")
	STR_HELP("Split the current line into a line which ends on the previous frame and a line which starts on the current frame")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		duplicate_lines(c, -1);
	}
};

static std::string remove_override_tags_keep_comments(std::string const& text) {
	std::string result;
	size_t pos = 0;

	while (pos < text.size()) {
		if (text[pos] != '{') {
			result += text[pos++];
			continue;
		}

		size_t end = text.find('}', pos);
		if (end == std::string::npos) {
			result += text.substr(pos);
			break;
		}

		std::string block = text.substr(pos, end - pos + 1);

		if (block.find('\\') == std::string::npos)
			result += block;

		pos = end + 1;
	}

	return result;
}

static void combine_lines(agi::Context *c, void (*combiner)(AssDialogue *, AssDialogue *, bool), wxString const& message, bool keepTypesetting) {
	auto sel = c->selectionController->GetSortedSelection();

	AssDialogue *first = sel[0];
	combiner(first, nullptr, keepTypesetting);
	std::vector<std::unique_ptr<AssDialogue>> removed;
	removed.reserve(sel.size() - 1);
	for (size_t i = 1; i < sel.size(); ++i) {
		combiner(first, sel[i], keepTypesetting);
		first->End = std::max(first->End, sel[i]->End);
		// Keep detached rows alive until selection listeners and commit listeners
		// have stopped referring to the previous selection. Deleting here left
		// dangling pointers during combined-row/fold refreshes.
		c->ass->Events.erase(c->ass->Events.iterator_to(*sel[i]));
		removed.emplace_back(sel[i]);
	}

	c->selectionController->SetSelectionAndActive({first}, first);

	c->ass->Commit(message, AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
}

static void combine_karaoke(AssDialogue *first, AssDialogue *second, bool keepTypesetting) {
	if (second) {
		first->Text = agi::Str(first->Text.get(), "{\\k", std::to_string((second->End - second->Start) / 10), "}", second->Text.get());
		auto source_line = agi::Str(first->SourceLineText.get(), " ", second->SourceLineText.get());
		first->SourceLineText = std::string(agi::Trim(source_line));
	}
	else
		first->Text = agi::Str("{\\k", std::to_string((first->End - first->Start) / 10), "}", first->Text.get());

	
}

static void combine_concat(AssDialogue *first, AssDialogue *second, bool keepTypesetting) {
	if (second) {
		if (keepTypesetting)
			first->Text = agi::Str(first->Text.get(), " ", second->Text.get());
		else
			first->Text = agi::Str(first->Text.get(), " ", remove_override_tags_keep_comments(second->Text.get()));

		auto source_line = agi::Str(first->SourceLineText.get(), " ", second->SourceLineText.get());
		first->SourceLineText = std::string(agi::Trim(source_line));
	}
}

static void combine_drop(AssDialogue *, AssDialogue *, bool keepTypesetting) { }

struct edit_line_join_as_karaoke final : public validate_sel_multiple_no_imagemask {
	CMD_NAME("edit/line/join/as_karaoke")
	STR_MENU("As &Karaoke")
	STR_DISP("As Karaoke")
	STR_HELP("Join selected lines in a single one, as karaoke")

	void operator()(agi::Context *c) override {
		combine_lines(c, combine_karaoke, _("join as karaoke"), false);
	}
};

struct edit_line_join_concatenate final : public validate_sel_multiple_no_imagemask {
	CMD_NAME("edit/line/join/concatenate")
	STR_MENU("&Concatenate")
	STR_DISP("Concatenate")
	STR_HELP("Join selected lines in a single one, concatenating text together")

	void operator()(agi::Context *c) override {
		combine_lines(c, combine_concat, _("join lines"), false);
	}
};

struct edit_line_join_concatenate_with_typesetting final : public validate_sel_multiple_no_imagemask {
	CMD_NAME("edit/line/join/concatenate_with_typesetting")
	STR_MENU("&Concatenate (with typesetting)")
	STR_DISP("Concatenate (with typesetting)")
	STR_HELP("Join selected lines in a single one, concatenating text together (with typesetting)")

	void operator()(agi::Context *c) override {
		combine_lines(c, combine_concat, _("join lines"), true);
	}
};

struct edit_line_join_keep_first final : public validate_sel_multiple_no_imagemask {
	CMD_NAME("edit/line/join/keep_first")
	STR_MENU("Keep &First")
	STR_DISP("Keep First")
	STR_HELP("Join selected lines in a single one, keeping text of first and discarding remaining")

	void operator()(agi::Context *c) override {
		combine_lines(c, combine_drop, _("join lines"), false);
	}
};

static bool try_paste_lines(agi::Context *c) {
	std::string data = GetClipboard();
	boost::trim_left(data);
	if (!data.starts_with("Dialogue:")) return false;

	boost::char_separator<char> sep("\r\n");
	for (auto curdata : boost::tokenizer<boost::char_separator<char>>(data, sep)) {
		boost::trim(curdata);
		try {
			AssDialogue parsed(curdata);
		}
		catch (...) {
			return false;
		}
	}

	auto pos = c->ass->iterator_to(*c->selectionController->GetActiveLine());
	paste_lines(c, false, [=](std::unique_ptr<AssDialogue> new_line) -> AssDialogue * {
		c->ass->Events.insert(pos, *new_line);
		return new_line.release();
	});

	return true;
}

struct edit_line_paste final : public Command {
	CMD_NAME("edit/line/paste")
	CMD_ICON(paste_button)
	STR_MENU("&Paste Lines")
	STR_DISP("Paste Lines")
	STR_HELP("Paste subtitles")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *) override {
		bool can_paste = false;
		if (wxTheClipboard->Open()) {
			can_paste = wxTheClipboard->IsSupported(wxDF_TEXT) || wxTheClipboard->IsSupported(wxDF_UNICODETEXT);
			wxTheClipboard->Close();
		}
		return can_paste;
	}

	void operator()(agi::Context *c) override {
		if (wxTextEntryBase *ctrl = dynamic_cast<wxTextEntryBase*>(c->parent->FindFocus())) {
			if (!try_paste_lines(c))
				ctrl->Paste();
		}
		else {
			auto pos = c->ass->iterator_to(*c->selectionController->GetActiveLine());
			paste_lines(c, false, [=](std::unique_ptr<AssDialogue> new_line) -> AssDialogue * {
				c->ass->Events.insert(pos, *new_line);
				return new_line.release();
			});
		}
	}
};

struct edit_line_paste_over final : public Command {
	CMD_NAME("edit/line/paste/over")
	STR_MENU("Paste Lines &Over...")
	STR_DISP("Paste Lines Over")
	STR_HELP("Paste subtitles over others")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		bool can_paste = !c->selectionController->GetSelectedSet().empty();
		if (can_paste && wxTheClipboard->Open()) {
			can_paste = wxTheClipboard->IsSupported(wxDF_TEXT) || wxTheClipboard->IsSupported(wxDF_UNICODETEXT);
			wxTheClipboard->Close();
		}
		return can_paste;
	}

	void operator()(agi::Context *c) override {
		auto const& sel = c->selectionController->GetSelectedSet();
		std::vector<bool> pasteOverOptions;

		// Only one line selected, so paste over downwards from the active line
		if (sel.size() < 2) {
			auto pos = c->ass->iterator_to(*c->selectionController->GetActiveLine());

			paste_lines(c, true, [&](std::unique_ptr<AssDialogue> new_line) -> AssDialogue * {
				if (pos == c->ass->Events.end()) return nullptr;

				auto& old = *pos;
				if (paste_over(c->parent, pasteOverOptions, *new_line, old))
					++pos;
				return &old;
			});
		}
		else {
			// Multiple lines selected, so paste over the selection
			auto sorted_selection = c->selectionController->GetSortedSelection();
			auto pos = begin(sorted_selection);
			paste_lines(c, true, [&](std::unique_ptr<AssDialogue> new_line) -> AssDialogue * {
				if (pos == end(sorted_selection)) return nullptr;

				auto& old = **pos;
				if (paste_over(c->parent, pasteOverOptions, *new_line, old))
					++pos;
				return &old;
			});
		}
	}
};

namespace {
std::string trim_text(std::string text) {
	boost::regex start(R"(^( |	|\\[nNh])+)");
	boost::regex end(R"(( |	|\\[nNh])+$)");

	text = regex_replace(text, start, "", boost::format_first_only);
	text = regex_replace(text, end, "", boost::format_first_only);
	return text;
}

void expand_times(AssDialogue *src, AssDialogue *dst) {
	dst->Start = std::min(dst->Start, src->Start);
	dst->End = std::max(dst->End, src->End);
}

bool check_start(AssDialogue *d1, AssDialogue *d2) {
	if (d1->Text.get().starts_with(d2->Text.get())) {
		d1->Text = trim_text(d1->Text.get().substr(d2->Text.get().size()));
		expand_times(d1, d2);
		return true;
	}
	return false;
}

bool check_end(AssDialogue *d1, AssDialogue *d2) {
	if (d1->Text.get().ends_with(d2->Text.get())) {
		d1->Text = trim_text(d1->Text.get().substr(0, d1->Text.get().size() - d2->Text.get().size()));
		expand_times(d1, d2);
		return true;
	}
	return false;
}

}

struct edit_line_recombine final : public validate_sel_multiple {
	CMD_NAME("edit/line/recombine")
	STR_MENU("Recom&bine Lines")
	STR_DISP("Recombine Lines")
	STR_HELP("Recombine subtitles which have been split and merged")

	void operator()(agi::Context *c) override {
		auto const& sel_set = c->selectionController->GetSelectedSet();
		if (sel_set.size() < 2) return;

		auto active_line = c->selectionController->GetActiveLine();

		std::vector<AssDialogue*> sel(sel_set.begin(), sel_set.end());
		boost::sort(sel, [](const AssDialogue *a, const AssDialogue *b) {
			return a->Start < b->Start;
		});

		for (auto &diag : sel)
			diag->Text = trim_text(diag->Text);

		auto end = sel.end() - 1;
		for (auto cur = sel.begin(); cur != end; ++cur) {
			auto d1 = *cur;
			auto d2 = cur + 1;

			// 1, 1+2 (or 2+1), 2 gets turned into 1, 2, 2 so kill the duplicate
			if (d1->Text == (*d2)->Text) {
				expand_times(d1, *d2);
				delete d1;
				continue;
			}

			// 1, 1+2, 1 turns into 1, 2, [empty]
			if (d1->Text.get().empty()) {
				delete d1;
				continue;
			}

			// If d2 is the last line in the selection it'll never hit the above test
			if (d2 == end && (*d2)->Text.get().empty()) {
				delete *d2;
				continue;
			}

			// 1, 1+2
			while (d2 <= end && check_start(*d2, d1))
				++d2;

			// 1, 2+1
			while (d2 <= end && check_end(*d2, d1))
				++d2;

			// 1+2, 2
			while (d2 <= end && check_end(d1, *d2))
				++d2;

			// 2+1, 2
			while (d2 <= end && check_start(d1, *d2))
				++d2;
		}

		// Remove now non-existent lines from the selection
		Selection lines, new_sel;
		boost::copy(c->ass->Events | agi::address_of, inserter(lines, lines.begin()));
		boost::set_intersection(lines, sel_set, inserter(new_sel, new_sel.begin()));

		if (new_sel.empty())
			new_sel.insert(*lines.begin());

		// Restore selection
		if (!new_sel.count(active_line))
			active_line = *new_sel.begin();
		c->selectionController->SetSelectionAndActive(std::move(new_sel), active_line);

		c->ass->Commit(_("combining"), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
	}
};

struct edit_line_split_by_karaoke final : public validate_sel_nonempty {
	CMD_NAME("edit/line/split/by_karaoke")
	STR_MENU("Split Lines (by karaoke)")
	STR_DISP("Split Lines (by karaoke)")
	STR_HELP("Use karaoke timing to split line into multiple smaller lines")

	void operator()(agi::Context *c) override {
		auto sel = c->selectionController->GetSortedSelection();
		if (sel.empty()) return;

		Selection new_sel;
		agi::ass::Karaoke kara;

		std::vector<std::unique_ptr<AssDialogue>> to_delete;
		for (auto line : sel) {
			SetKaraokeLine(kara, line);

			// If there aren't at least two tags there's nothing to split
			if (kara.size() < 2) continue;

			for (auto const& syl : kara) {
				auto new_line = new AssDialogue(*line);

				new_line->Start = syl.start_time;
				new_line->End = syl.start_time + syl.duration;
				new_line->Text = syl.GetText(false);

				c->ass->Events.insert(c->ass->iterator_to(*line), *new_line);

				new_sel.insert(new_line);
			}

			c->ass->Events.erase(c->ass->iterator_to(*line));
			to_delete.emplace_back(line);
		}

		if (to_delete.empty()) return;

		c->ass->Commit(_("splitting"), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);

		AssDialogue *new_active = c->selectionController->GetActiveLine();
		if (!new_sel.count(c->selectionController->GetActiveLine()))
			new_active = *new_sel.begin();
		c->selectionController->SetSelectionAndActive(std::move(new_sel), new_active);
	}
};

static std::string get_leading_override_blocks(std::string const& text) {
	std::string result;
	size_t pos = 0;

	while (pos < text.size() && text[pos] == '{') {
		size_t end = text.find('}', pos);
		if (end == std::string::npos)
			break;

		std::string block = text.substr(pos, end - pos + 1);

		if (block.find('\\') != std::string::npos)
			result += block;

		pos = end + 1;
	}

	return result;
}

void split_lines(agi::Context *c, AssDialogue *&n1, AssDialogue *&n2) {
	int pos = c->textSelectionController->GetSelectionStart();

	n1 = c->selectionController->GetActiveLine();
	n2 = new AssDialogue(*n1);
	c->ass->Events.insert(++c->ass->iterator_to(*n1), *n2);

	std::string orig = n1->Text;
	std::string leading_tags = get_leading_override_blocks(orig);

	n1->Text = boost::trim_right_copy(orig.substr(0, pos));
	n2->Text = leading_tags + boost::trim_left_copy(orig.substr(pos));
}

template<typename Func>
void split_lines(agi::Context *c, Func&& set_time) {
	AssDialogue *n1, *n2;
	split_lines(c, n1, n2);
	set_time(n1, n2);

	c->ass->Commit(_("split"), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
}

struct edit_line_split_estimate final : public validate_video_and_sel_nonempty {
	CMD_NAME("edit/line/split/estimate")
	STR_MENU("Split at cursor (estimate times)")
	STR_DISP("Split at cursor (estimate times)")
	STR_HELP("Split the current line at the cursor, dividing the original line's duration between the new ones")

	void operator()(agi::Context *c) override {
		split_lines(c, [](AssDialogue *n1, AssDialogue *n2) {
			size_t len = n1->Text.get().size() + n2->Text.get().size();
			if (!len) return;
			double splitPos = double(n1->Text.get().size()) / len;
			n2->Start = n1->End = (int)((n1->End - n1->Start) * splitPos) + n1->Start;
		});
	}
};

struct edit_line_split_preserve final : public validate_sel_nonempty {
	CMD_NAME("edit/line/split/preserve")
	STR_MENU("Split at cursor (preserve times)")
	STR_DISP("Split at cursor (preserve times)")
	STR_HELP("Split the current line at the cursor, setting both lines to the original line's times")

	void operator()(agi::Context *c) override {
		split_lines(c, [](AssDialogue *, AssDialogue *) { });
	}
};

struct edit_line_split_video final : public validate_video_and_sel_nonempty {
	CMD_NAME("edit/line/split/video")
	STR_MENU("Split at cursor (at video frame)")
	STR_DISP("Split at cursor (at video frame)")
	STR_HELP("Split the current line at the cursor, dividing the line's duration at the current video frame")

	void operator()(agi::Context *c) override {
		split_lines(c, [&](AssDialogue *n1, AssDialogue *n2) {
			int cur_frame = mid(
				c->videoController->FrameAtTime(n1->Start, agi::vfr::START),
				c->videoController->GetFrameN(),
				c->videoController->FrameAtTime(n1->End, agi::vfr::END));
			n1->End = n2->Start = c->videoController->TimeAtFrame(cur_frame, agi::vfr::END);
		});
	}
};

struct edit_redo final : public Command {
	CMD_NAME("edit/redo")
	CMD_ICON(redo_button)
	STR_HELP("Redo last undone action")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		return c->subsController->IsRedoStackEmpty() ?
			_("Nothing to &redo") :
			fmt_tl("&Redo %s", c->subsController->GetRedoDescription());
	}
	wxString StrDisplay(const agi::Context *c) const override {
		return c->subsController->IsRedoStackEmpty() ?
			_("Nothing to redo") :
			fmt_tl("Redo %s", c->subsController->GetRedoDescription());
	}

	bool Validate(const agi::Context *c) override {
		return !c->subsController->IsRedoStackEmpty();
	}

	void operator()(agi::Context *c) override {
		c->subsController->Redo();
	}
};

struct edit_undo final : public Command {
	CMD_NAME("edit/undo")
	CMD_ICON(undo_button)
	STR_HELP("Undo last action")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		return c->subsController->IsUndoStackEmpty() ?
			_("Nothing to &undo") :
			fmt_tl("&Undo %s", c->subsController->GetUndoDescription());
	}
	wxString StrDisplay(const agi::Context *c) const override {
		return c->subsController->IsUndoStackEmpty() ?
			_("Nothing to undo") :
			fmt_tl("Undo %s", c->subsController->GetUndoDescription());
	}

	bool Validate(const agi::Context *c) override {
		return !c->subsController->IsUndoStackEmpty();
	}

	void operator()(agi::Context *c) override {
		c->subsController->Undo();
	}
};

struct edit_revert final : public Command {
	CMD_NAME("edit/revert")
	STR_DISP("Revert")
	STR_MENU("Revert")
	STR_HELP("Revert the active line to its initial state (shown in the upper editor)")

	void operator()(agi::Context *c) override {
		AssDialogue *line = c->selectionController->GetActiveLine();
		line->Text = c->initialLineState->GetInitialText();
		c->ass->Commit(_("revert line"), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

struct edit_clear final : public Command {
	CMD_NAME("edit/clear")
	STR_DISP("Clear")
	STR_MENU("Clear")
	STR_HELP("Clear the current line's text")

	void operator()(agi::Context *c) override {
		AssDialogue *line = c->selectionController->GetActiveLine();
		line->Text = "";
		c->ass->Commit(_("clear line"), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

std::string get_text(AssDialogueBlock &d) { return d.GetText(); }
struct edit_clear_text final : public Command {
	CMD_NAME("edit/clear/text")
	STR_DISP("Clear Text")
	STR_MENU("Clear Text")
	STR_HELP("Clear the current line's text, leaving override tags")

	void operator()(agi::Context *c) override {
		AssDialogue *line = c->selectionController->GetActiveLine();
		auto blocks = line->ParseTags();
		line->Text = join(blocks
			| indirected
			| filtered([](AssDialogueBlock const& b) { return b.GetType() != AssBlockType::PLAIN; })
			| transformed(get_text),
			"");
		c->ass->Commit(_("clear line"), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

struct edit_insert_original final : public Command {
	CMD_NAME("edit/insert_original")
	STR_DISP("Insert Original")
	STR_MENU("Insert Original")
	STR_HELP("Insert the original line text at the cursor")

	void operator()(agi::Context *c) override {
		AssDialogue *line = c->selectionController->GetActiveLine();
		int sel_start = c->textSelectionController->GetSelectionStart();
		int sel_end = c->textSelectionController->GetSelectionEnd();

		line->Text = line->Text.get().substr(0, sel_start) + c->initialLineState->GetInitialText() + line->Text.get().substr(sel_end);
		c->ass->Commit(_("insert original"), AssFile::COMMIT_DIAG_TEXT, -1, line);
	}
};

struct edit_line_change_text final : public Command {
	CMD_NAME("edit/line/change-text")
	STR_MENU("Change text")
	STR_DISP("Change text")
	STR_HELP("Change only text in the selected lines")
	CMD_TYPE(COMMAND_VALIDATE)

	void operator()(agi::Context *c) override {
		EditChangeText(c);
	}
};

bool find_comment_block(std::string const& text, int pos, size_t& comment_start, size_t& comment_end) {
	if (pos < 0) pos = 0;
	if (pos > (int)text.size()) pos = text.size();

	size_t open = text.rfind('{', pos == 0 ? 0 : pos - 1);
	if (open == std::string::npos)
		return false;

	size_t close_before_pos = text.find('}', open + 1);
	if (close_before_pos != std::string::npos && close_before_pos < (size_t)pos)
		return false;

	size_t close = text.find('}', pos);
	if (close == std::string::npos)
		return false;

	comment_start = open;
	comment_end = close;

	return true;
}

bool cursor_is_in_comment(const agi::Context *c) {
	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line)
		return false;

	size_t comment_start, comment_end;

	return find_comment_block(
		line->Text.get(),
		c->textSelectionController->GetInsertionPoint(),
		comment_start,
		comment_end
	);
}

struct edit_comment final : public Command {
	CMD_NAME("edit/comment")
	STR_HELP("Toggle text comment")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		return cursor_is_in_comment(c) ? _("Uncomment") : _("Comment");
	}

	wxString StrDisplay(const agi::Context *c) const override {
		return StrMenu(c);
	}

	bool Validate(const agi::Context *c) override {
		return cursor_is_in_comment(c) || c->textSelectionController->GetSelectionStart() != c->textSelectionController->GetSelectionEnd();
	}

	void operator()(agi::Context *c) override {
		AssDialogue *line = c->selectionController->GetActiveLine();
		std::string text = line->Text.get();

		int sel_start = c->textSelectionController->GetSelectionStart();
		int sel_end = c->textSelectionController->GetSelectionEnd();
		int cursor_pos = c->textSelectionController->GetInsertionPoint();

		size_t comment_start, comment_end;
		if (find_comment_block(text, cursor_pos, comment_start, comment_end)) {
			text.erase(comment_end, 1);
			text.erase(comment_start, 1);
			line->Text = text;

			int new_pos = std::max<int>((int)comment_start, cursor_pos - 1);
			c->ass->Commit(_("Uncomment").Lower(), AssFile::COMMIT_DIAG_TEXT, -1, line);
			c->textSelectionController->SetSelection(new_pos, new_pos);

			return;
		}

		if (sel_start > sel_end)
			std::swap(sel_start, sel_end);

		text.insert(sel_end, "}");
		text.insert(sel_start, "{");
		line->Text = text;

		c->ass->Commit(_("Comment").Lower(), AssFile::COMMIT_DIAG_TEXT, -1, line);
		c->textSelectionController->SetSelection(sel_start + 1, sel_end + 1);
	}
};

}

namespace cmd {
	void init_edit() {
		reg(std::make_unique<edit_color_primary>());
		reg(std::make_unique<edit_color_secondary>());
		reg(std::make_unique<edit_color_outline>());
		reg(std::make_unique<edit_color_shadow>());
		reg(std::make_unique<edit_font>());
		reg(std::make_unique<edit_find_replace>());
		reg(std::make_unique<edit_find_in_folder>());
		reg(std::make_unique<edit_line_copy>());
		reg(std::make_unique<edit_line_cut>());
		reg(std::make_unique<edit_line_delete>());
		reg(std::make_unique<edit_line_duplicate>());
		reg(std::make_unique<edit_line_duplicate_shift>());
		reg(std::make_unique<edit_line_duplicate_shift_back>());
		reg(std::make_unique<edit_line_join_as_karaoke>());
		reg(std::make_unique<edit_line_join_concatenate>());
		reg(std::make_unique<edit_line_join_concatenate_with_typesetting>());
		reg(std::make_unique<edit_line_join_keep_first>());
		reg(std::make_unique<edit_line_paste>());
		reg(std::make_unique<edit_line_paste_over>());
		reg(std::make_unique<edit_line_recombine>());
		reg(std::make_unique<edit_line_split_by_karaoke>());
		reg(std::make_unique<edit_line_split_estimate>());
		reg(std::make_unique<edit_line_split_preserve>());
		reg(std::make_unique<edit_line_split_video>());
		reg(std::make_unique<edit_line_change_text>());
		reg(std::make_unique<edit_style_bold>());
		reg(std::make_unique<edit_style_italic>());
		reg(std::make_unique<edit_style_underline>());
		reg(std::make_unique<edit_style_strikeout>());
		reg(std::make_unique<edit_redo>());
		reg(std::make_unique<edit_undo>());
		reg(std::make_unique<edit_revert>());
		reg(std::make_unique<edit_insert_original>());
		reg(std::make_unique<edit_clear>());
		reg(std::make_unique<edit_clear_text>());
		reg(std::make_unique<edit_comment>());
	}
}
