// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
// Permission to use, copy, modify, and distribute this software for any purpose
// with or without fee is hereby granted.

#include "text_selection_controller.h"

#include <algorithm>
#include <wx/textctrl.h>

namespace {
long Utf8Position(wxTextCtrl const& ctrl, long position) {
	auto text = ctrl.GetRange(0, std::max(0L, position)).utf8_str();
	return static_cast<long>(text.length());
}

long NativePosition(wxTextCtrl const& ctrl, long utf8_position) {
	long low = 0;
	long high = ctrl.GetLastPosition();
	while (low < high) {
		long middle = low + (high - low) / 2;
		if (Utf8Position(ctrl, middle) < utf8_position)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}
}

void TextSelectionController::SetControl(wxTextCtrl *control) {
	if (ctrl) {
		ctrl->Unbind(wxEVT_KEY_UP, &TextSelectionController::UpdateUI, this);
		ctrl->Unbind(wxEVT_LEFT_UP, &TextSelectionController::UpdateUI, this);
		ctrl->Unbind(wxEVT_TEXT, &TextSelectionController::UpdateUI, this);
	}
	ctrl = control;
	if (ctrl) {
		ctrl->Bind(wxEVT_KEY_UP, &TextSelectionController::UpdateUI, this);
		ctrl->Bind(wxEVT_LEFT_UP, &TextSelectionController::UpdateUI, this);
		ctrl->Bind(wxEVT_TEXT, &TextSelectionController::UpdateUI, this);
		wxCommandEvent event;
		UpdateUI(event);
	}
}

TextSelectionController::~TextSelectionController() {
	SetControl(nullptr);
}

void TextSelectionController::UpdateUI(wxEvent& event) {
	event.Skip();
	if (changing || !ctrl) return;
	long native_start, native_end;
	ctrl->GetSelection(&native_start, &native_end);
	long new_insertion = Utf8Position(*ctrl, ctrl->GetInsertionPoint());
	long new_start = Utf8Position(*ctrl, native_start);
	long new_end = Utf8Position(*ctrl, native_end);
	if (new_insertion == insertion_point && new_start == selection_start && new_end == selection_end) return;
	insertion_point = new_insertion;
	selection_start = new_start;
	selection_end = new_end;
	AnnounceSelectionChanged();
}

void TextSelectionController::SetInsertionPoint(long position) {
	changing = true;
	insertion_point = position;
	selection_start = position;
	selection_end = position;
	if (ctrl) ctrl->SetInsertionPoint(NativePosition(*ctrl, position));
	changing = false;
	AnnounceSelectionChanged();
}

void TextSelectionController::SetSelection(long start, long end) {
	changing = true;
	selection_start = start;
	selection_end = end;
	insertion_point = end;
	if (ctrl) ctrl->SetSelection(NativePosition(*ctrl, start), NativePosition(*ctrl, end));
	changing = false;
	AnnounceSelectionChanged();
}

void TextSelectionController::CommitStagedChanges() {
	if (!has_staged_selection) return;
	if (staged_selection_start <= staged_selection_end)
		SetSelection(staged_selection_start, staged_selection_end);
	else {
		SetInsertionPoint(staged_selection_end == 0 ? staged_selection_start : 0);
		SetSelection(staged_selection_start, staged_selection_start);
		SetInsertionPoint(staged_selection_end);
	}
	has_staged_selection = false;
}
