// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "typesetting_textbox.h"
#include "visual_feature.h"
#include "visual_tool.h"

#include <map>
#include <array>
#include <memory>
#include <optional>
#include <functional>
#include <utility>
#include <vector>

#include <wx/image.h>
#include <wx/timer.h>

class OpenGLText;

class VisualToolTextBox final : public VisualTool<VisualDraggableFeature> {
public:
	enum class Action {
		None,
		Undo,
		Redo,
		Font,
		Border,
		Shadow,
		Primary,
		Outline,
		ShadowColour,
		AlignLeft,
		AlignCentre,
		AlignRight,
		AlignJustified,
		LineSpacing,
		Padding,
		Apply,
		Cancel
	};

	enum class DragMode {
		None,
		Draw,
		Select,
		Move,
		Resize
	};

private:

	std::unique_ptr<OpenGLText> gl_text;
	wxImage primary_icon;
	wxImage outline_icon;
	wxImage shadow_icon;
	std::unique_ptr<AssDialogue> prototype;
	std::vector<AssDialogue *> original_lines;
	typesetting::textbox::Document document;
	std::vector<typesetting::textbox::LayoutRow> layout;
	std::vector<typesetting::textbox::Document> undo_history;
	std::vector<typesetting::textbox::Document> redo_history;
	std::optional<typesetting::textbox::Document> drag_before;
	std::vector<AssDialogue> preview_changed;
	std::vector<AssDialogue> preview_added;
	std::string return_tool;

	bool active = false;
	bool waiting_for_box = false;
	bool leaving = false;
	bool mouse_selecting = false;
	bool caret_visible = true;
	bool right_to_left = false;
	wxTimer caret_timer;
	size_t caret = 0;
	size_t anchor = 0;
	DragMode drag_mode = DragMode::None;
	int resize_edges = 0;
	int hovered_resize_edges = 0;
	bool hovered_move_handle = false;
	Vector2D drag_origin;
	Vector2D drag_top_left;
	Vector2D drag_bottom_right;
	std::array<Vector2D, 4> drag_corners{};
	Action hovered_action = Action::None;
	Action dragging_slider = Action::None;
	Action editing_slider = Action::None;
	std::pair<size_t, size_t> slider_selection;
	std::optional<typesetting::textbox::Document> slider_edit_before;
	wxString slider_edit_text;
	size_t slider_edit_caret = 0;
	size_t slider_edit_anchor = 0;
	bool slider_mouse_selecting = false;
	mutable std::map<std::string, float> text_width_cache;

	void Load(bool create_new);
	bool OriginalLinesAreCurrent() const;
	void AbandonForFileChange();
	void UpdatePreview();
	void ClearPreview();
	bool WindowGoing() const;
	void Accept(bool preserve_selection = false);
	void Reject();
	void ExitTool();
	void OnFileChanged() override;
	void OnLineChanged() override;
	void OnCoordinateSystemsChanged() override;

	std::pair<size_t, size_t> SelectedRange() const;
	typesetting::textbox::TextStyle CurrentStyle() const;
	void ApplyToRange(std::function<void(typesetting::textbox::TextStyle&)> const& apply,
		std::pair<size_t, size_t> range);
	void Remember(typesetting::textbox::Document before);
	bool Undo();
	bool Redo();
	void ReplaceSelection(wxString const& replacement);
	void DeleteSelection();
	void CopySelection() const;
	void ResetCaretBlink();
	void MoveCaret(long amount, bool selecting);
	void MoveCaretVertical(int direction, bool selecting);
	void MoveCaretToRowEdge(bool end, bool selecting);
	size_t CaretAt(Vector2D script_position) const;

	void ShowFontPicker();
	void ShowColour(Action action);
	void SetAlignment(Action action);
	double SliderValue(Action action) const;
	void ApplySliderValue(Action action, double value,
		std::pair<size_t, size_t> range);
	void UpdateSlider(Action action, Vector2D position);
	void BeginSliderEdit(Action action);
	std::pair<size_t, size_t> SliderEditRange() const;
	void ReplaceSliderSelection(wxString const& replacement);
	void MoveSliderCaret(long amount, bool selecting);
	size_t SliderCaretAt(Action action, Vector2D position) const;
	void RefreshSliderEdit();
	void EndSliderEdit(bool accept);
	void UpdateActionTooltip(Action action);

	wxString LabelFor(Action action) const;
	wxString DisplayLabelFor(Action action) const;
	float MeasuredTextWidth(wxString const& label, bool bold) const;
	std::pair<Vector2D, Vector2D> ActionBounds(Action action) const;
	std::pair<Vector2D, Vector2D> SliderTrackBounds(Action action) const;
	std::pair<Vector2D, Vector2D> SliderValueBounds(Action action) const;
	float TopBarHeight() const;
	Action ActionAt(Vector2D position) const;
	bool ActionEnabled(Action action) const;
	void PerformAction(Action action);
	void UpdatePreviewInterface() const;
	void UpdateExternalSlider(Action action, double value, bool final);

	Vector2D MoveHandlePosition() const;
	void ScreenCorners(Vector2D out[4]) const;
	Vector2D ToDocumentCoords(Vector2D screen_position) const;
	Vector2D FromDocumentCoords(Vector2D document_position) const;
	bool HitMoveHandle(Vector2D screen_position) const;
	int HitResize(Vector2D screen_position) const;
	void NormalizeRectangle();
	void DrawAlignmentIcon(Action action, Vector2D first, Vector2D second, wxColour colour);
	void DrawTopBar();
	void DrawRectangleAndSelection();
	void Draw() override;

public:
	VisualToolTextBox(VideoDisplay *parent, agi::Context *context, bool create_new = false,
		std::string return_tool = "video/tool/cross");
	~VisualToolTextBox();

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyEvent(wxKeyEvent& event) override;
	void OnTextInput(wxKeyEvent& event);
	void OnCharHook(wxKeyEvent& event);
};
