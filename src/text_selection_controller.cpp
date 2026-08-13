// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "text_selection_controller.h"

#ifdef WITH_WXSTC
#include <wx/stc/stc.h>
#endif
#include <wx/textctrl.h>

#ifdef WITH_WXSTC
void TextSelectionController::SetControl(wxStyledTextCtrl* ctrl) {
	ctrl_stc = ctrl;
}
#endif

void TextSelectionController::SetControl(wxTextCtrl* ctrl) {
	ctrl_te = ctrl;
}

TextSelectionController::~TextSelectionController() {
#ifdef WITH_WXSTC
	if (ctrl_stc) ctrl_stc->Unbind(wxEVT_STC_UPDATEUI, &TextSelectionController::UpdateUI, this);
#endif
	if (ctrl_te) ctrl_te->Unbind(wxEVT_TEXT, &TextSelectionController::UpdateUI, this);
}

#define GET(var, new_value) do { \
	long tmp = new_value;      \
	if (tmp != var) {         \
		var = tmp;            \
		changed = true;       \
	}                         \
} while(false)

#define SET(var, new_value) do { \
	if (var != new_value) {              \
		var = new_value;                 \
		if (ctrl_te) ctrl_te->SetSelection(var, var); \
		if (ctrl_stc) ctrl_stc->SetCurrentPos(var); \
	}                                    \
} while (false)

void TextSelectionController::UpdateUI(wxEvent &evt) {
	if (changing) return;

	bool changed = false;
#ifdef WITH_WXSTC
	if (use_stc && ctrl_stc) {
		insertion_point = ctrl_stc->GetCurrentPos();
		selection_start = ctrl_stc->GetSelectionStart();
		selection_end = ctrl_stc->GetSelectionEnd();
	} else
#endif
	if (ctrl_te) {
		insertion_point = ctrl_te->GetInsertionPoint();
		selection_start = ctrl_te->GetInsertionPoint();
		selection_end = ctrl_te->GetInsertionPoint();
	}
	AnnounceSelectionChanged();
}

void TextSelectionController::SetInsertionPoint(long position) {
	changing = true;
#ifdef WITH_WXSTC
	if (use_stc && ctrl_stc) {
		ctrl_stc->SetCurrentPos(position);
	} else
#endif
	if (ctrl_te) {
		ctrl_te->SetInsertionPoint(position);
	}
	insertion_point = position;
	changing = false;
	AnnounceSelectionChanged();
}

void TextSelectionController::SetSelection(long start, long end) {
	changing = true;
#ifdef WITH_WXSTC
	if (use_stc && ctrl_stc) {
		ctrl_stc->SetSelection(start, end);
	} else
#endif
	if (ctrl_te) {
		ctrl_te->SetSelection(start, end);
	}
	selection_start = start;
	selection_end = end;
	changing = false;
	AnnounceSelectionChanged();
}


void TextSelectionController::CommitStagedChanges() {
	if (has_staged_selection) {
		if (staged_selection_start <= staged_selection_end) {
			SetSelection(staged_selection_start, staged_selection_end);
		} else {
			// commit some crimes to get this to work in all cases
			SetInsertionPoint(staged_selection_end == 0 ? staged_selection_start : 0);
			SetSelection(staged_selection_start, staged_selection_start);
			SetInsertionPoint(staged_selection_end);
		}
		has_staged_selection = false;
	}
}
