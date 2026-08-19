// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <wx/textctrl.h>
#include <wx/tipwin.h>

class Thesaurus;
namespace agi {
	class SpellChecker;
	struct Context;
	namespace ass { struct DialogueToken; }
}

/// @class SubsTextEditCtrl
/// @brief Native subtitle editor with bidi text, spell checking and syntax highlighting
class SubsTextEditCtrl final : public wxTextCtrl {
	/// Backend spellchecker to use
	std::unique_ptr<agi::SpellChecker> spellchecker;

	/// Backend thesaurus to use
	std::unique_ptr<Thesaurus> thesaurus;

	/// Project context, for splitting lines
	agi::Context *context;

	/// The word right-clicked on, used for spellchecker replacing
	std::string currentWord;

	/// The beginning of the word right-clicked on, for spellchecker replacing
	std::pair<long, long> currentWordPos;

	/// Spellchecker suggestions for the last right-clicked word
	std::vector<std::string> sugs;

	/// Thesaurus suggestions for the last right-clicked word
	std::vector<std::string> thesSugs;

	/// UTF-8 text and tokens used by syntax highlighting, spell checking and calltips
	std::string line_text;
	std::vector<agi::ass::DialogueToken> tokenized_line;

	std::string calltip_text;
	size_t calltip_position = 0;
	long cursor_pos = -1;
	wxTipWindow::Ref calltip;
	bool styling = false;
	bool right_to_left = false;

	void OnContextMenu(wxContextMenuEvent &);
	void OnKeyDown(wxKeyEvent &event);
	void OnText(wxCommandEvent &event);
	void OnLoseFocus(wxFocusEvent &event);
	void OnToggleRTL(wxCommandEvent &event);
	void OnUseSuggestion(wxCommandEvent &event);
	void OnSetDicLanguage(wxCommandEvent &event);
	void OnSetThesLanguage(wxCommandEvent &event);
	void SetStyles();
	void SetTextDirection(bool rtl);
	void ApplyTextDirection();
	void UpdateStyle();
	void UpdateCallTip();
	void CloseCallTip();
	void Subscribe(std::string const& name);
	wxTextAttr SyntaxStyle(int id, wxFont const& font, wxColour const& default_background) const;

	/// Add the thesaurus suggestions to a menu
	void AddThesaurusEntries(wxMenu &menu);

	/// Add the spell checker suggestions to a menu
	void AddSpellCheckerEntries(wxMenu &menu);

	/// Generate a languages submenu from a list of locales and a current language
	/// @param base_id ID to use for the first menu item
	/// @param curLang Currently selected language
	/// @param langs Full list of languages
	wxMenu *GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs);

public:
	SubsTextEditCtrl(wxWindow* parent, wxSize size, long style, agi::Context *context);
	~SubsTextEditCtrl();

	void SetTextTo(std::string const& text);
	void Paste() override;

	std::pair<long, long> GetBoundsOfWordAtPosition(long pos);
};
