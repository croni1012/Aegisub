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
#include "../dialog_glitch.h"
#include "../dialog_animated_text.h"
#include "../dialog_motion.h"
#include "../subtitle_line_combiner.h"
#include "../include/aegisub/context.h"
#include "../selection_controller.h"
#include "../project.h"
#include "../typesetting_gradient.h"
#include "../typesetting_glitch.h"
#include "../typesetting_animated_text.h"
#include "../typesetting_transform.h"
#include "../typesetting_image_insert.h"
#include "../typesetting_motion.h"
#include "../video_controller.h"
#include "../video_display.h"
#include "../visual_tool_auto_motion.h"
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
		if (c->videoDisplay->ToolIsType(typeid(VisualToolTransform)) ||
			c->videoDisplay->ToolIsType(typeid(VisualToolAutoMotion)))
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

	bool RefusedForAnimatedGlitch(agi::Context *c,
			wxString const& title = _("Transform")) {
		if (!typesetting::glitch::SelectionHasEnabledAnimation(c)) return false;
		wxMessageBox(_("A glitch effect with animation cannot be transformed."), title,
			wxOK | wxICON_WARNING, c->parent);
		return true;
	}

	template<VisualToolTransformMode M>
	struct transform_command : public Command {
		CMD_TYPE(COMMAND_VALIDATE)

		bool Validate(const agi::Context *c) override {
			return !!c->project->VideoProvider() && typesetting::CanTransform(c);
		}

		void operator()(agi::Context *c) override {
			if (RefusedForMask(c) || RefusedForAnimatedGlitch(c)) return;
			// Already in a transform: say what it is doing now rather than putting another in its
			// place. Replacing it takes the bar away and builds a new one, and picking one
			// transform from inside another should not make the bar blink.
			if (auto *running = dynamic_cast<VisualToolTransform *>(c->videoDisplay->GetTool())) {
				running->SetMode(M, false);
				return;
			}
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

struct typesetting_transform_auto_perspective final : public Command {
	CMD_NAME("typesetting/transform/auto_perspective")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Auto &perspective")
	STR_DISP("Auto perspective")
	STR_HELP("Draw four directed points and fit the selected lines into their perspective")

	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() && typesetting::CanTransform(c);
	}

	void operator()(agi::Context *c) override {
		if (RefusedForMask(c) || RefusedForAnimatedGlitch(c)) return;
		if (auto *running = dynamic_cast<VisualToolTransform *>(c->videoDisplay->GetTool())) {
			running->SetMode(VisualToolTransformMode::Distort, true);
			return;
		}
		std::string back = previous_tool(c);
		c->videoDisplay->SetTool(std::make_unique<VisualToolTransform>(c->videoDisplay, c,
			VisualToolTransformMode::Distort, std::move(back), true));
	}
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
		if (RefusedForMask(c) || RefusedForAnimatedGlitch(c)) return;
		if (c->videoDisplay->ToolIsType(typeid(VisualToolTransform))) {
			// A transform preview owns replacement rows in the asynchronous subtitle renderer.
			// Tear it down before mirroring changes the real event list.
			cmd::call("video/tool/cross", c);
		}
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
		auto const& added = editor.applied();
		// Commit before selecting the replacement rows. Selection changes are stored in the
		// current undo snapshot, so doing this in the opposite order replaces the pre-flip
		// selection with IDs which do not exist after undoing the flip.
		c->ass->Commit(Horizontal ? _("flip horizontally") : _("flip vertically"),
			AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
		if (!added.empty()) {
			Selection selection(added.begin(), added.end());
			c->selectionController->SetSelectionAndActive(std::move(selection), added.front());
		}
		// A full canonical reload is intentional here: the operation can replace one source
		// row with several drawing rows, which must supersede every queued transform preview.
		c->videoController->ReloadSubtitles();
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

struct typesetting_gradient_edit final : public Command {
	CMD_NAME("typesetting/gradient/edit")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Edit gradient")
	STR_DISP("Edit gradient")
	STR_HELP("Edit the gradient settings of the selected generated gradient")

	bool Validate(const agi::Context *c) override {
		auto active = c->selectionController->GetActiveLine();
		return active && c->imageMask->IsGradientGroup(active);
	}

	void operator()(agi::Context *c) override {
		ShowGradientDialog(c);
	}
};

struct typesetting_gradient_copy final : public Command {
	CMD_NAME("typesetting/gradient/copy")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Copy gradient")
	STR_DISP("Copy gradient")
	STR_HELP("Copy the selected generated gradient rows and settings")

	bool Validate(const agi::Context *c) override {
		auto active = c->selectionController->GetActiveLine();
		return active && c->imageMask->IsGradientGroup(active);
	}

	void operator()(agi::Context *c) override {
		cmd::call("edit/line/copy", c);
	}
};

struct typesetting_gradient_delete final : public Command {
	CMD_NAME("typesetting/gradient/delete")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Remove gradient effect")
	STR_DISP("Remove gradient effect")
	STR_HELP("Remove the selected generated gradient and restore its original source line")

	bool Validate(const agi::Context *c) override {
		auto active = c->selectionController->GetActiveLine();
		return active && c->imageMask->IsGradientGroup(active);
	}

	void operator()(agi::Context *c) override { typesetting::gradient::Revert(c); }
};

struct typesetting_glitch final : public Command {
	CMD_NAME("typesetting/glitch")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Glitch effect")
	STR_DISP("Glitch effect")
	STR_HELP("Create customizable, angled and animated glitch slices on the selected lines")

	bool Validate(const agi::Context *c) override {
		return !c->selectionController->GetSelectedSet().empty();
	}

	void operator()(agi::Context *c) override {
		ShowGlitchDialog(c);
	}
};

struct typesetting_glitch_edit final : public Command {
	CMD_NAME("typesetting/glitch/edit")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Edit glitch effect")
	STR_DISP("Edit glitch effect")
	STR_HELP("Edit the settings of the selected generated glitch effect")

	bool Validate(const agi::Context *c) override {
		auto active = c->selectionController->GetActiveLine();
		return active && c->imageMask->IsGlitchGroup(active);
	}

	void operator()(agi::Context *c) override { ShowGlitchDialog(c); }
};

struct typesetting_glitch_copy final : public Command {
	CMD_NAME("typesetting/glitch/copy")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Copy glitch effect")
	STR_DISP("Copy glitch effect")
	STR_HELP("Copy the selected generated glitch rows and settings")

	bool Validate(const agi::Context *c) override {
		auto active = c->selectionController->GetActiveLine();
		return active && c->imageMask->IsGlitchGroup(active);
	}

	void operator()(agi::Context *c) override { cmd::call("edit/line/copy", c); }
};

struct typesetting_glitch_delete final : public Command {
	CMD_NAME("typesetting/glitch/delete")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Remove glitch effect")
	STR_DISP("Remove glitch effect")
	STR_HELP("Remove the selected generated glitch effect and restore its original source lines")

	bool Validate(const agi::Context *c) override {
		auto active = c->selectionController->GetActiveLine();
		return active && c->imageMask->IsGlitchGroup(active);
	}

	void operator()(agi::Context *c) override { typesetting::glitch::Revert(c); }
};

struct typesetting_animated_text final : public Command {
	CMD_NAME("typesetting/animated_text")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Animated Text")
	STR_DISP("Animated Text")
	STR_HELP("Animate words, syllables or characters with frame-based ASS transforms")
	bool Validate(const agi::Context *c) override {
		return !c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override { ShowAnimatedTextDialog(c); }
};

struct typesetting_animated_text_edit final : public Command {
	CMD_NAME("typesetting/animated_text/edit")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Edit Animated Text")
	STR_DISP("Edit Animated Text")
	STR_HELP("Edit the selected Animated Text settings")
	bool Validate(const agi::Context *c) override {
		return typesetting::animated_text::IsEffect(*c->ass,
			c->selectionController->GetActiveLine());
	}
	void operator()(agi::Context *c) override { ShowAnimatedTextDialog(c); }
};

struct typesetting_animated_text_copy final : public Command {
	CMD_NAME("typesetting/animated_text/copy")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Copy Animated Text")
	STR_DISP("Copy Animated Text")
	STR_HELP("Copy the selected lines with their editable Animated Text settings")
	bool Validate(const agi::Context *c) override {
		return typesetting::animated_text::IsEffect(*c->ass,
			c->selectionController->GetActiveLine());
	}
	void operator()(agi::Context *c) override { cmd::call("edit/line/copy", c); }
};

struct typesetting_animated_text_delete final : public Command {
	CMD_NAME("typesetting/animated_text/delete")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("Remove Animated Text")
	STR_DISP("Remove Animated Text")
	STR_HELP("Restore the selected lines to their text before animation")
	bool Validate(const agi::Context *c) override {
		return typesetting::animated_text::IsEffect(*c->ass,
			c->selectionController->GetActiveLine());
	}
	void operator()(agi::Context *c) override { typesetting::animated_text::Revert(c); }
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

struct typesetting_image_insert_quick final : public Command {
	CMD_NAME("typesetting/image_insert/quick")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("&Quick insert")
	STR_DISP("Quick image insert")
	STR_HELP("Insert images using the saved basic or dynamic settings")
	bool Validate(const agi::Context *c) override {
		return !c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override { typesetting::image_insert::QuickInsert(c); }
};

struct typesetting_image_insert_insert final : public Command {
	CMD_NAME("typesetting/image_insert/insert")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("&Insert")
	STR_DISP("Image insert")
	STR_HELP("Convert an image to optimized ImageMask subtitle drawings")
	bool Validate(const agi::Context *c) override {
		return !c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override { typesetting::image_insert::Insert(c); }
};

struct typesetting_image_insert_edit final : public Command {
	CMD_NAME("typesetting/image_insert/edit")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("With &editing...")
	STR_DISP("ImageEditor insert")
	STR_HELP("Edit the current video frame in ImageEditor before inserting the result")
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() &&
			!c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override {
		typesetting::image_insert::EditWithImageEditor(c);
	}
};

struct typesetting_image_insert_settings final : public Command {
	CMD_NAME("typesetting/image_insert/settings")
	STR_MENU("&Settings")
	STR_DISP("Image insert settings")
	STR_HELP("Configure Quick insert and dynamic image filenames")
	void operator()(agi::Context *c) override { typesetting::image_insert::ShowSettings(c); }
};

struct typesetting_motion_apply final : public Command {
	CMD_NAME("typesetting/motion/apply")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("&Apply")
	STR_DISP("Apply motion")
	STR_HELP("Apply Mocha Corner Pin or Transform Data to the selected lines")
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() &&
			!c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override { ShowMotionApplyDialog(c); }
};

struct typesetting_motion_auto final : public Command {
	CMD_NAME("typesetting/motion/auto")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("&Auto motion")
	STR_DISP("Auto motion")
	STR_HELP("Select a video region and track its position, size and rotation")
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() &&
			!c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override {
		std::string back = previous_tool(c);
		c->videoDisplay->SetTool(std::make_unique<VisualToolAutoMotion>(
			c->videoDisplay, c, std::move(back)));
	}
};

struct typesetting_motion_revert final : public Command {
	CMD_NAME("typesetting/motion/revert")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("&Revert")
	STR_DISP("Revert motion")
	STR_HELP("Restore the source lines saved by Apply or Auto motion")
	bool Validate(const agi::Context *c) override {
		return !c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override {
		std::string error;
		if (!typesetting::motion::Revert(c, error))
			wxMessageBox(to_wx(error), _("Motion"), wxOK | wxICON_WARNING, c->parent);
	}
};

struct typesetting_motion_trim final : public Command {
	CMD_NAME("typesetting/motion/trim")
	CMD_TYPE(COMMAND_VALIDATE)
	STR_MENU("&Trim")
	STR_DISP("Motion trim")
	STR_HELP("Write the selected time range as JPEG frames or an H.264 MP4")
	bool Validate(const agi::Context *c) override {
		return !!c->project->VideoProvider() &&
			!c->selectionController->GetSelectedSet().empty();
	}
	void operator()(agi::Context *c) override { CreateMotionTrim(c); }
};

struct typesetting_motion_trim_settings final : public Command {
	CMD_NAME("typesetting/motion/trim/settings")
	STR_MENU("Trim &settings...")
	STR_DISP("Motion trim settings")
	STR_HELP("Choose the motion trim format and output directory")
	void operator()(agi::Context *c) override { ShowMotionTrimSettings(c); }
};

}

namespace cmd {
	void init_typesetting() {
		reg(std::make_unique<typesetting_transform_free>());
		reg(std::make_unique<typesetting_transform_arch>());
		reg(std::make_unique<typesetting_transform_distort>());
		reg(std::make_unique<typesetting_transform_auto_perspective>());
		reg(std::make_unique<typesetting_transform_warp>());
		reg(std::make_unique<typesetting_flip_horizontal>());
		reg(std::make_unique<typesetting_flip_vertical>());
		reg(std::make_unique<typesetting_gradient>());
		reg(std::make_unique<typesetting_gradient_edit>());
		reg(std::make_unique<typesetting_gradient_copy>());
		reg(std::make_unique<typesetting_gradient_delete>());
		reg(std::make_unique<typesetting_glitch>());
		reg(std::make_unique<typesetting_glitch_edit>());
		reg(std::make_unique<typesetting_glitch_copy>());
		reg(std::make_unique<typesetting_glitch_delete>());
		reg(std::make_unique<typesetting_animated_text>());
		reg(std::make_unique<typesetting_animated_text_edit>());
		reg(std::make_unique<typesetting_animated_text_copy>());
		reg(std::make_unique<typesetting_animated_text_delete>());
		reg(std::make_unique<typesetting_textbox>());
		reg(std::make_unique<typesetting_image_insert_quick>());
		reg(std::make_unique<typesetting_image_insert_insert>());
		reg(std::make_unique<typesetting_image_insert_edit>());
		reg(std::make_unique<typesetting_image_insert_settings>());
		reg(std::make_unique<typesetting_motion_auto>());
		reg(std::make_unique<typesetting_motion_apply>());
		reg(std::make_unique<typesetting_motion_revert>());
		reg(std::make_unique<typesetting_motion_trim>());
		reg(std::make_unique<typesetting_motion_trim_settings>());
	}
}
