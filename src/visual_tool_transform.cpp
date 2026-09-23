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

/// @file visual_tool_transform.cpp
/// @brief Reshaping the selected drawings by dragging on the video

#include "visual_tool_transform.h"

#include "text_to_shape.h"
#include "typesetting_perspective.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "command/command.h"
#include "compat.h"
#include "frame_main.h"
#include "gl_text.h"
#include "subtitle_line_combiner.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/color.h>
#include <libaegisub/format.h>

#include <algorithm>
#include <limits>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

#include <wx/colour.h>
#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>

namespace {
	/// The warp's last handle, the one below the shape that moves the whole of it. The corners
	/// and their direction handles come first, so it sits after the twelve of them.
	const int warp_move_handle = 12;

	/// The lines the tool draws, over a dimmed video: red, because on a busy frame black
	/// dashes disappear into the picture.
	const wxColour mesh_colour(255, 62, 62);

	const double pi = 3.14159265358979;

	/// A number as a tag wants it: no more precision than is useful, no trailing zeroes.
	std::string Number(double value) {
		std::string out = agi::format("%.3f", value);
		if (out.find('.') != std::string::npos) {
			while (!out.empty() && out.back() == '0') out.pop_back();
			if (!out.empty() && out.back() == '.') out.pop_back();
		}
		return out.empty() || out == "-0" ? "0" : out;
	}

	/// Turn a point about another, the way \frz turns text: anticlockwise on screen, which
	/// with y pointing down is this pair of formulas.
	Vector2D RotateAbout(Vector2D point, Vector2D pivot, double degrees) {
		if (std::abs(degrees) < 1e-9) return point;
		double radians = degrees * pi / 180.0;
		double sine = std::sin(radians), cosine = std::cos(radians);
		Vector2D offset = point - pivot;
		return pivot + Vector2D((float)(offset.X() * cosine + offset.Y() * sine),
		                        (float)(-offset.X() * sine + offset.Y() * cosine));
	}

	using Matrix2 = TransformMatrix2;

	Matrix2 Multiply(Matrix2 const& left, Matrix2 const& right) {
		return {left.a * right.a + left.b * right.c, left.a * right.b + left.b * right.d,
		        left.c * right.a + left.d * right.c, left.c * right.b + left.d * right.d};
	}

	Vector2D ApplyMatrix(Matrix2 const& map, Vector2D point) {
		return Vector2D((float)(map.a * point.X() + map.b * point.Y()),
		                (float)(map.c * point.X() + map.d * point.Y()));
	}

	/// The map that undoes this one. False for one that has collapsed, where nothing undoes it.
	bool Invert(Matrix2 const& map, Matrix2& out) {
		double det = map.a * map.d - map.b * map.c;
		if (std::abs(det) < 1e-12) return false;
		out = {map.d / det, -map.b / det, -map.c / det, map.a / det};
		return true;
	}

	/// A turn the way \frz turns: anticlockwise on screen, where y points down.
	Matrix2 Turn(double degrees) {
		double radians = degrees * pi / 180.0;
		double sine = std::sin(radians), cosine = std::cos(radians);
		return {cosine, sine, -sine, cosine};
	}

	/// What a line's own tags do to it.
	///
	/// The lean comes after the scale, not before: libass multiplies \fax by
	/// scale_x/scale_y before using it, which is the same as leaning the already-scaled
	/// glyph. Getting that order the wrong way round makes the lean come out short by
	/// exactly fscy/fscx.
	Matrix2 LineMatrix(Vector2D scale, Vector2D shear, double angle) {
		Matrix2 scaled{scale.X() / 100.0, 0, 0, scale.Y() / 100.0};
		Matrix2 sheared{1, shear.X(), shear.Y(), 1};
		return Multiply(Turn(angle), Multiply(scaled, sheared));
	}

	/// Split a map back into the numbers a line can carry, keeping the lean it already has
	/// along y.
	///
	/// \fay is left exactly as the line said it: a turn, two scales and \fax can describe
	/// any map on their own, so there is never a need to touch it - and rewriting it would
	/// change every other number for nothing. Holding it fixed leaves four unknowns for four
	/// equations, and for a line nobody has dragged the answer is what it already said.
	///
	/// Returns false for a map that has collapsed, where there would be nothing to say.
	bool SplitMatrix(Matrix2 const& map, double shear_y, double& angle, double& shear_x,
	                 Vector2D& scale) {
		// Turned back, the bottom row of the map has to read (sy * fay, sy) - and that one
		// ratio is what fixes the turn.
		double first = map.a - shear_y * map.b;
		double second = map.c - shear_y * map.d;
		if (std::hypot(first, second) < 1e-12) return false;

		double radians = std::atan2(-second, first);
		double cosine = std::cos(radians), sine = std::sin(radians);

		double along = cosine * map.a - sine * map.c;
		double across = sine * map.b + cosine * map.d;
		if (along < 0) {
			// The same map read the other way up. A line can carry a negative scale, but a
			// negative width reads as a mistake rather than as a mirror.
			radians += pi;
			cosine = -cosine;
			sine = -sine;
			along = -along;
			across = -across;
		}
		if (std::abs(along) < 1e-9) return false;

		angle = radians * 180.0 / pi;
		scale = Vector2D((float)(along * 100), (float)(across * 100));
		shear_x = (cosine * map.b - sine * map.d) / along;
		return true;
	}

	/// What a line says for a tag that carries one number, or the fallback if it says
	/// nothing. The base has readers for the tags it needs; these two it does not.
	double TagNumber(AssDialogue *line, const char *name, double fallback) {
		for (auto& block : line->ParseTags()) {
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				if (tag.Name == name && !tag.Params.empty())
					return tag.Params[0].Get<double>(fallback);
		}
		return fallback;
	}

	/// Whether text contains two words separated by whitespace or an ASS hard line break.
	/// UTF-8 bytes are deliberately treated as ordinary word bytes; whitespace and \N/\n are
	/// ASCII in ASS, so this neither splits nor damages non-Latin text.
	bool HasAtLeastTwoWords(std::string const& text) {
		int words = 0;
		bool in_word = false;
		for (size_t i = 0; i < text.size(); ++i) {
			unsigned char ch = static_cast<unsigned char>(text[i]);
			bool separator = std::isspace(ch) ||
				(text[i] == '\\' && i + 1 < text.size() &&
				 (text[i + 1] == 'N' || text[i + 1] == 'n'));
			if (separator) {
				in_word = false;
				if (text[i] == '\\') ++i;
				continue;
			}
			if (!in_word) {
				in_word = true;
				if (++words >= 2) return true;
			}
		}
		return false;
	}

	/// The same, but only from the first tag block.
	///
	/// This is what is in force where the line begins. Looking further in would pick up a value
	/// meant for one word - a \fscx on the emphasised one, say - and treat it as the line's own.
	double FirstBlockNumber(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	                        const char *name, double fallback) {
		for (auto& block : blocks) {
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
				if (tag.Name == name && !tag.Params.empty())
					return tag.Params[0].Get<double>(fallback);
			// Only the first block: what comes after belongs to part of the line.
			return fallback;
		}
		return fallback;
	}

	/// How far along its box a line's anchor sits, from its alignment.
	/// The convex hull of a set of points, anticlockwise in script coordinates.
	std::vector<Vector2D> ConvexHull(std::vector<Vector2D> const& points) {
		if (points.size() < 3) return {};

		std::vector<Vector2D> sorted = points;
		std::sort(sorted.begin(), sorted.end(), [](Vector2D a, Vector2D b) {
			return a.X() != b.X() ? a.X() < b.X() : a.Y() < b.Y();
		});
		auto turn = [](Vector2D o, Vector2D a, Vector2D b) {
			return (double)(a.X() - o.X()) * (b.Y() - o.Y()) -
				(double)(a.Y() - o.Y()) * (b.X() - o.X());
		};

		std::vector<Vector2D> hull(sorted.size() * 2);
		size_t at = 0;
		for (auto const& point : sorted) {
			while (at >= 2 && turn(hull[at - 2], hull[at - 1], point) <= 0) --at;
			hull[at++] = point;
		}
		size_t lower = at + 1;
		for (size_t i = sorted.size() - 1; i-- > 0;) {
			while (at >= lower && turn(hull[at - 2], hull[at - 1], sorted[i]) <= 0) --at;
			hull[at++] = sorted[i];
		}
		hull.resize(at ? at - 1 : 0);
		if (hull.size() < 3) return {};
		return hull;
	}

	/// The hull with the vertices that hardly bend it taken out, so a shape drawn as four
	/// corners is four corners again even when a click or a curve fit left a little wobble
	/// along an edge.
	std::vector<Vector2D> SimplifyHull(std::vector<Vector2D> hull, double tolerance) {
		bool dropped = true;
		while (dropped && hull.size() > 4) {
			dropped = false;
			double worst = tolerance;
			size_t at = hull.size();
			for (size_t i = 0; i < hull.size(); ++i) {
				Vector2D before = hull[(i + hull.size() - 1) % hull.size()];
				Vector2D after = hull[(i + 1) % hull.size()];
				Vector2D edge = after - before;
				double length = edge.Len();
				if (length < 1e-6) continue;
				double away = std::abs((double)(hull[i].X() - before.X()) * edge.Y() -
					(double)(hull[i].Y() - before.Y()) * edge.X()) / length;
				if (away >= worst) continue;
				worst = away;
				at = i;
			}
			if (at < hull.size()) {
				hull.erase(hull.begin() + at);
				dropped = true;
			}
		}
		return hull;
	}

	/// The smallest rectangle, at any angle, that holds a convex hull.
	///
	/// The smallest one always has a side along a side of the hull, so every hull edge is
	/// tried and the best kept. Returns false when the hull has no area.
	bool MinimumAreaBox(std::vector<Vector2D> const& hull, typesetting::OrientedBox& out) {
		if (hull.size() < 3) return false;

		double best_area = 0;
		bool found = false;
		for (size_t i = 0; i < hull.size(); ++i) {
			Vector2D edge = hull[(i + 1) % hull.size()] - hull[i];
			if (edge.Len() < 1e-6f) continue;

			typesetting::OrientedBox frame;
			frame.angle = (float)(std::atan2(edge.Y(), edge.X()) * 180.0 / pi);
			frame.centre = Vector2D(0.f, 0.f);

			Vector2D low, high;
			for (size_t j = 0; j < hull.size(); ++j) {
				Vector2D local = frame.ToLocal(hull[j]);
				if (!j) { low = high = local; }
				else { low = low.Min(local); high = high.Max(local); }
			}

			Vector2D size = high - low;
			double area = (double)size.X() * size.Y();
			if (found && !(area < best_area)) continue;

			found = true;
			best_area = area;
			out.angle = frame.angle;
			out.centre = frame.ToScript((low + high) / 2);
			out.half = size / 2;
		}
		return found && best_area > 0;
	}

	/// The same rectangle said the way round that was asked for.
	///
	/// A rectangle at some angle is the same rectangle a quarter turn on with its two sides
	/// swapped, and four such readings describe it. The smallest-area search returns whichever
	/// it came to first, so this picks the reading whose own x axis lies nearest the angle
	/// wanted - which is what keeps a scale along x meaning \fscx rather than \fscy.
	void FaceBox(typesetting::OrientedBox& box, float towards) {
		int best = 0;
		double closest = 0;
		for (int quarter = 0; quarter < 4; ++quarter) {
			double difference = box.angle + quarter * 90.0 - towards;
			while (difference > 180.0) difference -= 360.0;
			while (difference <= -180.0) difference += 360.0;
			if (quarter && !(std::abs(difference) < closest)) continue;
			closest = std::abs(difference);
			best = quarter;
		}
		double turned = box.angle + best * 90.0;
		while (turned > 180.0) turned -= 360.0;
		while (turned <= -180.0) turned += 360.0;
		box.angle = (float)turned;
		if (best % 2) box.half = Vector2D(box.half.Y(), box.half.X());
	}

	Vector2D AnchorFractions(int align) {
		int horizontal = (align - 1) % 3;
		int vertical = (align - 1) / 3;
		return Vector2D(horizontal == 0 ? 0.f : horizontal == 1 ? .5f : 1.f,
		                vertical == 2 ? 0.f : vertical == 1 ? .5f : 1.f);
	}

	/// A perspective target has to walk around one convex, non-degenerate quadrilateral.
	/// The sign is deliberately not normalized: clockwise and anticlockwise point orders are
	/// different directed mappings and Auto perspective preserves the one the user drew.
	bool DirectedQuad(std::vector<Vector2D> const& points) {
		if (points.size() != 4) return false;
		double direction = 0;
		for (int i = 0; i < 4; ++i) {
			Vector2D first = points[(i + 1) % 4] - points[i];
			Vector2D second = points[(i + 2) % 4] - points[(i + 1) % 4];
			double cross = first.X() * second.Y() - first.Y() * second.X();
			if (std::abs(cross) < 1e-3) return false;
			if (!direction) direction = cross;
			else if ((cross > 0) != (direction > 0)) return false;
		}
		return true;
	}
}

VisualToolTransform::VisualToolTransform(VideoDisplay *parent, agi::Context *context,
                                         VisualToolTransformMode mode,
                                         std::string return_tool,
                                         bool auto_perspective)
: VisualTool<VisualDraggableFeature>(parent, context)
, mode(mode)
, auto_perspective(auto_perspective)
, gl_text(std::make_unique<OpenGLText>())
, return_tool(std::move(return_tool))
{
	SettleForMode();
	selection_connection = context->selectionController->AddSelectionListener(
		[this] { ExitTool(); });
	connections.push_back(context->ass->AddCommitListener(
		&VisualToolTransform::OnFileReplaced, this));
	if (context->parent)
		context->parent->Bind(wxEVT_CHAR_HOOK, &VisualToolTransform::OnCharHook, this);
	preview_interface.AttachHost(parent->GetPreviewBar(), [this](int id) {
		// Focus first: Apply and Cancel can synchronously replace and destroy this tool.
		this->parent->SetFocus();
		Perform(static_cast<VisualToolTransformAction>(id));
	}, [this](int id, double value, bool) {
		switch (static_cast<VisualToolTransformAction>(id)) {
			case VisualToolTransformAction::UniformSize: UpdateUniformSize(value); break;
			case VisualToolTransformAction::ScaleX:
			case VisualToolTransformAction::ScaleY:
				UpdateScaleAxis(static_cast<VisualToolTransformAction>(id), value);
				break;
			case VisualToolTransformAction::Rotation: UpdateRotation(value); break;
			case VisualToolTransformAction::ShearX: UpdateShear(false, value); break;
			case VisualToolTransformAction::ShearY: UpdateShear(true, value); break;
			case VisualToolTransformAction::DistortAngleX: UpdateDistortAngle(0, value); break;
			case VisualToolTransformAction::DistortAngleY: UpdateDistortAngle(1, value); break;
			default: break;
		}
	});
	Collect();
}

void VisualToolTransform::SettleForMode() {
	recalc_bord = true;
	recalc_shad = true;
	recalc_blur = true;
	recalc_clip = true;
	maintain_decor = false;

	// A projective distortion has no one scale behind it, so there is no honest number to give
	// the border, the shadow or the blur: all three are left exactly as the line said them.
	// Standing the pair in as shapes was tried too, and it turns one line into three.
	if (mode == VisualToolTransformMode::Distort) {
		recalc_bord = false;
		recalc_shad = false;
		maintain_decor = false;
		recalc_blur = false;
	}

	// Auto perspective exposes no decoration recalculation choices. The line geometry follows
	// the directed quadrilateral, existing decoration values stay as authored, and only an
	// existing clip is carried through the same map automatically.
	if (auto_perspective) {
		recalc_bord = false;
		recalc_shad = false;
		maintain_decor = false;
		recalc_blur = false;
		recalc_clip = true;
		// Which of the two ways of sizing the result was last used is a habit rather than a
		// property of the lines, so it is remembered between sessions and between runs.
		auto_perspective_keep_original_size =
			OPT_GET("Tool/Visual/Perspective/Keep Original Size")->GetBool();
	}
}

void VisualToolTransform::SetMode(VisualToolTransformMode next, bool perspective) {
	if (leaving || (mode == next && auto_perspective == perspective)) return;

	// Whatever was being worked out belongs to the mode it was worked out in, so it goes the way
	// it would have gone had the tool been closed and another opened: nothing is written, and the
	// video stops showing the preview copies. The difference is only that the tool itself stays -
	// and with it its bar, which otherwise vanished and was built again on every change of mode.
	ClearPreview();
	editor.reset();
	textbox_document.reset();
	textbox_lines.clear();
	features.clear();
	sel_features.clear();
	undo_history.clear();
	redo_history.clear();
	touched = false;
	free_hold_mode = FreeHoldMode::None;
	// Balancing the lock Collect is about to take, exactly as closing the tool would have.
	LockEditing(false);

	mode = next;
	auto_perspective = perspective;
	SettleForMode();

	Collect();
	UpdatePreviewInterface();
	parent->Render();
}

VisualToolTransform::~VisualToolTransform() {
	// Switching to another tool destroys this one without going through ExitTool, and the
	// video would carry on showing a preview of something the file never said.
	//
	// Unless it is the window that is going. Then handing the lines back only makes the video
	// render one more frame on its way out, which is the flash seen when the program is closed in
	// the middle of a session - and there is nothing to put right anyway, since a preview never
	// lived anywhere but in the video's own copy of the file.
	if (!WindowGoing()) ClearPreview();
	if (c->parent)
		c->parent->Unbind(wxEVT_CHAR_HOOK, &VisualToolTransform::OnCharHook, this);
	// However this ends - including the window closing - the editor must not be left dead.
	LockEditing(false);
}

bool VisualToolTransform::WindowGoing() const {
	// The window has begun to come apart, so anything drawn or handed to the video from here would
	// only be one more frame nobody asked to see.
	return (c->frame && c->frame->IsClosing()) ||
		(c->parent && c->parent->IsBeingDeleted());
}

bool VisualToolTransform::TagsMode() const {
	return mode == VisualToolTransformMode::Free || mode == VisualToolTransformMode::Distort;
}

bool VisualToolTransform::CollectTextBox() {
	textbox_document.reset();
	textbox_lines.clear();
	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line || !c->imageMask || !c->imageMask->IsTextBoxGroup(line)) return false;
	textbox_lines = c->imageMask->GetGroupLines(line);
	if (textbox_lines.empty()) return false;
	auto loaded = typesetting::textbox::Load(*c->ass, *textbox_lines.front());
	if (!loaded) {
		textbox_lines.clear();
		return false;
	}
	textbox_document = std::move(*loaded);
	typesetting::textbox::Corners(*textbox_document, textbox_original_corners);
	return true;
}

typesetting::textbox::Document VisualToolTransform::TransformedTextBox() const {
	auto transformed = *textbox_document;
	Vector2D target[4];
	if (mode == VisualToolTransformMode::Distort)
		std::copy(corners, corners + 4, target);
	else
		for (int i = 0; i < 4; ++i) target[i] = MapPoint(textbox_original_corners[i]);
	typesetting::textbox::SetCorners(transformed, target);
	return transformed;
}

bool VisualToolTransform::Active() const {
	return TagsMode() ? !tag_lines.empty() : editor.has_value();
}

bool VisualToolTransform::LinesAlive() const {
	if (tag_lines.empty() && !editor) return false;

	std::set<AssDialogue const *> live;
	for (auto const& line : c->ass->Events) live.insert(&line);

	for (auto const& found : tag_lines)
		if (!live.count(found.line)) return false;
	for (auto line : textbox_lines)
		if (!live.count(line)) return false;
	if (editor)
		for (auto line : editor->lines())
			if (!live.count(line)) return false;
	return true;
}

void VisualToolTransform::OnFileReplaced(int type) {
	if (type != AssFile::COMMIT_NEW) return;

	// Everything this session was holding belonged to the file that has just gone. Dropped
	// before leaving, so that leaving does not try to hand those lines back to the video.
	tag_lines.clear();
	split_lines.clear();
	shear_split = false;
	split_built = false;
	editor.reset();
	textbox_document.reset();
	textbox_lines.clear();
	features.clear();
	sel_features.clear();
	ExitTool();
}

void VisualToolTransform::SendPreview() {
	if (!LinesAlive() || WindowGoing()) return;
	if (TagsMode()) {
		if (TextBoxMode()) {
			auto transformed = TransformedTextBox();
			auto generated = typesetting::textbox::Generate(c, *textbox_lines.front(), transformed);
			// Above every layer the file uses, for the same reason the box editor does it: while
			// it is being worked on it has to be seen, whatever else is stacked over it.
			int above_everything = textbox_lines.front()->Layer;
			for (auto const& line : c->ass->Events)
				above_everything = std::max(above_everything, line.Layer);
			if (above_everything < std::numeric_limits<int>::max()) ++above_everything;
			for (auto& row : generated) row.Layer = above_everything;
			std::vector<AssDialogue> hidden;
			hidden.reserve(textbox_lines.size());
			for (auto line : textbox_lines) {
				AssDialogue copy(*line);
				copy.Comment = true;
				hidden.push_back(std::move(copy));
			}
			std::vector<AssDialogue const *> changed, added;
			for (auto const& line : hidden) changed.push_back(&line);
			for (auto const& line : generated) added.push_back(&line);
			c->videoController->PreviewSubtitles(changed, added);
			return;
		}
		if (maintain_decor) EnsureDecor();
		auto const& lines = Lines();

		// The copies only have to live until the call returns: the video takes its own. All of
		// them are made before any pointer into the list is taken, or growing it would leave
		// those pointers behind.
		std::vector<AssDialogue> copies;
		/// Whether a copy is a line the file does not have, which has to be added rather than
		/// stood in for.
		std::vector<bool> extra;
		copies.reserve(lines.size() * 3 + tag_lines.size());
		for (auto const& found : lines) {
			// The shapes that stand for the border and the shadow, and then the letters over them.
			//
			// A preview adds its lines to the end of the file, so the letters have to go there with
			// the shapes or the shapes would be drawn over them - which is why the line they belong
			// to is silenced where it stands and put back at the end, in the order it all has to be
			// drawn in. Accepting does the same thing in the file, except that there it can insert.
			if (maintain_decor && !found.decor.empty()) {
				if (!found.owned) {
					AssDialogue silenced(*found.line);
					silenced.Comment = true;
					copies.push_back(std::move(silenced));
					extra.push_back(false);
				}
				for (auto const& decor : found.decor) {
					for (bool shadow : {true, false}) {
						if (shadow ? !decor.has_shadow : !decor.has_border) continue;
						AssDialogue copy(*found.line);
						copy.Text = DecorLineText(found, decor, shadow);
						copy.Comment = false;
						copies.push_back(std::move(copy));
						extra.push_back(true);
					}
				}
				// The shape preserves the border, not the glyph fill. Always keep the original
				// letters on top: omitting them can expose another language layer underneath.
				AssDialogue copy(*found.line);
				copy.Text = TagLineText(found);
				copy.Comment = false;
				copies.push_back(std::move(copy));
				extra.push_back(true);
				continue;
			}

			AssDialogue copy(*found.line);
			copy.Text = TagLineText(found);
			copies.push_back(std::move(copy));
			extra.push_back(found.owned != nullptr);
		}
		// A line that has been cut into pieces has to stop being drawn, or it would be drawn
		// underneath them - and a comment is how a line stops being drawn without being taken
		// out of the file.
		for (auto const& found : tag_lines) {
			if (!found.replaced) continue;
			AssDialogue silenced(*found.line);
			silenced.Comment = true;
			copies.push_back(std::move(silenced));
			extra.push_back(false);
		}

		// The pieces are not in the file, so they go in as lines of their own rather than as
		// changes to lines that are there.
		std::vector<AssDialogue const *> changed, added;
		for (size_t at = 0; at < copies.size(); ++at)
			(extra[at] ? added : changed).push_back(&copies[at]);
		c->videoController->PreviewSubtitles(changed, added);
		return;
	}

	if (!editor) return;
	// The copies only have to live until the call returns: the video takes its own.
	auto preview = editor->PreviewLines();
	std::vector<AssDialogue const *> silenced, drawings;
	silenced.reserve(preview.silenced.size());
	for (auto const& line : preview.silenced) silenced.push_back(line.get());
	drawings.reserve(preview.drawings.size());
	for (auto const& line : preview.drawings) drawings.push_back(line.get());
	c->videoController->PreviewSubtitles(silenced, drawings);
}

void VisualToolTransform::ClearPreview() {
	if (!LinesAlive()) return;
	if (TextBoxMode()) {
		std::vector<AssDialogue const *> pointers(textbox_lines.begin(), textbox_lines.end());
		if (!pointers.empty()) c->videoController->PreviewSubtitles(pointers);
		return;
	}
	if (TagsMode()) {
		std::vector<AssDialogue const *> pointers;
		for (auto const& found : tag_lines) pointers.push_back(found.line);
		if (!pointers.empty()) c->videoController->PreviewSubtitles(pointers);
		return;
	}

	if (!editor) return;
	// The real lines never changed, so handing them over again is what puts the video
	// back to showing them.
	std::vector<AssDialogue const *> pointers;
	for (auto line : editor->lines()) pointers.push_back(line);
	if (!pointers.empty()) c->videoController->PreviewSubtitles(pointers);
}

bool VisualToolTransform::CollectTags() {
	tag_lines.clear();
	split_lines.clear();
	shear_split = false;
	split_built = false;
	gesture_scale = Vector2D(1.f, 1.f);
	gesture_angle = 0;
	gesture_move = Vector2D(0.f, 0.f);
	gesture_anchor = Vector2D(0.f, 0.f);
	// Left behind once, and then the tool opened with the last lean still in force and the
	// lines moved the moment it appeared.
	gesture_shear = Vector2D(0.f, 0.f);
	frame_linear = TransformMatrix2();
	frame_offset = Vector2D(0.f, 0.f);
	uniform_size = 100.0;
	size_x = 100.0;
	size_y = 100.0;
	scale_growth = Vector2D(1.f, 1.f);
	distort_angle_offset[0] = distort_angle_offset[1] = 0;

	auto lines = TextBoxMode() ? textbox_lines : c->selectionController->GetSortedSelection();

	// A real quadrilateral in the active row completely defines Auto perspective's source.
	// Validate it first. Once it is known, the other rows still need their layout boxes and all
	// authored transform tags for the final solve, but their exact glyph ink outlines do not
	// participate in the map. On a long multi-line text row those outlines dominate tool entry.
	AssDialogue *active_dialogue = auto_perspective ?
		c->selectionController->GetActiveLine() : nullptr;
	std::vector<Vector2D> active_outline;
	std::optional<TagLine> active_tags;
	bool active_selected = active_dialogue &&
		std::find(lines.begin(), lines.end(), active_dialogue) != lines.end();
	if (active_selected && IsDisplayed(active_dialogue)) {
		active_tags = ReadLine(active_dialogue);
		if (active_tags->drawing) {
			active_outline = ShapeOutline(*active_tags);
			if (SimplifyHull(ConvexHull(active_outline), 2.0).size() != 4)
				active_outline.clear();
		}
	}
	bool fast_active_reference = !active_outline.empty();
	for (auto line : lines) {
		if (!IsDisplayed(line)) continue;
		if (active_tags && line == active_dialogue)
			tag_lines.push_back(std::move(*active_tags));
		else
			tag_lines.push_back(ReadLine(line, !fast_active_reference));
	}
	if (tag_lines.empty()) return false;

	BuildBox(fast_active_reference ? &active_outline : nullptr);
	return true;
}

VisualToolTransform::TagLine VisualToolTransform::ReadLine(AssDialogue *line,
		bool measure_ink) {
	TagLine found;
	found.line = line;
	// \pos, \org, \an and \q belong to the line as a whole, so where they are said does
	// not matter; everything else does, and is read from the start of the line only.
	found.pos = GetLinePosition(line);
	// Which is where the run begins for a line that moves, rather than where the line is now. What
	// every one of these tools works on is the frame on screen.
	found.placed = text_to_shape::WherePlaced(c, line);
	if (found.placed.told) found.pos = found.placed.at;
	else found.placed.at = found.placed.first = found.placed.second = found.pos;
	found.org = GetLineOrigin(line);

	AssStyle const default_style;
	AssStyle const *style = c->ass->GetStyle(line->Style);
	if (!style) style = &default_style;
	auto blocks = line->ParseTags();

	found.scale = Vector2D(
		(float)FirstBlockNumber(blocks, "\\fscx", style->scalex),
		(float)FirstBlockNumber(blocks, "\\fscy", style->scaley));
	found.shear = Vector2D(
		(float)FirstBlockNumber(blocks, "\\fax", 0),
		(float)FirstBlockNumber(blocks, "\\fay", 0));
	found.bord = Vector2D(
		(float)FirstBlockNumber(blocks, "\\xbord",
			FirstBlockNumber(blocks, "\\bord", style->outline_w)),
		(float)FirstBlockNumber(blocks, "\\ybord",
			FirstBlockNumber(blocks, "\\bord", style->outline_w)));
	found.shad = Vector2D(
		(float)FirstBlockNumber(blocks, "\\xshad",
			FirstBlockNumber(blocks, "\\shad", style->shadow_w)),
		(float)FirstBlockNumber(blocks, "\\yshad",
			FirstBlockNumber(blocks, "\\shad", style->shadow_w)));
	found.blur = FirstBlockNumber(blocks, "\\blur", 0);
	found.be = FirstBlockNumber(blocks, "\\be", 0);
	found.clip = ReadClip(line);
	double wrap_style = TagNumber(line, "\\q", -1);
	found.has_wrap_style = wrap_style >= 0;
	bool has_q2 = false;
	for (auto& block : line->ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags)
			if (tag.Name == "\\q" && !tag.Params.empty() &&
				std::abs(tag.Params[0].Get<double>(-1) - 2) <= 1e-6)
				has_q2 = true;
	}
	for (auto& block : line->ParseTags())
		if (block->GetType() == AssBlockType::DRAWING) found.drawing = true;
	std::string stripped_text = line->GetStrippedText();
	found.ensure_q2 = TagsMode() && !stripped_text.empty() &&
		!has_q2 && HasAtLeastTwoWords(stripped_text);
	found.angle = (float)FirstBlockNumber(blocks, "\\frz",
		FirstBlockNumber(blocks, "\\fr", style->angle));
	found.angle_x = FirstBlockNumber(blocks, "\\frx", 0);
	found.angle_y = FirstBlockNumber(blocks, "\\fry", 0);
	found.align = GetLineAlignment(line);

	// For text the extents start at zero, but a drawing's own coordinates can start
	// anywhere - and the renderer draws it there, so where its ink begins is part of
	// where its box is. Leaving that out is what blew the box up on a line that had
	// already become a shape.
	auto extents = GetLineBaseExtents(line);
	found.box_first = extents.first;
	found.box_second = extents.second;
	// What the letters fill inside that cell. A font and a walk along every letter, so it is
	// done here - once per line - rather than wherever the frame happens to be wanted.
	found.has_ink = measure_ink &&
		text_to_shape::MeasureInk(c, line, found.ink_first, found.ink_second);
	Vector2D size = extents.second - extents.first;
	found.size = Vector2D(size.X() * found.scale.X() / 100.f,
	                      size.Y() * found.scale.Y() / 100.f);
	found.ink = Vector2D(extents.first.X() * found.scale.X() / 100.f,
	                     extents.first.Y() * found.scale.Y() / 100.f);

	// Whether the margins are breaking this line as it stands. Only measured, not changed:
	// what it decides is whether the wrapping can safely be switched off.
	{
		int script_w = 0, script_h = 0;
		c->ass->GetResolution(script_w, script_h);
		auto margin = line->Margin;
		if (AssStyle *style = c->ass->GetStyle(line->Style))
			for (int i = 0; i < 3; ++i)
				if (margin[i] == 0) margin[i] = style->Margin[i];
		double room = std::max(0, script_w - margin[0] - margin[1]);
		found.wrapped = room > 0 && found.size.X() > room + 1;
	}

	found.start_pos = found.pos;
	found.start_org = found.org;
	found.start_scale = found.scale;
	found.start_shear = found.shear;
	found.start_bord = found.bord;
	found.start_shad = found.shad;
	found.start_blur = found.blur;
	found.start_be = found.be;
	found.start_angle = found.angle;
	return found;
}

std::vector<VisualToolTransform::TagLine> const& VisualToolTransform::Lines() const {
	return shear_split ? split_lines : tag_lines;
}

void VisualToolTransform::EnsureShearSplit() {
	if (!TagsMode() || shear_split) return;

	if (!split_built) {
		split_built = true;
		bool any = false;
		for (auto& found : tag_lines) {
			auto pieces = text_to_shape::SplitForShear(c, found.line);
			if (pieces.empty()) {
				// Nothing to gain, or nothing that could be put back exactly. Either way the
				// line goes on as itself.
				split_lines.push_back(found);
				continue;
			}
			found.replaced = true;
			any = true;
			for (auto const& piece : pieces) {
				auto owned = std::make_shared<AssDialogue>(*found.line);
				owned->Text = piece.text;
				TagLine read = ReadLine(owned.get());
				read.owned = std::move(owned);
				read.source = found.line;
				split_lines.push_back(std::move(read));
			}
		}
		// A selection where every line stands on its own already is left alone: breaking it
		// up would add lines to the file for nothing.
		if (!any) split_lines.clear();
	}

	if (!split_lines.empty()) shear_split = true;
}

void VisualToolTransform::EnsureDecor() {
	if (!TagsMode()) return;
	for (auto *set : {&tag_lines, &split_lines})
		for (auto& found : *set) {
			if (found.decor_built) continue;
			found.decor_built = true;
			found.decor = text_to_shape::BakeDecorations(c, found.line);
		}
}

std::pair<Vector2D, Vector2D> VisualToolTransform::MovedEnds(TagLine const& original,
	Applied const& applied) const {
	if (!original.placed.moving) return {applied.pos, applied.pos};
	if (auto_perspective)
		return {MapPoint(original.placed.first), MapPoint(original.placed.second)};

	// The gesture's own map, applied to each end from where the line stood. On the frame on screen
	// the two ends mix back to exactly the point the handle was let go at, whatever the map was,
	// because that is the point they were measured from.
	Matrix2 whole = TotalMatrix();
	if (!original.org)
		return {applied.pos + ApplyMatrix(whole, original.placed.first - original.start_pos),
		        applied.pos + ApplyMatrix(whole, original.placed.second - original.start_pos)};

	// With an \org the turn carries the anchor as well, so where the line runs on screen is its
	// \pos run turned about \org - and it is the run on screen that the gesture acts on. So each
	// end is taken there, moved, and turned back into what \move has to say.
	auto about = [](Vector2D point, Vector2D centre, double degrees) {
		return centre + ApplyMatrix(Turn(degrees), point - centre);
	};
	Vector2D was_centre = original.start_org ? original.start_org : original.start_pos;
	Vector2D now_centre = applied.org ? applied.org : applied.pos;
	Vector2D was_shown = about(original.start_pos, was_centre, original.start_angle);
	Vector2D now_shown = about(applied.pos, now_centre, applied.angle);

	auto taken = [&](Vector2D end) {
		Vector2D shown = about(end, was_centre, original.start_angle);
		Vector2D landed = now_shown + ApplyMatrix(whole, shown - was_shown);
		return about(landed, now_centre, -applied.angle);
	};
	return {taken(original.placed.first), taken(original.placed.second)};
}

bool VisualToolTransform::HasDecor() const {
	for (auto const& found : Lines())
		for (auto const& decor : found.decor)
			if (decor.has_border || decor.has_shadow) return true;
	return false;
}

std::string VisualToolTransform::DecorLineText(TagLine const& original,
	text_to_shape::Decoration const& decor, bool shadow) const {
	auto *tool = const_cast<VisualToolTransform *>(this);

	// The same tags the letters get, so that what comes out is scaled, leaned and turned exactly
	// as they are. What differs is where it hangs from, what paints it, and whether it is a shape.
	AssDialogue copy(*original.line);
	copy.Text = TagLineText(original);

	// A shadow with no border is the letters again, moved - so it stays letters. Sharper than any
	// polygon of them, a great deal smaller, and exactly right. With a border it has to be the
	// widened shape instead, or the shadow would be thinner than what it stands behind.
	bool as_shape = decor.has_border;

	Applied applied = ApplyGesture(original);
	// The corner of the stretch, which is what the shape hangs from. It was measured with the
	// line's own scale in it, so it follows that scale changing.
	auto followed = [](double now, double was) {
		return std::abs(was) > 1e-6 ? (float)(now / was) : 1.f;
	};
	Vector2D lean(decor.lean.X() * followed(applied.scale.X(), original.start_scale.X()),
	              decor.lean.Y() * followed(applied.scale.Y(), original.start_scale.Y()));

	// Read before the turn, the way the renderer reads a glyph's own place in the line - which
	// only works if the shape turns about the same point as the letters. A line with no \org turns
	// about its own \pos, and the shape's \pos is not the line's, so the centre is written out.
	// The corner is where a shape hangs from; letters hang from their own alignment, which the
	// line already says, so they are only moved.
	Vector2D shift = as_shape ? lean : Vector2D(0.f, 0.f);
	// The shadow is the same shape moved the way the renderer moves it: it adds the offset to the
	// glyph's own shift, which rides in the translation column of the transform - so the turn
	// reaches it, and neither the scale nor the lean does. Which is why it is added here, before
	// the turn, and kept exactly as the line asked for it.
	if (shadow) shift = shift + original.shad;

	// Both ends of the run, if the line is on one, each carrying the same offset.
	auto [first_end, second_end] = MovedEnds(original, applied);

	// Moving \pos moves what a line with no \org turns about, and turning about another point is
	// not the same turn - so the centre the letters use is written out either way.
	tool->SetOverride(&copy, "\\org", (original.org ? applied.org : applied.pos).PStr());
	if (as_shape) tool->SetOverride(&copy, "\\an", "7");
	{
		auto [name, value] = text_to_shape::PlacementOverride(original.placed,
			first_end + shift, second_end + shift);
		tool->SetOverride(&copy, name, value);
	}
	tool->SetOverride(&copy, "\\bord", "0");
	tool->SetOverride(&copy, "\\shad", "0");

	std::string paint = shadow ? decor.shadow_paint : decor.border_paint;
	std::string whole = copy.Text.get();
	size_t close = whole.find('}');
	if (close == std::string::npos) whole = "{}" + whole, close = 1;

	// The letters again, in the other colour: everything the line said stays, and what paints them
	// goes at the end of the first block so that nothing said there can undo it.
	if (!as_shape) {
		whole.insert(close, paint);
		return whole;
	}

	// Or the shape, and then only the first block is kept - which is where everything in force at
	// the stretch is said - with the shape in place of the words.
	std::string head = whole.substr(0, close + 1);
	head.insert(head.size() - 1, paint + "\\p1");
	std::string const& body = decor.bordered.empty() ? decor.letters : decor.bordered;
	return head + body + "{\\p0}";
}

void VisualToolTransform::LineCorners(TagLine const& original, Vector2D corners[4]) const {
	// A line turned out of the plane, or leaning, does not sit on screen as a rectangle - so
	// what has to be gone round is its four corners. Its unleaned, unturned box is somewhere
	// else entirely.
	if (Skewed(original)) {
		LineQuad(original, corners);
		return;
	}

	Vector2D fractions = AnchorFractions(original.align);
	Vector2D top_left = original.pos + original.ink -
		Vector2D(original.size.X() * fractions.X(), original.size.Y() * fractions.Y());
	Vector2D pivot = original.org ? original.org : original.pos;
	for (int corner = 0; corner < 4; ++corner) {
		Vector2D at = top_left +
			Vector2D(corner == 1 || corner == 2 ? original.size.X() : 0.f,
			         corner >= 2 ? original.size.Y() : 0.f);
		corners[corner] = RotateAbout(at, pivot, original.angle);
	}
}

void VisualToolTransform::BuildBox(
		std::vector<Vector2D> const *known_active_outline) {
	if (tag_lines.empty()) return;

	// The box lies the way the text lies, which only means something if the lines agree
	// about it - and if they do, a scale along the box is exactly \fscx and \fscy.
	//
	// A box says where its own x axis points as an angle clockwise on screen, and \frz turns
	// the other way, so the box takes the opposite sign. Reading it the same way round is
	// what used to put the frame at twice the text's angle: a rectangle nothing was lying in.
	float angle = tag_lines.front().angle;
	for (auto const& found : tag_lines)
		if (std::abs(found.angle - angle) > .01f) { angle = 0; break; }
	angle = -angle;

	typesetting::OrientedBox frame;
	frame.angle = angle;
	frame.centre = Vector2D(0.f, 0.f);

	std::vector<Vector2D> outline;
	outline.reserve(tag_lines.size() * 4);
	std::vector<Vector2D> active_outline;
	AssDialogue *active_dialogue = auto_perspective ?
		c->selectionController->GetActiveLine() : nullptr;
	Vector2D low, high;
	bool first = true;
	// The free transform measures its box flat - no lean, nothing out of the plane - because
	// that is the one space where a scale is \fscx and a lean is one number. The shape the
	// selection really has is put back afterwards, as the frame the handles sit on. A
	// distortion has no such split: its four corners *are* the shape, so it measures them.
	bool measure_flat = mode == VisualToolTransformMode::Free;
	for (auto& found : tag_lines) {
		// A shape is measured as the shape it is, not as the rectangle around it. A sign mask
		// leaning a few degrees has a triangle of air at every corner of its upright box, and a
		// box drawn round that air is not the box the mask is in - which is what made the frame
		// on a rotated logo cover most of the frame instead of the logo.
		std::vector<Vector2D> points;
		if (found.drawing) {
			if (found.line == active_dialogue && known_active_outline && !measure_flat)
				points = *known_active_outline;
			else
				points = ShapeOutline(found, measure_flat);
		}
		// Auto perspective may use the active quadrilateral as its source. Keep only a real
		// drawing outline from this pass; the fallback box below must not turn a malformed or
		// empty drawing into a reference quadrilateral.
		if (found.line == active_dialogue && found.drawing && !points.empty())
			active_outline = points;
		if (points.empty()) {
			// The letters where they can be measured, and the cell they sit in where they
			// cannot. A cell is the same height whatever is set in it, so a frame round one is
			// a frame with a band of nothing along the top - and under a scale of two hundred
			// percent that band is twice as deep as it reads on the page.
			Vector2D corners[4];
			bool got = measure_flat ? UnskewedCorners(found, corners) :
				InkCorners(found, corners);
			if (!got) LineCorners(found, corners);
			points.assign(corners, corners + 4);
		}
		for (auto const& corner : points) {
			outline.push_back(corner);
			Vector2D local = frame.ToLocal(corner);
			if (first) { low = high = local; first = false; }
			else { low = low.Min(local); high = high.Max(local); }
		}
	}

	box.angle = angle;
	box.centre = frame.ToScript((low + high) / 2);
	box.half = (high - low) / 2;

	// Text turned out of the plane stands on screen as a trapezium rather than as a rectangle,
	// and lines that disagree about their turn have no common one to measure along. Either way
	// the frame above holds a good deal of air, and the box drawn around the text is visibly
	// not the box the text is in. The smallest rectangle that holds the whole outline is, so it
	// is taken whenever it is meaningfully tighter - and read the way round the text lies, so
	// that the two scales go on meaning what they meant.
	// Both the tight control box and the authored selection quadrilateral need this same hull.
	// Calculating it once is important for drawings: their outlines can contain tens of
	// thousands of points, while the hull that remains is normally tiny.
	std::vector<Vector2D> outline_hull = ConvexHull(outline);
	typesetting::OrientedBox tight;
	double straight = 4.0 * (double)box.half.X() * box.half.Y();
	if (MinimumAreaBox(SimplifyHull(outline_hull, 1.0), tight) &&
		(!(straight > 0) || 4.0 * tight.half.X() * tight.half.Y() < straight * .98)) {
		FaceBox(tight, angle);
		box = tight;
	}

	shear_frame_angle = box.angle;
	// A box with no size has no handles worth dragging.
	box.half = box.half.Max(Vector2D(4.f, 4.f));

	// A frame larger than the video cannot be grabbed - its handles end up off screen, and only
	// zooming right out brings them back. It is a control frame and nothing more: the lines
	// follow the map it describes whatever size it is, so it is kept inside the script.
	//
	// Auto perspective has no such handles: the target is the quadrilateral that was drawn, and
	// this box is only the rectangle that is mapped onto it. Trimming it there would trim the
	// source of the map instead of a control, which squeezes and shifts the result - and a mask
	// large enough to reach the edge of the frame is exactly the case where it happened.
	//
	// And not for the free transform at all any more: its frame is the shape the selection
	// really has, so moving the frame moves it off the shape. On a mask that begins at a
	// negative x this shoved it a hundred and fifty-eight pixels to the right of the thing it
	// was drawn round. The handles of a frame that reaches past the edge of the script can
	// still be reached in the letterbox, or by zooming out, which is the smaller trouble.
	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);
	if (mode != VisualToolTransformMode::Free &&
		script_w > 0 && script_h > 0) {
		// A little inside the edge, so the handles on the far side are never flush against it.
		const float inset = 20.f;
		box.half = box.half.Min(Vector2D(script_w * .5f - inset, script_h * .5f - inset));
		box.half = box.half.Max(Vector2D(4.f, 4.f));
		box.centre = box.centre.Max(box.half + Vector2D(inset, inset)).Min(
			Vector2D(script_w - inset, script_h - inset) - box.half);
	}

	BuildFrameShape(nullptr, &outline_hull);
	BuildAutoPerspectiveBox(active_outline.empty() ? nullptr : &active_outline);
}

void VisualToolTransform::BuildFrameShape(std::vector<TagLine> const *given,
		std::vector<Vector2D> const *selection_hull) {
	if (!given) {
		frame_shape = typesetting::PointMap();
		rest_to_box = typesetting::PointMap();
		box.Corners(frame_quad);
		// Distort and Auto perspective both work from this, so their target corners say the
		// change from the visible shape the selection already rests in.
		if (!(mode == VisualToolTransformMode::Free ||
			mode == VisualToolTransformMode::Distort) ||
			TextBoxMode()) return;
	}
	std::vector<TagLine> const& lines = given ? *given : tag_lines;
	if (lines.empty()) return;

	// Nothing out of the ordinary anywhere: the box was measured flat and flat is what it is.
	// Asked before anything is gathered, because gathering means placing every drawing of the
	// selection - tens of thousands of points on a logo - to answer a question already settled.
	bool skewed = false;
	for (auto const& line : lines) if (Skewed(line)) { skewed = true; break; }
	if (!skewed) return;

	std::vector<Vector2D> hull;
	if (selection_hull) {
		hull = SimplifyHull(*selection_hull, 2.0);
	}
	else {
		// Everything that is drawn, where it is really drawn. This path is used by the
		// converted-drawing editor, which has no BuildBox geometry available to share.
		std::vector<Vector2D> drawn;
		for (auto const& line : lines) {
			std::vector<Vector2D> points;
			if (line.drawing) points = ShapeOutline(line);
			if (points.empty()) {
				Vector2D quad[4];
				if (!InkCorners(line, quad)) LineCorners(line, quad);
				points.assign(quad, quad + 4);
			}
			drawn.insert(drawn.end(), points.begin(), points.end());
		}
		if (drawn.size() < 3) return;
		hull = SimplifyHull(ConvexHull(drawn), 2.0);
	}
	if (hull.size() < 3) return;

	// The exact answer where there is one: the tightest quadrilateral round the lot. A shape's
	// own four corners, a line's own four corners, and the four corners of rows that lean and
	// turn together - all of those come out as four points.
	if (hull.size() == 4 && SetFrameQuad(hull)) return;

	// Otherwise the smallest rectangle that holds it - but fitted where the selection does not
	// lean, and leaned back afterwards, which makes it a parallelogram.
	//
	// A block of leaning rows *is* a parallelogram, and the smallest rectangle round one carries
	// a great deal of air at two of its corners: on a paragraph of sixteen rows leaning by one
	// the rectangle came to a hundred and forty-seven per cent of what is drawn where the
	// parallelogram comes to a hundred and twenty-nine - three hundred script pixels of overhang
	// at either end.
	TagLine const *found = given ? nullptr : ActiveTagLine();
	if (!found) found = &lines.front();
	Vector2D flat[4], real[4];
	if (UnskewedCorners(*found, flat)) {
		if (!InkCorners(*found, real)) LineCorners(*found, real);
		// The lean the line stands at, and the way back out of it. Both are read off the same
		// line, so whichever way the rest of the selection leans this is one consistent pair.
		auto out_of_lean = typesetting::QuadMap(box, flat);
		auto into_lean = typesetting::QuadMap(box, real);
		auto from_real = typesetting::QuadInverseMap(box, real);
		auto from_flat = typesetting::QuadInverseMap(box, flat);
		if (out_of_lean && into_lean && from_real && from_flat) {
			std::vector<Vector2D> straightened;
			straightened.reserve(hull.size());
			bool ok = true;
			for (auto const& point : hull) {
				Vector2D at = out_of_lean(from_real(point));
				if (!std::isfinite(at.X()) || !std::isfinite(at.Y())) { ok = false; break; }
				straightened.push_back(at);
			}
			typesetting::OrientedBox upright;
			if (ok && MinimumAreaBox(straightened, upright)) {
				FaceBox(upright, box.angle);
				upright.half = upright.half.Max(Vector2D(4.f, 4.f));
				Vector2D flat_corners[4], leaned[4];
				upright.Corners(flat_corners);
				bool finite = true;
				for (int i = 0; i < 4; ++i) {
					leaned[i] = into_lean(from_flat(flat_corners[i]));
					finite = finite && std::isfinite(leaned[i].X()) &&
						std::isfinite(leaned[i].Y());
				}
				if (finite && SetFrameQuadInOrder(leaned)) return;
			}
		}
	}

	// And if even that cannot be had, the smallest rectangle round the lot. Looser, but it
	// still sits on what is drawn - which the box, measured flat, would not.
	typesetting::OrientedBox tight;
	if (!MinimumAreaBox(hull, tight)) return;
	FaceBox(tight, box.angle);
	tight.half = tight.half.Max(Vector2D(4.f, 4.f));
	Vector2D corners[4];
	tight.Corners(corners);
	SetFrameQuadInOrder(corners);
}

bool VisualToolTransform::SetFrameQuad(std::vector<Vector2D> const& quad) {
	if (quad.size() != 4) return false;

	// In the order the box uses, so that a corner of the frame and a corner of the box mean
	// the same corner of the picture - and the handles keep meaning what they say.
	//
	// The ring is kept whole and only turned round and read the other way: eight readings, and
	// the one that sits closest to the box wins. Pairing each corner off with whichever box
	// corner happens to be nearest it was tried and is a trap - it can pair two of them
	// crosswise, and then the frame is a bow tie. Which is what it drew as under a distort,
	// where the box is measured from the real geometry and on strongly leaning text its long
	// side can lie along the lean rather than along the words.
	Vector2D corners[4], ordered[4];
	box.Corners(corners);
	double best = 0;
	bool found = false;
	for (int backwards = 0; backwards < 2; ++backwards) {
		for (int turn = 0; turn < 4; ++turn) {
			Vector2D reading[4];
			double away = 0;
			for (int i = 0; i < 4; ++i) {
				reading[i] = quad[backwards ? (turn - i + 4) % 4 : (turn + i) % 4];
				Vector2D from = reading[i] - corners[i];
				away += (double)from.X() * from.X() + (double)from.Y() * from.Y();
			}
			if (found && !(away < best)) continue;
			found = true;
			best = away;
			std::copy(reading, reading + 4, ordered);
		}
	}
	return found && SetFrameQuadInOrder(ordered);
}

bool VisualToolTransform::SetFrameQuadInOrder(Vector2D const quad[4]) {
	// A quadrilateral that has folded over itself is no frame at all, and neither is one whose
	// corners have ended up paired crosswise: that one has area and would pass a bare area test,
	// but it draws as a bow tie and every handle on it means the wrong corner. So: convex, and
	// the same way round as the box.
	Vector2D reference[4];
	box.Corners(reference);
	double twice_area = 0, box_area = 0;
	for (int i = 0; i < 4; ++i) {
		twice_area += quad[i].X() * quad[(i + 1) % 4].Y() -
			quad[(i + 1) % 4].X() * quad[i].Y();
		box_area += reference[i].X() * reference[(i + 1) % 4].Y() -
			reference[(i + 1) % 4].X() * reference[i].Y();
	}
	if (!(std::abs(twice_area) > 1.0)) return false;
	if (twice_area * box_area < 0) return false;
	for (int i = 0; i < 4; ++i) {
		Vector2D edge = quad[(i + 1) % 4] - quad[i];
		Vector2D onwards = quad[(i + 2) % 4] - quad[(i + 1) % 4];
		double turn = (double)edge.X() * onwards.Y() - (double)edge.Y() * onwards.X();
		if (turn * twice_area <= 0) return false;
	}

	auto shape = typesetting::QuadMap(box, quad);
	auto back = typesetting::QuadInverseMap(box, quad);
	if (!shape || !back) return false;
	Vector2D corners[4];
	box.Corners(corners);
	for (int i = 0; i < 4; ++i) {
		Vector2D at = shape(corners[i]);
		if (!std::isfinite(at.X()) || !std::isfinite(at.Y())) return false;
		Vector2D there_and_back = back(at);
		if (!std::isfinite(there_and_back.X()) || !std::isfinite(there_and_back.Y()))
			return false;
	}
	frame_shape = shape;
	rest_to_box = back;
	std::copy(quad, quad + 4, frame_quad);
	return true;
}

void VisualToolTransform::BuildEditorFrameShape() {
	frame_shape = typesetting::PointMap();
	rest_to_box = typesetting::PointMap();
	box.Corners(frame_quad);
	if (!editor) return;

	// The lines still say what they said - the editor works from copies of them - so where the
	// text really sits is read out of their tags, exactly, projection and all.
	//
	// And only out of the tags. Fitting a shape to the letters instead was tried and taken back
	// out: a shear costs no area, so the tightest parallelogram round any irregular set of points
	// is always somewhat oblique, and on a logo built of drawings - which say nothing about how
	// they were placed, having their shape in their own coordinates - it would lean far enough to
	// reach an outlying piece and come out nearly singular. A hairline thirty pixels long then
	// spanned the whole patch on its own and blew up to twelve hundred. A selection that says
	// nothing about leaning is left in its box, which is what it was in before any of this.
	std::vector<TagLine> read;
	for (auto line : editor->lines()) {
		if (!IsDisplayed(line)) continue;
		read.push_back(ReadLine(line));
	}
	if (!read.empty()) BuildFrameShape(&read);
}

VisualToolTransform::TagLine const *VisualToolTransform::ActiveTagLine() const {
	AssDialogue *active = c->selectionController->GetActiveLine();
	auto found = std::find_if(tag_lines.begin(), tag_lines.end(),
		[&](TagLine const& line) { return line.line == active; });
	return found == tag_lines.end() ? nullptr : &*found;
}

void VisualToolTransform::BuildAutoPerspectiveBox(
		std::vector<Vector2D> const *active_outline) {
	if (!auto_perspective) return;
	if (source_moved) return;

	auto_perspective_active_reference = false;
	TagLine const *active = ActiveTagLine();
	if (!active || !active->drawing) {
		std::copy(frame_quad, frame_quad + 4, source_corners);
		return;
	}

	// Only an actual quadrilateral drawing may stand in for the whole selection. Text and
	// every other drawing use the complete selection frame, exactly as Distort does.
	std::vector<Vector2D> source = active_outline ?
		SimplifyHull(ConvexHull(*active_outline), 2.0) :
		SimplifyHull(ConvexHull(ShapeOutline(*active)), 2.0);
	if (source.size() != 4) {
		std::copy(frame_quad, frame_quad + 4, source_corners);
		return;
	}
	auto_perspective_active_reference = true;

	// Match the active row's ring to the control box without crossing it. This preserves the
	// corner meaning used by the four directed target points, regardless of which way the
	// active row is rotated or projected.
	Vector2D reference[4], ordered[4];
	box.Corners(reference);
	double best = 0;
	bool found = false;
	for (int backwards = 0; backwards < 2; ++backwards) {
		for (int turn = 0; turn < 4; ++turn) {
			Vector2D reading[4];
			double away = 0;
			for (int i = 0; i < 4; ++i) {
				reading[i] = source[backwards ? (turn - i + 4) % 4 : (turn + i) % 4];
				Vector2D delta = reading[i] - reference[i];
				away += (double)delta.X() * delta.X() + (double)delta.Y() * delta.Y();
			}
			if (found && !(away < best)) continue;
			found = true;
			best = away;
			std::copy(reading, reading + 4, ordered);
		}
	}
	if (found) std::copy(ordered, ordered + 4, source_corners);
	else std::copy(frame_quad, frame_quad + 4, source_corners);
}

VisualDraggableFeature *VisualToolTransform::FeatureAt(size_t index) {
	size_t at = 0;
	for (auto& feature : features)
		if (at++ == index) return &feature;
	return nullptr;
}

std::vector<Vector2D> VisualToolTransform::ShapeOutline(TagLine const& found, bool unskewed) {
	std::vector<Vector2D> outline = GetLineDrawingPoints(found.line);
	if (outline.empty()) return outline;

	// Where the renderer really puts them, worked out by the same placement the conversion
	// uses: the scale, what the alignment does to the box, both leans, the two turns out of
	// the plane and the projection that follows them.
	//
	// Worked out by hand below this stopped at \frz, so a shape leaning out of the plane was
	// measured where it would have been had it lain flat - on a sign mask at \frx46 \fry-53 that
	// put every corner between seventy and two hundred and seventy pixels from the shape, and
	// the frame nowhere near it.
	auto placed = unskewed ? text_to_shape::DrawingPlace() :
		text_to_shape::PlaceDrawing(c, found.line, found.box_first, found.box_second);
	if (placed.ok) {
		for (auto& point : outline) point = placed.map(point);
		return outline;
	}

	// A line that animates what would have to be taken out of the numbers is better read as it
	// was written: the scale, the alignment and the turn, which is all this can be sure of.
	Vector2D fractions = AnchorFractions(found.align);
	Vector2D shift(found.size.X() * fractions.X(), found.size.Y() * fractions.Y());
	Vector2D pivot = found.org ? found.org : found.pos;
	for (auto& point : outline)
		point = RotateAbout(found.pos - shift +
			Vector2D(point.X() * found.scale.X() / 100.f,
			         point.Y() * found.scale.Y() / 100.f),
			pivot, found.angle);
	return outline;
}

typesetting::PointMap VisualToolTransform::DistortionMap(Vector2D const target[4],
		typesetting::PointMap inverse) const {
	auto forward = typesetting::QuadMap(box, target);
	if (!forward || !inverse) return forward;
	return [inverse, forward](Vector2D point) { return forward(inverse(point)); };
}

typesetting::PointMap VisualToolTransform::AutoPerspectiveMap() const {
	Vector2D original_size_target[4];
	Vector2D const *target = corners;
	if (touched && auto_perspective_keep_original_size &&
		AutoPerspectiveOriginalSizeTarget(original_size_target))
		target = original_size_target;

	// Until the four points are drawn there is no target, and the source must not act on its
	// own: it would distort the lines before anything had been asked of it.
	if (!touched) return typesetting::QuadMap(box, target);

	// Without a quadrilateral active reference this is literally Distort's selection inverse.
	// A valid active quadrilateral (or manually adjusted source handles) replaces only that
	// inverse; the shared projective solver and its application to all rows stay unchanged.
	auto inverse = auto_perspective_active_reference || source_moved ?
		typesetting::QuadInverseMap(box, source_corners) : rest_to_box;
	return DistortionMap(target, inverse);
}

bool VisualToolTransform::AutoPerspectiveOriginalSizeTarget(Vector2D target[4]) const {
	// The target quadrilateral describes a plane, not a box the selection has to fit inside.
	// Solve that plane against a centred reference rectangle with the selection's dimensions,
	// then remove only its planar scale. Projecting the same-sized reference back through the
	// solved rotations and shear gives a common target for the whole selection: line sizes,
	// distances between rows and their relative positions all remain authored.
	//
	// The percentage sizes that reference rather than being ruled out by the lock. Keeping the
	// authored proportions and choosing how large the whole of it is are two different
	// questions, and only the first of them is what the lock answers.
	double factor = uniform_size / 100.0;
	double width = std::max<double>(box.half.X() * 2.f * factor, 1.0);
	double height = std::max<double>(box.half.Y() * 2.f * factor, 1.0);
	Vector2D first(0.f, 0.f);
	Vector2D second(static_cast<float>(width), static_cast<float>(height));
	auto plane = typesetting::SolvePerspective(corners, 5, first, second,
		script_res / layout_res, Vector2D());
	if (!plane.ok || std::abs(plane.scale.Y()) <= 1e-9) return false;

	// \fax is applied before the two scales. Preserve the actual in-plane shear rather than
	// its scale-dependent ASS number while both axes are returned to one hundred percent.
	plane.shear_x *= plane.scale.X() / plane.scale.Y();
	plane.scale = Vector2D(100.f, 100.f);
	typesetting::PerspectiveQuad(plane, 5, first, second,
		script_res / layout_res, target);

	// "Centred" on a perspective plane means the image of its centre: the intersection of
	// the diagonals, not the arithmetic average (which drifts towards the wider/nearer edge).
	typesetting::OrientedBox reference;
	reference.centre = Vector2D();
	reference.half = box.half;
	Vector2D wanted_centre = typesetting::QuadMap(box, corners)(box.centre);
	Vector2D kept_centre = typesetting::QuadMap(reference, target)(reference.centre);
	Vector2D shift = wanted_centre - kept_centre;
	for (int i = 0; i < 4; ++i) {
		target[i] = target[i] + shift;
		if (!std::isfinite(target[i].X()) || !std::isfinite(target[i].Y())) return false;
	}
	return true;
}

namespace {
	/// A map done in the frame of a box lying at some angle: into that frame, the map, and
	/// back out again.
	Matrix2 InFrame(float box_angle, Matrix2 const& map) {
		double radians = box_angle * pi / 180.0;
		double sine = std::sin(radians), cosine = std::cos(radians);
		Matrix2 into{cosine, sine, -sine, cosine};
		Matrix2 out_of{cosine, -sine, sine, cosine};
		return Multiply(out_of, Multiply(map, into));
	}

	/// The gesture as a linear map: the scale along the box's axes, then the turn, then the
	/// lean - in the order they are applied to what is on screen.
	Matrix2 GestureMatrix(float box_angle, Vector2D scale, float turn,
	                      float shear_angle, Vector2D shear) {
		Matrix2 scaled{scale.X(), 0, 0, scale.Y()};
		Matrix2 leaning{1, shear.X(), shear.Y(), 1};
		return Multiply(InFrame(shear_angle, leaning),
			Multiply(Turn(turn), InFrame(box_angle, scaled)));
	}
}

Vector2D VisualToolTransform::ShapePoint(Vector2D box_local) const {
	Vector2D at = box.ToScript(box_local);
	if (!frame_shape) return at;
	Vector2D shaped = frame_shape(at);
	return std::isfinite(shaped.X()) && std::isfinite(shaped.Y()) ? shaped : at;
}

Vector2D VisualToolTransform::FrameOrigin() const {
	return ShapePoint(Vector2D(0.f, 0.f));
}

Vector2D VisualToolTransform::GesturePivot() const {
	// The middle of the frame as it was when this gesture began. Deliberately not the middle
	// of the *scaled* frame: that would depend on the scale, the scale is measured against a
	// point this pivot places, and the two would chase each other.
	return FrameOrigin() + gesture_move;
}

namespace {
	/// How far the renderer shifts a line sideways for its lean.
	///
	/// It leans text about the top of its box rather than about the point it is anchored at,
	/// and the amount goes with \fscx - libass multiplies \fax by scale_x/scale_y and then
	/// applies it to coordinates that have been scaled by scale_y. A drawing leans about the
	/// point it sits at, so for one of those this is nothing.
	Vector2D LeanOffset(double ascent, double shear_x, double scale_x, double angle) {
		double reach = ascent * shear_x * scale_x / 100.0;
		if (std::abs(reach) < 1e-9) return Vector2D(0.f, 0.f);
		double radians = angle * pi / 180.0;
		return Vector2D((float)(reach * std::cos(radians)), (float)(-reach * std::sin(radians)));
	}
}

TransformMatrix2 VisualToolTransform::TotalMatrix() const {
	return Multiply(frame_linear, GestureMatrix(box.angle, gesture_scale, gesture_angle,
		shear_frame_angle, gesture_shear));
}

bool VisualToolTransform::Perspective(TagLine const& original) {
	return std::abs(original.angle_x) > 1e-9 || std::abs(original.angle_y) > 1e-9;
}

bool VisualToolTransform::Skewed(TagLine const& original) {
	return Perspective(original) || std::abs(original.shear.X()) > 1e-9 ||
		std::abs(original.shear.Y()) > 1e-9;
}

void VisualToolTransform::LineQuad(TagLine const& original, Vector2D corners[4]) const {
	LineQuad(original, original.box_first, original.box_second, corners);
}

void VisualToolTransform::LineQuad(TagLine const& original, Vector2D first, Vector2D second,
                                   Vector2D corners[4]) const {
	typesetting::PerspectiveTags tags;
	tags.pos = original.pos;
	tags.org = original.org ? original.org : original.pos;
	tags.scale = original.scale;
	tags.shear_x = original.shear.X();
	tags.shear_y = original.shear.Y();
	tags.angle_z = original.angle;
	tags.angle_x = original.angle_x;
	tags.angle_y = original.angle_y;
	typesetting::PerspectiveQuad(tags, original.align, first, second,
		script_res / layout_res, corners);
}

void VisualToolTransform::FrameExtents(TagLine const& original, Vector2D& first,
                                      Vector2D& second) const {
	first = original.box_first;
	second = original.box_second;
	if (!original.has_ink) return;
	Vector2D cell = original.box_second - original.box_first;
	Vector2D size = original.ink_second - original.ink_first;
	if (!(size.X() > 1e-3f) || !(size.Y() > 1e-3f)) return;

	// The renderer hangs the cell by the alignment and the letters sit somewhere inside it.
	// Taking that difference off here lets everything downstream go on hanging the box it is
	// given by the same alignment, and still land on the letters.
	Vector2D fractions = AnchorFractions(original.align);
	first = original.ink_first -
		Vector2D((cell.X() - size.X()) * fractions.X(),
		         (cell.Y() - size.Y()) * fractions.Y());
	second = first + size;
}

bool VisualToolTransform::InkCorners(TagLine const& original, Vector2D corners[4]) const {
	if (!original.has_ink) return false;
	Vector2D first, second;
	FrameExtents(original, first, second);
	Vector2D size = second - first;
	if (!(size.X() > 1e-3f) || !(size.Y() > 1e-3f)) return false;
	LineQuad(original, first, second, corners);
	return true;
}

bool VisualToolTransform::UnskewedCorners(TagLine const& original, Vector2D corners[4]) const {
	Vector2D first, second;
	FrameExtents(original, first, second);
	if (!(second.X() - first.X() > 1e-3f) || !(second.Y() - first.Y() > 1e-3f)) return false;

	// The same placement the renderer would give it with neither lean nor turn out of the
	// plane. Written through the same projection as everything else so that the two shapes -
	// this one and the real one - differ by exactly one map, which is the frame's shape.
	typesetting::PerspectiveTags tags;
	tags.pos = original.pos;
	tags.org = original.org ? original.org : original.pos;
	tags.scale = original.scale;
	tags.shear_x = 0;
	tags.shear_y = 0;
	tags.angle_z = original.angle;
	tags.angle_x = 0;
	tags.angle_y = 0;
	typesetting::PerspectiveQuad(tags, original.align, first, second,
		script_res / layout_res, corners);
	for (int i = 0; i < 4; ++i)
		if (!std::isfinite(corners[i].X()) || !std::isfinite(corners[i].Y())) return false;
	return true;
}

bool VisualToolTransform::PerspectiveMovePlane(Vector2D const held_corners[4],
	typesetting::OrientedBox& source, Vector2D projected[4]) const {
	// The distortion is applied after the renderer's authored perspective. Compose the current
	// distortion onto a representative line plane, so moving through it works both before and
	// after a corner of the distortion frame has been changed.
	auto held_quad = typesetting::QuadMap(box, held_corners);
	if (!held_quad) return false;
	typesetting::PointMap held_distortion = held_quad;
	if (rest_to_box) {
		auto into = rest_to_box;
		held_distortion = [held_quad, into](Vector2D point) { return held_quad(into(point)); };
	}
	double best_area = 0;
	for (auto const& found : tag_lines) {
		if (!Perspective(found)) continue;

		Vector2D line_corners[4], moved_corners[4];
		LineQuad(found, line_corners);
		bool finite = true;
		for (int i = 0; i < 4; ++i) {
			moved_corners[i] = held_distortion(line_corners[i]);
			finite = finite && std::isfinite(moved_corners[i].X()) &&
				std::isfinite(moved_corners[i].Y());
		}
		if (!finite) continue;

		double twice_area = 0;
		for (int i = 0; i < 4; ++i)
			twice_area += moved_corners[i].X() * moved_corners[(i + 1) % 4].Y() -
				moved_corners[(i + 1) % 4].X() * moved_corners[i].Y();
		double area = std::abs(twice_area) * .5;
		if (!(area > best_area) || area < 1e-3) continue;

		best_area = area;
		source.angle = 0;
		source.centre = (found.box_first + found.box_second) / 2;
		source.half = ((found.box_second - found.box_first) / 2).Max(
			Vector2D(.5f, .5f));
		std::copy(moved_corners, moved_corners + 4, projected);
	}
	return best_area > 0;
}

VisualToolTransform::Applied VisualToolTransform::ApplyGesture(
	TagLine const& original) const {
	Applied out;

	// Four visible corners taken through the gesture, and a new perspective plane solved from
	// where they land. A distortion is nothing but that; and a line which already stands out of
	// the plane has to go the same way even under the free transform, because \frz turns it
	// *before* the projection - so writing a turn there does not turn what is on screen, it
	// reshapes it. Mapping the corners and solving again turns what is seen, exactly: every
	// gesture here is affine, and an affine image of a projected quadrilateral is a projected
	// quadrilateral. The gesture itself stays flat - nothing new is put out of the plane.
	if (mode == VisualToolTransformMode::Distort || Perspective(original)) {
		Vector2D corners[4];
		LineQuad(original, corners);
		for (auto& corner : corners) {
			// Auto perspective sizes the selection as one object in the source plane. Scaling
			// every line around its own anchor would change the glyphs but leave the distances
			// between rows behind; doing it around the common box centre keeps the whole layout
			// proportional, and mapping afterwards makes it follow the target perspective.
			//
			if (auto_perspective && !auto_perspective_keep_original_size) {
				float factor = static_cast<float>(uniform_size / 100.0);
				corner = box.centre + (corner - box.centre) * factor;
			}
			corner = MapPoint(corner);
		}

		// A distortion is most stable when expressed around the quad itself. Keeping a remote
		// authored origin is mathematically equivalent, but produces extreme pos/scale values
		// and loses precision.
		//
		// The free transform is the other way about: the line already had an origin, and an
		// animation elsewhere in the file may be turning about it, so where the gesture takes
		// that point is the origin worth keeping.
		Vector2D prefer = mode == VisualToolTransformMode::Distort || !original.org ?
			Vector2D() : MapPoint(original.org);
		auto solved = typesetting::SolvePerspective(corners, original.align,
			original.box_first, original.box_second, script_res / layout_res, prefer);
		// A preferred origin can still leave the arithmetic with nothing to solve. The middle
		// of the quadrilateral is the stable fallback.
		if (!solved.ok)
			solved = typesetting::SolvePerspective(corners, original.align,
				original.box_first, original.box_second, script_res / layout_res, Vector2D());
		if (solved.ok) {
			out.perspective = true;
			out.pos = solved.pos;
			out.org = solved.org;
			out.scale = solved.scale;
			out.shear_x = solved.shear_x;
			out.shear_y = solved.shear_y;
			// What the corners came to, and then what the three sliders have added to it. The
			// nudge is kept as a step away from the solution rather than as the answer itself,
			// so dragging a corner afterwards keeps it.
			out.angle = solved.angle_z;
			out.angle_x = solved.angle_x + distort_angle_offset[0];
			out.angle_y = solved.angle_y + distort_angle_offset[1];
			return out;
		}
		// Nothing came of it - a quadrilateral folded over itself has no plane behind it - so
		// the line is left to the ordinary path rather than written out as nonsense.
	}

	// The frame and the gesture, then whatever the line already had.
	Matrix2 combined = Multiply(TotalMatrix(),
		LineMatrix(original.scale, original.shear, original.angle));

	out.angle = original.angle;
	out.shear_x = original.shear.X();
	out.scale = original.scale;
	// The line keeps its own \fay, and everything else is solved around it.
	SplitMatrix(combined, original.shear.Y(), out.angle, out.shear_x, out.scale);

	// How far above the point it is anchored at the renderer leans it, in unscaled units.
	double ascent = 0;
	if (!original.drawing && std::abs(original.scale.Y()) > 1e-6)
		ascent = original.size.Y() / (original.scale.Y() / 100.0) *
			AnchorFractions(original.align).Y();

	// Where the line really is now, where it has to end up, and what to write so that it
	// does - the lean shift has to be taken off the one and put back on the other.
	Vector2D old_lean = LeanOffset(ascent, original.shear.X(), original.scale.X(),
		original.angle);
	Vector2D new_lean = LeanOffset(ascent, out.shear_x, out.scale.X(), out.angle);
	if (!original.org) {
		// With no explicit origin \pos is the pivot, so moving it moves the line directly.
		out.pos = MapPoint(original.pos + old_lean) - new_lean;
		out.org = original.org;
		return out;
	}

	// With an explicit \org, \pos is not a point on screen: \frz first turns it about
	// \org. Mapping that raw value made the gesture's turn act twice on the long vector
	// between the two tags, which pulled drawings and text away from the rest of a selection.
	// Map the visible anchor and the origin, then undo the newly solved turn to recover the
	// \pos value which renders at that mapped anchor.
	Vector2D standing = RotateAbout(original.pos, original.org, original.angle) + old_lean;
	out.org = MapPoint(original.org);
	Vector2D turned = MapPoint(standing) - out.org - new_lean;
	out.pos = out.org + ApplyMatrix(Turn(-out.angle), turned);
	return out;
}

Vector2D VisualToolTransform::FramePoint(Vector2D point) const {
	Vector2D origin = FrameOrigin();
	Vector2D from = point - origin;
	return origin + frame_offset +
		Vector2D((float)(frame_linear.a * from.X() + frame_linear.b * from.Y()),
		         (float)(frame_linear.c * from.X() + frame_linear.d * from.Y()));
}

Vector2D VisualToolTransform::FrameInverse(Vector2D point) const {
	double determinant = frame_linear.a * frame_linear.d - frame_linear.b * frame_linear.c;
	if (std::abs(determinant) < 1e-12) return point;
	Vector2D origin = FrameOrigin();
	Vector2D from = point - origin - frame_offset;
	return origin + Vector2D(
		(float)((frame_linear.d * from.X() - frame_linear.b * from.Y()) / determinant),
		(float)((frame_linear.a * from.Y() - frame_linear.c * from.X()) / determinant));
}

Vector2D VisualToolTransform::FrameGrowth() const {
	if (mode == VisualToolTransformMode::Distort) {
		// A projective map has no one scale: it stretches the far end more than the near one.
		// The average of each pair of opposite sides against the box it started as is what a
		// border can actually follow.
		// Against the shape the selection rests in, which is what the border is on now - not
		// against the box, which a leaning selection never filled. The same numbers for anything
		// flat, where the two are one and the same.
		Vector2D const *outline = frame_quad;
		Vector2D moved[4];
		for (int i = 0; i < 4; ++i) moved[i] = MapPoint(outline[i]);
		double across = ((moved[1] - moved[0]).Len() + (moved[2] - moved[3]).Len()) / 2;
		double down = ((moved[3] - moved[0]).Len() + (moved[2] - moved[1]).Len()) / 2;
		double was_across = std::max((double)((outline[1] - outline[0]).Len() +
			(outline[2] - outline[3]).Len()) / 2, 1e-6);
		double was_down = std::max((double)((outline[3] - outline[0]).Len() +
			(outline[2] - outline[1]).Len()) / 2, 1e-6);
		return Vector2D((float)(across / was_across), (float)(down / was_down));
	}

	double angle = 0, lean = 0;
	Vector2D grown(100.f, 100.f);
	SplitMatrix(TotalMatrix(), 0, angle, lean, grown);
	return Vector2D(std::abs(grown.X()) / 100.f, std::abs(grown.Y()) / 100.f);
}

bool VisualToolTransform::SquareOn() const {
	// A distort is never square-on: four corners that could be dragged into a rectangle again
	// would still have been dragged.
	if (mode == VisualToolTransformMode::Distort) return false;
	Matrix2 whole = TotalMatrix();
	return std::abs(whole.b) < 1e-6 && std::abs(whole.c) < 1e-6 &&
		whole.a > 0 && whole.d > 0;
}

Vector2D VisualToolTransform::MapPoint(Vector2D point) const {
	// The distort is a quadrilateral rather than a scale and a turn, so where its handles have
	// been put is the whole of what it says.
	if (mode == VisualToolTransformMode::Distort)
		return distort_map ? distort_map(point) : point;
	return FramePoint(GesturePoint(point));
}

Vector2D VisualToolTransform::GesturePoint(Vector2D point) const {
	// Measured in the box's own axes, which is the space a scale is \fscx and \fscy in - the
	// point handed in is already in the shape the selection has, so scaling it there scales
	// the shape with it, exactly as the renderer scales a leaning line.
	Vector2D origin = FrameOrigin();
	Vector2D local = box.ToLocal(point);
	local = gesture_anchor + (local - gesture_anchor) * gesture_scale;
	Vector2D turned = RotateAbout(box.ToScript(local), origin, gesture_angle);

	// The lean comes last, about the middle of the frame, so both edges give a little rather
	// than one edge doing all the moving.
	if (std::abs(gesture_shear.X()) > 1e-9 || std::abs(gesture_shear.Y()) > 1e-9) {
		typesetting::OrientedBox frame;
		frame.angle = shear_frame_angle;
		frame.centre = origin;
		Vector2D in_frame = frame.ToLocal(turned);
		turned = frame.ToScript(Vector2D(
			in_frame.X() + (float)gesture_shear.X() * in_frame.Y(),
			in_frame.Y() + (float)gesture_shear.Y() * in_frame.X()));
	}
	return turned + gesture_move;
}

void VisualToolTransform::RebaseGesture() {
	if (mode != VisualToolTransformMode::Free) return;

	// The gesture, as a map about the middle of the box, composed onto the frame. A
	// multiplication and one translation: nothing is measured again, so the box keeps the
	// shape it has taken instead of springing back to a rectangle.
	Matrix2 gesture = GestureMatrix(box.angle, gesture_scale, gesture_angle,
		shear_frame_angle, gesture_shear);
	Vector2D origin = FrameOrigin();
	Vector2D shift = GesturePoint(origin) - origin;

	frame_offset = frame_offset +
		Vector2D((float)(frame_linear.a * shift.X() + frame_linear.b * shift.Y()),
		         (float)(frame_linear.c * shift.X() + frame_linear.d * shift.Y()));
	frame_linear = Multiply(frame_linear, gesture);

	// Whatever the handle stretched by joins the total the border and the shadow follow, so a
	// drag and the slider come to the same thing.
	scale_growth = Vector2D(scale_growth.X() * std::abs(gesture_scale.X()),
	                        scale_growth.Y() * std::abs(gesture_scale.Y()));
	gesture_scale = Vector2D(1.f, 1.f);
	gesture_angle = 0;
	gesture_move = Vector2D(0.f, 0.f);
	gesture_anchor = Vector2D(0.f, 0.f);
	gesture_shear = Vector2D(0.f, 0.f);
}

void VisualToolTransform::RebaseKeepingShear() {
	// Only where the lean is the whole of what is live. A scale or a turn made with the same
	// gesture is applied before it, so taking the lean out from behind them would change the
	// shape rather than leave it where it stands. Nothing the tool does produces both at once,
	// but a plain rebase is the safe answer if anything ever does.
	bool lean_alone = std::abs(gesture_scale.X() - 1) < 1e-9 &&
		std::abs(gesture_scale.Y() - 1) < 1e-9 && std::abs(gesture_angle) < 1e-9 &&
		gesture_move.Len() < 1e-9;
	if (!lean_alone) {
		RebaseGesture();
		return;
	}
	Vector2D lean = gesture_shear;
	gesture_shear = Vector2D(0.f, 0.f);
	RebaseGesture();
	gesture_shear = lean;
}

std::string VisualToolTransform::TagLineText(TagLine const& original) const {
	// Opening Auto perspective is not itself an edit. Until four points have produced a target,
	// the renderer must receive the authored line byte-for-byte; tag neutralisation belongs only
	// to the internal calculation state.
	if (auto_perspective && !touched)
		return original.line->Text.get();

	AssDialogue copy(*original.line);
	auto *tool = const_cast<VisualToolTransform *>(this);
	if (original.ensure_q2)
		tool->SetOverride(&copy, "\\q", "2");

	// One map for the whole selection; what each line has to say to follow it depends on the
	// map it already had. Compared with what the line said when the tool opened, not with the
	// standing start a gesture is measured from, so that only what really changed is written.
	Applied applied = ApplyGesture(original);
	Vector2D moved = applied.pos;
	Vector2D scale = applied.scale;
	double angle = applied.angle;
	double shear_x = applied.shear_x;

	// Anything the line sets again partway through would override what is written just below,
	// so it is dealt with first - while the start of the line still holds the values those
	// later blocks were read against.
	{
		Vector2D ratio(
			std::abs(original.start_scale.X()) > 1e-6 ?
				scale.X() / original.start_scale.X() : 1.f,
			std::abs(original.start_scale.Y()) > 1e-6 ?
				scale.Y() / original.start_scale.Y() : 1.f);
		// A border or a shadow set part way through the line follows the same growth the one at
		// the start of it does, or the two halves of a line would end up drawn differently.
		Vector2D later = DecorGrowth();
		AdjustLaterTags(&copy, original, ratio, angle - original.start_angle,
			later.X(), later.Y(),
			std::sqrt(std::max<double>(later.X() * later.Y(), 1e-9)));
	}

	auto [first_end, second_end] = MovedEnds(original, applied);
	if (original.placed.moving) {
		// A line that moves goes on moving: both ends of its run are taken through the gesture, so
		// the whole run follows and not only the frame that was looked at.
		auto [name, value] = text_to_shape::PlacementOverride(original.placed,
			first_end, second_end);
		tool->SetOverride(&copy, name, value);
	}
	else if ((moved - original.start_pos).Len() > .005f)
		tool->SetOverride(&copy, "\\pos", moved.PStr());
	// Out of the plane the origin is part of the answer rather than something carried along,
	// so it is always written - the corners only come out right with the one it was solved for.
	if (applied.perspective) {
		tool->SetOverride(&copy, "\\org", applied.org.PStr());
		tool->SetOverride(&copy, "\\frx", Number(applied.angle_x));
		tool->SetOverride(&copy, "\\fry", Number(applied.angle_y));
		if (std::abs(applied.shear_y - original.start_shear.Y()) > 1e-4)
			tool->SetOverride(&copy, "\\fay", Number(applied.shear_y));
	}
	else if (original.org)
		tool->SetOverride(&copy, "\\org", applied.org.PStr());
	if (std::abs(scale.X() - original.start_scale.X()) > 1e-3)
		tool->SetOverride(&copy, "\\fscx", Number(scale.X()));
	if (std::abs(scale.Y() - original.start_scale.Y()) > 1e-3)
		tool->SetOverride(&copy, "\\fscy", Number(scale.Y()));
	if (std::abs(angle - original.start_angle) > 1e-3)
		tool->SetOverride(&copy, "\\frz", Number(angle));
	if (std::abs(shear_x - original.start_shear.X()) > 1e-4)
		tool->SetOverride(&copy, "\\fax", Number(shear_x));

	// The border and the shadow follow how much bigger the letters were made, and nothing else.
	//
	// A turn leaves them the size they were, so there is nothing for it to change; and
	// compensating an upright pen for a lean was tried and is a trap - what it takes to give back
	// a round pen through a lean of one is a pen half again as wide, so the border stretched
	// instead of holding still and which way it stretched depended on which way the lean went.
	// The renderer leans the pen along with the letters anyway, as it does to their own strokes.
	//
	// So only the scale is followed, and only where its switch is ticked. That is what the two
	// boxes on the bar now mean: bord and shad grow with the size, or they stay as written.
	Vector2D growth = DecorGrowth();
	Vector2D kept_bord = Vector2D(original.bord.X() * growth.X(),
	                              original.bord.Y() * growth.Y());
	// A shadow is a gap read against how wide the letters are, so it follows the same growth per
	// axis. Its direction is left alone: the offset is where the line said the light comes from.
	Vector2D kept_shad = Vector2D(original.shad.X() * growth.X(),
	                              original.shad.Y() * growth.Y());

	auto write_pair = [&](Vector2D value, Vector2D was, const char *both,
	                      const char *along, const char *across) {
		if ((value - was).Len() < 1e-3) return;
		// One tag while both axes agree, which is how a line is usually written; two only
		// when they have to differ.
		if (std::abs(value.X() - value.Y()) < 1e-4)
			tool->SetOverride(&copy, both, Number(value.X()));
		else {
			tool->SetOverride(&copy, along, Number(value.X()));
			tool->SetOverride(&copy, across, Number(value.Y()));
		}
	};

	if (recalc_bord)
		write_pair(kept_bord, original.start_bord, "\\bord", "\\xbord", "\\ybord");
	if (recalc_shad)
		write_pair(kept_shad, original.start_shad, "\\shad", "\\xshad", "\\yshad");
	// With the two of them standing on their own lines as shapes, the letters must not draw them
	// again - and the shapes are exact, where a pen could only have been approximate.
	if (maintain_decor && !original.decor.empty()) {
		tool->SetOverride(&copy, "\\bord", "0");
		tool->SetOverride(&copy, "\\shad", "0");
	}
	if (recalc_clip && original.clip.present) {
		// A rectangle that has been turned is a quadrilateral now, so the tag changes form -
		// and SetOverride takes the other form of the pair out for us.
		std::string mapped = MapClip(original.clip);
		if (!mapped.empty())
			tool->SetOverride(&copy, original.clip.inverse ? "\\iclip" : "\\clip",
				"(" + mapped + ")");
	}
	if (recalc_blur) {
		// A blur is a spread in pixels of the finished picture, so it follows how much bigger the
		// letters were made - and having no direction of its own, it follows the two axes
		// together. Like the pair above it takes no notice of a turn or a lean.
		double grow = std::sqrt(std::max<double>(growth.X() * growth.Y(), 1e-9));
		double blur = original.blur * grow;
		double be = original.be * grow;
		if (std::abs(blur - original.start_blur) > 1e-3)
			tool->SetOverride(&copy, "\\blur", Number(blur));
		if (std::abs(be - original.start_be) > .5)
			tool->SetOverride(&copy, "\\be", Number(std::round(be)));
	}

	return copy.Text.get();
}

namespace {
	/// A convex shape cut down to where some measure of a point is nought or more: Sutherland
	/// and Hodgman's method. The measure has to be affine - how far past a straight edge a
	/// point is, or how far it is from a vanishing line - so that where an edge crosses can be
	/// found by interpolating along it. Fewer than three points come back when nothing is left.
	template<typename Measure>
	std::vector<Vector2D> CutToHalfPlane(std::vector<Vector2D> shape, Measure measure) {
		if (shape.size() < 3) return {};
		std::vector<Vector2D> kept;
		kept.reserve(shape.size() + 2);
		for (size_t at = 0; at < shape.size(); ++at) {
			Vector2D current = shape[at];
			Vector2D next = shape[(at + 1) % shape.size()];
			double now = measure(current), then = measure(next);
			if (now >= 0) kept.push_back(current);
			if ((now >= 0) != (then >= 0)) {
				double along = std::abs(now - then) < 1e-12 ? 0. : now / (now - then);
				kept.push_back(current + (next - current) * (float)along);
			}
		}
		return kept.size() >= 3 ? kept : std::vector<Vector2D>();
	}

	/// The same, cut down to a box - which is four of those, in the box's own frame.
	std::vector<Vector2D> CutToBox(std::vector<Vector2D> shape,
	                               typesetting::OrientedBox const& box, Vector2D margin) {
		Vector2D limit = box.half + margin;
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return box.ToLocal(at).X() + limit.X(); });
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return limit.X() - box.ToLocal(at).X(); });
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return box.ToLocal(at).Y() + limit.Y(); });
		shape = CutToHalfPlane(std::move(shape),
			[&](Vector2D at) { return limit.Y() - box.ToLocal(at).Y(); });
		return shape;
	}
}

std::string VisualToolTransform::MapClip(TagLine::Clip const& clip) const {
	if (!clip.present) return {};

	// Square-on means no turn, no lean and no mirroring - and that has to be asked of the map
	// as a whole, not of the gesture being made. Asking only the gesture meant that on a logo
	// turned earlier, a plain scale still counted as square-on, and every band of a gradient
	// was written as the bounding box of its turned self.
	bool square_on = SquareOn();

	if (clip.rectangle) {
		Vector2D low = clip.first.Min(clip.second);
		Vector2D high = clip.first.Max(clip.second);

		if (square_on) {
			// Nothing to pad: a rectangular clip snaps to whole pixels, so two bands that share
			// an edge already tile without help.
			Vector2D first = MapPoint(low), second = MapPoint(high);
			Vector2D at = first.Min(second), to = first.Max(second);
			return agi::format("%s,%s,%s,%s", Number(at.X()), Number(at.Y()),
				Number(to.X()), Number(to.Y()));
		}

		// A turned rectangle is a quadrilateral, and only a drawing can say that - but a
		// drawing clip is drawn with soft edges, while a rectangular one snaps to whole pixels.
		// Two bands of a gradient that shared an edge would each cover it half way and leave a
		// dark seam, so the band is grown by half a unit on screen: which is half a unit
		// divided by however much the frame has grown, and never more than half the band
		// itself, or one band would swallow the next.
		Vector2D growth = FrameGrowth();
		Vector2D pad(
			std::min((high.X() - low.X()) * .5f, .5f / std::max(growth.X(), 1e-6f)),
			std::min((high.Y() - low.Y()) * .5f, .5f / std::max(growth.Y(), 1e-6f)));
		low = low - pad;
		high = high + pad;

		std::vector<Vector2D> shape = {
			low, Vector2D(high.X(), low.Y()), high, Vector2D(low.X(), high.Y())
		};

		// The part of the band that is nowhere near the box cuts nothing, and under a distort
		// it is also the part that reaches across the line the map sends to infinity. Cut it
		// away first and every coordinate stays where it belongs. The margin leaves room for
		// what spreads beyond the text itself - a border, a glow.
		if (mode == VisualToolTransformMode::Distort) {
			// The clip is given where the text really is and the map is expressed from the box,
			// so everything below is asked about the box's own copy of the point.
			auto into = rest_to_box;
			auto to_box = [&into](Vector2D at) { return into ? into(at) : at; };

			// Grown to cover the shape the selection rests in, which on a leaning selection
			// reaches outside the box - and a band cut away there is a band lost.
			typesetting::OrientedBox reach = box;
			if (into)
				for (auto const& corner : frame_quad) {
					Vector2D local = box.ToLocal(corner);
					reach.half = reach.half.Max(
						Vector2D(std::abs(local.X()), std::abs(local.Y())));
				}
			shape = CutToBox(std::move(shape), reach,
				reach.half * .25f + Vector2D(24.f, 24.f));
			// And to the side of the plane the map still says anything about. When the shape is
			// nearly folded over, the line it sends to infinity comes close enough to the box
			// that a band can still straddle it - and a band that does comes back inside out,
			// covering everything drawn before it.
			//
			// Where to stop is measured against the box itself: half of the least it has to put
			// up with anywhere on its own corners. Past that a point is magnified more than
			// twice as hard as anything the text goes through, which is far enough out to be of
			// no interest to a clip.
			Vector2D outline[4];
			box.Corners(outline);
			double least = 1e9;
			for (auto const& corner : outline)
				least = std::min(least, typesetting::QuadDepth(box, corners, corner));
			double floor_depth = std::max(least * .5, 1e-3);
			shape = CutToHalfPlane(std::move(shape), [&](Vector2D at) {
				return typesetting::QuadDepth(box, corners, to_box(at)) - floor_depth;
			});
		}
		if (shape.size() < 3) return {};

		for (auto& point : shape) point = MapPoint(point);
		// And if something still came back meaningless, the clip is better left as it was than
		// written out wrong.
		for (auto const& point : shape)
			if (!std::isfinite(point.X()) || !std::isfinite(point.Y()) ||
				std::abs(point.X()) > 1e5f || std::abs(point.Y()) > 1e5f) return {};

		std::string out = "m";
		for (size_t at = 0; at < shape.size(); ++at)
			out += " " + Number(shape[at].X()) + " " + Number(shape[at].Y()) +
				(at ? "" : " l");
		return out;
	}

	// A drawing: every pair of numbers is a point, whatever command it belongs to, so the
	// commands can be copied across untouched.
	double divisor = static_cast<double>(1 << std::max(0, clip.scale - 1));
	std::string out;
	double pending = 0;
	bool have_pending = false;
	size_t at = 0;
	while (at < clip.drawing.size()) {
		while (at < clip.drawing.size() &&
			std::isspace(static_cast<unsigned char>(clip.drawing[at]))) ++at;
		if (at >= clip.drawing.size()) break;

		size_t end = at;
		if (std::isalpha(static_cast<unsigned char>(clip.drawing[at]))) {
			while (end < clip.drawing.size() &&
				std::isalpha(static_cast<unsigned char>(clip.drawing[end]))) ++end;
			if (!out.empty()) out += ' ';
			out += clip.drawing.substr(at, end - at);
			at = end;
			continue;
		}

		while (end < clip.drawing.size() && !std::isspace(static_cast<unsigned char>(
			clip.drawing[end])) && !std::isalpha(static_cast<unsigned char>(
			clip.drawing[end]))) ++end;
		double value = 0;
		try { value = std::stod(clip.drawing.substr(at, end - at)); }
		catch (...) { at = end; continue; }
		at = end;

		if (!have_pending) { pending = value; have_pending = true; continue; }
		have_pending = false;
		Vector2D mapped = MapPoint(Vector2D((float)(pending / divisor),
		                                    (float)(value / divisor)));
		// A drawn clip cannot be cut down to the box the way a band can - it may be any shape at
		// all - so a point that has landed out past infinity is taken as a sign to leave the
		// whole clip alone rather than write a shape nobody meant.
		if (!std::isfinite(mapped.X()) || !std::isfinite(mapped.Y()) ||
			std::abs(mapped.X()) > 1e5f || std::abs(mapped.Y()) > 1e5f) return {};
		out += ' ' + Number(mapped.X() * divisor) + ' ' + Number(mapped.Y() * divisor);
	}

	if (out.empty()) return {};
	return clip.scale != 1 ? agi::format("%d,%s", clip.scale, out) : out;
}

void VisualToolTransform::HandleRole(int index, Vector2D& grabbed, Vector2D& anchor,
                                    int& role) const {
	// Four corners to size it both ways, then the middles of the four sides to size it one
	// way, then a leaning handle beyond each side. Turning is done outside the box.
	// What a handle drags is one point of the box; what stays is the point across from it.
	static const Vector2D corners[4] = {
		Vector2D(-1.f, -1.f), Vector2D(1.f, -1.f), Vector2D(1.f, 1.f), Vector2D(-1.f, 1.f)
	};
	static const Vector2D sides[4] = {
		Vector2D(0.f, -1.f), Vector2D(1.f, 0.f), Vector2D(0.f, 1.f), Vector2D(-1.f, 0.f)
	};

	role = index < 8 ? 0 : index < 12 ? 2 : 3;
	Vector2D unit = index < 4 ? corners[index] :
		index < 8 ? sides[index - 4] :
		index < 12 ? sides[index - 8] : sides[index - 12];
	grabbed = Vector2D(unit.X() * box.half.X(), unit.Y() * box.half.Y());
	anchor = Vector2D(-unit.X() * box.half.X(), -unit.Y() * box.half.Y());
}

void VisualToolTransform::AdjustLaterTags(AssDialogue *line, TagLine const& original,
                                         Vector2D scale_ratio, double turn, double grow_x,
                                         double grow_y, double grow) const {
	auto blocks = line->ParseTags();
	Matrix2 whole = TotalMatrix();

	// What is in force as the line is read. A block partway through changes it from there on,
	// which is exactly why its values have to be worked out rather than left alone.
	Vector2D state_scale = original.start_scale;
	Vector2D state_shear = original.start_shear;
	double state_angle = original.start_angle;

	bool first_block = true;
	bool changed = false;

	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		auto& tags = static_cast<AssDialogueBlockOverride*>(block.get())->Tags;

		// The state as this block leaves it, read before anything is written over it.
		for (auto const& tag : tags) {
			if (tag.Params.empty() || tag.Params[0].omitted) continue;
			std::string const& name = tag.Name;
			double value = tag.Params[0].Get<double>(0);
			if (name == "\\fscx") state_scale = Vector2D((float)value, state_scale.Y());
			else if (name == "\\fscy") state_scale = Vector2D(state_scale.X(), (float)value);
			else if (name == "\\frz" || name == "\\fr") state_angle = value;
			else if (name == "\\fax") state_shear = Vector2D((float)value, state_shear.Y());
			else if (name == "\\fay") state_shear = Vector2D(state_shear.X(), (float)value);
		}

		if (first_block) {
			// The first block is where the transform writes its own values; the ones after it
			// are what would otherwise undo them.
			first_block = false;
			continue;
		}

		// What this stretch of the line has to say to follow the same map. A scale or a turn
		// leaves the lean alone - the renderer applies it after the scale - but a lean does not,
		// so it is solved for rather than scaled.
		double solved_angle = state_angle;
		double solved_shear = state_shear.X();
		Vector2D solved_scale = state_scale;
		bool solved = SplitMatrix(
			Multiply(whole, LineMatrix(state_scale, state_shear, state_angle)),
			state_shear.Y(), solved_angle, solved_shear, solved_scale);

		for (auto& tag : tags) {
			if (tag.Params.empty() || tag.Params[0].omitted) continue;
			std::string const& name = tag.Name;
			double value = tag.Params[0].Get<double>(0);

			auto write = [&](double result) {
				tag.Params[0].Set(result);
				changed = true;
			};

			if (name == "\\fscx") write(value * scale_ratio.X());
			else if (name == "\\fscy") write(value * scale_ratio.Y());
			else if (name == "\\frz" || name == "\\fr") write(value + turn);
			else if (name == "\\fax") { if (solved) write(solved_shear); }
			// Unticked means untouched, wherever in the line the tag sits.
			else if (name == "\\bord" || name == "\\xbord") {
				if (recalc_bord) write(value * grow_x);
			}
			else if (name == "\\ybord") { if (recalc_bord) write(value * grow_y); }
			else if (name == "\\shad" || name == "\\xshad") {
				if (recalc_shad) write(value * grow_x);
			}
			else if (name == "\\yshad") { if (recalc_shad) write(value * grow_y); }
			else if (name == "\\blur" || name == "\\be") {
				if (recalc_blur) write(value * grow);
			}
		}
	}

	if (changed) line->UpdateText(blocks);
}

VisualToolTransform::TagLine::Clip VisualToolTransform::ReadClip(AssDialogue *line) {
	TagLine::Clip out;
	for (auto& block : line->ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags) {
			if (tag.Name != "\\clip" && tag.Name != "\\iclip") continue;
			auto const& params = tag.Params;
			out.present = true;
			out.inverse = tag.Name == "\\iclip";

			if (params.size() >= 4) {
				out.rectangle = true;
				out.first = Vector2D(params[0].Get<float>(0), params[1].Get<float>(0));
				out.second = Vector2D(params[2].Get<float>(0), params[3].Get<float>(0));
			}
			else if (params.size() == 2) {
				out.scale = params[0].Get<int>(1);
				out.drawing = params[1].Get<std::string>("");
			}
			else if (params.size() == 1) {
				out.drawing = params[0].Get<std::string>("");
			}
			else out.present = false;
		}
	}
	return out;
}

/// Read the selection's drawings, converting any text in memory as it goes.
void VisualToolTransform::Collect() {
	// What was being worked out, in case the shapes turn out to be the same ones: a
	// refresh is not a reason to throw a reshaping away.
	bool had_editor = editor.has_value();
	typesetting::OrientedBox old_box = box;
	typesetting::WarpNet old_net = net;
	Vector2D old_corners[4];
	for (int i = 0; i < 4; ++i) old_corners[i] = corners[i];
	double old_distort_angle[2];
	for (int i = 0; i < 2; ++i) old_distort_angle[i] = distort_angle_offset[i];
	bool was_touched = touched;

	features.clear();
	sel_features.clear();
	editor.reset();
	touched = false;

	AssDialogue *line = c->selectionController->GetActiveLine();
	if (line != session_line) {
		// Another line is another job, and the history belonged to the old one.
		session_line = line;
		source_moved = false;
		reported = false;
		undo_history.clear();
		redo_history.clear();
	}

	if (!typesetting::CanTransform(c)) return;

	// A line that is not on screen has nothing to preview, so the tool does not start on
	// one - it hands the video straight back instead of sitting there with no handles.
	if (!active_line) {
		ExitTool();
		return;
	}

	// A textbox document expresses its rectangle through its generated rows. Treating it as
	// one document here would resize that rectangle; Auto perspective deliberately places
	// the individual rows without changing their dimensions.
	if (!auto_perspective) CollectTextBox();
	else {
		textbox_document.reset();
		textbox_lines.clear();
	}

	// These modes say everything in tags, so they need no drawings and convert nothing: they
	// read the lines as they are.
	if (TagsMode()) {
		bool had_lines = !tag_lines.empty();

		tag_lines.clear();
		if (!CollectTags()) return;
		if (TextBoxMode()) {
			Vector2D edge = textbox_original_corners[1] - textbox_original_corners[0];
			box.angle = static_cast<float>(std::atan2(edge.Y(), edge.X()) * 180.0 / pi);
			box.centre = (textbox_original_corners[0] + textbox_original_corners[1] +
				textbox_original_corners[2] + textbox_original_corners[3]) / 4;
			box.half = Vector2D(
				(textbox_original_corners[1] - textbox_original_corners[0]).Len() * .25f +
					(textbox_original_corners[2] - textbox_original_corners[3]).Len() * .25f,
				(textbox_original_corners[3] - textbox_original_corners[0]).Len() * .25f +
					(textbox_original_corners[2] - textbox_original_corners[1]).Len() * .25f);
			box.half = box.half.Max(Vector2D(4.f, 4.f));
			// Its own rectangle, and no shape on top of it: the document boundary already is
			// the shape, and the rows are only a rendering of it.
			frame_shape = typesetting::PointMap();
			rest_to_box = typesetting::PointMap();
			std::copy(textbox_original_corners, textbox_original_corners + 4, frame_quad);
		}

		// The corners start on the untouched box, so nothing moves until one is dragged -
		// unless the same box came back, which means the same lines did, and then they go back
		// where the user left them. This is what keeps a distort alive through a zoom or a pan.
		if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, corners);
		// On the shape the selection rests in, which for anything that is a rectangle already is
		// the box itself - so a flat selection starts exactly where it always did.
		else std::copy(frame_quad, frame_quad + 4, corners);
		if (mode == VisualToolTransformMode::Distort && had_lines && was_touched &&
			(old_box.centre - box.centre).Len() < .01 &&
			(old_box.half - box.half).Len() < .01 &&
			std::abs(old_box.angle - box.angle) < .01) {
			for (int i = 0; i < 4; ++i) corners[i] = old_corners[i];
			// The two sliders are part of the same reshaping the corners are, so they come
			// back with them rather than being thrown away by a zoom or a pan.
			for (int i = 0; i < 2; ++i) distort_angle_offset[i] = old_distort_angle[i];
			touched = true;
		}

		// The distort is a projective map, and what it comes to in tags nearly always includes
		// a lean - which the renderer applies to each row from that row's own corner, not from
		// the top of the line. So the rows would slide against one another whatever is dragged,
		// and there is nothing to wait for: the lines are broken up as the tool opens.
		if (mode == VisualToolTransformMode::Distort && !TextBoxMode())
			EnsureShearSplit();

		LockEditing(true);
		PlaceFeatures();
		Rebuild();
		return;
	}

	typesetting::ShapeEditor found = TextBoxMode() ?
		typesetting::ShapeEditor(c, textbox_lines) : typesetting::ShapeEditor(c);
	if (!found.ok()) {
		// Nothing usable at all is worth saying out loud, once.
		if (!reported && !found.refusals().empty()) {
			reported = true;
			wxString message = _("The text could not be converted:");
			for (auto const& why : found.refusals())
				message += "\n\n" + to_wx(why);
			wxMessageBox(message, _("Convert text to shapes"), wxOK | wxICON_WARNING,
				c->parent);
		}
		return;
	}

	if (!reported && !found.refusals().empty()) {
		reported = true;
		wxString message = _("Some lines were left as text:");
		for (auto const& why : found.refusals())
			message += "\n\n" + to_wx(why);
		wxMessageBox(message, _("Convert text to shapes"), wxOK | wxICON_INFORMATION,
			c->parent);
	}

	box = found.Box();
	editor.emplace(std::move(found));

	BuildEditorFrameShape();

	// The handles start on the shape the selection rests in, so nothing moves until something is
	// dragged. The net's middle is a displacement rather than a place, so it is left alone.
	std::copy(frame_quad, frame_quad + 4, corners);
	typesetting::WarpReset(box, net);
	if (frame_shape) {
		for (auto& point : net.corner) point = frame_shape(point);
		for (auto& point : net.tangent) point = frame_shape(point);
	}
	// Kept as it rests, before anything is dragged: a bend is the difference from this, so from
	// here the tool opening moves nothing at all whatever shape the selection is in.
	rest_net = net;

	// Unless the same box came back, which means the same drawings did: then the handles go
	// back where the user left them. This is what keeps a reshaping alive through a zoom, a
	// pan or any other refresh.
	if (had_editor && was_touched &&
		(old_box.centre - box.centre).Len() < .01 &&
		(old_box.half - box.half).Len() < .01 &&
		std::abs(old_box.angle - box.angle) < .01) {
		net = old_net;
		for (int i = 0; i < 4; ++i) corners[i] = old_corners[i];
		touched = true;
	}

	LockEditing(true);
	PlaceFeatures();
	Rebuild();
}

void VisualToolTransform::LockMenus(bool locked) {
	// The menus that run something over the lines themselves. While the session is open the video
	// is being shown copies the file does not have yet, so a script that reads or rewrites the
	// selection would be working on something other than what is on screen - and would throw the
	// reshaping away as soon as it committed.
	//
	// Found by their own titles, translated the same way the menu bar translated them, because a
	// menu can come and go - the Muteki one appears only when its macros load - and counting from
	// the left would then grey out the wrong one.
	auto frame = dynamic_cast<wxFrame *>(c->parent);
	if (!frame) return;
	wxMenuBar *bar = frame->GetMenuBar();
	if (!bar) return;

	for (const char *title : {"A&I", "A&utomation", "Kintsugi Fansub"}) {
		int at = bar->FindMenu(wxGetTranslation(title));
		if (at != wxNOT_FOUND) bar->EnableTop(at, !locked);
	}
}

void VisualToolTransform::LockEditing(bool locked) {
	// Held by the sessions together, not by whichever of them built it. Switching modes from the
	// menu builds the new tool before the old one is destroyed, so the new one arrives to find
	// everything already disabled - nothing left to record - and the old one would then put it all
	// back on its way out, with a session still running.
	static int held = 0;
	static std::vector<wxWindow *> held_controls;

	if (locked) {
		if (held++ > 0) return;
	}
	else {
		if (held > 0) --held;
		if (held > 0) return;
	}

	LockMenus(locked);
	if (!c->editBox) return;

	if (!locked) {
		for (auto control : held_controls)
			control->Enable(true);
		held_controls.clear();
		c->editBox->Enable(true);
		return;
	}

	if (!held_controls.empty()) return;

	// Every control, not just the panel: a disabled panel still leaves its children looking
	// and behaving as usual on Windows, the text control included.
	std::function<void (wxWindow *)> walk = [&](wxWindow *window) {
		for (auto child : window->GetChildren()) {
			if (child->IsEnabled()) {
				child->Enable(false);
				held_controls.push_back(child);
			}
			walk(child);
		}
	};
	walk(c->editBox);
	c->editBox->Enable(false);

	// Disabling the panel hands the focus to whatever comes next, and the tool needs the
	// video to have it for Enter and Escape to reach here.
	parent->SetFocus();
}

int VisualToolTransform::HandleCount() const {
	switch (mode) {
		case VisualToolTransformMode::Free: return 16;
		// Two per side, so every edge can be bent. The corners stay where they are, which is
		// what keeps an arch an arch: the shape bends without going anywhere.
		case VisualToolTransformMode::Arch: return 8;
		case VisualToolTransformMode::Distort: return 4;
		// Four corners, two direction handles each, and the one under the shape that moves it.
		default: return warp_move_handle + 1;
	}
}

Vector2D VisualToolTransform::HandlePosition(int index) const {
	switch (mode) {
		case VisualToolTransformMode::Free: {
			Vector2D grabbed, anchor;
			int role = 0;
			HandleRole(index, grabbed, anchor, role);
			return MapPoint(ShapePoint(grabbed));
		}
		case VisualToolTransformMode::Arch: return net.tangent[index];
		case VisualToolTransformMode::Distort: return corners[index];
		default: {
			if (index == warp_move_handle) {
				// The middle of the bottom edge of the patch. SyncFeatures pushes it a little
				// further out from there, so it sits under the shape rather than on it.
				Vector2D control[16];
				typesetting::WarpControls(net, control);
				return typesetting::WarpPoint(control, .5, 1.);
			}
			return index < 4 ? net.corner[index] : net.tangent[index - 4];
		}
	}
}

void VisualToolTransform::MoveHandle(int index, Vector2D to) {
	if (index < 0 || index >= HandleCount()) return;
	switch (mode) {
		case VisualToolTransformMode::Free: {
			Vector2D grabbed, anchor;
			int role = 0;
			HandleRole(index, grabbed, anchor, role);

			if (role == 2) {
				// A leaning handle: how far the side has slid sideways, against how far it
				// sits from the middle, is the lean itself.
				typesetting::OrientedBox frame;
				frame.angle = shear_frame_angle;
				frame.centre = FrameOrigin();
				Vector2D wanted = frame.ToLocal(to - gesture_start - gesture_move);
				Vector2D from = frame.ToLocal(
					RotateAbout(ShapePoint(grabbed), frame.centre, gesture_angle));
				// `from` is where the side would be without any lean, so the difference
				// against the mouse is the whole lean rather than a step of one - adding the
				// lean so far would count it twice.
				if (std::abs(grabbed.X()) < 1e-6 && std::abs(from.Y()) > 1e-6)
					gesture_shear = Vector2D((float)((wanted.X() - from.X()) / from.Y()),
						gesture_shear.Y());
				else if (std::abs(from.X()) > 1e-6)
					gesture_shear = Vector2D(gesture_shear.X(),
						(float)((wanted.Y() - from.Y()) / from.X()));
				break;
			}

			if (role == 3) {
				// Move the entire selection on the screen axis named by the handle's arrow:
				// top/bottom are Y, left/right are X.
				Vector2D moved = to - gesture_start;
				bool vertical = index == 12 || index == 14;
				Vector2D wanted = vertical ? Vector2D(0.f, moved.Y()) :
					Vector2D(moved.X(), 0.f);
				TransformMatrix2 inverse;
				gesture_move = Invert(frame_linear, inverse) ? ApplyMatrix(inverse, wanted) : wanted;
				break;
			}

			// A corner or a side keeps the point across from it still, so the box scales
			// about that point - and the scale is what \fscx and \fscy will say. It is
			// measured along the box's own axes as they now lie, so a scale after a turn
			// still scales the box rather than the screen.
			// Both points in the shape the frame really has, because that is where the handle
			// is drawn - measuring the mouse against the flat rectangle instead made the box
			// spring the moment a leaning handle was grabbed.
			Vector2D held = box.ToLocal(ShapePoint(grabbed));
			Vector2D still = box.ToLocal(ShapePoint(anchor));
			gesture_anchor = still;
			// In the space the gesture works in, not on screen: the mouse has already been
			// brought back through the frame, and measuring the two against each other in
			// different spaces collapsed the box the moment a turned frame was scaled.
			Vector2D fixed_point = GesturePoint(ShapePoint(anchor));
			double radians = (box.angle - gesture_angle) * pi / 180.0;
			Vector2D along((float)std::cos(radians), (float)std::sin(radians));
			Vector2D across(-along.Y(), along.X());
			Vector2D reach = to - fixed_point;

			auto ratio = [](double got, double wanted, float fallback) {
				if (std::abs(wanted) < 1e-6) return fallback;
				double value = got / wanted;
				// A scale of nothing would collapse the line and leave no way back.
				if (std::abs(value) < .02) value = value < 0 ? -.02 : .02;
				return (float)value;
			};
			// Which axis this handle is for is its own business, taken from the flat unit: a
			// leaning frame gives a side handle a sideways reach as well, and letting that
			// drive a scale would turn every side handle into a corner one.
			bool drives_x = std::abs(grabbed.X() - anchor.X()) > 1e-6;
			bool drives_y = std::abs(grabbed.Y() - anchor.Y()) > 1e-6;
			Vector2D wanted(
				drives_x ? ratio(reach.X() * along.X() + reach.Y() * along.Y(),
					held.X() - still.X(), gesture_scale.X()) : gesture_scale.X(),
				drives_y ? ratio(reach.X() * across.X() + reach.Y() * across.Y(),
					held.Y() - still.Y(), gesture_scale.Y()) : gesture_scale.Y());

			// Holding alt keeps the proportions: the axis that moved the most decides, and
			// the other follows it - so a side handle scales both ways.
			if (alt_down) {
				float same = drives_x && drives_y ?
					std::max(std::abs(wanted.X()), std::abs(wanted.Y())) :
					drives_x ? std::abs(wanted.X()) : std::abs(wanted.Y());
				wanted = Vector2D(wanted.X() < 0 ? -same : same,
				                  wanted.Y() < 0 ? -same : same);
			}
			gesture_scale = wanted;
			break;
		}
		case VisualToolTransformMode::Arch:
			net.tangent[index] = to;
			break;
		case VisualToolTransformMode::Distort:
			corners[index] = to;
			break;
		default:
			// The handle under the shape moves the whole of it. Always measured from where the
			// gesture began, never from the last mouse move: one small step after another would
			// let rounding walk the mesh away.
			if (index == warp_move_handle) {
				Vector2D moved = to - hold_start;
				net = hold_net;
				// The corners and their handles are the whole of the boundary; what dragging the
				// mesh added to the middle is a difference from those, so it comes along on its
				// own.
				for (auto& point : net.corner) point = point + moved;
				for (auto& point : net.tangent) point = point + moved;
				break;
			}
			if (index < 4) {
				// A corner takes its two direction handles with it, so the shape swings
				// about the corner instead of the boundary snapping straight.
				typesetting::WarpMoveCorner(net, index, to - net.corner[index]);
			}
			else net.tangent[index - 4] = to;
			break;
	}
}

void VisualToolTransform::PlaceFeatures() {
	features.clear();
	sel_features.clear();
	if (!Active()) return;
	if (auto_perspective) {
		distort_map = AutoPerspectiveMap();
		source_feature_first = no_feature;
		target_feature_first = no_feature;
		target_move_feature = no_feature;

		size_t target_count = touched ? 4 : auto_perspective_points.size();
		if (target_count) {
			target_feature_first = features.size();
			for (size_t i = 0; i < target_count; ++i) {
				auto feature = std::make_unique<VisualDraggableFeature>();
				feature->type = DRAG_BIG_SQUARE;
				feature->layer = 2;
				features.push_back(*feature.release());
			}
		}
		if (touched) {
			target_move_feature = features.size();
			auto feature = std::make_unique<VisualDraggableFeature>();
			feature->type = DRAG_BIG_SQUARE;
			feature->layer = 1;
			features.push_back(*feature.release());
		}
		if (!touched && auto_perspective_points.empty()) {
			// Before target construction begins, the source rectangle can still be stated exactly.
			source_feature_first = features.size();
			for (int i = 0; i < 4; ++i) {
				auto feature = std::make_unique<VisualDraggableFeature>();
				feature->type = DRAG_SMALL_SQUARE;
				feature->layer = 1;
				features.push_back(*feature.release());
			}
		}
		SyncFeatures();
		return;
	}

	int count = HandleCount();
	for (int i = 0; i < count; ++i) {
		auto feature = std::make_unique<VisualDraggableFeature>();
		// In the warp the four corners are squares and the eight direction handles small
		// circles, the way Photoshop draws them; the corners sit on the higher layer so a
		// handle resting on one cannot steal the click.
		bool is_corner = mode == VisualToolTransformMode::Warp && i < 4;
		if (mode == VisualToolTransformMode::Free) {
			// Squares to size it and smaller squares beyond the sides to lean it. Rotation
			// owns the otherwise empty area outside the box, rather than four extra handles.
			feature->type = i < 8 ? DRAG_BIG_SQUARE : DRAG_SMALL_SQUARE;
			feature->layer = i < 4 || i >= 12 ? 1 : 0;
		}
		else if (mode == VisualToolTransformMode::Distort) {
			// Corners, so they look and catch the mouse the way the free transform's corners do.
			feature->type = DRAG_BIG_SQUARE;
			feature->layer = 1;
		}
		else if (mode == VisualToolTransformMode::Warp && i == warp_move_handle) {
			// A box with a crosshair through it, the way the drag tool draws the point a line
			// is positioned by - and on the higher layer, since it is the one handle that must
			// never be stolen by something resting on it.
			feature->type = DRAG_BIG_SQUARE;
			feature->layer = 2;
		}
		else {
			feature->type = mode == VisualToolTransformMode::Warp ?
				(is_corner ? DRAG_BIG_SQUARE : DRAG_SMALL_CIRCLE) : DRAG_BIG_CIRCLE;
			feature->layer = mode == VisualToolTransformMode::Warp && !is_corner ? 0 : 1;
		}
		features.push_back(*feature.release());
	}
	SyncFeatures();
}

void VisualToolTransform::SyncFeatures() {
	// A feature is hit-tested against the mouse in pixels, so where it sits is a canvas
	// position. The handles themselves are kept in script coordinates, because that is
	// what the shape is measured in and what has to survive a zoom.
	if (auto_perspective) {
		distort_map = AutoPerspectiveMap();
		if (target_feature_first != no_feature) {
			size_t target_count = touched ? 4 : auto_perspective_points.size();
			for (size_t i = 0; i < target_count; ++i)
				if (auto *handle = FeatureAt(target_feature_first + i))
					handle->pos = FromScriptCoords(touched ? corners[i] : auto_perspective_points[i]);
		}
		if (source_feature_first != no_feature)
			for (int i = 0; i < 4; ++i)
				if (auto *handle = FeatureAt(source_feature_first + i))
					handle->pos = FromScriptCoords(source_corners[i]);
		if (target_move_feature != no_feature) {
			Vector2D screen[4];
			for (int i = 0; i < 4; ++i) screen[i] = FromScriptCoords(corners[i]);
			Vector2D middle = (screen[0] + screen[1] + screen[2] + screen[3]) / 4;
			Vector2D bottom = (screen[2] + screen[3]) / 2;
			Vector2D away = bottom - middle;
			if (auto *move = FeatureAt(target_move_feature))
				move->pos = bottom +
					(away.Len() > 1e-3 ? away.Unit() * 30.f : Vector2D(0.f, 30.f));
		}
		return;
	}

	int index = 0;
	for (auto& feature : features) {
		Vector2D at = FromScriptCoords(HandlePosition(index));
		// The turning and leaning handles sit a little way beyond their corner or side,
		// measured on screen so they stay the same distance out however far the video is
		// zoomed. The leaning ones sit further out, to keep them off the sizing handles.
		if (mode == VisualToolTransformMode::Free && index >= 8) {
			Vector2D centre = FromScriptCoords(MapPoint(FrameOrigin()));
			Vector2D away = at - centre;
			if (away.Len() > 1e-3) at = at + away.Unit() * 28.f;
			if (index >= 12 && away.Len() > 1e-3) {
				// The lean and axis-position handles both have a twelve-pixel hit box.
				// Keep their centres far enough apart to leave a clear, unclickable gap.
				Vector2D tangent(-away.Y(), away.X());
				if (index == 13 || index == 14) tangent = tangent * -1.f;
				at = at + tangent.Unit() * 32.f;
			}
		}
		// The warp's move handle sits clear of the bottom edge, outwards from the middle of the
		// patch - so it stays off the shape whichever way round the shape lies.
		if (mode == VisualToolTransformMode::Warp && index == warp_move_handle) {
			Vector2D control[16];
			typesetting::WarpControls(net, control);
			Vector2D middle = FromScriptCoords(typesetting::WarpPoint(control, .5, .5));
			Vector2D away = at - middle;
			if (away.Len() > 1e-3) at = at + away.Unit() * 30.f;
		}
		feature.pos = at;
		++index;
	}

	// Everything that moves a corner comes through here, so this is the one place the map has
	// to be worked out again.
	// Taken back to the box first, so what the corners say is the change from the shape the
	// selection rests in onto themselves - which is nothing at all until one is dragged.
	if (mode == VisualToolTransformMode::Distort)
		distort_map = DistortionMap(corners, rest_to_box);
}

void VisualToolTransform::Rebuild() {
	if (TagsMode()) {
		SendPreview();
		parent->Render();
		return;
	}
	if (!editor) return;
	// The arch and the warp are the same patch with different numbers of handles free.
	// The border and the shadow always become shapes of their own here: a pen stays upright whatever
	// happens to the shape, so under a bend there is nothing else they could be - and nothing to
	// decide either, which is why there is no switch for it in these modes.
	// Measured from the net as the selection rests in it rather than from the box's own, and
	// read along that same shape - so at rest the difference is nothing and nothing moves, and
	// under a drag the bend runs along the words however the text is leaning or turned.
	auto map = rest_to_box ?
		typesetting::WarpMapFrom(box, rest_net, net, rest_to_box) :
		typesetting::WarpMap(box, net);
	// The visual transform only consumes the generated preview rows. Contour and layer
	// extraction is for gradient geometry and used to duplicate the entire glyph walk here.
	editor->Build(map, true, recalc_clip, true, false);
	SendPreview();
	parent->Render();
}

// ------------------------------------------------------------------ the tool's history

VisualToolTransform::HistoryState VisualToolTransform::Capture() const {
	HistoryState state;
	for (int i = 0; i < 4; ++i) state.corners[i] = corners[i];
	state.net = net;
	state.scale = gesture_scale;
	state.angle = gesture_angle;
	state.move = gesture_move;
	state.anchor = gesture_anchor;
	state.shear = gesture_shear;
	state.uniform_size = uniform_size;
	state.size_x = size_x;
	state.size_y = size_y;
	for (int i = 0; i < 2; ++i) state.distort_angle_offset[i] = distort_angle_offset[i];
	state.scale_growth = scale_growth;
	state.frame_linear = frame_linear;
	state.frame_offset = frame_offset;
	state.split = shear_split;
	for (int i = 0; i < 4; ++i) state.source_corners[i] = source_corners[i];
	state.source_moved = source_moved;
	state.auto_perspective_points = auto_perspective_points;
	state.touched = touched;
	return state;
}

void VisualToolTransform::RestoreState(HistoryState const& state) {
	for (int i = 0; i < 4; ++i) corners[i] = state.corners[i];
	net = state.net;
	gesture_scale = state.scale;
	gesture_angle = state.angle;
	gesture_move = state.move;
	gesture_anchor = state.anchor;
	gesture_shear = state.shear;
	uniform_size = state.uniform_size;
	size_x = state.size_x;
	size_y = state.size_y;
	for (int i = 0; i < 2; ++i) distort_angle_offset[i] = state.distort_angle_offset[i];
	scale_growth = state.scale_growth;
	if (mode == VisualToolTransformMode::Free) {
		frame_linear = state.frame_linear;
		frame_offset = state.frame_offset;
		shear_split = state.split && !split_lines.empty();
	}
	if (auto_perspective) {
		for (int i = 0; i < 4; ++i) source_corners[i] = state.source_corners[i];
		source_moved = state.source_moved;
		auto_perspective_points = state.auto_perspective_points;
		// Whether there is a target at all is part of the step: undoing the four points has to
		// take the handle that moves them away with them.
		touched = state.touched;
		PlaceFeatures();
		Rebuild();
		return;
	}
	SyncFeatures();
	Rebuild();
}

void VisualToolTransform::PushHistory() {
	undo_history.push_back(Capture());
	redo_history.clear();
}

bool VisualToolTransform::UndoHistory() {
	if (!Active() || undo_history.empty()) return false;
	redo_history.push_back(Capture());
	auto state = undo_history.back();
	undo_history.pop_back();
	RestoreState(state);
	return true;
}

bool VisualToolTransform::RedoHistory() {
	if (!Active() || redo_history.empty()) return false;
	undo_history.push_back(Capture());
	auto state = redo_history.back();
	redo_history.pop_back();
	RestoreState(state);
	return true;
}

// --------------------------------------------------------------- accepting, rejecting

void VisualToolTransform::Accept() {
	if (!LinesAlive()) {
		ExitTool();
		return;
	}
	if (TagsMode()) {
		if (TextBoxMode()) {
			auto transformed = TransformedTextBox();
			AssDialogue prototype(*textbox_lines.front());
			auto originals = textbox_lines;
			// Apply changes both the file and the selection. Neither listener may rebuild this
			// tool halfway through replacing the generated rows.
			selection_connection.Block();
			file_changed_connection.Block();
			typesetting::textbox::Apply(c, prototype, std::move(originals), transformed);
			file_changed_connection.Unblock();
			selection_connection.Unblock();
			ExitTool();
			return;
		}
		wxString what = auto_perspective ? _("auto perspective") :
			mode == VisualToolTransformMode::Distort ? _("distort") : _("free transform");
		if (maintain_decor) EnsureDecor();
		bool adding = shear_split || (maintain_decor && HasDecor());
		if (!adding) {
			// Only tags change, so no lines are added and nothing is turned into a comment.
			for (auto const& found : tag_lines)
				found.line->Text = TagLineText(found);
			commit_id = -1;
			VisualToolBase::Commit(what);
			ExitTool();
			return;
		}

		// The pieces go into the file now. The first of them takes over the line it was cut
		// from - so nothing has to be deleted and the line keeps its place - and the rest
		// follow it in order.
		auto& events = c->ass->Events;
		std::vector<AssDialogue *> written;

		auto put_after = [&](AssDialogue *previous, AssDialogue const& model,
		                     std::string const& text) {
			auto fresh = new AssDialogue(model);
			fresh->Text = text;
			fresh->Comment = false;
			auto at = events.iterator_to(*previous);
			++at;
			events.insert(at, *fresh);
			written.push_back(fresh);
			return fresh;
		};
		// Everything one stretch becomes, in the order it has to be drawn in: the shadow, the
		// border over it, and the letters over both.
		auto texts_for = [&](TagLine const& source) {
			std::vector<std::string> out;
			if (maintain_decor) {
				for (auto const& decor : source.decor) {
					if (decor.has_shadow) out.push_back(DecorLineText(source, decor, true));
					if (decor.has_border) out.push_back(DecorLineText(source, decor, false));
				}
			}
			// The letters always go last. Decoration shapes preserve the outline/shadow; they
			// must never replace the glyph fill itself.
			out.push_back(TagLineText(source));
			return out;
		};

		for (auto const& origin : tag_lines) {
			std::vector<std::string> texts;
			if (!origin.replaced) texts = texts_for(origin);
			else
				for (auto const& piece : split_lines) {
					if (piece.source != origin.line) continue;
					auto more = texts_for(piece);
					texts.insert(texts.end(), more.begin(), more.end());
				}
			if (texts.empty()) continue;

			// The first of them takes over the line it came from - so nothing has to be deleted
			// and the line keeps its place - and the rest follow it in order.
			origin.line->Text = texts.front();
			written.push_back(origin.line);
			AssDialogue *previous = origin.line;
			for (size_t at = 1; at < texts.size(); ++at)
				previous = put_after(previous, *origin.line, texts[at]);
		}

		commit_id = -1;
		// Lines were added, so this is not the text-only commit a visual tool normally makes.
		// Blocked around it for the same reason the base blocks it: the listener would send us
		// back through Collect in the middle of finishing up.
		file_changed_connection.Block();
		c->ass->Commit(what, AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
		file_changed_connection.Unblock();

		if (!written.empty()) {
			Selection selection(written.begin(), written.end());
			c->selectionController->SetSelectionAndActive(std::move(selection), written.front());
		}
		ExitTool();
		return;
	}

	if (!editor) return;

	// The first and only time the lines are touched. A fresh commit id keeps it a step of
	// its own in the undo history, and the commit is what makes the video re-read the file
	// rather than the preview copies.
	editor->Apply();

	// Lines were added, so this is not the text-only commit a visual tool normally makes -
	// and the drawings are what the user will want to carry on with, so they end up
	// selected.
	auto added = editor->applied();
	bool removing_textbox = TextBoxMode();
	std::vector<std::unique_ptr<AssDialogue>> removed_textbox_lines;
	if (removing_textbox) {
		// Arch and Warp bake the words into drawings. They no longer have a rectangular
		// text-flow model, so do not leave an Effect marker or textbox source metadata behind.
		for (auto line : added) {
			line->Effect = "";
			c->ass->DeleteExtradataValue(*line, typesetting::textbox::data_key);
		}
		removed_textbox_lines.reserve(textbox_lines.size());
		for (auto line : textbox_lines) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed_textbox_lines.emplace_back(line);
		}
		c->ass->CleanExtradata();
	}
	wxString message = mode == VisualToolTransformMode::Arch ? _("arch") :
		mode == VisualToolTransformMode::Distort ? _("distort") : _("warp");
	// Blocked around the commit for the same reason the base blocks it: the listener would
	// send us back through Collect in the middle of finishing up.
	file_changed_connection.Block();
	if (removing_textbox) selection_connection.Block();
	// The active line must never point at one of the detached textbox sources while commit
	// listeners inspect the file. This was the list.hpp:1310 assertion captured in the dump.
	if (!added.empty()) {
		Selection selection(added.begin(), added.end());
		c->selectionController->SetSelectionAndActive(std::move(selection), added.front());
	}
	c->ass->Commit(message, AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL |
		(removing_textbox ? AssFile::COMMIT_EXTRADATA : 0));
	file_changed_connection.Unblock();
	if (removing_textbox) selection_connection.Unblock();

	ExitTool();
}

void VisualToolTransform::Reject() {
	// Nothing in the file to put back. Leaving is what tells the video to show the lines
	// again instead of the preview copies.
	ExitTool();
}

void VisualToolTransform::ExitTool() {
	if (leaving) return;
	leaving = true;
	preview_interface.Clear();
	LockEditing(false);
	if (WindowGoing()) return;

	// However the session ends, the video has to stop showing the preview copies. After
	// Apply that means handing over lines that already carry the result, which is what
	// they should be showing anyway.
	ClearPreview();

	editor.reset();
	textbox_document.reset();
	textbox_lines.clear();
	features.clear();
	sel_features.clear();
	undo_history.clear();
	redo_history.clear();
	touched = false;

	// Switching tools destroys this object, so it cannot happen while one of its own
	// event handlers is still running.
	std::string command = return_tool;
	agi::Context *context = c;
	parent->CallAfter([command, context] { cmd::call(command, context); });
}

// ------------------------------------------------------------------------- the top bar

wxString VisualToolTransform::LabelFor(VisualToolTransformAction action) const {
	switch (action) {
		case VisualToolTransformAction::Apply: return _("Accept (ENTER)");
		case VisualToolTransformAction::Cancel: return _("Cancel (ESC)");
		case VisualToolTransformAction::AutoPerspectiveReset: return _("Reset");
		// The three that name a tag say only the tag. There is nothing to translate in
		// "bord", and "recalculate bord" was three times the width for it.
		case VisualToolTransformAction::RecalcBord: return "bord";
		case VisualToolTransformAction::RecalcShad: return "shad";
		case VisualToolTransformAction::RecalcBlur: return "blur";
		case VisualToolTransformAction::MaintainDecor: return _("maintain bord & shad");
		case VisualToolTransformAction::RecalcClip: return "clip";
		case VisualToolTransformAction::UniformSize: return _("Scale");
		case VisualToolTransformAction::ScaleLink: return _("Scale axes together");
		case VisualToolTransformAction::ScaleX: return _("Scale X");
		case VisualToolTransformAction::ScaleY: return _("Scale Y");
		case VisualToolTransformAction::Rotation: return _("Rotation");
		case VisualToolTransformAction::ShearX: return _("Shear X");
		case VisualToolTransformAction::ShearY: return _("Shear Y");
		case VisualToolTransformAction::DistortAngleX: return _("Rotate X");
		case VisualToolTransformAction::DistortAngleY: return _("Rotate Y");
		case VisualToolTransformAction::AutoPerspectiveKeepOriginalSize:
			return _("keep original size");
		default: return wxString();
	}
}

void VisualToolTransform::UpdatePreviewInterface() const {
	using Interface = VisualToolPreviewInterface;
	Interface::Page page;

	auto add = [&](VisualToolTransformAction action, Interface::ControlKind kind,
		Interface::ControlStyle style = Interface::ControlStyle::Neutral,
		bool selected = false) -> Interface::Control& {
		Interface::Control control;
		control.id = static_cast<int>(action);
		control.kind = kind;
		control.label = LabelFor(action);
		control.style = style;
		control.enabled = ActionEnabled(action);
		control.selected = selected;
		page.controls.push_back(std::move(control));
		return page.controls.back();
	};

	// Every slider on the bar is built the same way. They are a little narrower than the one
	// Size used to be, because the free transform now shows up to five of them in a row.
	auto slider = [&](VisualToolTransformAction action, double value, double low, double high,
		double step, wxString const& shown, wxString const& widest, int width,
		double fallback) -> Interface::Control& {
		auto& control = add(action, Interface::ControlKind::Slider);
		control.value = value;
		control.minimum = low;
		control.maximum = high;
		control.step = step;
		control.value_text = shown;
		control.value_text_sample = widest;
		control.width = width;
		// Where a right click puts it back to, which for every one of these is what it read
		// before anything was asked of it.
		control.default_value = fallback;
		return control;
	};
	wxString degree(wxUniChar(0x00B0));
	// A value too small to show is nothing, and nothing has no sign: a slider reading "-0.00"
	// says the shape leans the other way by an amount it cannot name. Done on the text rather
	// than on the number so it holds for however many places each of these is written to.
	auto without_minus_zero = [](std::string text) {
		if (text.empty() || text.front() != '-') return text;
		if (text.find_first_of("123456789") != std::string::npos) return text;
		return text.substr(1);
	};
	auto percent = [&](double value) {
		return to_wx(without_minus_zero(agi::format("%.0f%%", value)));
	};
	auto degrees = [&](double value) {
		return to_wx(without_minus_zero(agi::format("%.1f", value))) + degree;
	};
	auto lean = [&](double value) {
		return to_wx(without_minus_zero(agi::format("%.2f", value)));
	};
	wxString widest_angle = "-180.0" + degree;

	if (auto_perspective) {
		add(VisualToolTransformAction::Undo, Interface::ControlKind::Undo);
		add(VisualToolTransformAction::Redo, Interface::ControlKind::Redo);
		if (touched) {
			auto& reset = add(VisualToolTransformAction::AutoPerspectiveReset,
				Interface::ControlKind::Button);
			reset.before_accept = true;
		}
		add(VisualToolTransformAction::Apply, Interface::ControlKind::Button,
			Interface::ControlStyle::Accept);
		add(VisualToolTransformAction::Cancel, Interface::ControlKind::Button,
			Interface::ControlStyle::Cancel);
		// Live whether the authored size is being kept or not: with the lock on it sizes the
		// plane the selection is laid on, which is a size to choose like any other.
		slider(VisualToolTransformAction::UniformSize, uniform_size, 25, 200, 1,
			percent(uniform_size), "200%", 170, 100).enabled = touched;
		add(VisualToolTransformAction::AutoPerspectiveKeepOriginalSize,
			Interface::ControlKind::Toggle, Interface::ControlStyle::Neutral,
			auto_perspective_keep_original_size);
		wxString message = touched && auto_perspective_points.empty() ?
			_("Drag the 4 points to adjust the perspective.") : _("Draw 4 points.");
		if (!touched && tag_lines.size() > 1 && auto_perspective_active_reference)
			if (AssDialogue *active = c->selectionController->GetActiveLine()) {
				std::string pattern =
					from_wx(_("Line %d is fitted to the points, the rest follow it."));
				message += " " + to_wx(agi::format(pattern.c_str(), active->Row + 1));
			}
		if (!touched)
			message += " " + _("The yellow dashed rectangle is the reference.");
		page.message = message;
		preview_interface.SetPage(std::move(page));
		return;
	}

	add(VisualToolTransformAction::Undo, Interface::ControlKind::Undo);
	add(VisualToolTransformAction::Redo, Interface::ControlKind::Redo);
	add(VisualToolTransformAction::Apply, Interface::ControlKind::Button,
		Interface::ControlStyle::Accept);
	add(VisualToolTransformAction::Cancel, Interface::ControlKind::Button,
		Interface::ControlStyle::Cancel);
	const int slider_width = 140;
	if (mode == VisualToolTransformMode::Free) {
		if (scale_linked)
			slider(VisualToolTransformAction::UniformSize, uniform_size, 25, 400, 1,
				percent(uniform_size), "400%", slider_width, 100);
		else {
			slider(VisualToolTransformAction::ScaleX, size_x, 25, 400, 1, percent(size_x),
				"400%", slider_width, 100);
			slider(VisualToolTransformAction::ScaleY, size_y, 25, 400, 1, percent(size_y),
				"400%", slider_width, 100);
		}
		// Held, the one slider drives both axes; broken, each has its own. The icon says
		// which it is, so the button needs no words of its own.
		auto& chain = add(VisualToolTransformAction::ScaleLink,
			Interface::ControlKind::Button, Interface::ControlStyle::Neutral, scale_linked);
		chain.icon = scale_linked ? Interface::ControlIcon::Chain :
			Interface::ControlIcon::ChainBroken;
		chain.icon_only = true;

		// The turn, as the angle the active line stands at. The two leans are the box's own,
		// which is the one quantity the leaning handles set - so the number beside a handle and
		// the handle itself always say the same thing.
		Applied now = ActiveApplied();
		TagLine const *first = ActiveTagLine();
		if (!first && !tag_lines.empty()) first = &tag_lines.front();
		slider(VisualToolTransformAction::Rotation, now.angle, -180, 180, .5,
			degrees(now.angle), widest_angle, slider_width,
			first ? first->start_angle : 0.f);
		slider(VisualToolTransformAction::ShearX, gesture_shear.X(), -2, 2, .01,
			lean(gesture_shear.X()), "-2.00", slider_width, 0);
		slider(VisualToolTransformAction::ShearY, gesture_shear.Y(), -2, 2, .01,
			lean(gesture_shear.Y()), "-2.00", slider_width, 0);
	}
	else if (mode == VisualToolTransformMode::Distort) {
		// The two axes the four corners cannot say on their own. The turn in the plane is left
		// out because dragging the quadrilateral is that turn, and so is the size.
		Applied now = ActiveApplied();
		slider(VisualToolTransformAction::DistortAngleX, now.angle_x, -180, 180, .5,
			degrees(now.angle_x), widest_angle, slider_width,
			now.angle_x - distort_angle_offset[0]);
		slider(VisualToolTransformAction::DistortAngleY, now.angle_y, -180, 180, .5,
			degrees(now.angle_y), widest_angle, slider_width,
			now.angle_y - distort_angle_offset[1]);
	}
	page.controls.push_back({0, Interface::ControlKind::Spacer});

	// The three that follow the size, in the order they are written on a line. Only the free
	// transform has anything to choose here: a distortion is a projective map with no one
	// scale behind it, so there is no number it could give any of them.
	if (mode == VisualToolTransformMode::Free) {
		add(VisualToolTransformAction::RecalcBord, Interface::ControlKind::Toggle,
			Interface::ControlStyle::Neutral, recalc_bord);
		add(VisualToolTransformAction::RecalcShad, Interface::ControlKind::Toggle,
			Interface::ControlStyle::Neutral, recalc_shad);
		add(VisualToolTransformAction::RecalcBlur, Interface::ControlKind::Toggle,
			Interface::ControlStyle::Neutral, recalc_blur);
	}
	add(VisualToolTransformAction::RecalcClip, Interface::ControlKind::Toggle,
		Interface::ControlStyle::Neutral, recalc_clip);

	preview_interface.SetPage(std::move(page));
}

std::pair<Vector2D, Vector2D> VisualToolTransform::ActionBounds(
	VisualToolTransformAction action) const {
	UpdatePreviewInterface();
	return preview_interface.BoundsFor(static_cast<int>(action), *gl_text, canvas_size);
}

float VisualToolTransform::TopBarHeight() const {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return 0.f;
	return preview_interface.Height(*gl_text, canvas_size);
}

bool VisualToolTransform::ActionEnabled(VisualToolTransformAction action) const {
	if (auto_perspective)
		return action == VisualToolTransformAction::Undo ? !undo_history.empty() :
			action == VisualToolTransformAction::Redo ? !redo_history.empty() :
			action == VisualToolTransformAction::AutoPerspectiveReset ? touched :
			action == VisualToolTransformAction::Apply ? touched :
			action == VisualToolTransformAction::Cancel ? true :
			action == VisualToolTransformAction::UniformSize ? touched :
			action == VisualToolTransformAction::AutoPerspectiveKeepOriginalSize ? true : false;

	switch (action) {
		case VisualToolTransformAction::Undo: return !undo_history.empty();
		case VisualToolTransformAction::Redo: return !redo_history.empty();
		// Anything that has been dragged is worth writing, even if it has since been
		// undone back to where it started: the text may still have to become a drawing.
		case VisualToolTransformAction::Apply: return touched;
		case VisualToolTransformAction::Cancel: return true;
		case VisualToolTransformAction::UniformSize:
		case VisualToolTransformAction::ScaleX:
		case VisualToolTransformAction::ScaleY:
		case VisualToolTransformAction::ScaleLink:
		case VisualToolTransformAction::Rotation:
		case VisualToolTransformAction::ShearX:
		case VisualToolTransformAction::ShearY:
			return mode == VisualToolTransformMode::Free;
		// A guided perspective solves all three from the four points, so there is nothing
		// left to nudge - and its bar deliberately shows next to nothing.
		case VisualToolTransformAction::DistortAngleX:
		case VisualToolTransformAction::DistortAngleY:
			return mode == VisualToolTransformMode::Distort && !auto_perspective;
		// Only where there is something to choose between. A distortion has no one scale for
		// any of them to follow, and the arch and the warp keep the pair as shapes because
		// under a bend there is no other answer.
		case VisualToolTransformAction::RecalcBord:
		case VisualToolTransformAction::RecalcShad:
		case VisualToolTransformAction::RecalcBlur:
			return mode == VisualToolTransformMode::Free;
		case VisualToolTransformAction::MaintainDecor:
			return TagsMode();
		// A clip is script coordinates whether the text stayed text or became an outline, so
		// it can follow the reshaping in every mode - and has to be allowed not to, since a
		// clip that was drawn around something else should stay where it was drawn.
		case VisualToolTransformAction::RecalcClip:
			return true;
		default: return false;
	}
}

VisualToolTransformAction VisualToolTransform::ActionAt(Vector2D point) const {
	if (!Active()) return VisualToolTransformAction::None;
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return VisualToolTransformAction::None;
	return static_cast<VisualToolTransformAction>(
		preview_interface.HitTest(point, *gl_text, canvas_size));
}

void VisualToolTransform::Perform(VisualToolTransformAction action) {
	if (!ActionEnabled(action)) return;
	switch (action) {
		case VisualToolTransformAction::Undo: UndoHistory(); break;
		case VisualToolTransformAction::Redo: RedoHistory(); break;
		case VisualToolTransformAction::AutoPerspectiveReset:
			PushHistory();
			auto_perspective_points.clear();
			box.Corners(corners);
			touched = false;
			PlaceFeatures();
			Rebuild();
			break;
		case VisualToolTransformAction::Apply: Accept(); break;
		case VisualToolTransformAction::Cancel: Reject(); break;
		// Fine-tuning a value and keeping the pair exactly are answers to the same question, so
		// whichever is turned on turns the other off.
		case VisualToolTransformAction::RecalcBord:
			recalc_bord = !recalc_bord;
			if (recalc_bord) maintain_decor = false;
			Rebuild();
			break;
		case VisualToolTransformAction::RecalcShad:
			recalc_shad = !recalc_shad;
			if (recalc_shad) maintain_decor = false;
			Rebuild();
			break;
		case VisualToolTransformAction::MaintainDecor:
			maintain_decor = !maintain_decor;
			if (maintain_decor) {
				recalc_bord = false;
				recalc_shad = false;
				EnsureDecor();
			}
			Rebuild();
			break;
		case VisualToolTransformAction::RecalcBlur:
			recalc_blur = !recalc_blur;
			Rebuild();
			break;
		case VisualToolTransformAction::RecalcClip:
			recalc_clip = !recalc_clip;
			Rebuild();
			break;
		case VisualToolTransformAction::ScaleLink:
			scale_linked = !scale_linked;
			// Nothing moves either way. Breaking the chain leaves both axes where the one
			// slider had put them; holding it again takes the pair as one number, so that a
			// later drag keeps whatever proportion they were left in.
			if (scale_linked)
				uniform_size = std::sqrt(std::max(size_x * size_y, 1e-9));
			parent->Render();
			UpdatePreviewInterface();
			break;
		case VisualToolTransformAction::AutoPerspectiveKeepOriginalSize:
			auto_perspective_keep_original_size = !auto_perspective_keep_original_size;
			OPT_SET("Tool/Visual/Perspective/Keep Original Size")->SetBool(
				auto_perspective_keep_original_size);
			if (touched) {
				// The lock uses a different target quadrilateral: the same perspective plane at
				// the selection's authored size. Refresh the cached projective map before the
				// preview is rebuilt, otherwise toggling only changes the checkbox and the old
				// fill-the-area map remains in use.
				SyncFeatures();
				Rebuild();
			}
			else parent->Render();
			break;
		default: break;
	}
}

Vector2D VisualToolTransform::DecorGrowth() const {
	// Only what was asked of the scale, the gesture in progress included. A turn and a lean
	// leave the letters the size they were, so nothing measured in pixels has to follow them -
	// which is what the two switches now say, and all they say.
	return Vector2D(scale_growth.X() * std::abs(gesture_scale.X()),
	                scale_growth.Y() * std::abs(gesture_scale.Y()));
}

void VisualToolTransform::ApplyScaleRatio(double ratio_x, double ratio_y) {
	if (mode != VisualToolTransformMode::Free) return;
	if (std::abs(ratio_x - 1) < 1e-12 && std::abs(ratio_y - 1) < 1e-12) return;
	// Fold any handle gesture in first, then grow the accumulated linear map along the box's
	// own axes - which is what the two scales mean to a line. The translation stays put, so
	// the complete selection grows about its displayed centre.
	RebaseKeepingShear();
	scale_growth = Vector2D((float)(scale_growth.X() * std::abs(ratio_x)),
	                        (float)(scale_growth.Y() * std::abs(ratio_y)));
	frame_linear = Multiply(frame_linear,
		InFrame(box.angle, Matrix2{ratio_x, 0, 0, ratio_y}));
	SyncFeatures();
	touched = true;
	Rebuild();
}

void VisualToolTransform::UpdateUniformSize(double value) {
	if (!auto_perspective && mode != VisualToolTransformMode::Free) return;
	double next = std::clamp(value, 25.0, auto_perspective ? 200.0 : 400.0);
	if (std::abs(next - uniform_size) < 1e-9) return;
	if (mode == VisualToolTransformMode::Free) {
		double ratio = next / uniform_size;
		uniform_size = next;
		// The pair follow the one number, so breaking the chain afterwards starts from
		// whatever the linked slider had reached.
		size_x *= ratio;
		size_y *= ratio;
		ApplyScaleRatio(ratio, ratio);
	}
	else {
		uniform_size = next;
		// The lock lays the selection on the target plane at its authored size, and the
		// percentage is part of that size - so the map itself has to be worked out again.
		if (touched) { SyncFeatures(); Rebuild(); }
		else parent->Render();
	}
	UpdatePreviewInterface();
}

void VisualToolTransform::UpdateScaleAxis(VisualToolTransformAction action, double value) {
	if (mode != VisualToolTransformMode::Free) return;
	bool vertical = action == VisualToolTransformAction::ScaleY;
	double& current = vertical ? size_y : size_x;
	double next = std::clamp(value, 25.0, 400.0);
	if (std::abs(next - current) < 1e-9 || std::abs(current) < 1e-9) return;
	double ratio = next / current;
	current = next;
	// What the one slider would show if the chain were held again: the pair as a whole.
	uniform_size = std::sqrt(std::max(size_x * size_y, 1e-9));
	ApplyScaleRatio(vertical ? 1.0 : ratio, vertical ? ratio : 1.0);
	UpdatePreviewInterface();
}

VisualToolTransform::Applied VisualToolTransform::ActiveApplied() const {
	TagLine const *found = ActiveTagLine();
	// The sliders have to say something even when what is selected is not among the lines
	// the tool could read, and the first of them is the one the box was measured from.
	if (!found && !tag_lines.empty()) found = &tag_lines.front();
	return found ? ApplyGesture(*found) : Applied();
}

void VisualToolTransform::UpdateRotation(double value) {
	if (mode != VisualToolTransformMode::Free) return;
	double next = std::clamp(value, -180.0, 180.0);
	double turn = next - ActiveApplied().angle;
	// The shorter way round, so a slider at one end of its travel never spins the selection
	// most of a full turn to reach the other.
	while (turn > 180.0) turn -= 360.0;
	while (turn < -180.0) turn += 360.0;
	if (std::abs(turn) < 1e-9) return;

	// About the middle of the box as it is displayed, which is where the frame's translation
	// already puts it - so turning the map on the left leaves that point alone.
	RebaseKeepingShear();
	frame_linear = Multiply(Turn(turn), frame_linear);
	SyncFeatures();
	touched = true;
	Rebuild();
	UpdatePreviewInterface();
}

void VisualToolTransform::UpdateShear(bool vertical, double value) {
	if (mode != VisualToolTransformMode::Free) return;
	float next = (float)std::clamp(value, -2.0, 2.0);
	if (std::abs(next - (vertical ? gesture_shear.Y() : gesture_shear.X())) < 1e-9f) return;
	// The gesture's own lean, which is exactly what a leaning handle sets - so the two are
	// interchangeable, and whichever is used the other shows it. Deliberately not folded into
	// the frame first: a handle measures its lean from the same standing start, and rebasing
	// here would leave the number reading nought while the box went on leaning.
	gesture_shear = vertical ? Vector2D(gesture_shear.X(), next) :
		Vector2D(next, gesture_shear.Y());
	SyncFeatures();
	touched = true;
	Rebuild();
	UpdatePreviewInterface();
}

void VisualToolTransform::UpdateDistortAngle(int axis, double value) {
	if (auto_perspective || mode != VisualToolTransformMode::Distort) return;
	if (axis < 0 || axis > 1) return;
	Applied now = ActiveApplied();
	double next = std::clamp(value, -180.0, 180.0);
	double turn = next - (axis == 0 ? now.angle_x : now.angle_y);
	while (turn > 180.0) turn -= 360.0;
	while (turn < -180.0) turn += 360.0;
	if (std::abs(turn) < 1e-9) return;
	// Kept as a step away from what the corners solve to, so dragging one afterwards keeps
	// the nudge instead of throwing it away.
	distort_angle_offset[axis] += turn;
	touched = true;
	Rebuild();
	UpdatePreviewInterface();
}

void VisualToolTransform::DrawTopBar() {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return;
	preview_interface.Draw(gl, *gl_text, canvas_size, static_cast<int>(hovered_action));
}

bool VisualToolTransform::AddAutoPerspectivePoint(Vector2D point) {
	if (touched) return false;

	auto candidate = auto_perspective_points;
	candidate.push_back(point);
	if (candidate.size() < 4) {
		auto_perspective_points = std::move(candidate);
		PlaceFeatures();
		parent->Render();
		return false;
	}

	// Keep the first three when the closing point would fold or cross the plane. The next
	// click replaces only that last attempt, which makes correcting it feel like mask drawing.
	if (!DirectedQuad(candidate)) {
		parent->Render();
		return false;
	}

	PushHistory();
	for (int i = 0; i < 4; ++i) corners[i] = candidate[i];
	auto_perspective_points.clear();
	touched = true;
	PlaceFeatures();
	Rebuild();
	return true;
}

void VisualToolTransform::DrawAutoPerspectiveSource() {
	if (touched) return;

	// What the four points are fitted from: an active quadrilateral, or otherwise the complete
	// selection frame used by Distort. Shown so the proportion
	// the result is stretched by is something to be seen rather than worked out backwards from
	// the outcome - a tall box on a wide quadrilateral says at a glance why the text spreads.
	wxColour yellow(255, 255, 0);
	gl.SetLineColour(yellow, 1.f, 1);
	for (int i = 0; i < 4; ++i)
		gl.DrawDashedLine(FromScriptCoords(source_corners[i]),
			FromScriptCoords(source_corners[(i + 1) % 4]), 6.f);

	if (source_feature_first == no_feature) return;
	for (int i = 0; i < 4; ++i) {
		auto *handle = FeatureAt(source_feature_first + i);
		if (!handle) continue;
		// Solid and outlined more heavily under the pointer, so it is clear which one a drag
		// would take hold of before the drag starts.
		bool under_mouse = handle == active_feature;
		gl.SetLineColour(yellow, 1.f, under_mouse ? 2 : 1);
		gl.SetFillColour(yellow, under_mouse ? .9f : .35f);
		handle->Draw(gl);
	}
}

void VisualToolTransform::DrawAutoPerspectivePath() {
	// Strengthen the completed quad and its handles without changing the drawing preview.
	constexpr float completed_opacity_scale = 1.25f;
	auto draw_target = [&](size_t index, Vector2D point, bool quiet) {
		auto *handle = target_feature_first == no_feature ? nullptr :
			FeatureAt(target_feature_first + index);
		bool under_mouse = handle && handle == active_feature;
		wxColour outline = to_wx(line_color_secondary_opt->GetColor());
		wxColour active = to_wx(highlight_color_secondary_opt->GetColor());
		gl.SetLineColour(under_mouse ? active : outline,
			under_mouse ? 1.f : quiet ? .66f * completed_opacity_scale : 1.f,
			under_mouse ? 2 : 1);
		gl.SetFillColour(*wxBLACK, 0.f);
		Vector2D at = handle ? handle->pos : FromScriptCoords(point);
		gl.DrawRectangle(at - Vector2D(5.f, 5.f), at + Vector2D(5.f, 5.f));
	};

	if (auto_perspective_points.empty()) {
		if (!touched) return;

		wxColour path = to_wx(line_color_secondary_opt->GetColor());
		gl.SetLineColour(path, .62f * completed_opacity_scale, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawLine(FromScriptCoords(corners[i]), FromScriptCoords(corners[(i + 1) % 4]));
		for (int i = 0; i < 4; ++i)
			draw_target(i, corners[i], true);

		auto *move = target_move_feature == no_feature ? nullptr : FeatureAt(target_move_feature);
		if (!move) return;
		Vector2D bottom = (FromScriptCoords(corners[2]) + FromScriptCoords(corners[3])) / 2;
		wxColour outline = to_wx(line_color_secondary_opt->GetColor());
		wxColour fill = to_wx(highlight_color_primary_opt->GetColor());
		bool under_mouse = move == active_feature;
		gl.SetLineColour(outline, (under_mouse ? .75f : .53f) * completed_opacity_scale,
			under_mouse ? 2 : 1);
		gl.DrawLine(bottom, move->pos);
		gl.SetFillColour(fill, (under_mouse ? .55f : .38f) * completed_opacity_scale);
		move->Draw(gl);
		return;
	}

	wxColour path = to_wx(line_color_secondary_opt->GetColor());
	gl.SetLineColour(path, 1.f, 2);
	for (size_t i = 1; i < auto_perspective_points.size(); ++i)
		gl.DrawLine(FromScriptCoords(auto_perspective_points[i - 1]),
			FromScriptCoords(auto_perspective_points[i]));

	// The moving edge and its closing edge preview the same connected contour used by the
	// mask tools. Only the fixed, clicked points are retained.
	Vector2D pointer = mouse_pos;
	// The preview follows the cursor into the letterbox too, since a click there places a
	// corner as well; stopping the dashed edges at the video rect pointed them at a spot
	// where nothing would appear. mouse_pos is tested for validity because leaving the
	// display resets it to a near-zero float rather than NaN, which would otherwise pass
	// the top bar test on its own.
	if (pointer && pointer.Y() >= TopBarHeight()) {
		Vector2D last = FromScriptCoords(auto_perspective_points.back());
		gl.DrawDashedLine(last, pointer, 5.f);
		if (auto_perspective_points.size() > 1)
			gl.DrawDashedLine(pointer, FromScriptCoords(auto_perspective_points.front()), 5.f);
	}

	for (size_t i = 0; i < auto_perspective_points.size(); ++i)
		draw_target(i, auto_perspective_points[i], false);
}

void VisualToolTransform::DrawCorner(Vector2D at, bool current, bool selected) {
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour active = to_wx(highlight_color_secondary_opt->GetColor());
	bool soft_hover = mode == VisualToolTransformMode::Warp;
	gl.SetLineColour(current || selected ? active : outline, 1.f,
		current && !soft_hover ? 2 : 1);
	if (selected) gl.SetFillColour(active, .46f);
	else if (current && soft_hover) gl.SetFillColour(active, .14f);
	else gl.SetFillColour(*wxBLACK, 0.f);
	gl.DrawRectangle(at - Vector2D(5.f, 5.f), at + Vector2D(5.f, 5.f));
}

void VisualToolTransform::DrawShapeHandles() {
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour base_fill = to_wx(highlight_color_primary_opt->GetColor());
	wxColour active_fill = to_wx(highlight_color_secondary_opt->GetColor());

	int index = 0;
	for (auto& feature : features) {
		bool current = &feature == active_feature;
		bool selected = sel_features.count(&feature) != 0;
		if (mode == VisualToolTransformMode::Warp && index == warp_move_handle)
			selected = false;
		// The distort's four, and the warp's first four, are corners of the box. The warp's
		// others steer its curves and the arch's bend its edges: those are not corners and are
		// not drawn as any.
		bool corner = mode == VisualToolTransformMode::Distort ||
			(mode == VisualToolTransformMode::Warp && index < 4);
		// The move handle is left to the framework: a big square is drawn as a box with a
		// crosshair through it, which is what the drag tool's own \pos handle looks like - so
		// the one handle that moves the shape looks like the tool that moves things.
		if (corner) DrawCorner(feature.pos, current, selected);
		else {
			bool soft_hover = mode == VisualToolTransformMode::Arch ||
				mode == VisualToolTransformMode::Warp;
			gl.SetLineColour(current || selected ? active_fill : outline, 1.f,
				current && !soft_hover ? 2 : 1);
			gl.SetFillColour(current || selected ? active_fill : base_fill,
				selected ? .58f : current ? (soft_hover ? .42f : .9f) : .3f);
			feature.Draw(gl);
		}
		++index;
	}
}

void VisualToolTransform::DrawFreeHandles() {
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour active = to_wx(highlight_color_secondary_opt->GetColor());

	int index = 0;
	for (auto& feature : features) {
		bool current = &feature == active_feature;
		gl.SetLineColour(current ? active : outline, 1.f, current ? 2 : 1);
		gl.SetFillColour(*wxBLACK, 0.f);

		if (index < 8) {
			DrawCorner(feature.pos, current);
		}
		else if (index < 12) {
			// Leaning: a parallelogram, always the same one. It says which way the handle
			// leans the selection - along the box or across it - and nothing more.
			//
			// It deliberately does not follow the lean in force. Read from the gesture it drew
			// upright on a line that already leaned; read from \fax it fell the wrong way and
			// by the wrong amount, because the renderer applies that inside the line, behind
			// the turn and the two scales, which conjugate it.
			bool horizontal = index == 8 || index == 10;
			float slide = horizontal ? 1.75f : -1.75f;

			Vector2D at = feature.pos;
			if (horizontal) {
				gl.DrawLine(at + Vector2D(-5.f + slide, -5.f), at + Vector2D(5.f + slide, -5.f));
				gl.DrawLine(at + Vector2D(5.f + slide, -5.f), at + Vector2D(5.f - slide, 5.f));
				gl.DrawLine(at + Vector2D(5.f - slide, 5.f), at + Vector2D(-5.f - slide, 5.f));
				gl.DrawLine(at + Vector2D(-5.f - slide, 5.f), at + Vector2D(-5.f + slide, -5.f));
			}
			else {
				gl.DrawLine(at + Vector2D(-5.f, -5.f + slide), at + Vector2D(-5.f, 5.f + slide));
				gl.DrawLine(at + Vector2D(-5.f, 5.f + slide), at + Vector2D(5.f, 5.f - slide));
				gl.DrawLine(at + Vector2D(5.f, 5.f - slide), at + Vector2D(5.f, -5.f - slide));
				gl.DrawLine(at + Vector2D(5.f, -5.f - slide), at + Vector2D(-5.f, -5.f + slide));
			}
		}
		else {
			// Positioning: the square and crosshair follow the drag tool's position handle,
			// while the arrowheads say which single axis this instance moves on.
			bool vertical = index == 12 || index == 14;
			Vector2D axis = vertical ? Vector2D(0.f, 1.f) : Vector2D(1.f, 0.f);
			Vector2D across(-axis.Y(), axis.X());
			DrawCorner(feature.pos, current);
			gl.DrawLine(feature.pos - axis * 3.f, feature.pos + axis * 3.f);
			gl.SetFillColour(current ? active : outline, .85f);
			gl.DrawTriangle(feature.pos - axis * 4.f,
				feature.pos - axis + across * 2.f,
				feature.pos - axis - across * 2.f);
			gl.DrawTriangle(feature.pos + axis * 4.f,
				feature.pos + axis + across * 2.f,
				feature.pos + axis - across * 2.f);
		}
		++index;
	}
	gl.SetFillColour(*wxBLACK, 0.f);
}

bool VisualToolTransform::FreePointInside(Vector2D point) const {
	Vector2D original[4], quad[4];
	if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, original);
	else std::copy(frame_quad, frame_quad + 4, original);
	for (int i = 0; i < 4; ++i) quad[i] = MapPoint(original[i]);

	int positive = 0, negative = 0;
	for (int i = 0; i < 4; ++i) {
		Vector2D edge = quad[(i + 1) % 4] - quad[i];
		Vector2D reach = point - quad[i];
		double side = edge.X() * reach.Y() - edge.Y() * reach.X();
		if (side > 0) ++positive;
		else if (side < 0) ++negative;
	}
	return !(positive && negative);
}

void VisualToolTransform::DrawFreeRotationGuide() {
	if (!mouse_pos || mouse_pos.Y() < TopBarHeight() || active_feature) return;
	if (free_hold_mode != FreeHoldMode::Rotate && FreePointInside(ToScriptCoords(mouse_pos))) return;

	Vector2D pivot = FromScriptCoords(MapPoint(FrameOrigin()));
	Vector2D reach = mouse_pos - pivot;
	if (reach.Len() < 18.f) return;

	wxColour colour = to_wx(highlight_color_secondary_opt->GetColor());
	gl.SetLineColour(colour, .9f, 1);
	gl.DrawLine(pivot, mouse_pos);

	Vector2D direction = reach.Unit();
	Vector2D above(direction.Y(), -direction.X());
	// Keep the rotation glyph on the visually upper side of the guide. Its arc has a
	// six-pixel radius, so nine pixels leaves a small gap instead of crossing the line.
	if (above.Y() > 0.f || (std::abs(above.Y()) < .001f && above.X() > 0.f)) above = above * -1.f;
	Vector2D icon = mouse_pos - direction * 14.f + above * 9.f;
	const int steps = 12;
	const double sweep = 285.0;
	Vector2D previous;
	for (int step = 0; step <= steps; ++step) {
		double radians = (step * sweep / steps - 45.0) * pi / 180.0;
		Vector2D at = icon + Vector2D((float)(std::cos(radians) * 6.0),
			(float)(std::sin(radians) * 6.0));
		if (step) gl.DrawLine(previous, at);
		previous = at;
	}
	double radians = (sweep - 45.0) * pi / 180.0;
	Vector2D along((float)-std::sin(radians), (float)std::cos(radians));
	Vector2D outward((float)std::cos(radians), (float)std::sin(radians));
	gl.SetFillColour(colour, .9f);
	gl.DrawTriangle(previous + along * 4.f, previous + outward * 3.f - along * 2.f,
		previous - outward * 3.f - along * 2.f);
	gl.SetFillColour(*wxBLACK, 0.f);
}

// ------------------------------------------------------------------------ the dragging

void VisualToolTransform::Commit(wxString) {
	// Nothing. A drag has nothing to write yet, and committing would send the video back
	// to the file - which is where the preview was being lost: every mouse move pushed the
	// reshaped copies to the video, and the framework's commit right afterwards replaced
	// them with the untouched lines again. Only Apply commits, through the base directly.
}

bool VisualToolTransform::InitializeDrag(VisualDraggableFeature *feature) {
	if (!Active()) return false;
	if (auto_perspective) {
		SetSelection(feature, true);
		PushHistory();
		for (int i = 0; i < 4; ++i) hold_corners[i] = corners[i];
		hold_start = ToScriptCoords(feature->pos);
		return true;
	}

	if (mode == VisualToolTransformMode::Free) {
		// A handle here does not stand for itself, it stands for the whole gesture - so only
		// one of them may be dragged at a time. The framework drags everything that is
		// selected, which is why the selection is cut down to the one being grabbed.
		SetSelection(feature, true);

		int index = 0;
		for (auto& other : features) {
			if (&other == feature) break;
			++index;
		}

		// Fold what has been done so far into the starting point. Without this, a scale
		// measured from one corner would still be in force when another corner is grabbed,
		// and the box would jump the moment it was touched.
		PushHistory();

		// The leaning handles, and only those: the renderer leans each row from its own
		// corner, so until the rows are lines of their own there is no lean that can be
		// written for them. Recorded first, so a step back puts the lines together again.
		if (index >= 8 && index < 12) EnsureShearSplit();

		RebaseGesture();
		if (index >= 12)
			gesture_start = ToScriptCoords(feature->pos);
		else if (index >= 8) {
			// A leaning handle is drawn a little way beyond the side it leans, so the mouse
			// does not start on the point the lean is measured from. The difference is kept
			// here and taken off every reading, or the first movement of the drag would jump
			// by the whole of it - which on a frame that already leans is a good deal.
			Vector2D grabbed, anchor;
			int role = 0;
			HandleRole(index, grabbed, anchor, role);
			gesture_start = FrameInverse(ToScriptCoords(feature->pos)) - ShapePoint(grabbed);
		}
	}
	// The warp's move handle stands for the whole mesh rather than for a point of it, so what
	// the mesh was and where the handle started are what every mouse move is measured against.
	if (mode == VisualToolTransformMode::Warp) {
		int index = 0;
		for (auto& other : features) {
			if (&other == feature) break;
			++index;
		}
		if (index != warp_move_handle)
			if (auto *move = FeatureAt(warp_move_handle)) sel_features.erase(move);
		if (index == warp_move_handle) {
			// This is an action handle, not a mesh control point. It may be dragged, but it
			// must not join a point selection or move together with selected warp points.
			SetSelection(feature, true);
			hold_net = net;
			hold_start = ToScriptCoords(feature->pos);
		}
	}

	// Recorded before the drag changes anything, so undo lands on the shape as it was when
	// the drag started. The free transform has already done this above, together with
	// folding the previous gesture in.
	if (mode != VisualToolTransformMode::Free) PushHistory();
	return true;
}

void VisualToolTransform::UpdateDrag(VisualDraggableFeature *feature) {
	if (!Active()) return;
	if (auto_perspective) {
		size_t index = 0;
		for (auto& other : features) {
			if (&other == feature) break;
			++index;
		}

		if (source_feature_first != no_feature) {
			if (index >= source_feature_first && index < source_feature_first + 4) {
				source_corners[index - source_feature_first] = ToScriptCoords(feature->pos);
				source_moved = true;
				SyncFeatures();
				Rebuild();
				return;
			}
		}
		if (target_feature_first != no_feature) {
			size_t target_count = touched ? 4 : auto_perspective_points.size();
			if (index >= target_feature_first && index < target_feature_first + target_count) {
				size_t target = index - target_feature_first;
				Vector2D next = ToScriptCoords(feature->pos);
				if (!touched) {
					auto_perspective_points[target] = next;
					SyncFeatures();
					parent->Render();
					return;
				}

				std::vector<Vector2D> candidate(corners, corners + 4);
				candidate[target] = next;
				if (!DirectedQuad(candidate)) {
					SyncFeatures();
					return;
				}
				corners[target] = next;
				SyncFeatures();
				Rebuild();
				return;
			}
		}
		if (index != target_move_feature) return;

		auto inverse = typesetting::QuadInverseMap(box, hold_corners);
		auto plane = typesetting::QuadMap(box, hold_corners);
		Vector2D moved = inverse(ToScriptCoords(feature->pos)) - inverse(hold_start);
		Vector2D source[4], next[4];
		box.Corners(source);
		for (int i = 0; i < 4; ++i) {
			Vector2D shifted = source[i] + moved;
			if (typesetting::QuadDepth(box, hold_corners, shifted) <= 1e-4) return;
			next[i] = plane(shifted);
			if (!std::isfinite(next[i].X()) || !std::isfinite(next[i].Y()) ||
				std::abs(next[i].X()) > 1e5f || std::abs(next[i].Y()) > 1e5f) return;
		}
		std::copy(next, next + 4, corners);
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	// Only the handle under the mouse says anything in the free transform, and it says it
	// about the whole box.
	if (mode == VisualToolTransformMode::Free && active_feature && feature != active_feature)
		return;
	if ((mode == VisualToolTransformMode::Arch || mode == VisualToolTransformMode::Warp) &&
		sel_features.size() > 1) {
		// The framework has already moved every selected feature to its drag target. Consume
		// all of those targets before SyncFeatures writes their displayed positions back.
		if (feature != *sel_features.begin()) return;
		std::vector<std::pair<int, Vector2D>> targets;
		int at = 0;
		for (auto& candidate : features) {
			if (sel_features.count(&candidate))
				targets.emplace_back(at, ToScriptCoords(candidate.pos));
			++at;
		}
		for (auto const& target : targets) MoveHandle(target.first, target.second);
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	int index = 0;
	for (auto& other : features) {
		if (&other == feature) break;
		++index;
	}
	// Back through the frame first: a gesture is measured in the space the box was read in,
	// and the mouse is on screen.
	MoveHandle(index, mode == VisualToolTransformMode::Free && index < 12 ?
		FrameInverse(ToScriptCoords(feature->pos)) : ToScriptCoords(feature->pos));

	// All of them, not just the one being dragged: the others belong to a box that has just
	// changed shape, and they were being left behind.
	SyncFeatures();

	touched = true;
	Rebuild();
}

bool VisualToolTransform::InitializeHold() {
	if (auto_perspective) return false;
	if (mode == VisualToolTransformMode::Free) {
		// Inside the box moves it; the otherwise empty area outside turns it. Measuring the
		// turn as an angle naturally makes a given mouse movement finer farther from the box.
		if (tag_lines.empty()) return false;
		Vector2D at = ToScriptCoords(mouse_pos);

		PushHistory();
		RebaseGesture();
		if (FreePointInside(at)) {
			free_hold_mode = FreeHoldMode::Move;
			gesture_start = FrameInverse(at);
		}
		else {
			// Measured where it is seen, about the middle of the frame as displayed. Measuring
			// it inside the frame instead made a drag over the same arc turn by different
			// amounts once the selection had been stretched unevenly.
			Vector2D from = at - MapPoint(FrameOrigin());
			if (from.Len() < 1e-3) return false;
			free_hold_mode = FreeHoldMode::Rotate;
			gesture_start_angle = (float)(std::atan2(from.Y(), from.X()) * 180.0 / pi);
		}
		return true;
	}

	if (mode == VisualToolTransformMode::Distort) {
		// The same as the free transform: anywhere inside what is on screen takes hold of the
		// whole of it, which is the one gesture that needs no handle.
		if (tag_lines.empty()) return false;
		Vector2D at = ToScriptCoords(mouse_pos);

		// Inside means on the same side of all four edges. Which side that is depends on which
		// way round the corners have ended up after being dragged about, so either will do as
		// long as it is the same one every time.
		int one_way = 0, the_other = 0;
		for (int i = 0; i < 4; ++i) {
			Vector2D edge = corners[(i + 1) % 4] - corners[i];
			Vector2D reach = at - corners[i];
			double side = edge.X() * reach.Y() - edge.Y() * reach.X();
			if (side > 0) ++one_way;
			else if (side < 0) ++the_other;
		}
		if (one_way && the_other) return false;

		PushHistory();
		for (int i = 0; i < 4; ++i) hold_corners[i] = corners[i];
		hold_start = at;
		return true;
	}

	// Only the arch and the warp have a mesh to grab; the rest is handles and nothing else.
	if (!editor || mode == VisualToolTransformMode::Distort ||
		mode == VisualToolTransformMode::Free) return false;

	Vector2D control[16];
	typesetting::WarpControls(net, control);

	// A tolerance in script units for what counts as "on the mesh", worked out from a few
	// pixels on screen - so it stays the same size to the eye at any zoom.
	Vector2D at = ToScriptCoords(mouse_pos);
	Vector2D nearby = ToScriptCoords(mouse_pos + Vector2D(8.f, 0.f));
	double tolerance = std::max<double>((nearby - at).Len(), 1.0);

	if (!typesetting::WarpLocate(control, at, tolerance, hold_u, hold_v)) return false;

	PushHistory();
	hold_net = net;

	// The arch has no handle for the middle of the mesh, so taking hold of it there moves the
	// whole thing instead - the same gesture the free transform and the distort have.
	if (mode == VisualToolTransformMode::Arch) {
		hold_start = at;
		return true;
	}

	hold_origin = typesetting::WarpPoint(control, hold_u, hold_v);
	return true;
}

void VisualToolTransform::UpdateHold() {
	if (mode == VisualToolTransformMode::Free) {
		if (free_hold_mode == FreeHoldMode::Move)
			gesture_move = FrameInverse(ToScriptCoords(mouse_pos)) - gesture_start;
		else if (free_hold_mode == FreeHoldMode::Rotate) {
			// A turn goes on the outside of everything already done, the same way the slider
			// puts it there. Inside, as one more gesture, it would be conjugated by whatever
			// stretch had been folded into the frame - and a rotation inside an uneven stretch
			// is not a rotation at all: it leans and squashes the shape as it goes round.
			Vector2D from = ToScriptCoords(mouse_pos) - MapPoint(FrameOrigin());
			if (from.Len() < 1e-3) return;
			double now = std::atan2(from.Y(), from.X()) * 180.0 / pi;
			double step = gesture_start_angle - now;
			while (step > 180.0) step -= 360.0;
			while (step < -180.0) step += 360.0;
			if (std::abs(step) > 1e-9) {
				RebaseKeepingShear();
				frame_linear = Multiply(Turn(step), frame_linear);
				gesture_start_angle = (float)now;
			}
		}
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	if (mode == VisualToolTransformMode::Distort) {
		// Prefer the perspective already authored on the selected rows. If there is none, the
		// distortion's own quadrilateral is the plane, which becomes projective as soon as one
		// of its corners is moved.
		typesetting::OrientedBox move_source = box;
		Vector2D move_corners[4];
		if (!PerspectiveMovePlane(hold_corners, move_source, move_corners))
			std::copy(hold_corners, hold_corners + 4, move_corners);

		auto inverse = typesetting::QuadInverseMap(move_source, move_corners);
		auto plane = typesetting::QuadMap(move_source, move_corners);
		Vector2D moved = inverse(ToScriptCoords(mouse_pos)) - inverse(hold_start);
		Vector2D next[4];
		for (int i = 0; i < 4; ++i) {
			Vector2D shifted = inverse(hold_corners[i]) + moved;
			// Do not let a drag cross the plane's vanishing line, where coordinates turn inside
			// out and become unbounded. The last valid position remains on screen instead.
			if (typesetting::QuadDepth(move_source, move_corners, shifted) <= 1e-4) return;
			next[i] = plane(shifted);
			if (!std::isfinite(next[i].X()) || !std::isfinite(next[i].Y()) ||
				std::abs(next[i].X()) > 1e5f || std::abs(next[i].Y()) > 1e5f) return;
		}
		std::copy(next, next + 4, corners);
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	if (!editor || mode == VisualToolTransformMode::Distort ||
		mode == VisualToolTransformMode::Free) return;

	// Always from where the gesture began, never from the last frame of it: applying one
	// small delta after another would let rounding walk the mesh away.
	net = hold_net;

	if (mode == VisualToolTransformMode::Arch) {
		Vector2D moved = ToScriptCoords(mouse_pos) - hold_start;
		// The corners and their handles are the whole of the shape here; what dragging the mesh
		// added to the middle is a difference from those, so it comes along on its own.
		for (auto& point : net.corner) point = point + moved;
		for (auto& point : net.tangent) point = point + moved;
		SyncFeatures();
		touched = true;
		Rebuild();
		return;
	}

	typesetting::WarpDragInside(net, hold_u, hold_v, ToScriptCoords(mouse_pos) - hold_origin);
	SyncFeatures();
	touched = true;
	Rebuild();
}

void VisualToolTransform::EndHold() {
	free_hold_mode = FreeHoldMode::None;
}

void VisualToolTransform::OnMouseEvent(wxMouseEvent& event) {
	if (box_selecting) {
		mouse_pos = event.GetPosition();
		if (event.LeftIsDown()) {
			parent->Render();
			return;
		}

		box_selecting = false;
		Vector2D low = box_select_start.Min(mouse_pos);
		Vector2D high = box_select_start.Max(mouse_pos);
		if (mode == VisualToolTransformMode::Warp)
			if (auto *move = FeatureAt(warp_move_handle)) sel_features.erase(move);
		if (!box_select_add) sel_features.clear();
		size_t index = 0;
		for (auto& feature : features) {
			if (mode == VisualToolTransformMode::Warp && index++ == warp_move_handle)
				continue;
			if (feature.pos.X() >= low.X() && feature.pos.X() <= high.X() &&
				feature.pos.Y() >= low.Y() && feature.pos.Y() <= high.Y())
				SetSelection(&feature, false);
		}
		if (parent->HasCapture()) parent->ReleaseMouse();
		parent->SetFocus();
		parent->Render();
		return;
	}

	if (Active() && !dragging && !holding) {
		Vector2D point(event.GetPosition());
		auto action = ActionAt(point);
		if (action != VisualToolTransformAction::None || point.Y() < TopBarHeight()) {
			// The bar swallows what happens over it, so a button does not also start a
			// drag on a handle that happens to lie underneath.
			if (hovered_action != action) {
				hovered_action = action;
				parent->Render();
			}
			if (event.LeftDown() && action != VisualToolTransformAction::None)
				Perform(action);
			return;
		}
	}
	if (hovered_action != VisualToolTransformAction::None) {
		hovered_action = VisualToolTransformAction::None;
		parent->Render();
	}
	if (!auto_perspective &&
		(mode == VisualToolTransformMode::Arch || mode == VisualToolTransformMode::Warp) &&
		event.LeftDown()) {
		Vector2D point(event.GetPosition());
		bool on_handle = std::any_of(features.begin(), features.end(),
			[&](VisualDraggableFeature const& feature) { return feature.IsMouseOver(point); });
		bool on_mesh = false;
		if (!on_handle) {
			Vector2D control[16];
			typesetting::WarpControls(net, control);
			Vector2D script = ToScriptCoords(point);
			Vector2D nearby = ToScriptCoords(point + Vector2D(8.f, 0.f));
			double u = 0, v = 0;
			on_mesh = typesetting::WarpLocate(control, script,
				std::max<double>((nearby - script).Len(), 1.0), u, v);
		}
		if (!on_handle && !on_mesh) {
			box_selecting = true;
			box_select_add = event.CmdDown();
			box_select_start = point;
			mouse_pos = point;
			parent->CaptureMouse();
			parent->Render();
			return;
		}
	}
	if (auto_perspective && event.LeftDown()) {
		Vector2D point(event.GetPosition());
		bool on_handle = std::any_of(features.begin(), features.end(), [&](VisualDraggableFeature& feature) {
				return feature.IsMouseOver(point);
			});
		if (on_handle || touched) {
			VisualTool<VisualDraggableFeature>::OnMouseEvent(event);
			return;
		}
		// A perspective quad routinely has corners past the frame edge, so a click outside
		// the video is accepted; the top bar already returned above.
		AddAutoPerspectivePoint(ToScriptCoords(point));
		return;
	}
	VisualTool<VisualDraggableFeature>::OnMouseEvent(event);
	if (mode == VisualToolTransformMode::Warp && !event.LeftIsDown())
		if (auto *move = FeatureAt(warp_move_handle)) sel_features.erase(move);
}

bool VisualToolTransform::OnMouseWheel(wxMouseEvent& event) {
	// The native preview bar handles its own hover wheel events. This is the equivalent
	// path for the in-video fallback bar, and consumes the wheel so it cannot also seek.
	if (event.GetWheelAxis() == wxMOUSE_WHEEL_VERTICAL &&
		ActionAt(Vector2D(event.GetPosition())) == VisualToolTransformAction::UniformSize) {
		int rotation = event.GetWheelRotation();
		int delta = event.GetWheelDelta();
		if (rotation && delta) {
			int notches = rotation / delta;
			if (!notches) notches = rotation > 0 ? 1 : -1;
			UpdateUniformSize(uniform_size + notches);
		}
		return false;
	}
	return VisualTool<VisualDraggableFeature>::OnMouseWheel(event);
}

bool VisualToolTransform::HandleKey(int key, bool control, bool shift) {
	if (!Active()) return false;
	if (control && (key == 'Z' || key == 'Y')) {
		bool redo = key == 'Y' || shift;
		if (redo ? RedoHistory() : UndoHistory()) return true;
		// With nothing left to undo the guided page still keeps the keys: behind an unapplied
		// preview they would otherwise reach the file's own history.
		if (auto_perspective) return true;
		// Nothing left to step back to. Undoing past the start of a session would undo
		// whatever the user did before it, with a preview still on screen to confuse them,
		// so it closes the preview instead.
		if (!redo) Reject();
		return true;
	}
	if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
		Perform(VisualToolTransformAction::Apply);
		return true;
	}
	if (key == WXK_ESCAPE) {
		Perform(VisualToolTransformAction::Cancel);
		return true;
	}
	if (mode == VisualToolTransformMode::Free && !control &&
		(key == WXK_LEFT || key == WXK_RIGHT || key == WXK_UP || key == WXK_DOWN)) {
		Vector2D step(
			key == WXK_LEFT ? -1.f : key == WXK_RIGHT ? 1.f : 0.f,
			key == WXK_UP ? -1.f : key == WXK_DOWN ? 1.f : 0.f);
		PushHistory();
		RebaseGesture();
		frame_offset = frame_offset + step;
		SyncFeatures();
		touched = true;
		Rebuild();
		return true;
	}
	return false;
}

void VisualToolTransform::OnCharHook(wxKeyEvent& event) {
	if (!HandleKey(event.GetKeyCode(), event.CmdDown(), event.ShiftDown()))
		event.Skip();
}

bool VisualToolTransform::OnKeyEvent(wxKeyEvent& event) {
	return HandleKey(event.GetKeyCode(), event.CmdDown(), event.ShiftDown());
}

void VisualToolTransform::OnLineChanged() {
	// Another active line is another job, and the shapes being held belong to this one.
	ExitTool();
}

void VisualToolTransform::OnCoordinateSystemsChanged() {
	// The handles are kept in script coordinates, so a new mapping to the screen means the
	// same handles at different pixels - not new handles, and certainly not a reason to go
	// back to the file and lose the reshaping.
	SyncFeatures();
	parent->Render();
}

void VisualToolTransform::DoRefresh() {
	// A refresh in the middle of a gesture would re-read the shape, and the box and the
	// handles belong to the drawings as they were.
	if (dragging || holding) return;
	Collect();
}

void VisualToolTransform::Draw() {
	if (!Active()) return;

	wxColour line_colour = to_wx(line_color_primary_opt->GetColor());

	auto screen = [&](Vector2D point) { return FromScriptCoords(point); };

	// The whole frame is dimmed while the reshaping is being worked out - fainter than the
	// vector clip dims what it cuts away, because here the picture underneath is the thing
	// being judged. Everything the tool draws comes afterwards, so none of it is dimmed.
	gl.SetLineColour(*wxBLACK, 0.f, 1);
	gl.SetFillColour(*wxBLACK,
		static_cast<float>(shaded_area_alpha_opt->GetDouble() * .55));
	gl.DrawRectangle(video_pos, video_pos + video_size);
	gl.SetFillColour(*wxBLACK, 0.f);

	if (auto_perspective) {
		DrawAutoPerspectiveSource();
		DrawAutoPerspectivePath();
		DrawTopBar();
		return;
	}

	// The mesh, in thin dashed red. Dashed by drawing every other piece of the curve
	// rather than by dashing each piece, which at this size would come out solid.
	auto patch_curve = [&](Vector2D const control[16], bool along_u, double at) {
		const int steps = 24;
		Vector2D previous = screen(along_u ? typesetting::WarpPoint(control, 0, at)
		                                   : typesetting::WarpPoint(control, at, 0));
		for (int step = 1; step <= steps; ++step) {
			double t = (double)step / steps;
			Vector2D next = screen(along_u ? typesetting::WarpPoint(control, t, at)
			                               : typesetting::WarpPoint(control, at, t));
			if (step % 2) gl.DrawLine(previous, next);
			previous = next;
		}
	};

	if (mode == VisualToolTransformMode::Free) {
		// The box as it now stands, and where it started - so a scale or a turn can be seen
		// against what it was.
		// Only where the box is now. Showing where it was as well left a stale rectangle
		// lying about on screen with nothing to say.
		Vector2D original[4], now[4];
		if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, original);
		else std::copy(frame_quad, frame_quad + 4, original);
		for (int i = 0; i < 4; ++i) now[i] = MapPoint(original[i]);

		gl.SetLineColour(mesh_colour, 1.f, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawDashedLine(screen(now[i]), screen(now[(i + 1) % 4]), 5.f);
	}
	else if (mode == VisualToolTransformMode::Distort) {
		// The shape the drawings started in, and the quadrilateral the corners now describe.
		Vector2D outline[4];
		if (TextBoxMode()) std::copy(textbox_original_corners, textbox_original_corners + 4, outline);
		else std::copy(frame_quad, frame_quad + 4, outline);
		gl.SetLineColour(line_colour, .6f, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawDashedLine(screen(outline[i]), screen(outline[(i + 1) % 4]), 6.f);

		gl.SetLineColour(mesh_colour, 1.f, 1);
		for (int i = 0; i < 4; ++i)
			gl.DrawDashedLine(screen(corners[i]), screen(corners[(i + 1) % 4]), 5.f);
	}
	else {
		Vector2D control[16];
		typesetting::WarpControls(net, control);
		gl.SetLineColour(mesh_colour, 1.f, 1);

		if (mode == VisualToolTransformMode::Arch) {
			// Only the frame: the arch bends the four edges, and a grid inside it would say
			// nothing they do not.
			patch_curve(control, true, 0);
			patch_curve(control, true, 1);
			patch_curve(control, false, 0);
			patch_curve(control, false, 1);

			// And the thin arms from each corner to the handles that steer its curves, so it is
			// plain which handle belongs to which edge.
			for (int corner = 0; corner < 4; ++corner) {
				int first, second;
				typesetting::WarpCornerTangents(corner, first, second);
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[first]));
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[second]));
			}
		}
		else {
			// The four lines each way that divide the patch into nine cells.
			for (int line = 0; line < 4; ++line) {
				patch_curve(control, true, line / 3.0);
				patch_curve(control, false, line / 3.0);
			}

			// And the thin arms from each corner to the handles that steer its curves.
			for (int corner = 0; corner < 4; ++corner) {
				int first, second;
				typesetting::WarpCornerTangents(corner, first, second);
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[first]));
				gl.DrawLine(screen(net.corner[corner]), screen(net.tangent[second]));
			}
		}
	}

	if (mode == VisualToolTransformMode::Free) {
		DrawFreeRotationGuide();
		DrawFreeHandles();
	}
	else DrawShapeHandles();
	if (box_selecting) {
		Vector2D low = box_select_start.Min(mouse_pos);
		Vector2D high = box_select_start.Max(mouse_pos);
		wxColour selection = to_wx(highlight_color_secondary_opt->GetColor());
		gl.SetLineColour(selection, 1.f, 1);
		gl.SetFillColour(selection, .12f);
		gl.DrawRectangle(low, high);
		gl.SetFillColour(*wxBLACK, 0.f);
	}
	DrawTopBar();
}
