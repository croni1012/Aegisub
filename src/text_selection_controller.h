// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
// Permission to use, copy, modify, and distribute this software for any purpose
// with or without fee is hereby granted.

#include <libaegisub/signal.h>

class wxEvent;
class wxTextCtrl;

class TextSelectionController {
	long selection_start = 0;
	long selection_end = 0;
	long insertion_point = 0;
	bool changing = false;
	long staged_selection_start = 0;
	long staged_selection_end = 0;
	bool has_staged_selection = false;
	wxTextCtrl *ctrl = nullptr;

	void UpdateUI(wxEvent &evt);
	agi::signal::Signal<> AnnounceSelectionChanged;

public:
	void SetSelection(long start, long end);
	void SetInsertionPoint(long point);
	void StageSetSelection(long start, long end) { staged_selection_start = start; staged_selection_end = end; has_staged_selection = true; }
	void StageSetInsertionPoint(long point) { StageSetSelection(point, point); }
	void CommitStagedChanges();
	void DropStagedChanges() { has_staged_selection = false; }

	long GetSelectionStart() const { return selection_start; }
	long GetSelectionEnd() const { return selection_end; }
	long GetInsertionPoint() const { return insertion_point; }
	long GetStagedSelectionStart() const { return has_staged_selection ? staged_selection_start : selection_start; }
	long GetStagedSelectionEnd() const { return has_staged_selection ? staged_selection_end : selection_end; }
	long GetStagedInsertionPoint() const { return has_staged_selection ? staged_selection_end : insertion_point; }

	wxTextCtrl *GetControl() const { return ctrl; }
	void SetControl(wxTextCtrl *control);
	~TextSelectionController();

	DEFINE_SIGNAL_ADDERS(AnnounceSelectionChanged, AddSelectionListener)
};
