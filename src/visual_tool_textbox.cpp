// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "visual_tool_textbox.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "command/command.h"
#include "compat.h"
#include "dialogs.h"
#include "font_size_object.h"
#include "frame_main.h"
#include "format.h"
#include "gl_text.h"
#include "subtitle_line_combiner.h"
#include "include/aegisub/context.h"
#include "line_change_flags.h"
#include "libresrc/libresrc.h"
#include "project.h"
#include "selection_controller.h"
#include "typesetting_transform.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/color.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <wx/clipbrd.h>
#include <wx/font.h>
#include <wx/utils.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

namespace {

constexpr float row_height = 34.f;
constexpr float row_gap = 6.f;
constexpr float slider_label_left = 10.f;
constexpr float slider_label_gap = 12.f;
constexpr float slider_track_width = 42.f;
constexpr float slider_value_gap = 5.f;
constexpr float slider_value_width = 36.f;
constexpr float slider_right_padding = 7.f;
constexpr float move_handle_offset = 30.f;
const wxColour textbox_mesh_colour(255, 62, 62);

std::vector<VisualToolTextBox::Action> const& AllActions() {
	static std::vector<VisualToolTextBox::Action> actions = {
		VisualToolTextBox::Action::Undo, VisualToolTextBox::Action::Redo,
		VisualToolTextBox::Action::Font, VisualToolTextBox::Action::Primary,
		VisualToolTextBox::Action::Outline, VisualToolTextBox::Action::ShadowColour,
		VisualToolTextBox::Action::AlignLeft, VisualToolTextBox::Action::AlignCentre,
		VisualToolTextBox::Action::AlignRight, VisualToolTextBox::Action::AlignJustified,
		VisualToolTextBox::Action::Border, VisualToolTextBox::Action::Shadow,
		VisualToolTextBox::Action::LineSpacing, VisualToolTextBox::Action::Padding
	};
	return actions;
}

std::string Decimal(double value) {
	std::string text = agi::format("%.2f", value);
	while (text.size() > 1 && text.back() == '0') text.pop_back();
	if (!text.empty() && text.back() == '.') text.pop_back();
	return text;
}

bool ParseSliderValue(wxString text, double& value) {
	text.Replace(",", ".");
	return agi::util::try_parse(from_wx(text), &value);
}

bool IsSlider(VisualToolTextBox::Action action) {
	return action == VisualToolTextBox::Action::Border ||
		action == VisualToolTextBox::Action::Shadow ||
		action == VisualToolTextBox::Action::LineSpacing ||
		action == VisualToolTextBox::Action::Padding;
}

bool IsIconButton(VisualToolTextBox::Action action) {
	return action == VisualToolTextBox::Action::Undo || action == VisualToolTextBox::Action::Redo ||
		action == VisualToolTextBox::Action::Font || action == VisualToolTextBox::Action::Primary ||
		action == VisualToolTextBox::Action::Outline || action == VisualToolTextBox::Action::ShadowColour ||
		 action == VisualToolTextBox::Action::AlignLeft || action == VisualToolTextBox::Action::AlignCentre ||
		action == VisualToolTextBox::Action::AlignRight || action == VisualToolTextBox::Action::AlignJustified;
}

std::pair<double, double> SliderRange(VisualToolTextBox::Action action) {
	if (action == VisualToolTextBox::Action::LineSpacing) return {-100.0, 200.0};
	if (action == VisualToolTextBox::Action::Padding) return {0.0, 200.0};
	return {0.0, 50.0};
}

#ifdef __WXMSW__
bool AltGrDown(wxKeyEvent const& event) {
	return event.ControlDown() && (event.AltDown() || (GetKeyState(VK_RMENU) & 0x8000));
}

#else
bool AltGrDown(wxKeyEvent const& event) {
	return event.ControlDown() && event.AltDown();
}
#endif

}

VisualToolTextBox::VisualToolTextBox(VideoDisplay *parent, agi::Context *context,
	bool create_new, std::string return_tool)
: VisualTool<VisualDraggableFeature>(parent, context)
, gl_text(std::make_unique<OpenGLText>())
, return_tool(std::move(return_tool)) {
	if (this->return_tool == "video/tool/textbox") this->return_tool = "video/tool/cross";
	primary_icon = GETBUNDLE(button_color_one, 20).GetBitmap(wxSize(20, 20)).ConvertToImage();
	outline_icon = GETBUNDLE(button_color_three, 20).GetBitmap(wxSize(20, 20)).ConvertToImage();
	shadow_icon = GETBUNDLE(button_color_four, 20).GetBitmap(wxSize(20, 20)).ConvertToImage();
	Load(create_new);
	preview_interface.AttachHost(parent->GetPreviewBar(), [this](int id) {
		this->parent->SetFocus();
		PerformAction(static_cast<Action>(id));
	}, [this](int id, double value, bool final) {
		UpdateExternalSlider(static_cast<Action>(id), value, final);
	});
	caret_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
		if (!active || leaving || waiting_for_box) return;
		caret_visible = !caret_visible;
		this->parent->Render();
	});
	caret_timer.Start(500);
	if (c->parent)
		c->parent->Bind(wxEVT_CHAR_HOOK, &VisualToolTextBox::OnCharHook, this);
	parent->Bind(wxEVT_CHAR, &VisualToolTextBox::OnTextInput, this);
	parent->SetCursor(wxCursor(wxCURSOR_IBEAM));
	parent->SetFocus();
	parent->CallAfter([parent] {
		if (!parent->IsBeingDeleted()) parent->SetFocus();
	});
}

VisualToolTextBox::~VisualToolTextBox() {
	caret_timer.Stop();
	bool window_going = WindowGoing();
	if (c->parent)
		c->parent->Unbind(wxEVT_CHAR_HOOK, &VisualToolTextBox::OnCharHook, this);
	parent->Unbind(wxEVT_CHAR, &VisualToolTextBox::OnTextInput, this);
	if (!leaving && !window_going) ClearPreview();
	if (!window_going) parent->SetCursor(wxNullCursor);
}

bool VisualToolTextBox::WindowGoing() const {
	return (c->frame && c->frame->IsClosing()) ||
		(c->parent && c->parent->IsBeingDeleted()) || parent->IsBeingDeleted();
}

void VisualToolTextBox::Load(bool create_new) {
	if (create_new) {
		original_lines = c->selectionController->GetSortedSelection();
		if (original_lines.empty()) return;
		prototype = std::make_unique<AssDialogue>(*original_lines.front());
		document = typesetting::textbox::FromSelection(c, original_lines);
		caret = anchor = document.text.length();
		waiting_for_box = true;
		active = true;
		UpdatePreview();
		return;
	}

	AssDialogue *line = c->selectionController->GetActiveLine();
	if (!line || !c->imageMask->IsTextBoxGroup(line)) return;
	original_lines = c->imageMask->GetGroupLines(line);
	if (original_lines.empty()) return;
	AssDialogue *group_anchor = original_lines.front();
	auto loaded = typesetting::textbox::Load(*c->ass, *group_anchor);
	if (!loaded) return;
	prototype = std::make_unique<AssDialogue>(*group_anchor);
	document = std::move(*loaded);
	caret = anchor = document.text.length();
	active = true;
	UpdatePreview();
}

void VisualToolTextBox::OnFileChanged() {
	if (leaving || WindowGoing()) return;
	if (!OriginalLinesAreCurrent()) AbandonForFileChange();
	else Reject();
}

void VisualToolTextBox::OnLineChanged() {
	if (!leaving && !WindowGoing()) {
		if (!OriginalLinesAreCurrent()) {
			AbandonForFileChange();
			return;
		}
		if (ActionEnabled(Action::Apply)) Accept(true);
		else Reject();
	}
}

void VisualToolTextBox::OnCoordinateSystemsChanged() {
	if (active && !OriginalLinesAreCurrent()) {
		AbandonForFileChange();
		return;
	}
	if (active && !waiting_for_box) UpdatePreview();
}

bool VisualToolTextBox::OriginalLinesAreCurrent() const {
	if (original_lines.empty()) return false;
	size_t found = 0;
	for (auto& line : c->ass->Events)
		if (std::find(original_lines.begin(), original_lines.end(), &line) != original_lines.end())
			++found;
	return found == original_lines.size();
}

void VisualToolTextBox::AbandonForFileChange() {
	if (leaving) return;
	leaving = true;
	active = false;
	preview_changed.clear();
	preview_added.clear();
	original_lines.clear();
	ExitTool();
}

void VisualToolTextBox::UpdatePreview() {
	if (!active || !prototype) {
		parent->Render();
		return;
	}
	preview_changed.clear();
	preview_changed.reserve(original_lines.size());
	for (auto line : original_lines) {
		AssDialogue hidden(*line);
		hidden.Comment = true;
		preview_changed.push_back(std::move(hidden));
	}
	preview_added.clear();
	layout.clear();
	if (!waiting_for_box) {
		auto generated = typesetting::textbox::Generate(c, *prototype, document, &layout);
		Vector2D corners[4];
		typesetting::textbox::Corners(document, corners);
		AssDialogue background(*prototype);
		background.Comment = false;
		background.ExtradataIds = std::vector<uint32_t>();
		background.Text = "{\\an7\\pos(0,0)\\fscx100\\fscy100\\frz0\\frx0\\fry0"
			"\\fax0\\fay0\\bord0\\shad0\\1c&H000000&\\1a&H80&\\p1}"
			"m " + Decimal(corners[0].X()) + " " + Decimal(corners[0].Y()) +
			" l " + Decimal(corners[1].X()) + " " + Decimal(corners[1].Y()) +
			" " + Decimal(corners[2].X()) + " " + Decimal(corners[2].Y()) +
			" " + Decimal(corners[3].X()) + " " + Decimal(corners[3].Y());
		preview_added.reserve(generated.size() + 1);
		preview_added.push_back(std::move(background));
		std::move(generated.begin(), generated.end(), std::back_inserter(preview_added));
	}
	std::vector<AssDialogue const *> changed, added;
	for (auto const& line : preview_changed) changed.push_back(&line);
	for (auto const& line : preview_added) added.push_back(&line);
	c->videoController->PreviewSubtitles(changed, added);
	parent->Render();
}

void VisualToolTextBox::ClearPreview() {
	if (original_lines.empty() || WindowGoing() || !OriginalLinesAreCurrent()) return;
	std::vector<AssDialogue const *> originals(original_lines.begin(), original_lines.end());
	c->videoController->PreviewSubtitles(originals);
	preview_changed.clear();
	preview_added.clear();
}

void VisualToolTextBox::Accept(bool preserve_selection) {
	if (!ActionEnabled(Action::Apply) || leaving) return;
	if (!OriginalLinesAreCurrent()) {
		AbandonForFileChange();
		return;
	}
	Selection restore_selection;
	AssDialogue *restore_active = nullptr;
	if (preserve_selection) {
		restore_selection = c->selectionController->GetSelectedSet();
		restore_active = c->selectionController->GetActiveLine();
		// BaseGrid announces its new active row before it replaces the selected
		// set. That announcement makes the textbox accept and regenerate its rows
		// reentrantly, so the selected set captured here can still consist solely
		// of the old textbox rows. Preserve the newly active row explicitly; unlike
		// the regenerated source rows, its pointer remains live across Apply().
		if (restore_active) restore_selection.insert(restore_active);
	}
	leaving = true;
	active = false;
	file_changed_connection.Block();
	typesetting::textbox::Apply(c, *prototype, original_lines, document);
	file_changed_connection.Unblock();
	preview_changed.clear();
	preview_added.clear();
	if (preserve_selection) {
		Selection live_selection;
		for (auto& line : c->ass->Events)
			if (restore_selection.count(&line)) live_selection.insert(&line);
		if (!live_selection.empty()) {
			if (!live_selection.count(restore_active)) restore_active = *live_selection.begin();
			c->selectionController->SetSelectionAndActive(std::move(live_selection), restore_active);
		}
	}
	original_lines.clear();
	ExitTool();
}

void VisualToolTextBox::Reject() {
	if (leaving) return;
	leaving = true;
	active = false;
	ClearPreview();
	ExitTool();
}

void VisualToolTextBox::ExitTool() {
	preview_interface.Clear();
	if (WindowGoing()) return;
	agi::Context *context = c;
	std::string command = return_tool.empty() ? "video/tool/cross" : return_tool;
	parent->CallAfter([command = std::move(command), context] {
		if (!context->videoDisplay || context->videoDisplay->IsBeingDeleted() ||
			!context->project->VideoProvider()) return;
		cmd::call(command, context);
	});
}

std::pair<size_t, size_t> VisualToolTextBox::SelectedRange() const {
	return std::minmax(caret, anchor);
}

typesetting::textbox::TextStyle VisualToolTextBox::CurrentStyle() const {
	auto [start, end] = SelectedRange();
	size_t at = start < document.styles.size() ? start :
		(start && start - 1 < document.styles.size() ? start - 1 : document.styles.size());
	return at < document.styles.size() ? document.styles[at] : document.base_style;
}

void VisualToolTextBox::ApplyToRange(
	std::function<void(typesetting::textbox::TextStyle&)> const& apply,
	std::pair<size_t, size_t> range) {
	auto [start, end] = range;
	if (start == end) {
		start = 0;
		end = document.styles.size();
		apply(document.base_style);
	}
	for (size_t i = start; i < std::min(end, document.styles.size()); ++i)
		apply(document.styles[i]);
}

void VisualToolTextBox::Remember(typesetting::textbox::Document before) {
	if (before == document) return;
	if (undo_history.size() >= 64) undo_history.erase(undo_history.begin());
	undo_history.push_back(std::move(before));
	redo_history.clear();
}

bool VisualToolTextBox::Undo() {
	if (undo_history.empty()) return false;
	redo_history.push_back(document);
	document = std::move(undo_history.back());
	undo_history.pop_back();
	caret = anchor = std::min(caret, document.text.length());
	UpdatePreview();
	return true;
}

bool VisualToolTextBox::Redo() {
	if (redo_history.empty()) return false;
	undo_history.push_back(document);
	document = std::move(redo_history.back());
	redo_history.pop_back();
	caret = anchor = std::min(caret, document.text.length());
	UpdatePreview();
	return true;
}

void VisualToolTextBox::ReplaceSelection(wxString const& replacement) {
	auto before = document;
	auto [start, end] = SelectedRange();
	auto style = CurrentStyle();
	document.text = document.text.Left(start) + replacement + document.text.Mid(end);
	document.styles.erase(document.styles.begin() + std::min(start, document.styles.size()),
		document.styles.begin() + std::min(end, document.styles.size()));
	document.styles.insert(document.styles.begin() + std::min(start, document.styles.size()),
		replacement.length(), style);
	caret = anchor = start + replacement.length();
	Remember(std::move(before));
	UpdatePreview();
}

void VisualToolTextBox::DeleteSelection() {
	auto [start, end] = SelectedRange();
	if (start != end) ReplaceSelection(wxString());
}

void VisualToolTextBox::CopySelection() const {
	auto [start, end] = SelectedRange();
	if (start == end || !wxTheClipboard->Open()) return;
	wxTheClipboard->SetData(new wxTextDataObject(document.text.Mid(start, end - start)));
	wxTheClipboard->Close();
}

void VisualToolTextBox::ResetCaretBlink() {
	caret_visible = true;
	if (caret_timer.IsRunning()) caret_timer.Start(500);
}

void VisualToolTextBox::MoveCaret(long amount, bool selecting) {
	long next = std::clamp<long>(static_cast<long>(caret) + amount, 0,
		static_cast<long>(document.text.length()));
	caret = static_cast<size_t>(next);
	if (!selecting) anchor = caret;
	parent->Render();
}

void VisualToolTextBox::MoveCaretVertical(int direction, bool selecting) {
	if (layout.empty()) return;
	size_t current_row = 0;
	for (size_t i = 0; i < layout.size(); ++i)
		if (caret >= layout[i].start && caret <= layout[i].end) { current_row = i; break; }
	long target = std::clamp<long>(static_cast<long>(current_row) + direction, 0,
		static_cast<long>(layout.size() - 1));
	auto const& row = layout[current_row];
	size_t local = std::min(caret - row.start, row.carets.size() - 1);
	double wanted_x = row.carets[local];
	auto const& next_row = layout[target];
	size_t best = 0;
	double best_distance = std::numeric_limits<double>::max();
	for (size_t i = 0; i < next_row.carets.size(); ++i) {
		double distance = std::abs(next_row.carets[i] - wanted_x);
		if (distance < best_distance) { best = i; best_distance = distance; }
	}
	caret = next_row.start + best;
	if (!selecting) anchor = caret;
	parent->Render();
}

void VisualToolTextBox::MoveCaretToRowEdge(bool end, bool selecting) {
	for (auto const& row : layout) {
		if (caret < row.start || caret > row.end) continue;
		caret = end ? row.end : row.start;
		if (!selecting) anchor = caret;
		parent->Render();
		return;
	}
}

size_t VisualToolTextBox::CaretAt(Vector2D point) const {
	if (layout.empty()) return 0;
	size_t row_index = 0;
	double best_y = std::numeric_limits<double>::max();
	for (size_t i = 0; i < layout.size(); ++i) {
		double centre = layout[i].y + layout[i].height * .5;
		double distance = std::abs(point.Y() - centre);
		if (distance < best_y) { best_y = distance; row_index = i; }
	}
	auto const& row = layout[row_index];
	size_t best = 0;
	double best_x = std::numeric_limits<double>::max();
	for (size_t i = 0; i < row.carets.size(); ++i) {
		double distance = std::abs(point.X() - row.carets[i]);
		if (distance < best_x) { best_x = distance; best = i; }
	}
	return row.start + best;
}

void VisualToolTextBox::ShowFontPicker() {
	auto before = document;
	auto range = SelectedRange();
	auto current = CurrentStyle();
	wxFont font(static_cast<int>(std::lround(current.size)), wxFONTFAMILY_DEFAULT,
		current.italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
		current.bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL, current.underline,
		to_wx(current.font));
	font.SetStrikethrough(current.strikeout);
	FontSizeObject sizes;
	sizes.fs = current.size; sizes.fscx = current.scale_x;
	sizes.fscy = current.scale_y; sizes.fsp = current.spacing;
	bool accepted = GetFontFromUser(c->parent, to_wx(current.font), font, sizes,
		c->ass.get(), [this, range](wxString face, wxFont selected, FontSizeObject size,
			LineChangeFlags flags) {
			ApplyToRange([&](typesetting::textbox::TextStyle& style) {
				if (flags.fn) style.font = from_wx(face);
				if (flags.fs) style.size = size.fs;
				if (flags.fscx) style.scale_x = size.fscx;
				if (flags.fscy) style.scale_y = size.fscy;
				if (flags.fsp) style.spacing = size.fsp;
				if (flags.b) style.bold = selected.GetWeight() == wxFONTWEIGHT_BOLD;
				if (flags.i) style.italic = selected.GetStyle() == wxFONTSTYLE_ITALIC;
				if (flags.u) style.underline = selected.GetUnderlined();
				if (flags.s) style.strikeout = selected.GetStrikethrough();
			}, range);
			UpdatePreview();
		}, false);
	if (!accepted) {
		document = std::move(before);
		UpdatePreview();
	}
	else Remember(std::move(before));
	parent->SetFocus();
}

void VisualToolTextBox::ShowColour(Action action) {
	auto before = document;
	auto range = SelectedRange();
	auto current = CurrentStyle();
	agi::Color original = action == Action::Primary ? current.primary :
		action == Action::Outline ? current.outline : current.shadow_colour;
	bool accepted = GetColorFromUser(c->parent, original, true,
		[this, action, range](agi::Color colour) {
			ApplyToRange([&](typesetting::textbox::TextStyle& style) {
				if (action == Action::Primary) style.primary = colour;
				else if (action == Action::Outline) style.outline = colour;
				else style.shadow_colour = colour;
			}, range);
			UpdatePreview();
		});
	if (!accepted) {
		document = std::move(before);
		UpdatePreview();
	}
	else Remember(std::move(before));
	parent->SetFocus();
}

void VisualToolTextBox::SetAlignment(Action action) {
	typesetting::textbox::Alignment alignment = action == Action::AlignCentre ?
		typesetting::textbox::Alignment::Centre : action == Action::AlignRight ?
		typesetting::textbox::Alignment::Right : action == Action::AlignJustified ?
		typesetting::textbox::Alignment::Justified : typesetting::textbox::Alignment::Left;
	if (document.alignment == alignment) return;
	auto before = document;
	document.alignment = alignment;
	Remember(std::move(before));
	UpdatePreview();
}

double VisualToolTextBox::SliderValue(Action action) const {
	auto current = CurrentStyle();
	if (action == Action::Border) return current.border;
	if (action == Action::Shadow) return current.shadow;
	if (action == Action::LineSpacing) return document.line_spacing;
	return document.padding;
}

void VisualToolTextBox::ApplySliderValue(Action action, double value,
	std::pair<size_t, size_t> range) {
	auto [minimum, maximum] = SliderRange(action);
	value = std::clamp(value, minimum, maximum);
	if (action == Action::LineSpacing) document.line_spacing = value;
	else if (action == Action::Padding) document.padding = value;
	else ApplyToRange([&](typesetting::textbox::TextStyle& style) {
		(action == Action::Border ? style.border : style.shadow) = value;
	}, range);
}

void VisualToolTextBox::UpdateSlider(Action action, Vector2D position) {
	auto [first, second] = SliderTrackBounds(action);
	float track_left = first.X();
	float track_right = second.X();
	double ratio = std::clamp((position.X() - track_left) /
		std::max(1.f, track_right - track_left), 0.f, 1.f);
	auto [minimum, maximum] = SliderRange(action);
	double value = std::round((minimum + ratio * (maximum - minimum)) * 2.0) / 2.0;
	ApplySliderValue(action, value, slider_selection);
	UpdatePreview();
}

void VisualToolTextBox::BeginSliderEdit(Action action) {
	if (editing_slider != Action::None) EndSliderEdit(true);
	editing_slider = action;
	slider_selection = SelectedRange();
	slider_edit_before = document;
	slider_edit_text = to_wx(Decimal(SliderValue(action)));
	slider_edit_caret = slider_edit_anchor = slider_edit_text.length();
	slider_mouse_selecting = false;
	ResetCaretBlink();
	parent->SetFocus();
	parent->Render();
}

std::pair<size_t, size_t> VisualToolTextBox::SliderEditRange() const {
	return std::minmax(slider_edit_caret, slider_edit_anchor);
}

void VisualToolTextBox::ReplaceSliderSelection(wxString const& replacement) {
	auto [start, end] = SliderEditRange();
	slider_edit_text = slider_edit_text.Left(start) + replacement + slider_edit_text.Mid(end);
	slider_edit_caret = slider_edit_anchor = start + replacement.length();
	ResetCaretBlink();
	RefreshSliderEdit();
}

void VisualToolTextBox::MoveSliderCaret(long amount, bool selecting) {
	auto [start, end] = SliderEditRange();
	long next = static_cast<long>(slider_edit_caret);
	if (!selecting && start != end)
		next = amount < 0 ? static_cast<long>(start) : static_cast<long>(end);
	else
		next = std::clamp(next + amount, 0L, static_cast<long>(slider_edit_text.length()));
	slider_edit_caret = static_cast<size_t>(next);
	if (!selecting) slider_edit_anchor = slider_edit_caret;
	ResetCaretBlink();
	parent->Render();
}

size_t VisualToolTextBox::SliderCaretAt(Action action, Vector2D position) const {
	auto [first, second] = SliderValueBounds(action);
	float text_left = (first.X() + second.X() -
		MeasuredTextWidth(slider_edit_text, false)) * .5f;
	size_t closest = 0;
	float closest_distance = std::abs(position.X() - text_left);
	for (size_t i = 1; i <= slider_edit_text.length(); ++i) {
		float x = text_left + MeasuredTextWidth(slider_edit_text.Left(i), false);
		float distance = std::abs(position.X() - x);
		if (distance < closest_distance) {
			closest = i;
			closest_distance = distance;
		}
	}
	return closest;
}

void VisualToolTextBox::RefreshSliderEdit() {
	if (editing_slider == Action::None || !slider_edit_before) return;
	double value = 0.0;
	document = *slider_edit_before;
	if (ParseSliderValue(slider_edit_text, value))
		ApplySliderValue(editing_slider, value, slider_selection);
	UpdatePreview();
}

void VisualToolTextBox::EndSliderEdit(bool accept) {
	if (editing_slider == Action::None || !slider_edit_before) return;
	auto before = std::move(*slider_edit_before);
	double value = 0.0;
	bool valid = ParseSliderValue(slider_edit_text, value);
	document = before;
	if (accept && valid) {
		ApplySliderValue(editing_slider, value, slider_selection);
		Remember(std::move(before));
	}
	editing_slider = Action::None;
	slider_edit_before.reset();
	slider_edit_text.clear();
	slider_edit_caret = slider_edit_anchor = 0;
	if (slider_mouse_selecting && parent->HasCapture()) parent->ReleaseMouse();
	slider_mouse_selecting = false;
	UpdatePreview();
	parent->SetFocus();
}

void VisualToolTextBox::UpdateActionTooltip(Action action) {
	if (action == Action::None || IsSlider(action)) {
		parent->UnsetToolTip();
		return;
	}
	parent->SetToolTip(LabelFor(action));
}

wxString VisualToolTextBox::LabelFor(Action action) const {
	switch (action) {
		case Action::Font: return _("Font");
		case Action::Border: return _("Border");
		case Action::Shadow: return _("Shadow");
		case Action::Primary: return _("Text color");
		case Action::Outline: return _("Border color");
		case Action::ShadowColour: return _("Shadow color");
		case Action::AlignLeft: return _("Align left");
		case Action::AlignCentre: return _("Align centre");
		case Action::AlignRight: return _("Align right");
		case Action::AlignJustified: return _("Justify");
		case Action::LineSpacing: return _("Line spacing");
		case Action::Padding: return _("Padding");
		case Action::Apply: return _("Accept (CTRL+ENTER)");
		case Action::Cancel: return _("Cancel (ESC)");
		default: return wxString();
	}
}

wxString VisualToolTextBox::DisplayLabelFor(Action action) const {
	switch (action) {
		case Action::LineSpacing: return _("Spacing");
		case Action::Apply: return _("Accept");
		case Action::Cancel: return _("Cancel");
		default: return LabelFor(action);
	}
}

float VisualToolTextBox::MeasuredTextWidth(wxString const& label, bool bold) const {
	std::string key = (bold ? "b:" : "r:") + from_wx(label);
	auto found = text_width_cache.find(key);
	if (found != text_width_cache.end()) return found->second;
	gl_text->SetFont("Verdana", 8, bold, false);
	int width, height;
	gl_text->GetExtent(from_wx(label), width, height);
	text_width_cache.emplace(key, static_cast<float>(width));
	return static_cast<float>(width);
}

std::pair<Vector2D, Vector2D> VisualToolTextBox::ActionBounds(Action wanted) const {
	float left = 8.f, top = 8.f;
	for (auto action : AllActions()) {
		float width = IsIconButton(action) ? 34.f : IsSlider(action) ?
			MeasuredTextWidth(DisplayLabelFor(action), false) + slider_label_left +
				slider_label_gap + slider_track_width + slider_value_gap +
				slider_value_width + slider_right_padding :
			MeasuredTextWidth(DisplayLabelFor(action), true) + 24.f;
		if (action == wanted)
			return {Vector2D(left, top), Vector2D(left + width, top + row_height)};
		left += width + row_gap;
	}
	return {Vector2D(left, top), Vector2D(left, top + row_height)};
}

std::pair<Vector2D, Vector2D> VisualToolTextBox::SliderTrackBounds(Action action) const {
	auto [first, second] = ActionBounds(action);
	float left = first.X() + slider_label_left +
		MeasuredTextWidth(DisplayLabelFor(action), false) + slider_label_gap;
	return {Vector2D(left, first.Y()),
		Vector2D(left + slider_track_width, second.Y())};
}

std::pair<Vector2D, Vector2D> VisualToolTextBox::SliderValueBounds(Action action) const {
	auto [track_first, track_second] = SliderTrackBounds(action);
	auto [first, second] = ActionBounds(action);
	return {Vector2D(track_second.X() + slider_value_gap, first.Y() + 6.f),
		Vector2D(second.X() - slider_right_padding, second.Y() - 6.f)};
}

float VisualToolTextBox::TopBarHeight() const {
	if (preview_interface.HasExternalHost()) return 0.f;
	return 8.f + row_height + 8.f;
}

VisualToolTextBox::Action VisualToolTextBox::ActionAt(Vector2D position) const {
	if (preview_interface.HasExternalHost()) return Action::None;
	for (auto action : AllActions()) {
		auto bounds = ActionBounds(action);
		if (position.X() >= bounds.first.X() && position.X() <= bounds.second.X() &&
			position.Y() >= bounds.first.Y() && position.Y() <= bounds.second.Y()) return action;
	}
	return Action::None;
}

bool VisualToolTextBox::ActionEnabled(Action action) const {
	if (action == Action::Cancel) return true;
	if (!active) return false;
	if (action == Action::Undo) return !undo_history.empty();
	if (action == Action::Redo) return !redo_history.empty();
	if (action == Action::Apply)
		return !waiting_for_box && std::abs(document.bottom_right.X() - document.top_left.X()) >= 2 &&
			std::abs(document.bottom_right.Y() - document.top_left.Y()) >= 2;
	return true;
}

void VisualToolTextBox::PerformAction(Action action) {
	if (!ActionEnabled(action)) return;
	switch (action) {
		case Action::Undo: Undo(); break;
		case Action::Redo: Redo(); break;
		case Action::Font: ShowFontPicker(); break;
		case Action::Primary:
		case Action::Outline:
		case Action::ShadowColour: ShowColour(action); break;
		case Action::AlignLeft:
		case Action::AlignCentre:
		case Action::AlignRight:
		case Action::AlignJustified: SetAlignment(action); break;
		case Action::Apply: Accept(); break;
		case Action::Cancel: Reject(); break;
		default: break;
	}
}

void VisualToolTextBox::UpdateExternalSlider(Action action, double value, bool final) {
	if (!IsSlider(action) || !ActionEnabled(action)) return;
	if (dragging_slider != action) {
		if (dragging_slider != Action::None && drag_before)
			Remember(std::move(*drag_before));
		dragging_slider = action;
		slider_selection = SelectedRange();
		drag_before = document;
	}
	ApplySliderValue(action, value, slider_selection);
	UpdatePreview();
	if (final) {
		if (drag_before) Remember(std::move(*drag_before));
		drag_before.reset();
		dragging_slider = Action::None;
		parent->SetFocus();
	}
}

void VisualToolTextBox::UpdatePreviewInterface() const {
	using Interface = VisualToolPreviewInterface;
	Interface::Page page;
	auto add = [&](Action action, Interface::ControlKind kind, wxString label,
		Interface::ControlStyle style = Interface::ControlStyle::Neutral) -> Interface::Control& {
		Interface::Control control;
		control.id = static_cast<int>(action);
		control.kind = kind;
		control.label = std::move(label);
		control.style = style;
		control.enabled = ActionEnabled(action);
		page.controls.push_back(std::move(control));
		return page.controls.back();
	};
	add(Action::Undo, Interface::ControlKind::Undo, wxString());
	add(Action::Redo, Interface::ControlKind::Redo, wxString());
	auto& font = add(Action::Font, Interface::ControlKind::Button, _("Font"));
	font.icon_only = true;
	font.icon = Interface::ControlIcon::Font;
	auto icon_button = [&](Action action, wxString label, wxImage const& bitmap) {
		auto& control = add(action, Interface::ControlKind::Button, std::move(label));
		control.icon_only = true;
		control.bitmap = wxBitmap(bitmap);
	};
	icon_button(Action::Primary, _("Text color"), primary_icon);
	icon_button(Action::Outline, _("Border color"), outline_icon);
	icon_button(Action::ShadowColour, _("Shadow color"), shadow_icon);
	auto alignment = [&](Action action, wxString label, typesetting::textbox::Alignment value,
		Interface::ControlIcon icon) {
		auto& control = add(action, Interface::ControlKind::Button, std::move(label));
		control.selected = document.alignment == value;
		control.icon_only = true;
		control.icon = icon;
	};
	alignment(Action::AlignLeft, _("Left"), typesetting::textbox::Alignment::Left,
		Interface::ControlIcon::AlignLeft);
	alignment(Action::AlignCentre, _("Centre"), typesetting::textbox::Alignment::Centre,
		Interface::ControlIcon::AlignCentre);
	alignment(Action::AlignRight, _("Right"), typesetting::textbox::Alignment::Right,
		Interface::ControlIcon::AlignRight);
	alignment(Action::AlignJustified, _("Justify"), typesetting::textbox::Alignment::Justified,
		Interface::ControlIcon::AlignJustified);
	for (auto action : {Action::Border, Action::Shadow, Action::LineSpacing, Action::Padding}) {
		auto [minimum, maximum] = SliderRange(action);
		auto& control = add(action, Interface::ControlKind::Slider, DisplayLabelFor(action));
		control.value = SliderValue(action);
		control.minimum = minimum;
		control.maximum = maximum;
		control.step = .5;
		control.value_text = to_wx(Decimal(control.value));
		control.width = action == Action::LineSpacing ? 175 : 155;
	}
	add(Action::Apply, Interface::ControlKind::Button, _("Accept"), Interface::ControlStyle::Accept);
	add(Action::Cancel, Interface::ControlKind::Button, _("Cancel"), Interface::ControlStyle::Cancel);
	preview_interface.SetPage(std::move(page));
}

void VisualToolTextBox::NormalizeRectangle() {
	float left = std::min(document.top_left.X(), document.bottom_right.X());
	float right = std::max(document.top_left.X(), document.bottom_right.X());
	float top = std::min(document.top_left.Y(), document.bottom_right.Y());
	float bottom = std::max(document.top_left.Y(), document.bottom_right.Y());
	document.top_left = Vector2D(left, top);
	document.bottom_right = Vector2D(right, bottom);
}

Vector2D VisualToolTextBox::MoveHandlePosition() const {
	Vector2D corner[4];
	ScreenCorners(corner);
	Vector2D at = (corner[2] + corner[3]) / 2;
	Vector2D centre = (corner[0] + corner[1] + corner[2] + corner[3]) / 4;
	Vector2D away = at - centre;
	return away.Len() > .01f ? at + away.Unit() * move_handle_offset :
		at + Vector2D(0.f, move_handle_offset);
}

void VisualToolTextBox::ScreenCorners(Vector2D out[4]) const {
	Vector2D script[4];
	typesetting::textbox::Corners(document, script);
	for (int i = 0; i < 4; ++i) out[i] = FromScriptCoords(script[i]);
}

Vector2D VisualToolTextBox::ToDocumentCoords(Vector2D position) const {
	if (!document.transformed) return ToScriptCoords(position);
	float left = std::min(document.top_left.X(), document.bottom_right.X());
	float right = std::max(document.top_left.X(), document.bottom_right.X());
	float top = std::min(document.top_left.Y(), document.bottom_right.Y());
	float bottom = std::max(document.top_left.Y(), document.bottom_right.Y());
	typesetting::OrientedBox source{
		Vector2D((left + right) * .5f, (top + bottom) * .5f), 0.f,
		Vector2D((right - left) * .5f, (bottom - top) * .5f)};
	Vector2D corners[4];
	typesetting::textbox::Corners(document, corners);
	return typesetting::QuadInverseMap(source, corners)(ToScriptCoords(position));
}

Vector2D VisualToolTextBox::FromDocumentCoords(Vector2D position) const {
	if (!document.transformed) return FromScriptCoords(position);
	float left = std::min(document.top_left.X(), document.bottom_right.X());
	float right = std::max(document.top_left.X(), document.bottom_right.X());
	float top = std::min(document.top_left.Y(), document.bottom_right.Y());
	float bottom = std::max(document.top_left.Y(), document.bottom_right.Y());
	typesetting::OrientedBox source{
		Vector2D((left + right) * .5f, (top + bottom) * .5f), 0.f,
		Vector2D((right - left) * .5f, (bottom - top) * .5f)};
	Vector2D corners[4];
	typesetting::textbox::Corners(document, corners);
	return FromScriptCoords(typesetting::QuadMap(source, corners)(position));
}

bool VisualToolTextBox::HitMoveHandle(Vector2D position) const {
	Vector2D delta = position - MoveHandlePosition();
	return std::abs(delta.X()) < 7.f && std::abs(delta.Y()) < 7.f;
}

int VisualToolTextBox::HitResize(Vector2D position) const {
	Vector2D corner[4];
	ScreenCorners(corner);
	constexpr float reach = 7.f;
	for (auto const& handle : {
		std::pair{corner[0], 1 | 4}, std::pair{corner[1], 2 | 4},
		std::pair{corner[3], 1 | 8}, std::pair{corner[2], 2 | 8},
		std::pair{(corner[0] + corner[1]) / 2, 4},
		std::pair{(corner[3] + corner[2]) / 2, 8},
		std::pair{(corner[0] + corner[3]) / 2, 1},
		std::pair{(corner[1] + corner[2]) / 2, 2}}) {
		Vector2D delta = position - handle.first;
		if (std::abs(delta.X()) <= reach && std::abs(delta.Y()) <= reach) return handle.second;
	}
	return 0;
}

void VisualToolTextBox::OnMouseEvent(wxMouseEvent& event) {
	if (leaving) return;
	mouse_pos = event.GetPosition();
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	Action action = ActionAt(mouse_pos);
	if (hovered_action != action) {
		hovered_action = action;
		UpdateActionTooltip(action);
		parent->Render();
	}
	int next_hovered_edges = !waiting_for_box && mouse_pos.Y() >= TopBarHeight() ?
		HitResize(mouse_pos) : 0;
	bool next_hovered_move_handle = !waiting_for_box && mouse_pos.Y() >= TopBarHeight() &&
		HitMoveHandle(mouse_pos);
	if (hovered_resize_edges != next_hovered_edges ||
		hovered_move_handle != next_hovered_move_handle) {
		hovered_resize_edges = next_hovered_edges;
		hovered_move_handle = next_hovered_move_handle;
		parent->Render();
	}

	if (editing_slider != Action::None && slider_mouse_selecting &&
		(event.Dragging() || event.LeftUp())) {
		slider_edit_caret = SliderCaretAt(editing_slider, mouse_pos);
		ResetCaretBlink();
		if (event.LeftUp()) {
			if (parent->HasCapture()) parent->ReleaseMouse();
			slider_mouse_selecting = false;
		}
		parent->Render();
		return;
	}

	if (dragging_slider != Action::None && (event.Dragging() || event.LeftUp())) {
		UpdateSlider(dragging_slider, mouse_pos);
		UpdateActionTooltip(dragging_slider);
		if (event.LeftUp()) {
			if (parent->HasCapture()) parent->ReleaseMouse();
			if (drag_before) Remember(std::move(*drag_before));
			drag_before.reset();
			dragging_slider = Action::None;
			parent->SetFocus();
		}
		return;
	}

	if ((event.LeftDown() || event.LeftDClick()) && IsSlider(hovered_action)) {
		auto [value_first, value_second] = SliderValueBounds(hovered_action);
		if (mouse_pos.X() >= value_first.X() && mouse_pos.X() <= value_second.X() &&
			mouse_pos.Y() >= value_first.Y() && mouse_pos.Y() <= value_second.Y()) {
			if (editing_slider != hovered_action) BeginSliderEdit(hovered_action);
			if (event.LeftDClick()) {
				slider_edit_anchor = 0;
				slider_edit_caret = slider_edit_text.length();
				slider_mouse_selecting = false;
			}
			else {
				size_t at = SliderCaretAt(hovered_action, mouse_pos);
				if (!event.ShiftDown()) slider_edit_anchor = at;
				slider_edit_caret = at;
				slider_mouse_selecting = true;
				if (!parent->HasCapture()) parent->CaptureMouse();
			}
			ResetCaretBlink();
			parent->SetFocus();
			parent->Render();
			return;
		}
	}

	if (event.LeftDown() && hovered_action != Action::None) {
		if (IsSlider(hovered_action)) {
			if (editing_slider != Action::None) EndSliderEdit(true);
			dragging_slider = hovered_action;
			slider_selection = SelectedRange();
			drag_before = document;
			if (!parent->HasCapture()) parent->CaptureMouse();
			UpdateSlider(dragging_slider, mouse_pos);
			UpdateActionTooltip(dragging_slider);
			return;
		}
		if (editing_slider != Action::None) EndSliderEdit(true);
		PerformAction(hovered_action);
		return;
	}
	if (event.LeftDown() && editing_slider != Action::None) EndSliderEdit(true);
	if (!active || mouse_pos.Y() < TopBarHeight()) { parent->Render(); return; }

	if (waiting_for_box) {
		if (event.LeftDown()) {
			drag_mode = DragMode::Draw;
			document.top_left = document.bottom_right = ToScriptCoords(mouse_pos);
			if (!parent->HasCapture()) parent->CaptureMouse();
		}
		if (drag_mode == DragMode::Draw && (event.Dragging() || event.LeftUp())) {
			document.bottom_right = ToScriptCoords(mouse_pos);
			parent->Render();
		}
		if (drag_mode == DragMode::Draw && event.LeftUp()) {
			if (parent->HasCapture()) parent->ReleaseMouse();
			NormalizeRectangle();
			if ((document.bottom_right - document.top_left).Len() >= 4.f) {
				waiting_for_box = false;
				UpdatePreview();
			}
			drag_mode = DragMode::None;
			parent->SetFocus();
		}
		return;
	}

	if (event.LeftDClick()) {
		Vector2D point = ToDocumentCoords(mouse_pos);
		bool inside = point.X() >= document.top_left.X() && point.X() <= document.bottom_right.X() &&
			point.Y() >= document.top_left.Y() && point.Y() <= document.bottom_right.Y();
		if (inside) {
			if (parent->HasCapture()) parent->ReleaseMouse();
			drag_mode = DragMode::None;
			mouse_selecting = false;
			size_t at = CaretAt(point);
			size_t start = at, end = at;
			while (start &&
				!std::iswspace(static_cast<wint_t>(document.text[start - 1].GetValue()))) --start;
			while (end < document.text.length() &&
				!std::iswspace(static_cast<wint_t>(document.text[end].GetValue()))) ++end;
			anchor = start;
			caret = end;
			ResetCaretBlink();
			parent->SetFocus();
			parent->Render();
			return;
		}
		Accept();
		return;
	}

	if (event.LeftDown()) {
		resize_edges = hovered_resize_edges;
		drag_origin = ToScriptCoords(mouse_pos);
		drag_top_left = document.top_left;
		drag_bottom_right = document.bottom_right;
		typesetting::textbox::Corners(document, drag_corners.data());
		if (hovered_move_handle) {
			drag_mode = DragMode::Move;
			drag_before = document;
		}
		else if (resize_edges) {
			drag_mode = DragMode::Resize;
			drag_before = document;
		}
		else {
			Vector2D point = ToDocumentCoords(mouse_pos);
			bool inside = point.X() >= document.top_left.X() && point.X() <= document.bottom_right.X() &&
				point.Y() >= document.top_left.Y() && point.Y() <= document.bottom_right.Y();
			if (inside) {
				drag_mode = DragMode::Select;
				caret = CaretAt(point);
				if (!shift_down) anchor = caret;
				ResetCaretBlink();
				mouse_selecting = true;
			}
			else {
				parent->SuppressTextboxDoubleClick(event.GetPosition());
				Accept();
				return;
			}
		}
		if (drag_mode != DragMode::None && !parent->HasCapture()) parent->CaptureMouse();
		parent->SetFocus();
	}

	if ((event.Dragging() || event.LeftUp()) && drag_mode == DragMode::Select) {
		caret = CaretAt(ToDocumentCoords(mouse_pos));
		ResetCaretBlink();
		parent->Render();
	}
	if ((event.Dragging() || event.LeftUp()) && drag_mode == DragMode::Move) {
		Vector2D delta = ToScriptCoords(mouse_pos) - drag_origin;
		if (document.transformed) {
			Vector2D moved[4];
			for (int i = 0; i < 4; ++i) moved[i] = drag_corners[i] + delta;
			typesetting::textbox::SetCorners(document, moved);
		}
		else {
			document.top_left = drag_top_left + delta;
			document.bottom_right = drag_bottom_right + delta;
		}
		UpdatePreview();
	}
	if ((event.Dragging() || event.LeftUp()) && drag_mode == DragMode::Resize) {
		Vector2D point = ToScriptCoords(mouse_pos);
		if (document.transformed) {
			float old_left = std::min(drag_top_left.X(), drag_bottom_right.X());
			float old_right = std::max(drag_top_left.X(), drag_bottom_right.X());
			float old_top = std::min(drag_top_left.Y(), drag_bottom_right.Y());
			float old_bottom = std::max(drag_top_left.Y(), drag_bottom_right.Y());
			typesetting::OrientedBox source{
				Vector2D((old_left + old_right) * .5f, (old_top + old_bottom) * .5f), 0.f,
				Vector2D((old_right - old_left) * .5f, (old_bottom - old_top) * .5f)};
			Vector2D local = typesetting::QuadInverseMap(source, drag_corners.data())(point);
			float left = old_left, right = old_right, top = old_top, bottom = old_bottom;
			if (resize_edges & 1) left = std::min(local.X(), old_right - 2.f);
			if (resize_edges & 2) right = std::max(local.X(), old_left + 2.f);
			if (resize_edges & 4) top = std::min(local.Y(), old_bottom - 2.f);
			if (resize_edges & 8) bottom = std::max(local.Y(), old_top + 2.f);
			document.top_left = Vector2D(left, top);
			document.bottom_right = Vector2D(right, bottom);
			auto project = typesetting::QuadMap(source, drag_corners.data());
			Vector2D local_corners[4] = {
				Vector2D(left, top), Vector2D(right, top),
				Vector2D(right, bottom), Vector2D(left, bottom)};
			Vector2D resized[4];
			for (int i = 0; i < 4; ++i) resized[i] = project(local_corners[i]);
			typesetting::textbox::SetCorners(document, resized);
			UpdatePreview();
		}
		else {
		document.top_left = drag_top_left;
		document.bottom_right = drag_bottom_right;
		if (resize_edges & 1) document.top_left = Vector2D(point.X(), document.top_left.Y());
		if (resize_edges & 2) document.bottom_right = Vector2D(point.X(), document.bottom_right.Y());
		if (resize_edges & 4) document.top_left = Vector2D(document.top_left.X(), point.Y());
		if (resize_edges & 8) document.bottom_right = Vector2D(document.bottom_right.X(), point.Y());
		NormalizeRectangle();
		UpdatePreview();
		}
	}
	if (event.LeftUp() && drag_mode != DragMode::None) {
		if (parent->HasCapture()) parent->ReleaseMouse();
		if (drag_before) {
			Remember(std::move(*drag_before));
			drag_before.reset();
		}
		drag_mode = DragMode::None;
		mouse_selecting = false;
		parent->Render();
	}
}

bool VisualToolTextBox::OnKeyEvent(wxKeyEvent& event) {
	int key = event.GetKeyCode();
	bool command = event.CmdDown();
	bool altgr = AltGrDown(event);
	bool shortcut = command && !altgr;
	bool shift = event.ShiftDown();
	ResetCaretBlink();
	if (editing_slider != Action::None) {
		if (key == WXK_ESCAPE) { EndSliderEdit(false); return true; }
		if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) { EndSliderEdit(true); return true; }
		if (shortcut && key == 'A') {
			slider_edit_anchor = 0;
			slider_edit_caret = slider_edit_text.length();
			parent->Render();
			return true;
		}
		if (key == WXK_LEFT) { MoveSliderCaret(-1, shift); return true; }
		if (key == WXK_RIGHT) { MoveSliderCaret(1, shift); return true; }
		if (key == WXK_HOME || key == WXK_END) {
			slider_edit_caret = key == WXK_HOME ? 0 : slider_edit_text.length();
			if (!shift) slider_edit_anchor = slider_edit_caret;
			parent->Render();
			return true;
		}
		if (key == WXK_BACK) {
			auto [start, end] = SliderEditRange();
			if (start != end) ReplaceSliderSelection(wxString());
			else if (slider_edit_caret) {
				slider_edit_anchor = slider_edit_caret - 1;
				ReplaceSliderSelection(wxString());
			}
			return true;
		}
		if (key == WXK_DELETE) {
			auto [start, end] = SliderEditRange();
			if (start != end) ReplaceSliderSelection(wxString());
			else if (slider_edit_caret < slider_edit_text.length()) {
				slider_edit_anchor = slider_edit_caret + 1;
				ReplaceSliderSelection(wxString());
			}
			return true;
		}
		if (!shortcut) {
			event.DoAllowNextEvent();
			// wxGTK only generates the translated character event when the hook is
			// also skipped. DoAllowNextEvent alone is sufficient on MSW, but not on
			// GTK, where keeping the hook handled swallows all printable input.
			event.Skip();
			return true;
		}
		return true;
	}
	if (key == WXK_ESCAPE) { Reject(); return true; }
	if (!active || waiting_for_box) return false;
	if (shortcut && (key == 'Z' || key == 'Y'))
		return key == 'Y' || shift ? Redo() : Undo();
	if (shortcut && key == 'A') {
		anchor = 0; caret = document.text.length(); parent->Render(); return true;
	}
	if (shortcut && key == 'C') { CopySelection(); return true; }
	if (shortcut && key == 'X') { CopySelection(); DeleteSelection(); return true; }
	if (shortcut && key == 'V') {
		if (wxTheClipboard->Open()) {
			if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
				wxTextDataObject data;
				wxTheClipboard->GetData(data);
				wxString text = data.GetText();
				text.Replace("\r\n", "\n"); text.Replace("\r", "\n");
				ReplaceSelection(text);
			}
			wxTheClipboard->Close();
		}
		return true;
	}
	if (key == WXK_LEFT) { MoveCaret(-1, shift); return true; }
	if (key == WXK_RIGHT) { MoveCaret(1, shift); return true; }
	if (key == WXK_UP) { MoveCaretVertical(-1, shift); return true; }
	if (key == WXK_DOWN) { MoveCaretVertical(1, shift); return true; }
	if (key == WXK_HOME) { MoveCaretToRowEdge(false, shift); return true; }
	if (key == WXK_END) { MoveCaretToRowEdge(true, shift); return true; }
	if (key == WXK_BACK) {
		auto [start, end] = SelectedRange();
		if (start != end) DeleteSelection();
		else if (caret) { anchor = caret - 1; DeleteSelection(); }
		return true;
	}
	if (key == WXK_DELETE) {
		auto [start, end] = SelectedRange();
		if (start != end) DeleteSelection();
		else if (caret < document.text.length()) { anchor = caret + 1; DeleteSelection(); }
		return true;
	}
	if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
		if (shift) ReplaceSelection("\n");
		else Accept();
		return true;
	}
	if (key == WXK_TAB) { ReplaceSelection("    "); return true; }
	// Text itself arrives through wxEVT_CHAR after the platform has applied the active
	// keyboard layout, dead keys and IME. Let that event through the char hook while still
	// preventing the video's hotkeys from consuming this physical key.
	if (!shortcut) {
		event.DoAllowNextEvent();
		// Keep the hook unhandled so wxGTK can pass the key through its input
		// method and emit the layout-aware wxEVT_CHAR consumed by OnTextInput.
		event.Skip();
		return true;
	}
	return false;
}

void VisualToolTextBox::OnTextInput(wxKeyEvent& event) {
	if (!active || waiting_for_box || leaving) { event.Skip(); return; }
	int key = event.GetKeyCode();
	if (key == WXK_WINDOWS_LEFT || key == WXK_WINDOWS_RIGHT || key == WXK_WINDOWS_MENU)
		return;
	int unicode = event.GetUnicodeKey();
	if (unicode == WXK_NONE) {
		// wx reports system keys such as the Windows keys only as non-Unicode
		// key codes. Never reinterpret those numeric constants as text characters.
		if (key >= WXK_START) return;
		unicode = key;
	}
	if (editing_slider != Action::None) {
		wxUniChar character(unicode);
		if ((unicode >= '0' && unicode <= '9') || unicode == '-' || unicode == '.' || unicode == ',') {
			ReplaceSliderSelection(wxString(character));
		}
		return;
	}
	if (unicode >= 32 && unicode != 127) {
		ReplaceSelection(wxString(static_cast<wxUniChar>(unicode)));
		return;
	}
	event.Skip();
}

void VisualToolTextBox::OnCharHook(wxKeyEvent& event) {
	int key = event.GetKeyCode();
	if (editing_slider != Action::None) {
		if (key == WXK_ESCAPE) {
			EndSliderEdit(false);
			return;
		}
		if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
			EndSliderEdit(true);
			return;
		}
		event.Skip();
		return;
	}
	if (key == WXK_ESCAPE) {
		Reject();
		return;
	}
	if (!event.ShiftDown() && (key == WXK_RETURN || key == WXK_NUMPAD_ENTER)) {
		Accept();
		return;
	}
	event.Skip();
}

void VisualToolTextBox::DrawAlignmentIcon(Action action, Vector2D first,
	Vector2D second, wxColour colour) {
	float centre_x = (first.X() + second.X()) * .5f;
	float top = first.Y() + 9.f;
	gl.SetLineColour(colour, 1.f, 2);
	for (int row = 0; row < 4; ++row) {
		float width = action == Action::AlignJustified || row % 2 == 0 ? 16.f : 10.f;
		float left = action == Action::AlignLeft ? centre_x - 8.f :
			action == Action::AlignRight ? centre_x + 8.f - width : centre_x - width * .5f;
		gl.DrawLine(Vector2D(left, top + row * 5.f), Vector2D(left + width, top + row * 5.f));
	}
}

void VisualToolTextBox::DrawTopBar() {
	UpdatePreviewInterface();
	if (preview_interface.HasExternalHost()) return;
	preview_interface.DrawBackground(gl, canvas_size, TopBarHeight());
	gl_text->SetFont("Verdana", 9, false, false);

	for (auto action : AllActions()) {
		auto [first, second] = ActionBounds(action);
		bool enabled = ActionEnabled(action);
		bool alignment_active =
			(action == Action::AlignLeft && document.alignment == typesetting::textbox::Alignment::Left) ||
			(action == Action::AlignCentre && document.alignment == typesetting::textbox::Alignment::Centre) ||
			(action == Action::AlignRight && document.alignment == typesetting::textbox::Alignment::Right) ||
			(action == Action::AlignJustified && document.alignment == typesetting::textbox::Alignment::Justified);
		if (action == Action::Undo || action == Action::Redo) {
			preview_interface.DrawHistory(gl, {first, second}, action == Action::Redo,
				enabled, hovered_action == action);
			continue;
		}
		if (action == Action::Apply || action == Action::Cancel) {
			auto style = action == Action::Apply ?
				VisualToolPreviewInterface::ControlStyle::Accept :
				VisualToolPreviewInterface::ControlStyle::Cancel;
			preview_interface.DrawButton(gl, *gl_text, {first, second}, DisplayLabelFor(action),
				style, enabled, hovered_action == action);
			continue;
		}
		preview_interface.DrawPanel(gl, {first, second}, enabled,
			hovered_action == action, alignment_active);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		if (action == Action::Font) {
			gl_text->SetFont("Verdana", 9, true, false);
			gl_text->SetColour(agi::Color(255, 255, 255, 255));
			std::string label = "fn";
			int width, height;
			gl_text->GetExtent(label, width, height);
			gl_text->Print(label, static_cast<int>((first.X() + second.X() - width) * .5f),
				static_cast<int>((first.Y() + second.Y() - height) * .5f));
			gl_text->SetFont("Verdana", 9, false, false);
			continue;
		}
		wxImage const *icon = action == Action::Primary ? &primary_icon : action == Action::Outline ? &outline_icon :
			action == Action::ShadowColour ? &shadow_icon : nullptr;
		if (icon) {
			Vector2D centre = (first + second) * .5f;
			gl.DrawImage(*icon, centre - Vector2D(10.f, 10.f), centre + Vector2D(10.f, 10.f));
			continue;
		}
		if (action == Action::AlignLeft || action == Action::AlignCentre ||
			action == Action::AlignRight || action == Action::AlignJustified) {
			DrawAlignmentIcon(action, first, second,
				enabled ? *wxWHITE : wxColour(150, 150, 150));
			continue;
		}
		if (IsSlider(action)) {
			gl_text->SetFont("Verdana", 8, false, false);
			gl_text->SetColour(agi::Color(255, 255, 255, 255));
			std::string label = from_wx(DisplayLabelFor(action));
			int width, height;
			gl_text->GetExtent(label, width, height);
			float label_left = first.X() + slider_label_left;
			gl_text->Print(label, static_cast<int>(label_left),
				static_cast<int>((first.Y() + second.Y() - height) * .5f));
			auto [track_first, track_second] = SliderTrackBounds(action);
			float track_left = track_first.X();
			float track_right = track_second.X();
			float y = (first.Y() + second.Y()) * .5f;
			gl.SetLineColour(wxColour(130, 135, 140), 1.f, 3);
			gl.DrawLine(Vector2D(track_left, y), Vector2D(track_right, y));
			auto [minimum, maximum] = SliderRange(action);
			double ratio = (SliderValue(action) - minimum) / (maximum - minimum);
			gl.SetFillColour(wxColour(80, 220, 255), 1.f);
			gl.DrawCircle(Vector2D(track_left + static_cast<float>(std::clamp(ratio, 0.0, 1.0)) *
				(track_right - track_left), y), 5.f);
			auto [value_first, value_second] = SliderValueBounds(action);
			gl.SetFillColour(wxColour(32, 35, 38), 1.f);
			gl.SetLineColour(action == editing_slider ? wxColour(80, 220, 255) :
				wxColour(95, 100, 105), 1.f, action == editing_slider ? 2 : 1);
			gl.DrawRectangle(value_first, value_second);
			std::string value = action == editing_slider ? from_wx(slider_edit_text) :
				Decimal(SliderValue(action));
			gl_text->GetExtent(value, width, height);
			float text_left = (value_first.X() + value_second.X() - width) * .5f;
			float text_top = (value_first.Y() + value_second.Y() - height) * .5f;
			if (action == editing_slider) {
				auto [selection_start, selection_end] = SliderEditRange();
				if (selection_start != selection_end) {
					float selection_left = text_left +
						MeasuredTextWidth(slider_edit_text.Left(selection_start), false);
					float selection_right = text_left +
						MeasuredTextWidth(slider_edit_text.Left(selection_end), false);
					gl.SetFillColour(wxColour(55, 130, 220), .75f);
					gl.SetLineColour(wxColour(55, 130, 220), 0.f, 1);
					gl.DrawRectangle(Vector2D(selection_left, value_first.Y() + 2.f),
						Vector2D(selection_right, value_second.Y() - 2.f));
				}
			}
			gl_text->SetColour(agi::Color(255, 255, 255, 255));
			gl_text->Print(value, static_cast<int>(text_left), static_cast<int>(text_top));
			if (action == editing_slider && caret_visible &&
				slider_edit_caret == slider_edit_anchor) {
				float caret_x = text_left +
					MeasuredTextWidth(slider_edit_text.Left(slider_edit_caret), false);
				gl.SetLineColour(*wxWHITE, 1.f, 1);
				gl.DrawLine(Vector2D(caret_x, value_first.Y() + 3.f),
					Vector2D(caret_x, value_second.Y() - 3.f));
			}
			continue;
		}
		std::string label = from_wx(DisplayLabelFor(action));
		int width, height;
		gl_text->GetExtent(label, width, height);
		gl_text->SetColour(agi::Color(255, 255, 255, 255));
		gl_text->Print(label, static_cast<int>(first.X() + 11.f),
			static_cast<int>((first.Y() + second.Y() - height) * .5f));
	}
}

void VisualToolTextBox::DrawRectangleAndSelection() {
	if (waiting_for_box && drag_mode != DragMode::Draw) return;
	Vector2D corner[4];
	ScreenCorners(corner);
	if (waiting_for_box) {
		corner[0] = FromScriptCoords(Vector2D(std::min(document.top_left.X(), document.bottom_right.X()),
			std::min(document.top_left.Y(), document.bottom_right.Y())));
		corner[2] = FromScriptCoords(Vector2D(std::max(document.top_left.X(), document.bottom_right.X()),
			std::max(document.top_left.Y(), document.bottom_right.Y())));
		corner[1] = Vector2D(corner[2].X(), corner[0].Y());
		corner[3] = Vector2D(corner[0].X(), corner[2].Y());
	}
	if (waiting_for_box) {
		gl.SetFillColour(*wxBLACK, .5f);
		gl.SetLineColour(*wxBLACK, 0.f, 1);
		gl.DrawTriangle(corner[0], corner[1], corner[2]);
		gl.DrawTriangle(corner[0], corner[2], corner[3]);
	}
	gl.SetFillColour(*wxBLACK, 0.f);
	gl.SetLineColour(textbox_mesh_colour, 1.f, 1);
	for (int i = 0; i < 4; ++i)
		gl.DrawDashedLine(corner[i], corner[(i + 1) % 4], 5.f);
	wxColour outline = to_wx(line_color_secondary_opt->GetColor());
	wxColour current = to_wx(highlight_color_secondary_opt->GetColor());
	for (auto const& handle : {
		std::pair{corner[0], 1 | 4}, std::pair{corner[1], 2 | 4},
		std::pair{corner[3], 1 | 8}, std::pair{corner[2], 2 | 8},
		std::pair{(corner[0] + corner[1]) / 2, 4},
		std::pair{(corner[3] + corner[2]) / 2, 8},
		std::pair{(corner[0] + corner[3]) / 2, 1},
		std::pair{(corner[1] + corner[2]) / 2, 2}}) {
		bool active_handle = hovered_resize_edges == handle.second ||
			(drag_mode == DragMode::Resize && resize_edges == handle.second);
		gl.SetLineColour(active_handle ? current : outline, 1.f, active_handle ? 2 : 1);
		gl.SetFillColour(*wxBLACK, 0.f);
		gl.DrawRectangle(handle.first - Vector2D(5.f, 5.f),
			handle.first + Vector2D(5.f, 5.f));
	}
	if (waiting_for_box) return;

	Vector2D move_handle = MoveHandlePosition();
	bool active_move_handle = hovered_move_handle || drag_mode == DragMode::Move;
	gl.SetLineColour(outline, 1.f, 1);
	gl.SetFillColour(to_wx((active_move_handle ? highlight_color_secondary_opt :
		highlight_color_primary_opt)->GetColor()), .3f);
	gl.DrawRectangle(move_handle - Vector2D(6.f, 6.f), move_handle + Vector2D(6.f, 6.f));
	gl.DrawLine(move_handle - Vector2D(0.f, 12.f), move_handle + Vector2D(0.f, 12.f));
	gl.DrawLine(move_handle - Vector2D(12.f, 0.f), move_handle + Vector2D(12.f, 0.f));

	auto [selected_start, selected_end] = SelectedRange();
	if (selected_start != selected_end) {
		gl.SetFillColour(wxColour(70, 145, 255), .28f);
		gl.SetLineColour(wxColour(70, 145, 255), 0.f, 1);
		for (auto const& row : layout) {
			size_t start = std::max(selected_start, row.start);
			size_t end = std::min(selected_end, row.end);
			if (start >= end || row.carets.empty()) continue;
			double x1 = row.carets[start - row.start];
			double x2 = row.carets[end - row.start];
			Vector2D selected[4] = {
				FromDocumentCoords(Vector2D(static_cast<float>(x1), static_cast<float>(row.y))),
				FromDocumentCoords(Vector2D(static_cast<float>(x2), static_cast<float>(row.y))),
				FromDocumentCoords(Vector2D(static_cast<float>(x2), static_cast<float>(row.y + row.height))),
				FromDocumentCoords(Vector2D(static_cast<float>(x1), static_cast<float>(row.y + row.height)))};
			gl.DrawTriangle(selected[0], selected[1], selected[2]);
			gl.DrawTriangle(selected[0], selected[2], selected[3]);
		}
	}
	else if (caret_visible) {
		for (auto const& row : layout) {
			if (caret < row.start || caret > row.end || row.carets.empty()) continue;
			double x = row.carets[caret - row.start];
			gl.SetLineColour(*wxWHITE, 1.f, 2);
			Vector2D offset(2.f, 2.f);
			gl.DrawLine(FromDocumentCoords(Vector2D(static_cast<float>(x), static_cast<float>(row.y))) + offset,
				FromDocumentCoords(Vector2D(static_cast<float>(x), static_cast<float>(row.y + row.height))) + offset);
			break;
		}
	}
}

void VisualToolTextBox::Draw() {
	if (!active || leaving) return;
	DrawTopBar();
	DrawRectangleAndSelection();
}
