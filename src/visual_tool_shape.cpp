// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "visual_tool_shape.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "dialogs.h"
#include "format.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/color.h>
#include <algorithm>
#include <cmath>
#include <wx/dcmemory.h>
#include <wx/event.h>
#include <wx/menu.h>
#include <wx/numdlg.h>
#include <wx/settings.h>
#include <wx/textdlg.h>
#include <wx/toolbar.h>

namespace {
	constexpr int shape_tool_id = 1800;
	constexpr int fill_tool_id = 1801;
	constexpr int size_tool_id = 1802;
	constexpr int freehand_tool_id = 1803;
	constexpr int radius_tool_id = 1804;
	constexpr int shape_menu_base = 1810;
	constexpr int size_menu_base = 1850;
	constexpr int radius_menu_base = 1880;
	constexpr int blur_menu_base = 1920;
	constexpr float top_bar_height = 54.f;
	constexpr double pi = 3.14159265358979323846;

	std::string decimal_string(double value, int precision) {
		std::string result = precision == 1 ? agi::format("%.1f", value) : agi::format("%.2f", value);
		while (result.size() > 1 && result.back() == '0') result.pop_back();
		if (!result.empty() && result.back() == '.') result.pop_back();
		if (result == "-0") result = "0";
		return result;
	}

	struct ShapeChoice {
		VisualShapeKind kind;
		const char *label;
	};

	constexpr ShapeChoice shape_choices[] = {
		{VisualShapeKind::Line, "Line"},
		{VisualShapeKind::Rectangle, "Rectangle"},
		{VisualShapeKind::Ellipse, "Circle"},
		{VisualShapeKind::Triangle, "Triangle"},
		{VisualShapeKind::Diamond, "Diamond"},
		{VisualShapeKind::Hexagon, "Hexagon"},
		{VisualShapeKind::Heart, "Heart"},
		{VisualShapeKind::WideHeart, "Wide heart"},
		{VisualShapeKind::Star5, "5-point star"},
		{VisualShapeKind::Star6, "6-point star"},
		{VisualShapeKind::Star8, "8-point star"},
		{VisualShapeKind::Arrow, "Arrow"},
		{VisualShapeKind::Freehand, "Freehand"}
	};

	bool is_closed(VisualShapeKind kind) {
		return kind != VisualShapeKind::Line && kind != VisualShapeKind::Freehand;
	}

	wxString shape_label(VisualShapeKind kind) {
		for (auto const& choice : shape_choices)
			if (choice.kind == kind) return wxGetTranslation(choice.label);
		return wxString();
	}

	void draw_dropdown_indicator(wxMemoryDC& dc, int size) {
		int corner_size = std::max(6, size / 4);
		dc.SetPen(*wxTRANSPARENT_PEN);
		dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
		dc.DrawRectangle(size - corner_size, size - corner_size, corner_size, corner_size);
		int right = size - 1;
		int bottom = size - 1;
		int half_width = std::max(2, corner_size / 2 - 1);
		wxPoint points[]{{right - half_width * 2, bottom - half_width * 2},
			{right, bottom - half_width * 2}, {right - half_width, bottom}};
		dc.SetBrush(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT)));
		dc.DrawPolygon(3, points);
	}

	wxBitmap shape_bitmap(VisualShapeKind kind, int size, bool filled = false, bool dropdown = false) {
		// wxMemoryDC does not update a 32-bit bitmap's alpha channel reliably on
		// Windows, which made these runtime-generated toolbar images appear as
		// empty black rectangles. Use the native button-face colour instead.
		wxBitmap bitmap(size, size, 24);
		wxMemoryDC dc(bitmap);
		dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
		dc.Clear();
		wxColour colour = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT);
		dc.SetPen(wxPen(colour, std::max(2, size / 7)));
		dc.SetBrush(filled ? wxBrush(colour) : *wxTRANSPARENT_BRUSH);
		int a = std::max(1, size / 10);
		int b = size - a - 1;
		int w = b - a;
		int h = b - a;
		switch (kind) {
			case VisualShapeKind::Line:
			dc.DrawLine(a, b, b, a);
			break;
			case VisualShapeKind::Ellipse:
			dc.DrawEllipse(a, a, w, h);
			break;
			case VisualShapeKind::Triangle: {
				wxPoint p[]{{size / 2, a}, {b, b}, {a, b}};
				dc.DrawPolygon(3, p);
				break;
			}
			case VisualShapeKind::Diamond: {
				wxPoint p[]{{size / 2, a}, {b, size / 2}, {size / 2, b}, {a, size / 2}};
				dc.DrawPolygon(4, p);
				break;
			}
			case VisualShapeKind::Heart:
			case VisualShapeKind::WideHeart: {
				wxPoint p[]{{size / 2, b}, {a, size / 2}, {a + 1, a + 2},
					{size / 3, a}, {size / 2, a + 3}, {size * 2 / 3, a},
					{b - 1, a + 2}, {b, size / 2}};
				dc.DrawPolygon(8, p);
				break;
			}
			case VisualShapeKind::Star5:
			case VisualShapeKind::Star6:
			case VisualShapeKind::Star8: {
				int tips = kind == VisualShapeKind::Star5 ? 5 : kind == VisualShapeKind::Star6 ? 6 : 8;
				std::vector<wxPoint> p;
				for (int i = 0; i < tips * 2; ++i) {
					double angle = -pi / 2 + i * pi / tips;
					double radius = (i & 1) ? size * .20 : size * .40;
					p.emplace_back(static_cast<int>(std::lround(size / 2 + std::cos(angle) * radius)),
						static_cast<int>(std::lround(size / 2 + std::sin(angle) * radius)));
				}
				dc.DrawPolygon(static_cast<int>(p.size()), p.data());
				break;
			}
			case VisualShapeKind::Arrow: {
				wxPoint p[]{{a, size / 2 - 2}, {size * 2 / 3, size / 2 - 2},
					{size * 2 / 3, a}, {b, size / 2}, {size * 2 / 3, b},
					{size * 2 / 3, size / 2 + 2}, {a, size / 2 + 2}};
				dc.DrawPolygon(7, p);
				break;
			}
			case VisualShapeKind::Freehand:
			dc.DrawLine(a, b - 1, size / 3, a + h / 2);
			dc.DrawLine(size / 3, a + h / 2, size * 2 / 3, b - h / 3);
			dc.DrawLine(size * 2 / 3, b - h / 3, b, a);
			break;
			case VisualShapeKind::Hexagon: {
				wxPoint p[]{{size / 3, a}, {size * 2 / 3, a}, {b, size / 2},
					{size * 2 / 3, b}, {size / 3, b}, {a, size / 2}};
				dc.DrawPolygon(6, p);
				break;
			}
			default:
			dc.DrawRectangle(a, a, w, h);
			break;
		}
		if (dropdown) draw_dropdown_indicator(dc, size);
		dc.SelectObject(wxNullBitmap);
		return bitmap;
	}

	wxBitmap radius_bitmap(int size, int radius, bool dropdown = true) {
		wxBitmap bitmap(size, size, 24);
		wxMemoryDC dc(bitmap);
		dc.SetBackground(wxBrush(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)));
		dc.Clear();
		dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT), std::max(2, size / 8)));
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		int a = std::max(1, size / 10);
		int extent = std::max(4, size - a * 2 - 1);
		int shown_radius = radius > 0 ? std::max(2, std::min(extent / 2, radius * extent / 32)) : 1;
		dc.DrawRoundedRectangle(a, a, extent, extent, shown_radius);
		if (dropdown) draw_dropdown_indicator(dc, size);
		dc.SelectObject(wxNullBitmap);
		return bitmap;
	}

	std::vector<Vector2D> regular_polygon(Vector2D centre, float rx, float ry,
		int count, double start_angle = -pi / 2) {
		std::vector<Vector2D> result;
		result.reserve(count);
		for (int i = 0; i < count; ++i) {
			double angle = start_angle + 2.0 * pi * i / count;
			result.emplace_back(centre.X() + static_cast<float>(std::cos(angle) * rx),
				centre.Y() + static_cast<float>(std::sin(angle) * ry));
		}
		return result;
	}

	bool can_round(VisualShapeKind kind) {
		switch (kind) {
			case VisualShapeKind::Rectangle:
			case VisualShapeKind::Triangle:
			case VisualShapeKind::Diamond:
			case VisualShapeKind::Hexagon:
			case VisualShapeKind::Star5:
			case VisualShapeKind::Star6:
			case VisualShapeKind::Star8:
			case VisualShapeKind::Arrow:
			return true;
			default:
			return false;
		}
	}

	struct RoundedCorner {
		Vector2D incoming;
		Vector2D corner;
		Vector2D outgoing;
	};

	std::vector<RoundedCorner> rounded_corners(std::vector<Vector2D> const& points, float radius) {
		std::vector<RoundedCorner> result;
		if (points.size() < 3 || radius <= 0.f) return result;
		result.reserve(points.size());
		for (size_t i = 0; i < points.size(); ++i) {
			Vector2D corner = points[i];
			Vector2D to_previous = points[(i + points.size() - 1) % points.size()] - corner;
			Vector2D to_next = points[(i + 1) % points.size()] - corner;
			float distance = std::min({radius, to_previous.Len() * .5f, to_next.Len() * .5f});
			if (distance < .01f || to_previous.Len() < .01f || to_next.Len() < .01f)
				result.push_back({corner, corner, corner});
			else
				result.push_back({corner + to_previous.Unit() * distance, corner,
					corner + to_next.Unit() * distance});
		}
		return result;
	}

	std::vector<Vector2D> rounded_polygon_points(std::vector<Vector2D> const& points, float radius) {
		auto corners = rounded_corners(points, radius);
		if (corners.empty()) return points;
		std::vector<Vector2D> result;
		result.reserve(corners.size() * 5);
		for (auto const& corner : corners) {
			result.push_back(corner.incoming);
			for (int step = 1; step <= 4; ++step) {
				float t = step / 4.f;
				float inverse = 1.f - t;
				result.push_back(corner.incoming * (inverse * inverse) +
					corner.corner * (2.f * inverse * t) + corner.outgoing * (t * t));
			}
		}
		return result;
	}

	agi::Color active_colour(AssDialogue *line, AssStyle const *style) {
		agi::Color colour = style ? style->primary : agi::Color(255, 255, 255);
		if (!line) return colour;
		for (auto const& block : line->ParseTags()) {
			if (block->GetType() != AssBlockType::OVERRIDE) continue;
			auto const& override_block = static_cast<AssDialogueBlockOverride const&>(*block);
			for (auto const& tag : override_block.Tags) {
				if ((tag.Name == "\\c" || tag.Name == "\\1c") && !tag.Params.empty() && !tag.Params[0].omitted) {
					try { colour = agi::Color(tag.Params[0].Get<std::string>()); }
					catch (...) { }
				}
			}
		}
		return colour;
	}

	void append_polygon(std::string& drawing, std::vector<Vector2D> const& points,
		double centre_x, double centre_y) {
		if (points.size() < 3) return;
		if (!drawing.empty()) drawing += " ";
		drawing += "m ";
		for (size_t i = 0; i < points.size(); ++i) {
			if (i == 1) drawing += " l ";
			else if (i > 1) drawing += " ";
			drawing += decimal_string(points[i].X() - centre_x, 1) + " " +
				decimal_string(points[i].Y() - centre_y, 1);
		}
	}

	void append_rounded_polygon(std::string& drawing, std::vector<Vector2D> const& points,
		float radius, double centre_x, double centre_y) {
		if (radius <= 0 || points.size() < 3) {
			append_polygon(drawing, points, centre_x, centre_y);
			return;
		}
		auto corners = rounded_corners(points, static_cast<float>(radius));
		if (corners.empty()) {
			append_polygon(drawing, points, centre_x, centre_y);
			return;
		}
		auto coord = [&](Vector2D point) {
			return decimal_string(point.X() - centre_x, 1) + " " +
				decimal_string(point.Y() - centre_y, 1);
		};
		if (!drawing.empty()) drawing += " ";
		drawing += "m " + coord(corners.front().incoming);
		for (size_t i = 0; i < corners.size(); ++i) {
			auto const& corner = corners[i];
			if (i > 0) drawing += " l " + coord(corner.incoming);
			Vector2D control1 = corner.incoming + (corner.corner - corner.incoming) * (2.f / 3.f);
			Vector2D control2 = corner.outgoing + (corner.corner - corner.outgoing) * (2.f / 3.f);
			drawing += " b " + coord(control1) + " " + coord(control2) + " " + coord(corner.outgoing);
		}
	}

	std::vector<Vector2D> inset_polygon(std::vector<Vector2D> const& points, float distance) {
		if (points.size() < 3 || distance <= 0.f) return points;
		float area_twice = 0.f;
		for (size_t i = 0; i < points.size(); ++i)
			area_twice += points[i].Cross(points[(i + 1) % points.size()]);
		float direction = area_twice >= 0.f ? 1.f : -1.f;
		std::vector<Vector2D> result;
		result.reserve(points.size());
		for (size_t i = 0; i < points.size(); ++i) {
			Vector2D previous_edge = points[i] - points[(i + points.size() - 1) % points.size()];
			Vector2D next_edge = points[(i + 1) % points.size()] - points[i];
			if (previous_edge.Len() < .01f || next_edge.Len() < .01f) {
				result.push_back(points[i]);
				continue;
			}
			Vector2D previous_normal = previous_edge.Perpendicular().Unit() * direction;
			Vector2D next_normal = next_edge.Perpendicular().Unit() * direction;
			Vector2D join = previous_normal + next_normal;
			if (join.Len() < .01f) {
				result.push_back(points[i] + next_normal * distance);
				continue;
			}
			join = join.Unit();
			float denominator = join.Dot(next_normal);
			if (std::abs(denominator) < .25f) denominator = std::copysign(.25f, denominator == 0.f ? 1.f : denominator);
			float scale = std::clamp(distance / denominator, -distance * 4.f, distance * 4.f);
			result.push_back(points[i] + join * scale);
		}
		return result;
	}

	void append_outline(std::string& drawing, std::vector<Vector2D> const& points,
		float stroke_size, int corner_radius, double centre_x, double centre_y) {
		if (points.size() < 3) return;
		append_rounded_polygon(drawing, points, static_cast<float>(corner_radius), centre_x, centre_y);
		auto inner = inset_polygon(points, std::max(.1f, stroke_size));
		std::reverse(inner.begin(), inner.end());
		append_rounded_polygon(drawing, inner,
			std::max(0.f, static_cast<float>(corner_radius) - stroke_size), centre_x, centre_y);
	}

	void append_stroke(std::string& drawing, std::vector<Vector2D> const& points,
		float size, double centre_x, double centre_y) {
		if (points.size() < 2) return;
		float radius = std::max(.05f, size * .5f);
		auto make_clockwise = [](std::vector<Vector2D>& polygon) {
			float area_twice = 0.f;
			for (size_t i = 0; i < polygon.size(); ++i)
				area_twice += polygon[i].Cross(polygon[(i + 1) % polygon.size()]);
			if (area_twice < 0.f) std::reverse(polygon.begin(), polygon.end());
		};
		for (size_t i = 0; i + 1 < points.size(); ++i) {
			Vector2D direction = points[i + 1] - points[i];
			if (direction.Len() < .001f) continue;
			Vector2D offset = direction.Perpendicular().Unit() * radius;
			std::vector<Vector2D> segment{points[i] + offset, points[i] - offset,
				points[i + 1] - offset, points[i + 1] + offset};
			make_clockwise(segment);
			append_polygon(drawing, segment, centre_x, centre_y);
		}
		for (auto point : points) {
			auto cap = regular_polygon(point, radius, radius, 12);
			make_clockwise(cap);
			append_polygon(drawing, cap, centre_x, centre_y);
		}
	}

	void append_ellipse(std::string& drawing, std::vector<Vector2D> const& geometry,
		float stroke_size, bool filled, double centre_x, double centre_y) {
		if (geometry.empty()) return;
		float left = geometry.front().X(), right = left;
		float top = geometry.front().Y(), bottom = top;
		for (auto point : geometry) {
			left = std::min(left, point.X()); right = std::max(right, point.X());
			top = std::min(top, point.Y()); bottom = std::max(bottom, point.Y());
		}

		auto contour = [&](float l, float t, float r, float b, bool reverse) {
			if (r - l < 1.f || b - t < 1.f) return;
			constexpr float kappa = .5522847498f;
			float cx = (l + r) * .5f, cy = (t + b) * .5f;
			float rx = (r - l) * .5f, ry = (b - t) * .5f;
			auto coord = [&](float x, float y) {
				return decimal_string(x - centre_x, 1) + " " + decimal_string(y - centre_y, 1);
			};
			if (!drawing.empty()) drawing += " ";
			drawing += "m " + coord(cx + rx, cy);
			if (!reverse) {
				drawing += " b " + coord(cx + rx, cy + kappa * ry) + " " + coord(cx + kappa * rx, cy + ry) + " " + coord(cx, cy + ry);
				drawing += " b " + coord(cx - kappa * rx, cy + ry) + " " + coord(cx - rx, cy + kappa * ry) + " " + coord(cx - rx, cy);
				drawing += " b " + coord(cx - rx, cy - kappa * ry) + " " + coord(cx - kappa * rx, cy - ry) + " " + coord(cx, cy - ry);
				drawing += " b " + coord(cx + kappa * rx, cy - ry) + " " + coord(cx + rx, cy - kappa * ry) + " " + coord(cx + rx, cy);
			}
			else {
				drawing += " b " + coord(cx + rx, cy - kappa * ry) + " " + coord(cx + kappa * rx, cy - ry) + " " + coord(cx, cy - ry);
				drawing += " b " + coord(cx - kappa * rx, cy - ry) + " " + coord(cx - rx, cy - kappa * ry) + " " + coord(cx - rx, cy);
				drawing += " b " + coord(cx - rx, cy + kappa * ry) + " " + coord(cx - kappa * rx, cy + ry) + " " + coord(cx, cy + ry);
				drawing += " b " + coord(cx + kappa * rx, cy + ry) + " " + coord(cx + rx, cy + kappa * ry) + " " + coord(cx + rx, cy);
			}
		};

		contour(left, top, right, bottom, false);
		if (!filled) {
			float inset = std::max(.1f, stroke_size);
			if (right - left > inset * 2.f && bottom - top > inset * 2.f)
				contour(left + inset, top + inset, right - inset, bottom - inset, true);
		}
	}
}

VisualToolShape::VisualToolShape(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualDraggableFeature>(parent, context)
, gl_text(std::make_unique<OpenGLText>())
{
	parent->SetCursor(wxCursor(wxCURSOR_CROSS));
	preview_interface.AttachHost(parent->GetPreviewBar(), [this](int id) {
		this->parent->SetFocus();
		PerformAction(static_cast<Action>(id));
	});
}

VisualToolShape::~VisualToolShape() {
	parent->SetCursor(wxNullCursor);
	if (toolBar)
		toolBar->Unbind(wxEVT_TOOL, &VisualToolShape::OnToolbar, this);
	if (toolBar)
		toolBar->Unbind(wxEVT_TOOL_RCLICKED, &VisualToolShape::OnToolbar, this);
}

void VisualToolShape::SetToolbar(wxToolBar *toolbar) {
	toolBar = toolbar;
	toolbar_icon_size = std::max(16, static_cast<int>(toolBar->GetToolBitmapSize().GetWidth()));
	toolBar->AddSeparator();
	toolBar->AddTool(shape_tool_id, shape_label(shape),
		wxBitmapBundle::FromBitmap(shape_bitmap(last_geometric_shape, toolbar_icon_size, false, true)),
		_("Shape (right-click for list)"));
	toolBar->AddTool(freehand_tool_id, _("Freehand"),
		wxBitmapBundle::FromBitmap(shape_bitmap(VisualShapeKind::Freehand, toolbar_icon_size)),
		_("Freehand"), wxITEM_CHECK);
	toolBar->AddTool(fill_tool_id, _("Filled"),
		wxBitmapBundle::FromBitmap(shape_bitmap(VisualShapeKind::Rectangle, toolbar_icon_size, true)),
		_("Toggle filled or outlined shape"), wxITEM_CHECK);
	toolBar->AddTool(size_tool_id, _("Size"),
		wxBitmapBundle::FromBitmap(shape_bitmap(VisualShapeKind::Line, toolbar_icon_size, false, true)),
		_("Stroke size"));
	toolBar->AddTool(radius_tool_id, _("Corner radius"),
		wxBitmapBundle::FromBitmap(radius_bitmap(toolbar_icon_size, corner_radius)),
		_("Corner radius"));
	toolBar->ToggleTool(fill_tool_id, filled);
	toolBar->Realize();
	toolBar->Show(true);
	toolBar->Bind(wxEVT_TOOL, &VisualToolShape::OnToolbar, this);
	toolBar->Bind(wxEVT_TOOL_RCLICKED, &VisualToolShape::OnToolbar, this);
	UpdateToolbar();
}

void VisualToolShape::OnToolbar(wxCommandEvent& event) {
	Vector2D position(parent->ScreenToClient(wxGetMousePosition()));
	if (event.GetId() == shape_tool_id)
		ShowShapeMenu(position);
	else if (event.GetId() == freehand_tool_id) {
		shape = toolBar->GetToolState(freehand_tool_id) ? VisualShapeKind::Freehand : last_geometric_shape;
		ResetCurrentShape();
		UpdateToolbar();
		parent->Render();
	}
	else if (event.GetId() == fill_tool_id) {
		filled = toolBar->GetToolState(fill_tool_id);
		parent->Render();
	}
	else if (event.GetId() == size_tool_id)
		ShowSizeMenu(position);
	else if (event.GetId() == radius_tool_id)
		ShowRadiusMenu(position);
}

void VisualToolShape::ShowShapeMenu(Vector2D position) {
	wxMenu menu;
	for (size_t i = 0; i < std::size(shape_choices); ++i) {
		auto const& choice = shape_choices[i];
		if (choice.kind == VisualShapeKind::Freehand) continue;
		wxString label = shape_label(choice.kind);
		auto item = new wxMenuItem(&menu, shape_menu_base + static_cast<int>(i),
			label, label, wxITEM_NORMAL);
		item->SetBitmap(shape_bitmap(choice.kind, 20));
		menu.Append(item);
	}
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(position.X()), static_cast<int>(position.Y())));
	if (selected >= shape_menu_base && selected < shape_menu_base + static_cast<int>(std::size(shape_choices))) {
		last_geometric_shape = shape_choices[selected - shape_menu_base].kind;
		shape = last_geometric_shape;
		ResetCurrentShape();
		UpdateToolbar();
		parent->Render();
	}
}

void VisualToolShape::ShowSizeMenu(Vector2D position) {
	static constexpr double sizes[] = {.5, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
	wxMenu menu;
	for (size_t i = 0; i < std::size(sizes); ++i) {
		auto item = menu.AppendRadioItem(size_menu_base + static_cast<int>(i),
			agi::wxformat(_("%s px"), to_wx(decimal_string(sizes[i], 1))));
		if (std::abs(stroke_size - sizes[i]) < .001) item->Check(true);
	}
	menu.AppendSeparator();
	menu.Append(size_menu_base + 100, _("Custom size..."));
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(position.X()), static_cast<int>(position.Y())));
	if (selected >= size_menu_base && selected < size_menu_base + static_cast<int>(std::size(sizes)))
		stroke_size = sizes[selected - size_menu_base];
	else if (selected == size_menu_base + 100) {
		wxTextEntryDialog dialog(c->parent, _("Enter the stroke size in pixels:"),
			_("Custom stroke size"), to_wx(decimal_string(stroke_size, 1)));
		if (dialog.ShowModal() == wxID_OK) {
			wxString value = dialog.GetValue();
			value.Replace(",", ".");
			double parsed = stroke_size;
			if (value.ToDouble(&parsed) && parsed >= .1 && parsed <= 500.0) stroke_size = parsed;
		}
	}
	UpdateToolbar();
}

void VisualToolShape::ShowRadiusMenu(Vector2D position) {
	static constexpr int radii[] = {0, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64};
	wxMenu menu;
	for (size_t i = 0; i < std::size(radii); ++i) {
		auto item = menu.AppendRadioItem(radius_menu_base + static_cast<int>(i),
			agi::wxformat(_("%d px"), radii[i]));
		if (corner_radius == radii[i]) item->Check(true);
	}
	menu.AppendSeparator();
	menu.Append(radius_menu_base + 100, _("Custom radius..."));
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(position.X()), static_cast<int>(position.Y())));
	if (selected >= radius_menu_base && selected < radius_menu_base + static_cast<int>(std::size(radii)))
		corner_radius = radii[selected - radius_menu_base];
	else if (selected == radius_menu_base + 100) {
		long value = wxGetNumberFromUser(_("Enter the corner radius in pixels:"), _("Radius:"),
			_("Custom corner radius"), corner_radius, 0, 500, c->parent);
		if (value >= 0) corner_radius = static_cast<int>(value);
	}
	UpdateToolbar();
	parent->Render();
}

void VisualToolShape::ShowBlurMenu(Vector2D position) {
	std::vector<double> values;
	for (int value = 0; value <= 20; ++value) values.push_back(value / 10.0);
	for (int value = 25; value <= 50; value += 5) values.push_back(value / 10.0);
	for (int value = 6; value <= 10; ++value) values.push_back(static_cast<double>(value));
	wxMenu menu;
	for (size_t i = 0; i < values.size(); ++i) {
		auto item = menu.AppendRadioItem(blur_menu_base + static_cast<int>(i),
			agi::wxformat(_("Blur: %.1f"), values[i]));
		if (std::abs(blur - values[i]) < .001) item->Check(true);
	}
	menu.AppendSeparator();
	menu.Append(blur_menu_base + 100, _("Custom blur..."));
	int selected = parent->GetPopupMenuSelectionFromUser(menu,
		wxPoint(static_cast<int>(position.X()), static_cast<int>(position.Y())));
	if (selected >= blur_menu_base && selected < blur_menu_base + static_cast<int>(values.size()))
		blur = values[selected - blur_menu_base];
	else if (selected == blur_menu_base + 100) {
		wxTextEntryDialog dialog(c->parent, _("Enter the blur value:"), _("Custom blur"),
			agi::wxformat("%.1f", blur));
		if (dialog.ShowModal() == wxID_OK) {
			wxString value = dialog.GetValue();
			value.Replace(",", ".");
			double parsed = blur;
			if (value.ToDouble(&parsed) && parsed >= 0.0 && parsed <= 100.0) blur = parsed;
		}
	}
	parent->Render();
}

void VisualToolShape::ShowColourPicker() {
	agi::Color initial;
	if (!has_selected_colour) {
		AssDialogue *source = c->selectionController->GetActiveLine();
		AssStyle *style = source ? c->ass->GetStyle(source->Style.get()) : nullptr;
		initial = active_colour(source, style);
	}
	else initial = from_wx(selected_colour);
	wxColour previous_colour = selected_colour;
	bool previously_selected = has_selected_colour;
	bool accepted = GetColorFromUser(c->parent, initial, false, [this](agi::Color colour) {
		selected_colour = to_wx(colour);
		has_selected_colour = true;
		parent->Render();
	});
	if (!accepted) {
		selected_colour = previous_colour;
		has_selected_colour = previously_selected;
		parent->Render();
	}
}

void VisualToolShape::UpdateToolbar() {
	if (!toolBar) return;
	toolBar->SetToolNormalBitmap(shape_tool_id,
		wxBitmapBundle::FromBitmap(shape_bitmap(last_geometric_shape, toolbar_icon_size, false, true)));
	toolBar->SetToolNormalBitmap(size_tool_id,
		wxBitmapBundle::FromBitmap(shape_bitmap(VisualShapeKind::Line, toolbar_icon_size, false, true)));
	toolBar->SetToolNormalBitmap(radius_tool_id,
		wxBitmapBundle::FromBitmap(radius_bitmap(toolbar_icon_size, corner_radius)));
	toolBar->SetToolShortHelp(shape_tool_id,
		agi::wxformat(_("Shape: %s (right-click for list)"), shape_label(last_geometric_shape)));
	toolBar->ToggleTool(freehand_tool_id, shape == VisualShapeKind::Freehand);
	toolBar->SetToolShortHelp(size_tool_id, agi::wxformat(_("Stroke size: %s px"),
		to_wx(decimal_string(stroke_size, 1))));
	toolBar->SetToolShortHelp(radius_tool_id, agi::wxformat(_("Corner radius: %d px"), corner_radius));
	toolBar->Realize();
}

bool VisualToolShape::IsClosedShape() const {
	return is_closed(shape);
}

std::vector<Vector2D> VisualToolShape::Geometry() const {
	if (shape == VisualShapeKind::Freehand) return freehand_points;
	if (!shape_start || !shape_end) return {};

	Vector2D end = shape_end;
	if (alt_down && shape == VisualShapeKind::Line) {
		Vector2D delta = end - shape_start;
		float length = delta.Len();
		if (length > .01f) {
			double angle = std::round(delta.Angle() / (pi / 4.0)) * (pi / 4.0);
			end = shape_start + Vector2D(static_cast<float>(std::cos(angle) * length),
				static_cast<float>(std::sin(angle) * length));
		}
	}
	if (shape == VisualShapeKind::Line) return {shape_start, end};
	if (alt_down) {
		float dx = end.X() - shape_start.X();
		float dy = end.Y() - shape_start.Y();
		float side = std::max(std::abs(dx), std::abs(dy));
		end = shape_start + Vector2D(std::copysign(side, dx == 0.f ? 1.f : dx),
			std::copysign(side, dy == 0.f ? 1.f : dy));
	}
	float left = std::min(shape_start.X(), end.X());
	float right = std::max(shape_start.X(), end.X());
	float top = std::min(shape_start.Y(), end.Y());
	float bottom = std::max(shape_start.Y(), end.Y());
	Vector2D centre((left + right) * .5f, (top + bottom) * .5f);
	float rx = (right - left) * .5f;
	float ry = (bottom - top) * .5f;
	if (rx < .5f || ry < .5f) return {};

	switch (shape) {
		case VisualShapeKind::Rectangle:
		return {{left, top}, {right, top}, {right, bottom}, {left, bottom}};
		case VisualShapeKind::Ellipse:
		return regular_polygon(centre, rx, ry, 64);
		case VisualShapeKind::Triangle:
		return {{centre.X(), top}, {right, bottom}, {left, bottom}};
		case VisualShapeKind::Diamond:
		return {{centre.X(), top}, {right, centre.Y()}, {centre.X(), bottom}, {left, centre.Y()}};
		case VisualShapeKind::Hexagon:
		return regular_polygon(centre, rx, ry, 6);
		case VisualShapeKind::Heart:
		case VisualShapeKind::WideHeart: {
			std::vector<Vector2D> result;
			result.reserve(64);
			float width_factor = shape == VisualShapeKind::WideHeart ? 1.15f : 1.f;
			for (int i = 0; i < 64; ++i) {
				double t = 2.0 * pi * i / 64.0;
				double x = 16.0 * std::pow(std::sin(t), 3.0) / 17.0 * width_factor;
				double y = (13.0 * std::cos(t) - 5.0 * std::cos(2 * t) -
					2.0 * std::cos(3 * t) - std::cos(4 * t)) / 18.0;
				result.emplace_back(centre.X() + static_cast<float>(x * rx),
					centre.Y() - static_cast<float>(y * ry));
			}
			return result;
		}
		case VisualShapeKind::Star5:
		case VisualShapeKind::Star6:
		case VisualShapeKind::Star8: {
			int tips = shape == VisualShapeKind::Star5 ? 5 : shape == VisualShapeKind::Star6 ? 6 : 8;
			std::vector<Vector2D> result;
			result.reserve(tips * 2);
			for (int i = 0; i < tips * 2; ++i) {
				double angle = -pi / 2 + i * pi / tips;
				float inner = (i & 1) ? .42f : 1.f;
				result.emplace_back(centre.X() + static_cast<float>(std::cos(angle) * rx * inner),
					centre.Y() + static_cast<float>(std::sin(angle) * ry * inner));
			}
			return result;
		}
		case VisualShapeKind::Arrow:
		return {{left, centre.Y() - ry * .22f}, {left + rx * 1.15f, centre.Y() - ry * .22f},
			{left + rx * 1.15f, top}, {right, centre.Y()}, {left + rx * 1.15f, bottom},
			{left + rx * 1.15f, centre.Y() + ry * .22f}, {left, centre.Y() + ry * .22f}};
		default:
		return {};
	}
}

bool VisualToolShape::CanCreate() const {
	return c->selectionController->GetActiveLine() && !pending_shapes.empty();
}

std::pair<Vector2D, Vector2D> VisualToolShape::ActionBounds(Action action) {
	if (action == Action::Undo)
		return {Vector2D(12.f, 10.f), Vector2D(46.f, 44.f)};
	if (action == Action::Redo)
		return {Vector2D(54.f, 10.f), Vector2D(88.f, 44.f)};
	constexpr float top = 10.f;
	constexpr float height = 34.f;
	constexpr float gap = 8.f;
	auto width_for = [&](Action item) {
		wxString label;
		bool button = item == Action::Accept || item == Action::Clear;
		if (item == Action::Blur) label = agi::wxformat(_("Blur: %.1f"), blur);
		else if (item == Action::Colour) label = _("Color");
		else if (item == Action::Accept) label = _("Accept (ENTER)");
		else if (item == Action::Clear) label = _("Cancel (ESC)");
		gl_text->SetFont("Verdana", button ? 9 : 10, button, false);
		int text_width, text_height;
		gl_text->GetExtent(from_wx(label), text_width, text_height);
		if (item == Action::Blur) return static_cast<float>(text_width + 24);
		if (item == Action::Colour) return static_cast<float>(text_width + 47);
		return static_cast<float>(text_width + 24);
	};
	float left = 96.f;
	for (Action item : {Action::Blur, Action::Colour, Action::Accept, Action::Clear}) {
		float width = width_for(item);
		if (item == action) return {Vector2D(left, top), Vector2D(left + width, top + height)};
		left += width + gap;
	}
	float width = width_for(Action::Blur);
	return {Vector2D(left, top), Vector2D(left + width, top + height)};
}

VisualToolShape::Action VisualToolShape::ActionAt(Vector2D position) {
	if (preview_interface.HasExternalHost()) return Action::None;
	for (Action action : {Action::Undo, Action::Redo}) {
		bool enabled = action == Action::Undo ? !undo_history.empty() : !redo_history.empty();
		if (!enabled) continue;
		auto [top_left, bottom_right] = ActionBounds(action);
		if (position.X() >= top_left.X() && position.X() <= bottom_right.X() &&
			position.Y() >= top_left.Y() && position.Y() <= bottom_right.Y()) return action;
	}
	for (Action action : {Action::Blur, Action::Colour, Action::Accept, Action::Clear}) {
		if ((action == Action::Accept || action == Action::Clear) && !CanCreate()) continue;
		auto [top_left, bottom_right] = ActionBounds(action);
		if (position.X() >= top_left.X() && position.X() <= bottom_right.X() &&
			position.Y() >= top_left.Y() && position.Y() <= bottom_right.Y()) return action;
	}
	return Action::None;
}

void VisualToolShape::PerformAction(Action action) {
	switch (action) {
		case Action::Undo: UndoHistory(); break;
		case Action::Redo: RedoHistory(); break;
		case Action::Blur:
			ShowBlurMenu(Vector2D(parent->ScreenToClient(wxGetMousePosition())));
			break;
		case Action::Colour: ShowColourPicker(); break;
		case Action::Accept: CreateShape(); break;
		case Action::Clear: ClearPreview(); break;
		default: break;
	}
}

void VisualToolShape::UpdatePreviewInterface() {
	using Interface = VisualToolPreviewInterface;
	Interface::Page page;
	auto add = [&](Action action, Interface::ControlKind kind, wxString label,
		bool enabled = true, Interface::ControlStyle style = Interface::ControlStyle::Neutral) {
		Interface::Control control;
		control.id = static_cast<int>(action);
		control.kind = kind;
		control.label = std::move(label);
		control.enabled = enabled;
		control.style = style;
		page.controls.push_back(std::move(control));
	};
	add(Action::Undo, Interface::ControlKind::Undo, wxString(), !undo_history.empty());
	add(Action::Redo, Interface::ControlKind::Redo, wxString(), !redo_history.empty());
	add(Action::Blur, Interface::ControlKind::Button,
		agi::wxformat(_("Blur: %.1f"), blur));
	Interface::Control colour;
	colour.id = static_cast<int>(Action::Colour);
	colour.label = _("Color");
	colour.kind = Interface::ControlKind::Button;
	AssDialogue *active = c->selectionController->GetActiveLine();
	AssStyle *active_style = active ? c->ass->GetStyle(active->Style.get()) : nullptr;
	colour.swatch = has_selected_colour ? selected_colour : to_wx(active_colour(active, active_style));
	page.controls.push_back(std::move(colour));
	add(Action::Accept, Interface::ControlKind::Button, _("Accept"), CanCreate(),
		Interface::ControlStyle::Accept);
	add(Action::Clear, Interface::ControlKind::Button, _("Cancel"), CanCreate(),
		Interface::ControlStyle::Cancel);
	page.message = _("Shapes will be created in a single line. Hold ALT to constrain to a regular shape.");
	preview_interface.SetPage(std::move(page));
}

void VisualToolShape::ResetCurrentShape() {
	freehand_points.clear();
	shape_start = Vector2D();
	shape_end = Vector2D();
	drawing = false;
	if (parent->HasCapture()) parent->ReleaseMouse();
}

void VisualToolShape::FinishCurrentShape() {
	auto geometry = Geometry();
	if (geometry.size() >= (IsClosedShape() ? 3u : 2u)) {
		PushHistory();
		pending_shapes.push_back({std::move(geometry), shape, IsClosedShape(), filled, stroke_size, corner_radius});
	}
	ResetCurrentShape();
}

void VisualToolShape::PushHistory() {
	constexpr size_t maximum_history = 32;
	if (undo_history.size() == maximum_history)
		undo_history.erase(undo_history.begin());
	undo_history.push_back(pending_shapes);
	redo_history.clear();
}

bool VisualToolShape::UndoHistory() {
	if (undo_history.empty()) return false;
	redo_history.push_back(pending_shapes);
	pending_shapes = std::move(undo_history.back());
	undo_history.pop_back();
	ResetCurrentShape();
	parent->Render();
	return true;
}

bool VisualToolShape::RedoHistory() {
	if (redo_history.empty()) return false;
	undo_history.push_back(pending_shapes);
	pending_shapes = std::move(redo_history.back());
	redo_history.pop_back();
	ResetCurrentShape();
	parent->Render();
	return true;
}

void VisualToolShape::ClearPreview() {
	ResetCurrentShape();
	pending_shapes.clear();
	undo_history.clear();
	redo_history.clear();
	hovered_action = Action::None;
	parent->Render();
}

void VisualToolShape::OnMouseEvent(wxMouseEvent& event) {
	mouse_pos = event.GetPosition();
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();

	hovered_action = ActionAt(mouse_pos);
	if (event.LeftDown()) {
		if (hovered_action != Action::None) {
			PerformAction(hovered_action);
			return;
		}
		if (!preview_interface.HasExternalHost() && mouse_pos.Y() < top_bar_height) return;
		shape_start = shape_end = ToScriptCoords(mouse_pos);
		freehand_points.clear();
		if (shape == VisualShapeKind::Freehand)
			freehand_points.push_back(shape_start);
		drawing = true;
		if (!parent->HasCapture()) parent->CaptureMouse();
		parent->SetFocus();
	}

	if (drawing && (event.Dragging() || event.LeftUp())) {
		shape_end = ToScriptCoords(mouse_pos);
		if (shape == VisualShapeKind::Freehand &&
			(freehand_points.empty() ||
			(FromScriptCoords(freehand_points.back()) - mouse_pos).Len() >= 2.f))
			freehand_points.push_back(shape_end);
	}
	if (drawing && event.LeftUp()) {
		FinishCurrentShape();
		parent->SetFocus();
	}
	parent->Render();
}

bool VisualToolShape::OnKeyEvent(wxKeyEvent& event) {
	int key_code = event.GetKeyCode();
	if (event.CmdDown() && (key_code == 'Z' || key_code == 'Y')) {
		return key_code == 'Y' || event.ShiftDown() ? RedoHistory() : UndoHistory();
	}
	if ((key_code == WXK_RETURN || key_code == WXK_NUMPAD_ENTER) && CanCreate() && !drawing) {
		CreateShape();
		return true;
	}
	if (key_code == WXK_ESCAPE &&
		(!pending_shapes.empty() || drawing || !freehand_points.empty())) {
		ClearPreview();
		return true;
	}
	return false;
}

void VisualToolShape::DrawShape(PendingShape const& item) {
	auto geometry = item.geometry;
	if (item.closed && item.corner_radius > 0 && can_round(item.kind))
		geometry = rounded_polygon_points(geometry, static_cast<float>(item.corner_radius));
	if (geometry.size() < 2) return;
	std::vector<Vector2D> screen;
	std::vector<float> flat;
	screen.reserve(geometry.size() + 1);
	flat.reserve((geometry.size() + 1) * 2);
	for (auto point : geometry) screen.push_back(FromScriptCoords(point));
	if (item.closed) screen.push_back(screen.front());
	for (auto point : screen) {
		flat.push_back(point.X());
		flat.push_back(point.Y());
	}

	wxColour colour = to_wx(highlight_color_primary_opt->GetColor());
	int visible_width = std::max(1, static_cast<int>(std::lround(
		item.stroke_size * video_size.X() / std::max(1.f, script_res.X()))));
	if (item.filled && item.closed) {
		std::vector<float> polygon_flat(flat.begin(), flat.end() - 2);
		std::vector<int> starts{0};
		std::vector<int> counts{static_cast<int>(geometry.size())};
		gl.SetFillColour(colour, .24f);
		gl.SetLineColour(colour, .9f, 2);
		gl.DrawMultiPolygon(polygon_flat, starts, counts, video_pos, video_size, false);
	}
	gl.SetLineColour(colour, .95f, visible_width);
	gl.DrawLineStrip(2, flat);
}

void VisualToolShape::Draw() {
	UpdatePreviewInterface();
	for (auto const& item : pending_shapes) DrawShape(item);
	if (drawing) {
		auto geometry = Geometry();
		if (geometry.size() >= 2)
			DrawShape({std::move(geometry), shape, IsClosedShape(), filled, stroke_size, corner_radius});
	}
	if (preview_interface.HasExternalHost()) return;

	// Portable fallback for hosts which do not provide the shared native preview strip.
	preview_interface.DrawBackground(gl, canvas_size, top_bar_height);
	for (auto action : {Action::Undo, Action::Redo}) {
		auto [top_left, bottom_right] = ActionBounds(action);
		bool enabled = action == Action::Undo ? !undo_history.empty() : !redo_history.empty();
		preview_interface.DrawHistory(gl, {top_left, bottom_right}, action == Action::Redo,
			enabled, hovered_action == action);
	}

	auto draw_setting = [&](Action action, wxString const& label, wxColour swatch = wxColour()) {
		auto [top_left, bottom_right] = ActionBounds(action);
		bool hovered = hovered_action == action;
		preview_interface.DrawPanel(gl, {top_left, bottom_right}, true, hovered);
		float text_left = top_left.X() + 12.f;
		if (swatch.IsOk()) {
			gl.SetFillColour(swatch, 1.f);
			gl.SetLineColour(swatch, 0.f, 1);
			gl.DrawRectangle(top_left + Vector2D(9.f, 8.f), top_left + Vector2D(27.f, 26.f));
			text_left = top_left.X() + 35.f;
		}
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		int text_width, text_height;
		std::string text = from_wx(label);
		gl_text->GetExtent(text, text_width, text_height);
		gl_text->Print(text, static_cast<int>(text_left),
			static_cast<int>((top_left.Y() + bottom_right.Y() - text_height) * .5f));
	};
	draw_setting(Action::Blur, agi::wxformat(_("Blur: %.1f"), blur));
	AssDialogue *active = c->selectionController->GetActiveLine();
	AssStyle *active_style = active ? c->ass->GetStyle(active->Style.get()) : nullptr;
	wxColour display_colour = has_selected_colour ? selected_colour : to_wx(active_colour(active, active_style));
	draw_setting(Action::Colour, _("Color"), display_colour);

	{
		auto draw_button = [&](Action action, wxString const& translated_label,
			wxColour enabled_colour) {
			auto bounds = ActionBounds(action);
			bool enabled = CanCreate();
			bool hovered = hovered_action == action;
			auto style = enabled_colour == wxColour(31, 153, 76) ?
				VisualToolPreviewInterface::ControlStyle::Accept :
				VisualToolPreviewInterface::ControlStyle::Cancel;
			preview_interface.DrawButton(gl, *gl_text, bounds, translated_label, style,
				enabled, hovered);
		};
		draw_button(Action::Accept, _("Accept (ENTER)"),
			wxColour(31, 153, 76));
		draw_button(Action::Clear, _("Cancel (ESC)"),
			wxColour(183, 54, 61));
	}

	auto [clear_top_left, clear_bottom_right] = ActionBounds(Action::Clear);
	preview_interface.DrawMessage(*gl_text, {clear_top_left, clear_bottom_right},
		clear_bottom_right.X() + 16.f,
		_("Shapes will be created in a single line. Hold ALT to constrain to a regular shape."));
}

void VisualToolShape::CreateShape() {
	if (!CanCreate()) return;
	AssDialogue *source = c->selectionController->GetActiveLine();
	AssStyle *style = c->ass->GetStyle(source->Style.get());
	agi::Color colour = has_selected_colour ? from_wx(selected_colour) : active_colour(source, style);

	float min_x = pending_shapes.front().geometry.front().X(), max_x = min_x;
	float min_y = pending_shapes.front().geometry.front().Y(), max_y = min_y;
	for (auto const& item : pending_shapes) {
		for (auto point : item.geometry) {
			min_x = std::min(min_x, point.X()); max_x = std::max(max_x, point.X());
			min_y = std::min(min_y, point.Y()); max_y = std::max(max_y, point.Y());
		}
	}
	double centre_x = std::round(((min_x + max_x) * .5) * 100.0) / 100.0;
	double centre_y = std::round(((min_y + max_y) * .5) * 100.0) / 100.0;
	std::string drawing_text;
	for (auto const& item : pending_shapes) {
		if (item.kind == VisualShapeKind::Ellipse)
			append_ellipse(drawing_text, item.geometry, item.stroke_size, item.filled, centre_x, centre_y);
		else if (item.closed && item.filled)
			append_rounded_polygon(drawing_text, item.geometry,
				can_round(item.kind) ? item.corner_radius : 0, centre_x, centre_y);
		else if (item.closed)
			append_outline(drawing_text, item.geometry, item.stroke_size,
				can_round(item.kind) ? item.corner_radius : 0, centre_x, centre_y);
		else {
			auto path = item.geometry;
			append_stroke(drawing_text, path, item.stroke_size, centre_x, centre_y);
		}
	}

	auto line = new AssDialogue;
	line->Style = source->Style;
	line->Layer = source->Layer;
	int frame_time = c->videoController->TimeAtFrame(frame_number, agi::vfr::EXACT);
	if (source->Start <= frame_time && frame_time < source->End) {
		line->Start = source->Start;
		line->End = source->End;
	}
	else {
		line->Start = c->videoController->TimeAtFrame(frame_number, agi::vfr::START);
		line->End = c->videoController->TimeAtFrame(frame_number, agi::vfr::END);
	}

	std::string colour_tag = colour.GetAssOverrideFormatted();
	std::string blur_text = agi::format("%.2f", blur);
	while (blur_text.size() > 1 && blur_text.back() == '0') blur_text.pop_back();
	if (!blur_text.empty() && blur_text.back() == '.') blur_text.pop_back();
	AssStyle default_style;
	AssStyle const& effective_style = style ? *style : default_style;
	std::string tags = "{";
	if (effective_style.alignment != 7) tags += "\\an7";
	if (effective_style.shadow_w != 0.0) tags += "\\shad0";
	if (effective_style.outline_w != 0.0) tags += "\\bord0";
	if (effective_style.scalex != 100.0) tags += "\\fscx100";
	if (effective_style.scaley != 100.0) tags += "\\fscy100";
	if (blur != 0.0) tags += "\\blur" + blur_text;
	tags += agi::format("\\pos(%s,%s)", decimal_string(centre_x, 2), decimal_string(centre_y, 2));
	if (effective_style.primary.a != 0) tags += "\\1a&H00&";
	tags += "\\1c" + colour_tag + "\\p1}";
	line->Text = tags + drawing_text;

	c->ass->Events.insert(c->ass->iterator_to(*source), *line);
	c->ass->Commit(_("Add shape"), AssFile::COMMIT_DIAG_ADDREM);
	c->selectionController->SetSelectionAndActive({line}, line);
	ClearPreview();
}
