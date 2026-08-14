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
// OR IN CONNECTION WITH THE USE OR THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <wx/fwd.h>

class wxKeyEvent;
class wxStyledTextCtrl;

namespace osx {
namespace ime {
	/// Initialize IME support for a Scintilla text control
	void inject(wxStyledTextCtrl *ctrl);

	/// Clean up IME support for a Scintilla text control
	void invalidate(wxStyledTextCtrl *ctrl);

	/// Process a key event through the IME system
	/// Returns true if the event was handled by the IME
	bool process_key_event(wxStyledTextCtrl *ctrl, wxKeyEvent &evt);
}
}
