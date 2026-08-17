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

#include "subs_edit_ctrl.h"

#include "ass_dialogue.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "options.h"
#include "include/aegisub/context.h"
#include "include/aegisub/spellchecker.h"
#include "selection_controller.h"
#include "text_selection_controller.h"
#include "thesaurus.h"
#include "utils.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/calltip_provider.h>
#include <libaegisub/character_count.h>
#include <libaegisub/spellchecker.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <functional>

#include <wx/clipbrd.h>
#include <wx/intl.h>
#include <wx/menu.h>
#include <wx/settings.h>

// Maximum number of languages (locales)
// It should be above 100 (at least 242) and probably not more than 1000
#define LANGS_MAX 1000

/// Event ids
// Check menu.h for id range allocation before editing this enum
enum {
	EDIT_MENU_SPLIT_PRESERVE = (wxID_HIGHEST + 1) + 4000,
	EDIT_MENU_SPLIT_ESTIMATE,
	EDIT_MENU_SPLIT_VIDEO,
	EDIT_MENU_COMMENT,
	EDIT_MENU_CUT,
	EDIT_MENU_COPY,
	EDIT_MENU_PASTE,
	EDIT_MENU_SELECT_ALL,
	EDIT_MENU_ADD_TO_DICT,
	EDIT_MENU_REMOVE_FROM_DICT,
	EDIT_MENU_SUGGESTION,
	EDIT_MENU_SUGGESTIONS,
	EDIT_MENU_THESAURUS = (wxID_HIGHEST + 1) + 5000,
	EDIT_MENU_THESAURUS_SUGS,
	EDIT_MENU_DIC_LANGUAGE = (wxID_HIGHEST + 1) + 6000,
	EDIT_MENU_DIC_LANGS,
	EDIT_MENU_THES_LANGUAGE = EDIT_MENU_DIC_LANGUAGE + LANGS_MAX,
	EDIT_MENU_THES_LANGS,
	EDIT_MENU_RTL = (wxID_HIGHEST + 1) + 7000
};

SubsTextEditCtrl::SubsTextEditCtrl(wxWindow* parent, wxSize wsize, long style, agi::Context *context)
: wxTextCtrl(parent, -1, "", wxDefaultPosition, wsize, wxTE_MULTILINE | wxTE_WORDWRAP | style)
, spellchecker(SpellCheckerFactory::GetSpellChecker())
, thesaurus(std::make_unique<Thesaurus>())
, context(context)
{
	// Set font
	wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	font.SetEncoding(wxFONTENCODING_DEFAULT);
	wxString fontname = FontFace("Subtitle/Edit Box");
	if (!fontname.empty()) font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt());
	SetFont(font);

	// Apply RTL mode from config if enabled
	if (OPT_GET("Subtitle/Edit Box/RTL Mode")->GetBool())
		SetLayoutDirection(wxLayout_RightToLeft);

	using std::bind;

	Bind(wxEVT_CHAR_HOOK, &SubsTextEditCtrl::OnKeyDown, this);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Cut, this), EDIT_MENU_CUT);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Copy, this), EDIT_MENU_COPY);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Paste, this), EDIT_MENU_PASTE);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::SelectAll, this), EDIT_MENU_SELECT_ALL);

	if (context) {
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/preserve", context), EDIT_MENU_SPLIT_PRESERVE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/estimate", context), EDIT_MENU_SPLIT_ESTIMATE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/video", context), EDIT_MENU_SPLIT_VIDEO);
	}

	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnToggleRTL, this, EDIT_MENU_RTL);
	Bind(wxEVT_CONTEXT_MENU, &SubsTextEditCtrl::OnContextMenu, this);

	// Subscribe to font and color changes
	OPT_SUB("Subtitle/Edit Box/Font Face", [this](agi::OptionValue const&) {
		wxFont font = GetFont();
		font.SetFaceName(FontFace("Subtitle/Edit Box"));
		SetFont(font);
	});
	OPT_SUB("Subtitle/Edit Box/Font Size", [this](agi::OptionValue const&) {
		wxFont font = GetFont();
		font.SetPointSize(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt());
		SetFont(font);
	});

	OPT_SUB("Subtitle/Edit Box/RTL Mode", [this](agi::OptionValue const& opt) {
		SetLayoutDirection(opt.GetBool() ? wxLayout_RightToLeft : wxLayout_LeftToRight);
		Refresh();
	});

	Bind(wxEVT_MENU, [this](wxCommandEvent&) {
		if (spellchecker) spellchecker->AddWord(currentWord);
		SetFocus();
	}, EDIT_MENU_ADD_TO_DICT);

	Bind(wxEVT_MENU, [this](wxCommandEvent&) {
		if (spellchecker) spellchecker->RemoveWord(currentWord);
		SetFocus();
	}, EDIT_MENU_REMOVE_FROM_DICT);
}

SubsTextEditCtrl::~SubsTextEditCtrl() {
}

void SubsTextEditCtrl::OnKeyDown(wxKeyEvent &event) {
	// Handle Shift+Return for soft line breaks (ASS newline)
	if (event.GetKeyCode() == WXK_RETURN && event.GetModifiers() == wxMOD_SHIFT) {
		long sel_start = GetInsertionPoint();
		long sel_end = sel_start;
		GetSelection(&sel_start, &sel_end);

		wxString text = GetValue();
		wxString new_text = text.substr(0, sel_start) + 
		                    (OPT_GET("Subtitle/Edit Box/Soft Line Break")->GetBool() ? "\\n" : "\\N") +
		                    text.substr(sel_end);
		
		SetValue(new_text);
		SetInsertionPoint(sel_start + 2);
		event.Skip(false);
		return;
	}

	// For all other keys, let the default handler process them
	event.Skip();
}



void SubsTextEditCtrl::SetTextTo(std::string const& text) {
	SetEvtHandlerEnabled(false);
	Freeze();

	long insertion_point = GetInsertionPoint();

	// Get current value as UTF-8 for proper position tracking
	wxString cur_wx = GetValue();
	std::string cur(cur_wx.utf8_str());

	// Compute old character index (clamped)
	size_t clamp_pos = std::min<size_t>(cur.size(), static_cast<size_t>(std::max<long>(0, insertion_point)));
	std::string_view cur_view(cur);
	size_t old_pos = agi::CharacterCount(cur_view.substr(0, clamp_pos), 0);

	if (context) {
		context->textSelectionController->SetSelection(0, 0);
		SetValue(to_wx(text));
		auto pos = agi::IndexOfCharacter(text, old_pos);
		context->textSelectionController->SetSelection(pos, pos);
	}
	else {
		SetSelection(0, 0);
		SetValue(to_wx(text));
		auto pos = agi::IndexOfCharacter(text, old_pos);
		SetSelection(pos, pos);
	}

	SetEvtHandlerEnabled(true);
	Thaw();
}

void SubsTextEditCtrl::Paste() {
	std::string data = GetClipboard();

	boost::replace_all(data, "\r\n", "\\N");
	boost::replace_all(data, "\n", "\\N");
	boost::replace_all(data, "\r", "\\N");

	wxString cur_text = GetValue();
	long sel_start, sel_end;
	GetSelection(&sel_start, &sel_end);

	wxString new_text = cur_text.substr(0, sel_start) + to_wx(data) + cur_text.substr(sel_end);
	SetValue(new_text);

	long new_pos = sel_start + data.size();
	SetInsertionPoint(new_pos);
}

void SubsTextEditCtrl::OnContextMenu(wxContextMenuEvent &event) {
	wxPoint pos = event.GetPosition();
	long insertion_point;
	
	if (pos == wxDefaultPosition)
		insertion_point = GetInsertionPoint();
	else {
		wxPoint screen_pos = ScreenToClient(pos);
		insertion_point = XYToPosition(screen_pos.x, screen_pos.y);
		if (insertion_point < 0) insertion_point = GetInsertionPoint();
	}

	currentWordPos = GetBoundsOfWordAtPosition(insertion_point);
	wxString text = GetValue();
	currentWord = std::string(text.substr(currentWordPos.first, currentWordPos.second).utf8_str());

	wxMenu menu;

	cmd::Command *comment = cmd::get("edit/comment");
	menu.Append(EDIT_MENU_COMMENT, comment->StrMenu(context));
	menu.AppendSeparator();

	if (spellchecker) {
		AddSpellCheckerEntries(menu);

		// Append language list
		menu.Append(-1, _("Spell checker language"), GetLanguagesMenu(
			EDIT_MENU_DIC_LANGS,
			to_wx(OPT_GET("Tool/Spell Checker/Language")->GetString()),
			to_wx(spellchecker->GetLanguageList())));
		menu.AppendSeparator();
	}

	AddThesaurusEntries(menu);

	// Standard actions
	menu.Append(EDIT_MENU_CUT,_("Cu&t"))->Enable(GetStringSelection().length() > 0);
	menu.Append(EDIT_MENU_COPY,_("&Copy"))->Enable(GetStringSelection().length() > 0);
	menu.Append(EDIT_MENU_PASTE,_("&Paste"))->Enable(CanPaste());
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_SELECT_ALL,_("Select &All"));
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_RTL, _("Right to left Reading order"));

	// Split
	if (context) {
		menu.AppendSeparator();
		menu.Append(EDIT_MENU_SPLIT_PRESERVE, _("Split at cursor (preserve times)"));
		menu.Append(EDIT_MENU_SPLIT_ESTIMATE, _("Split at cursor (estimate times)"));
		cmd::Command *split_video = cmd::get("edit/line/split/video");
		menu.Append(EDIT_MENU_SPLIT_VIDEO, split_video->StrMenu(context))->Enable(split_video->Validate(context));
	}

	PopupMenu(&menu);
}

void SubsTextEditCtrl::AddSpellCheckerEntries(wxMenu &menu) {
	if (currentWord.empty()) return;

	if (spellchecker->CanRemoveWord(currentWord))
		menu.Append(EDIT_MENU_REMOVE_FROM_DICT, fmt_tl("Remove \"%s\" from dictionary", currentWord));

	sugs = spellchecker->GetSuggestions(currentWord);
	if (spellchecker->CheckWord(currentWord)) {
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No spell checker suggestions"))->Enable(false);
		else {
			auto subMenu = new wxMenu;
			for (size_t i = 0; i < sugs.size(); ++i)
				subMenu->Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));

			menu.Append(-1, fmt_tl("Spell checker suggestions for \"%s\"", currentWord), subMenu);
		}
	}
	else {
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No correction suggestions"))->Enable(false);

		for (size_t i = 0; i < sugs.size(); ++i)
			menu.Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));

		// Append "add word"
		menu.Append(EDIT_MENU_ADD_TO_DICT, fmt_tl("Add \"%s\" to dictionary", currentWord))->Enable(spellchecker->CanAddWord(currentWord));
	}
}

void SubsTextEditCtrl::AddThesaurusEntries(wxMenu &menu) {
	if (currentWord.empty()) return;

	auto results = thesaurus->Lookup(currentWord);

	thesSugs.clear();

	if (results.size()) {
		auto thesMenu = new wxMenu;

		int curThesEntry = 0;
		for (auto const& result : results) {
			// Single word, insert directly
			if (result.second.empty()) {
				thesMenu->Append(EDIT_MENU_THESAURUS_SUGS+curThesEntry, to_wx(result.first));
				thesSugs.push_back(result.first);
				++curThesEntry;
			}
			// Multiple, create submenu
			else {
				auto subMenu = new wxMenu;
				for (auto const& sug : result.second) {
					subMenu->Append(EDIT_MENU_THESAURUS_SUGS+curThesEntry, to_wx(sug));
					thesSugs.push_back(sug);
					++curThesEntry;
				}

				thesMenu->Append(-1, to_wx(result.first), subMenu);
			}
		}

		menu.Append(-1, fmt_tl("Thesaurus suggestions for \"%s\"", currentWord), thesMenu);
	}
	else
		menu.Append(EDIT_MENU_THESAURUS,_("No thesaurus suggestions"))->Enable(false);

	// Append language list
	menu.Append(-1,_("Thesaurus language"), GetLanguagesMenu(
		EDIT_MENU_THES_LANGS,
		to_wx(OPT_GET("Tool/Thesaurus/Language")->GetString()),
		to_wx(thesaurus->GetLanguageList())));
	menu.AppendSeparator();
}

wxMenu *SubsTextEditCtrl::GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs) {
	auto languageMenu = new wxMenu;
	languageMenu->AppendRadioItem(base_id, _("Disable"))->Check(curLang.empty());

	for (size_t i = 0; i < langs.size(); ++i)
		languageMenu->AppendRadioItem(base_id + i + 1, LocalizedLanguageName(langs[i]))->Check(langs[i] == curLang);

	return languageMenu;
}

void SubsTextEditCtrl::OnToggleRTL([[maybe_unused]] wxCommandEvent &event) {
	bool current_rtl = OPT_GET("Subtitle/Edit Box/RTL Mode")->GetBool();
	bool new_rtl = !current_rtl;
	OPT_SET("Subtitle/Edit Box/RTL Mode")->SetBool(new_rtl);
	SetLayoutDirection(new_rtl ? wxLayout_RightToLeft : wxLayout_LeftToRight);
	Refresh();
}

void SubsTextEditCtrl::OnUseSuggestion(wxCommandEvent &event) {
	std::string suggestion;
	int sugIdx = event.GetId() - EDIT_MENU_THESAURUS_SUGS;
	if (sugIdx >= 0)
		suggestion = thesSugs[sugIdx];
	else
		suggestion = sugs[event.GetId() - EDIT_MENU_SUGGESTIONS];

	size_t pos;
	while ((pos = suggestion.rfind('(')) != std::string::npos) {
		// If there's only one suggestion for a word it'll be in the form "(noun) word",
		// so we need to trim the "(noun) " part
		if (pos == 0) {
			pos = suggestion.find(')');
			if (pos != std::string::npos) {
				if (pos + 1< suggestion.size() && suggestion[pos + 1] == ' ') ++pos;
				suggestion.erase(0, pos + 1);
			}
			break;
		}

		// Some replacements have notes about their usage after the word in the
		// form "word (generic term)" that we need to remove (plus the leading space)
		suggestion.resize(pos - 1);
	}

	// Replace the selected word using wxTextCtrl APIs.
	// The old implementation used Scintilla's line_text/SetTextRaw state,
	// which is not present after migrating SubsTextEditCtrl to wxTextCtrl.
	wxString text = GetValue();
	wxString suggestion_wx = to_wx(suggestion);
	text.replace(currentWordPos.first, currentWordPos.second, suggestion_wx);
	SetValue(text);
	SetSelection(currentWordPos.first, currentWordPos.first + static_cast<int>(suggestion_wx.length()));
	SetFocus();
}

void SubsTextEditCtrl::OnSetDicLanguage(wxCommandEvent &event) {
	std::vector<std::string> langs = spellchecker->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_DIC_LANGS - 1;
	std::string lang;
	if (index >= 0)
		lang = langs[index];

	OPT_SET("Tool/Spell Checker/Language")->SetString(lang);

	Refresh();
}

void SubsTextEditCtrl::OnSetThesLanguage(wxCommandEvent &event) {
	if (!thesaurus) return;

	std::vector<std::string> langs = thesaurus->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_THES_LANGS - 1;
	std::string lang;
	if (index >= 0) lang = langs[index];
	OPT_SET("Tool/Thesaurus/Language")->SetString(lang);

	Refresh();
}

std::pair<int, int> SubsTextEditCtrl::GetBoundsOfWordAtPosition(int pos) {
	wxString text = GetValue();
	if (pos < 0 || pos > (int)text.length()) return {0, 0};

	// Find the start of the word (scan backwards)
	int start = pos;
	while (start > 0 && wxIsalnum(text[start - 1]))
		start--;

	// Find the end of the word (scan forward)
	int end = pos;
	while (end < (int)text.length() && wxIsalnum(text[end]))
		end++;

	return {start, end - start};
}
