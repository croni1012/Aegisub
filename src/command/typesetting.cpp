// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file typesetting.cpp
/// @brief Reshaping the selected lines: the Typesetting menu
///
/// The entries put the video into the matching transform tool rather than opening a
/// dialog, because the reshaping is done by dragging on the video. They are plain menu
/// items and not the radio buttons the other visual tools use: from a menu called
/// Transform, "which tool is currently active" is not the question being asked - and the
/// tool hands the video back to whichever one was in use as soon as it is finished with.

#include "command.h"

#include "../ass_file.h"
#include "../compat.h"
#include "../dialog_gradient.h"
#include "../floating_tag_windows.h"
#include "../frame_main.h"
#include "../subtitle_line_combiner.h"
#include "../include/aegisub/context.h"
#include "../selection_controller.h"
#include "../project.h"
#include "../typesetting_transform.h"
#include "../video_display.h"
#include "../visual_tool_transform.h"
#include "../visual_tool_textbox.h"

#include <string>
#include <typeinfo>

#include <wx/msgdlg.h>

namespace {
	using cmd::Command;

	/// Which visual tool to give the video back to when a transform is finished.
	///
	/// Remembered rather than looked up at the end, because by then the transform tool
	/// is the active one and the answer would always be the same. It also survives
	/// switching straight from one transform to another, which would otherwise forget
	/// where the user came from.
	std::string remembered_tool = "video/tool/cross";

	std::string const& previous_tool(const agi::Context *c) {
		if (c->videoDisplay->ToolIsType(typeid(VisualToolTransform)))
			return remembered_tool;

		static const char *tools[] = {
			"video/tool/textbox", "video/tool/cross", "video/tool/drag", "video/tool/rotate/z",
			"video/tool/rotate/xy", "video/tool/perspective", "video/tool/scale",
			"video/tool/clip", "video/tool/vector_clip", "video/tool/mask_edit",
			"video/tool/mask", "video/tool/shape"
		};
		remembered_tool = "video/tool/cross";
		for (auto name : tools) {
			if (cmd::get(name)->IsActive(c)) {
				remembered_tool = name;
				break;
			}
		}
		return remembered_tool;
	}

	/// Whether the selection holds a line that pins an image, in which case there is nothing here
	/// that can be done with it: the line is a rectangle standing in for a picture rather than
	/// typesetting, so reshaping it would move the frame and leave the picture where it was. Said
	/// out loud rather than greyed out, so that it is clear why nothing happened.
	bool RefusedForMask(agi::Context *c, wxString const& title = _("Transform")) {
		for (auto line : c->selectionController->GetSelectedSet()) {
			if (!IsImageMaskLine(line)) continue;
			wxMessageBox(_("Image mask not supported."), title,
				wxOK | wxICON_WARNING, c->parent);
			return true;
		}
		return false;
	}

	template<VisualToolTransformMode M>
	struct transform_command : public Command {
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider() && typesetting::CanTransform(c);
		}

		void operator()(agi::Context *c) override {
			if (RefusedForMask(c)) return;
			std::string back = previous_tool(c);
			c->videoDisplay->SetTool(
				std::make_unique<VisualToolTransform>(c->videoDisplay, c, M, std::move(back)));
		}
	};

struct typesetting_transform_free final :
	public transform_command<VisualToolTransformMode::Free> {
	CMD_NAME("typesetting/transform/free")
	STR_MENU("&Free transform")
	STR_DISP("Free transform")
	STR_HELP("Scale, turn and move the selected lines on the video, written as tags")
};

struct typesetting_transform_arch final :
	public transform_command<VisualToolTransformMode::Arch> {
	CMD_NAME("typesetting/transform/arch")
	STR_MENU("&Arch")
	STR_DISP("Arch")
	STR_HELP("Bend the selected lines up or down by dragging on the video")
};

struct typesetting_transform_distort final :
	public transform_command<VisualToolTransformMode::Distort> {
	CMD_NAME("typesetting/transform/distort")
	STR_MENU("&Distort")
	STR_DISP("Distort")
	STR_HELP("Drag the four corners of the selected lines on the video")
};

struct typesetting_transform_warp final :
	public transform_command<VisualToolTransformMode::Warp> {
	CMD_NAME("typesetting/transform/warp")
	STR_MENU("&Warp")
	STR_DISP("Warp")
	STR_HELP("Bend the selected lines by dragging a 3x3 mesh over the video")
};

/// Mirroring needs no handles and no video, so it is a plain command: convert to shapes,
/// mirror, done. Nothing else about the lines is touched.
template<bool Horizontal>
struct flip_command : public Command {
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return typesetting::CanTransform(c);
	}

	void operator()(agi::Context *c) override {
		if (RefusedForMask(c)) return;
		typesetting::ShapeEditor editor(c);
		if (!editor.ok()) {
			if (!editor.refusals().empty()) {
				wxString message = _("The text could not be converted:");
				for (auto const& why : editor.refusals())
					message += "\n\n" + to_wx(why);
				wxMessageBox(message, _("Convert text to shapes"), wxOK | wxICON_WARNING,
					c->parent);
			}
			return;
		}
		// No subdivision: a mirror keeps straight lines straight, so splitting them up
		// would only make the drawing longer.
		editor.Build(typesetting::FlipMap(editor.Box().centre, Horizontal), false);
		editor.Apply();
		c->ass->Commit(Horizontal ? _("flip horizontally") : _("flip vertically"),
			AssFile::COMMIT_DIAG_FULL);
	}
};

struct typesetting_flip_horizontal final : public flip_command<true> {
	CMD_NAME("typesetting/transform/flip/horizontal")
	STR_MENU("Flip &horizontally")
	STR_DISP("Flip horizontally")
	STR_HELP("Turn the selected lines into shapes and mirror them left to right")
};

struct typesetting_flip_vertical final : public flip_command<false> {
	CMD_NAME("typesetting/transform/flip/vertical")
	STR_MENU("Flip &vertically")
	STR_DISP("Flip vertically")
	STR_HELP("Turn the selected lines into shapes and mirror them top to bottom")
};

struct typesetting_gradient final : public Command {
	CMD_NAME("typesetting/gradient")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Gradient")
	STR_DISP("Gradient")
	STR_HELP("Create a linear, radial or per-character colour gradient on the selected lines")

	bool Validate(const agi::Context *c) override {
		return !c->selectionController->GetSelectedSet().empty();
	}

	void operator()(agi::Context *c) override {
		if (RefusedForMask(c, _("Gradient"))) return;
		ShowGradientDialog(c);
	}
};

struct typesetting_textbox final : public Command {
	CMD_NAME("typesetting/textbox")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Textbox")
	STR_DISP("Textbox")
	STR_HELP("Put the selected lines into a wrapping textbox drawn on the video")

	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() &&
			!c->selectionController->GetSelectedSet().empty();
	}

	void operator()(agi::Context *c) override {
		if (RefusedForMask(c, _("Textbox"))) return;
		std::string back = previous_tool(c);
		c->videoDisplay->SetTool(std::make_unique<VisualToolTextBox>(
			c->videoDisplay, c, true, std::move(back)));
	}
};

template<FloatingTagWindow Window>
struct floating_window_command : public Command {
	CMD_TYPE(COMMAND_TOGGLE)

	bool IsActive(const agi::Context *c) override {
		return c->frame && c->frame->IsFloatingTagWindowShown(Window);
	}

	void operator()(agi::Context *c) override {
		if (c->frame) c->frame->ToggleFloatingTagWindow(Window);
	}
};

struct typesetting_floating_basic final : floating_window_command<FloatingTagWindow::Basic> {
	CMD_NAME("typesetting/floating/basic")
	STR_MENU("Basic") STR_DISP("Basic")
	STR_HELP("Show or hide the basic tag window")
};

struct typesetting_floating_border_shadow final : floating_window_command<FloatingTagWindow::BorderShadow> {
	CMD_NAME("typesetting/floating/border-shadow")
	STR_MENU("Bord && Shad") STR_DISP("Bord & Shad")
	STR_HELP("Show or hide the border and shadow tag window")
};

struct typesetting_floating_font final : floating_window_command<FloatingTagWindow::Font> {
	CMD_NAME("typesetting/floating/font")
	STR_MENU("Font") STR_DISP("Font")
	STR_HELP("Show or hide the font tag window")
};

struct typesetting_floating_alignment final : floating_window_command<FloatingTagWindow::Alignment> {
	CMD_NAME("typesetting/floating/alignment")
	STR_MENU("Alignment") STR_DISP("Alignment")
	STR_HELP("Show or hide the alignment tag window")
};

struct typesetting_floating_transform final : floating_window_command<FloatingTagWindow::Transform> {
	CMD_NAME("typesetting/floating/transform")
	STR_MENU("Transformation") STR_DISP("Transformation")
	STR_HELP("Show or hide the transformation tag window")
};

struct typesetting_floating_delete final : floating_window_command<FloatingTagWindow::DeleteTags> {
	CMD_NAME("typesetting/floating/delete")
	STR_MENU("Tag deletion") STR_DISP("Tag deletion")
	STR_HELP("Show or hide the tag deletion window")
};

struct typesetting_floating_all final : public Command {
	CMD_NAME("typesetting/floating/all")
	STR_MENU("All") STR_DISP("All")
	STR_HELP("Show all floating tag windows in one group")

	void operator()(agi::Context *c) override {
		if (c->frame) c->frame->ShowAllFloatingTagWindows();
	}
};

}

namespace cmd {
	void init_typesetting() {
		reg(std::make_unique<typesetting_transform_free>());
		reg(std::make_unique<typesetting_transform_arch>());
		reg(std::make_unique<typesetting_transform_distort>());
		reg(std::make_unique<typesetting_transform_warp>());
		reg(std::make_unique<typesetting_flip_horizontal>());
		reg(std::make_unique<typesetting_flip_vertical>());
		reg(std::make_unique<typesetting_gradient>());
		reg(std::make_unique<typesetting_textbox>());
		reg(std::make_unique<typesetting_floating_all>());
		reg(std::make_unique<typesetting_floating_basic>());
		reg(std::make_unique<typesetting_floating_border_shadow>());
		reg(std::make_unique<typesetting_floating_font>());
		reg(std::make_unique<typesetting_floating_alignment>());
		reg(std::make_unique<typesetting_floating_transform>());
		reg(std::make_unique<typesetting_floating_delete>());
	}
}
