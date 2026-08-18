// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

/// @file visual_tool_drag.cpp
/// @brief Position all visible subtitles by dragging visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_drag.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <boost/range/algorithm/binary_search.hpp>

#include <wx/dcmemory.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/toolbar.h>

static const DraggableFeatureType DRAG_ORIGIN = DRAG_BIG_TRIANGLE;
static const DraggableFeatureType DRAG_START = DRAG_BIG_SQUARE;
static const DraggableFeatureType DRAG_END = DRAG_BIG_CIRCLE;

namespace {
	constexpr int center_menu_horizontal = 1200;
	constexpr int center_menu_vertical = 1201;
	constexpr int center_menu_both = 1202;

	wxString center_mode_label(VisualToolDragCenterMode mode) {
		switch (mode) {
			case VisualToolDragCenterMode::Horizontal: return _("Center horizontally");
			case VisualToolDragCenterMode::Vertical: return _("Center vertically");
			case VisualToolDragCenterMode::Both: return _("Center horizontally and vertically");
		}
		return wxString();
	}

	wxBitmap center_mode_bitmap(VisualToolDragCenterMode mode, int size, bool dropdown = true) {
		wxBitmap bitmap(size, size, 24);
		wxMemoryDC dc(bitmap);
		wxColour background = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
		wxColour colour = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
		dc.SetBackground(wxBrush(background));
		dc.Clear();
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		dc.SetPen(wxPen(colour, std::max(1, size / 12)));

		int left = std::max(1, size / 10);
		int right = size - left - 1;
		int top = std::max(2, size / 5);
		int bottom = size - top - 1;
		dc.DrawRectangle(left, top, right - left, bottom - top);

		int centre_x = size / 2;
		int centre_y = size / 2;
		int arrow_left = left + std::max(2, size / 8);
		int arrow_right = right - std::max(2, size / 8);
		int arrow_top = top + std::max(2, size / 8);
		int arrow_bottom = bottom - std::max(2, size / 8);
		int head = std::max(2, size / 7);
		if (mode == VisualToolDragCenterMode::Horizontal || mode == VisualToolDragCenterMode::Both) {
			dc.DrawLine(arrow_left, centre_y, arrow_right, centre_y);
			dc.DrawLine(arrow_left, centre_y, arrow_left + head, centre_y - head);
			dc.DrawLine(arrow_left, centre_y, arrow_left + head, centre_y + head);
			dc.DrawLine(arrow_right, centre_y, arrow_right - head, centre_y - head);
			dc.DrawLine(arrow_right, centre_y, arrow_right - head, centre_y + head);
		}
		if (mode == VisualToolDragCenterMode::Vertical || mode == VisualToolDragCenterMode::Both) {
			dc.DrawLine(centre_x, arrow_top, centre_x, arrow_bottom);
			dc.DrawLine(centre_x, arrow_top, centre_x - head, arrow_top + head);
			dc.DrawLine(centre_x, arrow_top, centre_x + head, arrow_top + head);
			dc.DrawLine(centre_x, arrow_bottom, centre_x - head, arrow_bottom - head);
			dc.DrawLine(centre_x, arrow_bottom, centre_x + head, arrow_bottom - head);
		}

		if (dropdown) {
			int corner = std::max(5, size / 4);
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.SetBrush(wxBrush(background));
			dc.DrawRectangle(size - corner, size - corner, corner, corner);
			wxPoint triangle[]{{size - corner + 1, size - corner + 1},
				{size - 1, size - corner + 1}, {size - 1, size - 1}};
			dc.SetBrush(wxBrush(colour));
			dc.DrawPolygon(3, triangle);
		}

		dc.SelectObject(wxNullBitmap);
		return bitmap;
	}

}

#define COMMAND_NAME(id) ( \
	id == DRAG_LOCK ? "video/tool/drag/change" : \
	"" \
)

VisualToolDrag::VisualToolDrag(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualToolDragDraggableFeature>(parent, context)
{
	connections.push_back(c->selectionController->AddSelectionListener(&VisualToolDrag::OnSelectedSetChanged, this));
	auto const& sel_set = c->selectionController->GetSelectedSet();
	selection.insert(begin(selection), begin(sel_set), end(sel_set));
}

VisualToolDrag::~VisualToolDrag() {
	parent->SetCursor(wxNullCursor);
	if (!toolbar) return;
	toolbar->Unbind(wxEVT_TOOL, &VisualToolDrag::OnSubTool, this);
	toolbar->Unbind(wxEVT_TOOL_RCLICKED, &VisualToolDrag::OnSubTool, this);
}

void VisualToolDrag::AddTool(int id) {
	cmd::Command *command = cmd::get(COMMAND_NAME(id));
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolbar->AddTool(id, command->StrDisplay(c), command->Icon(icon_size), command->GetTooltip("Video"));
}

void VisualToolDrag::UpdateTool(int id) {
	cmd::Command *command = cmd::get(COMMAND_NAME(id));
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();

	toolbar->SetToolLongHelp(id, command->StrDisplay(c));
	toolbar->SetToolNormalBitmap(id, command->Icon(icon_size));
	toolbar->SetToolShortHelp(id, command->GetTooltip("Video"));
}

void VisualToolDrag::SetToolbar(wxToolBar *tb) {
	toolbar = tb;
	toolbar->AddSeparator();
	toolbar->AddTool(DRAG_TOGGLE, _("Toggle between \\move and \\pos"), GETBUNDLE(visual_move_conv_move, OPT_GET("App/Toolbar Icon Size")->GetInt()));
	AddTool(DRAG_LOCK);
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolbar->AddTool(DRAG_CENTER, center_mode_label(center_mode),
		wxBitmapBundle::FromBitmap(center_mode_bitmap(center_mode, icon_size)),
		_("Center selected lines in a drawn rectangle (right-click for modes)"), wxITEM_CHECK);
	toolbar->Realize();
	toolbar->Show(true);

	toolbar->Bind(wxEVT_TOOL, &VisualToolDrag::OnSubTool, this);
	toolbar->Bind(wxEVT_TOOL_RCLICKED, &VisualToolDrag::OnSubTool, this);
}

void VisualToolDrag::UpdateToggleButtons() {
	bool to_move = true;
	if (active_line) {
		Vector2D p1, p2;
		int t1, t2;
		to_move = !GetLineMove(active_line, p1, p2, t1, t2);
	}

	if (to_move == button_is_move) return;

	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolbar->SetToolNormalBitmap(DRAG_TOGGLE, to_move ? GETBUNDLE(visual_move_conv_move, icon_size) : GETBUNDLE(visual_move_conv_pos, icon_size));
	button_is_move = to_move;
}

void VisualToolDrag::OnSubTool(wxCommandEvent &e) {
	int id = e.GetId();
	if (id == DRAG_CENTER) {
		if (e.GetEventType() == wxEVT_TOOL_RCLICKED)
			ShowCenterMenu(Vector2D(parent->ScreenToClient(wxGetMousePosition())));
		else
			SetCentering(toolbar->GetToolState(DRAG_CENTER));
		return;
	}

	if (id == DRAG_LOCK) {
		cmd::Command *command = cmd::get(COMMAND_NAME(id));
		command->operator()(c);

		return;
	}

	// Toggle \move <-> \pos
	VideoController *vc = c->videoController.get();
	for (auto line : selection) {
		Vector2D p1, p2;
		int t1, t2;

		bool has_move = GetLineMove(line, p1, p2, t1, t2);

		if (has_move)
			SetOverride(line, "\\pos", p1.PStr());
		else {
			p1 = GetLinePosition(line);
			// Round the start and end times to exact frames
			int start = vc->TimeAtFrame(vc->FrameAtTime(line->Start, agi::vfr::START)) - line->Start;
			int end = vc->TimeAtFrame(vc->FrameAtTime(line->Start, agi::vfr::END)) - line->Start;
			SetOverride(line, "\\move", agi::format("(%s,%s,%d,%d)", p1.Str(), p1.Str(), start, end));
		}
	}

	Commit();
	OnFileChanged();
	UpdateToggleButtons();
}

void VisualToolDrag::UpdateCenterButton() {
	if (!toolbar) return;
	int icon_size = OPT_GET("App/Toolbar Icon Size")->GetInt();
	toolbar->SetToolNormalBitmap(DRAG_CENTER,
		wxBitmapBundle::FromBitmap(center_mode_bitmap(center_mode, icon_size)));
	toolbar->SetToolShortHelp(DRAG_CENTER,
		_("Center selected lines in a drawn rectangle (right-click for modes)"));
	toolbar->ToggleTool(DRAG_CENTER, centering);
	toolbar->Realize();
}

void VisualToolDrag::ShowCenterMenu(Vector2D position) {
	wxMenu menu;
	auto append_mode = [&](int id, VisualToolDragCenterMode mode) {
		auto item = menu.AppendRadioItem(id, center_mode_label(mode));
		item->SetBitmap(center_mode_bitmap(mode, 20, false));
		item->Check(center_mode == mode);
	};
	append_mode(center_menu_horizontal, VisualToolDragCenterMode::Horizontal);
	append_mode(center_menu_vertical, VisualToolDragCenterMode::Vertical);
	append_mode(center_menu_both, VisualToolDragCenterMode::Both);

	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(position.X()), static_cast<int>(position.Y())));
	if (selected == center_menu_horizontal)
		center_mode = VisualToolDragCenterMode::Horizontal;
	else if (selected == center_menu_vertical)
		center_mode = VisualToolDragCenterMode::Vertical;
	else if (selected == center_menu_both)
		center_mode = VisualToolDragCenterMode::Both;
	else
		return;

	SetCentering(true);
	UpdateCenterButton();
}

void VisualToolDrag::SetCentering(bool enabled) {
	centering = enabled;
	drawing_center_rectangle = false;
	center_rectangle_start = Vector2D();
	center_rectangle_end = Vector2D();
	active_feature = nullptr;
	parent->SetCursor(enabled ? wxCursor(wxCURSOR_CROSS) : wxNullCursor);
	UpdateCenterButton();
	parent->Render();
}

void VisualToolDrag::ApplyCentering() {
	Vector2D centre = ToScriptCoords((center_rectangle_start + center_rectangle_end) / 2.f);
	bool changed = false;
	for (auto line : c->selectionController->GetSelectedSet()) {
		Vector2D move_start, move_end;
		int move_start_time, move_end_time;
		if (GetLineMove(line, move_start, move_end, move_start_time, move_end_time)) continue;
		Vector2D current = GetLinePosition(line);
		Vector2D target = current;
		if (center_mode == VisualToolDragCenterMode::Horizontal || center_mode == VisualToolDragCenterMode::Both)
			target = Vector2D(centre.X(), target.Y());
		if (center_mode == VisualToolDragCenterMode::Vertical || center_mode == VisualToolDragCenterMode::Both)
			target = Vector2D(target.X(), centre.Y());
		SetOverride(line, "\\pos", target.PStr());
		changed = true;
	}
	if (changed) {
		Commit(_("positioning"));
		OnFileChanged();
	}
}

void VisualToolDrag::OnCenterMouseEvent(wxMouseEvent &event) {
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	mouse_pos = event.GetPosition();
	if (event.Leaving() && !drawing_center_rectangle) {
		mouse_pos = Vector2D();
		parent->Render();
		return;
	}

	if (event.LeftDown()) {
		center_rectangle_start = mouse_pos;
		center_rectangle_end = mouse_pos;
		drawing_center_rectangle = true;
		if (!parent->HasCapture()) parent->CaptureMouse();
	}
	else if (drawing_center_rectangle) {
		center_rectangle_end = mouse_pos;
		if (event.LeftUp() || !event.LeftIsDown()) {
			drawing_center_rectangle = false;
			if (parent->HasCapture()) parent->ReleaseMouse();
			parent->SetFocus();
			Vector2D size = center_rectangle_end - center_rectangle_start;
			if (std::abs(size.X()) >= 2.f && std::abs(size.Y()) >= 2.f)
				ApplyCentering();
			center_rectangle_start = Vector2D();
			center_rectangle_end = Vector2D();
		}
	}
	parent->Render();
}

void VisualToolDrag::ApplyEmptyClickSelection() {
	if (box_selection_additive || alt_down || features.size() <= 1) return;
	Feature *active_line_feature = nullptr;
	for (auto selected : sel_features) {
		if (selected->line && selected->line == c->selectionController->GetActiveLine()) {
			active_line_feature = selected;
			break;
		}
	}
	if (!active_line_feature) return;

	auto line = active_line_feature->line;
	c->selectionController->SetSelectionAndActive({line}, line);
	auto it = c->ass->iterator_to(*line);
	auto next = it;
	++next;
	if (next != c->ass->Events.end()) {
		c->selectionController->NextLine();
		c->selectionController->PrevLine();
	}
}

void VisualToolDrag::ApplyBoxSelection() {
	Vector2D minimum = box_selection_start.Min(box_selection_end);
	Vector2D maximum = box_selection_start.Max(box_selection_end);
	std::set<Feature *> next = box_selection_additive ? box_selection_original :
		std::set<Feature *>{};
	Feature *first_hit = nullptr;
	for (auto& feature : features) {
		if (!feature.pos || feature.pos.X() < minimum.X() || feature.pos.X() > maximum.X() ||
			feature.pos.Y() < minimum.Y() || feature.pos.Y() > maximum.Y()) continue;
		next.insert(&feature);
		if (!first_hit) first_hit = &feature;
	}
	if (!first_hit && next.empty()) return;

	Selection lines;
	for (auto feature : next)
		if (feature->line) lines.insert(feature->line);
	if (lines.empty()) return;

	sel_features = std::move(next);
	AssDialogue *active = c->selectionController->GetActiveLine();
	if (!lines.count(active)) active = first_hit ? first_hit->line : *lines.begin();
	c->selectionController->SetSelectionAndActive(std::move(lines), active);
}

bool VisualToolDrag::OnBoxSelectionMouseEvent(wxMouseEvent &event) {
	if (!box_selecting) {
		if (!event.LeftDown() || event.LeftDClick()) return false;
		Vector2D position = event.GetPosition();
		for (auto& feature : features)
			if (feature.IsMouseOver(position)) return false;

		shift_down = event.ShiftDown();
		ctrl_down = event.CmdDown();
		alt_down = event.AltDown();
		mouse_pos = position;
		box_selection_start = position;
		box_selection_end = position;
		box_selection_additive = ctrl_down || shift_down;
		box_selection_original = sel_features;
		box_selecting = true;
		if (!parent->HasCapture()) parent->CaptureMouse();
		parent->Render();
		return true;
	}

	mouse_pos = event.GetPosition();
	box_selection_end = mouse_pos;
	if (event.LeftUp() || !event.LeftIsDown()) {
		Vector2D distance = box_selection_end - box_selection_start;
		box_selecting = false;
		if (parent->HasCapture()) parent->ReleaseMouse();
		parent->SetFocus();
		if (distance.SquareLen() >= 9.f)
			ApplyBoxSelection();
		else
			ApplyEmptyClickSelection();
		box_selection_original.clear();
		box_selection_start = Vector2D();
		box_selection_end = Vector2D();
	}
	parent->Render();
	return true;
}

void VisualToolDrag::OnLineChanged() {
	UpdateToggleButtons();
}

void VisualToolDrag::OnFileChanged() {
	/// @todo it should be possible to preserve the selection in some cases
	features.clear();
	sel_features.clear();
	primary = nullptr;
	active_feature = nullptr;

	for (auto& diag : c->ass->Events) {
		if (IsDisplayed(&diag, false))
			MakeFeatures(&diag);
	}

	UpdateToggleButtons();
}

void VisualToolDrag::OnFrameChanged() {
	if (primary && !IsDisplayed(primary->line, false))
		primary = nullptr;

	auto feat = features.begin();
	auto end = features.end();

	for (auto& diag : c->ass->Events) {
		if (IsDisplayed(&diag, false)) {
			// Features don't exist and should
			if (feat == end || feat->line != &diag)
				MakeFeatures(&diag, feat);
			// Move past already existing features for the line
			else
				while (feat != end && feat->line == &diag) ++feat;
		}
		else {
			// Remove all features for this line (if any)
			while (feat != end && feat->line == &diag) {
				if (&*feat == active_feature) active_feature = nullptr;
				feat->line = nullptr;
				RemoveSelection(&*feat);
				feat = features.erase(feat);
			}
		}
	}
}

template<class C, class T> static bool line_not_present(C const& set, T const& it) {
	return std::none_of(set.begin(), set.end(), [&](typename C::value_type const& cmp) {
		return cmp->line == it->line;
	});
}

void VisualToolDrag::OnSelectedSetChanged() {
	auto const& new_sel_set = c->selectionController->GetSelectedSet();
	std::vector<AssDialogue *> new_sel(begin(new_sel_set), end(new_sel_set));

	bool any_changed = false;
	for (auto it = features.begin(); it != features.end(); ) {
		bool was_selected = boost::binary_search(selection, it->line);
		bool is_selected = boost::binary_search(new_sel, it->line);
		if (was_selected && !is_selected) {
			sel_features.erase(&*it++);
			any_changed = true;
		}
		else {
			if (is_selected && !was_selected && it->type == DRAG_START && line_not_present(sel_features, it)) {
				sel_features.insert(&*it);
				any_changed = true;
			}
			++it;
		}
	}

	if (any_changed)
		parent->Render();
	selection = std::move(new_sel);
}

void VisualToolDrag::Draw() {
	DrawAllFeatures();

	// Load colors from options
	wxColour line_color = to_wx(line_color_primary_opt->GetColor());

	// Draw connecting lines
	for (auto& feature : features) {
		if (feature.type == DRAG_START) continue;

		Feature *p2 = &feature;
		Feature *p1 = feature.parent;

		// Move end marker has an arrow; origin doesn't
		bool has_arrow = p2->type == DRAG_END;
		int arrow_len = has_arrow ? 10 : 0;

		// Don't show the connecting line if the features are very close
		Vector2D direction = p2->pos - p1->pos;
		if (direction.SquareLen() < (20 + arrow_len) * (20 + arrow_len)) continue;

		direction = direction.Unit();
		// Get the start and end points of the line
		Vector2D start = p1->pos + direction * 10;
		Vector2D end = p2->pos - direction * (10 + arrow_len);

		if (has_arrow) {
			gl.SetLineColour(line_color, 0.8f, 2);

			// Arrow line
			gl.DrawLine(start, end);

			// Arrow head
			Vector2D t_half_base_w = Vector2D(-direction.Y(), direction.X()) * 4;
			gl.DrawTriangle(end + direction * arrow_len, end + t_half_base_w, end - t_half_base_w);
		}
		// Draw dashed line
		else {
			gl.SetLineColour(line_color, 0.5f, 2);
			gl.DrawDashedLine(start, end, 6);
		}
	}

	if (centering && drawing_center_rectangle) {
		wxColour rectangle_colour = to_wx(highlight_color_primary_opt->GetColor());
		gl.SetFillColour(rectangle_colour, .08f);
		gl.SetLineColour(rectangle_colour, 1.f, 2);
		gl.DrawRectangle(center_rectangle_start, center_rectangle_end);
		Vector2D minimum = center_rectangle_start.Min(center_rectangle_end);
		Vector2D maximum = center_rectangle_start.Max(center_rectangle_end);
		Vector2D centre = (minimum + maximum) / 2.f;
		gl.SetLineColour(rectangle_colour, .9f, 1);
		if (center_mode == VisualToolDragCenterMode::Horizontal || center_mode == VisualToolDragCenterMode::Both)
			gl.DrawDashedLine(Vector2D(centre.X(), minimum.Y()), Vector2D(centre.X(), maximum.Y()), 5);
		if (center_mode == VisualToolDragCenterMode::Vertical || center_mode == VisualToolDragCenterMode::Both)
			gl.DrawDashedLine(Vector2D(minimum.X(), centre.Y()), Vector2D(maximum.X(), centre.Y()), 5);
	}

	if (box_selecting) {
		Vector2D minimum = box_selection_start.Min(box_selection_end);
		Vector2D maximum = box_selection_start.Max(box_selection_end);
		wxColour colour = to_wx(highlight_color_secondary_opt->GetColor());
		gl.SetFillColour(colour, .06f);
		gl.SetLineColour(colour, 0.f, 1);
		gl.DrawRectangle(minimum, maximum);
		gl.SetLineColour(colour, 1.f, 1);
		auto draw_dashed_edge = [&](Vector2D start, Vector2D end) {
			if ((end - start).SquareLen() > .01f)
				gl.DrawDashedLine(start, end, 5);
		};
		draw_dashed_edge(minimum, Vector2D(maximum.X(), minimum.Y()));
		draw_dashed_edge(Vector2D(maximum.X(), minimum.Y()), maximum);
		draw_dashed_edge(maximum, Vector2D(minimum.X(), maximum.Y()));
		draw_dashed_edge(Vector2D(minimum.X(), maximum.Y()), minimum);
	}
}

void VisualToolDrag::MakeFeatures(AssDialogue *diag) {
	MakeFeatures(diag, features.end());
}

void VisualToolDrag::MakeFeatures(AssDialogue *diag, feature_list::iterator pos) {
	Vector2D p1 = FromScriptCoords(GetLinePosition(diag));

	// Create \pos feature
	auto feat = std::make_unique<Feature>();
	auto parent = feat.get();
	feat->pos = p1;
	feat->type = DRAG_START;
	feat->line = diag;

	if (boost::binary_search(selection, diag))
		sel_features.insert(feat.get());
	features.insert(pos, *feat.release());

	Vector2D p2;
	int t1, t2;

	// Create move destination feature
	if (GetLineMove(diag, p1, p2, t1, t2)) {
		feat = std::make_unique<Feature>();
		feat->pos = FromScriptCoords(p2);
		feat->layer = 1;
		feat->type = DRAG_END;
		feat->time = t2;
		feat->line = diag;
		feat->parent = parent;

		parent->layer = -1;
		parent->time = t1;
		parent->parent = feat.get();

		features.insert(pos, *feat.release());
	}

	// Create org feature
	if (Vector2D org = GetLineOrigin(diag)) {
		feat = std::make_unique<Feature>();
		feat->pos = FromScriptCoords(org);
		feat->layer = -1;
		feat->type = DRAG_ORIGIN;
		feat->time = 0;
		feat->line = diag;
		feat->parent = parent;
		features.insert(pos, *feat.release());
	}
}

bool VisualToolDrag::InitializeDrag(Feature *feature) {
	primary = feature;

	// Set time of clicked feature to the current frame and shift all other
	// selected features by the same amount
	if (feature->type != DRAG_ORIGIN) {
		int time = c->videoController->TimeAtFrame(frame_number) - feature->line->Start;
		int change = time - feature->time;

		for (auto feat : sel_features)
			feat->time += change;
	}
	return true;
}

void VisualToolDrag::UpdateDrag(Feature *feature) {
	int mode = OPT_GET("Tool/Drag Type")->GetInt();
	if (mode == 1)
		feature->pos = Vector2D(feature->pos, FromScriptCoords(GetLinePosition(feature->line)));
	else if (mode == 2)
		feature->pos = Vector2D(FromScriptCoords(GetLinePosition(feature->line)), feature->pos);

	if (feature->type == DRAG_ORIGIN) {
		SetOverride(feature->line, "\\org", ToScriptCoords(feature->pos).PStr());
		return;
	}

	Feature *end_feature = feature->parent;
	if (feature->type == DRAG_END)
		std::swap(feature, end_feature);

	if (feature->parent) {
		Vector2D p1;
		Vector2D p2 = ToScriptCoords(end_feature->pos);
		int t1; int t2;
		GetLineMove(feature->line, p1, p2, t1, t2);

		if (mode == 1)
			end_feature->pos = Vector2D(end_feature->pos, FromScriptCoords(p2));
		else if (mode == 2)
			end_feature->pos = Vector2D(FromScriptCoords(p2), end_feature->pos);
	}

	if (!feature->parent)
		SetOverride(feature->line, "\\pos", ToScriptCoords(feature->pos).PStr());
	else
		SetOverride(feature->line, "\\move", agi::format("(%s,%s,%d,%d)"
			, ToScriptCoords(feature->pos).Str()
			, ToScriptCoords(end_feature->pos).Str()
			, feature->time , end_feature->time));
}

void VisualToolDrag::OnDoubleClick() {
	Vector2D d = ToScriptCoords(mouse_pos) - (primary ? ToScriptCoords(primary->pos) : GetLinePosition(active_line));

	for (auto line : c->selectionController->GetSelectedSet()) {
		Vector2D p1, p2;
		int t1, t2;
		if (GetLineMove(line, p1, p2, t1, t2)) {
			if (t1 > 0 || t2 > 0)
				SetOverride(line, "\\move", agi::format("(%s,%s,%d,%d)", (p1 + d).Str(), (p2 + d).Str(), t1, t2));
			else
				SetOverride(line, "\\move", agi::format("(%s,%s)", (p1 + d).Str(), (p2 + d).Str()));
		}
		else
			SetOverride(line, "\\pos", (GetLinePosition(line) + d).PStr());

		if (Vector2D org = GetLineOrigin(line))
			SetOverride(line, "\\org", (org + d).PStr());
	}

	Commit(_("positioning"));

	OnFileChanged();
}
