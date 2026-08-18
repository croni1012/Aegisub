// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.

#include "subs_edit_ctrl.h"

#include "ass_dialogue.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/spellchecker.h"
#include "options.h"
#include "selection_controller.h"
#include "text_selection_controller.h"
#include "theme.h"
#include "thesaurus.h"
#include "utils.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/calltip_provider.h>
#include <libaegisub/character_count.h>
#include <libaegisub/spellchecker.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <algorithm>
#include <functional>
#include <string_view>

#include <wx/intl.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/tipwin.h>

#define LANGS_MAX 1000

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

namespace {
long EditorStyle(long style) {
	long result = wxTE_MULTILINE | wxTE_WORDWRAP | style;
#ifdef __WXMSW__
	result |= wxTE_RICH2;
#endif
	return result;
}

std::string ToUtf8(wxString const& text) {
	auto utf8 = text.utf8_str();
	return std::string(utf8.data(), utf8.length());
}

size_t Utf8Position(wxTextCtrl const& ctrl, long position) {
	return ToUtf8(ctrl.GetRange(0, std::max(0L, position))).size();
}

long TextPosition(std::string_view text, size_t byte_position) {
	byte_position = std::min(byte_position, text.size());
	return static_cast<long>(wxString::FromUTF8(text.data(), byte_position).length());
}

char const *StyleName(int id) {
	using namespace agi::ass::SyntaxStyle;
	switch (id) {
		case COMMENT: return "Comment";
		case DRAWING_CMD: return "Drawing Command";
		case OVERRIDE: return "Brackets";
		case PUNCTUATION: return "Slashes";
		case TAG: return "Tags";
		case ERROR: return "Error";
		case PARAMETER: return "Parameters";
		case LINE_BREAK: return "Line Break";
		case KARAOKE_TEMPLATE: return "Karaoke Template";
		case KARAOKE_VARIABLE: return "Karaoke Variable";
		default: return "Normal";
	}
}
}

SubsTextEditCtrl::SubsTextEditCtrl(wxWindow *parent, wxSize size, long style, agi::Context *context)
: wxTextCtrl(parent, wxID_ANY, {}, wxDefaultPosition, size, EditorStyle(style))
, spellchecker(SpellCheckerFactory::GetSpellChecker())
, thesaurus(std::make_unique<Thesaurus>())
, context(context)
{
	using std::bind;

	Bind(wxEVT_CHAR_HOOK, &SubsTextEditCtrl::OnKeyDown, this);
	Bind(wxEVT_TEXT, &SubsTextEditCtrl::OnText, this);
	Bind(wxEVT_KILL_FOCUS, &SubsTextEditCtrl::OnLoseFocus, this);
	Bind(wxEVT_CONTEXT_MENU, &SubsTextEditCtrl::OnContextMenu, this);
	Bind(wxEVT_IDLE, bind(&SubsTextEditCtrl::UpdateCallTip, this));
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Cut, this), EDIT_MENU_CUT);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Copy, this), EDIT_MENU_COPY);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Paste, this), EDIT_MENU_PASTE);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::SelectAll, this), EDIT_MENU_SELECT_ALL);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnToggleRTL, this, EDIT_MENU_RTL);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnUseSuggestion, this, EDIT_MENU_SUGGESTIONS, EDIT_MENU_THESAURUS - 1);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnUseSuggestion, this, EDIT_MENU_THESAURUS_SUGS, EDIT_MENU_DIC_LANGUAGE - 1);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnSetDicLanguage, this, EDIT_MENU_DIC_LANGS, EDIT_MENU_THES_LANGUAGE - 1);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnSetThesLanguage, this, EDIT_MENU_THES_LANGS, EDIT_MENU_THES_LANGS + LANGS_MAX);

	if (context) {
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/comment", context), EDIT_MENU_COMMENT);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/preserve", context), EDIT_MENU_SPLIT_PRESERVE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/estimate", context), EDIT_MENU_SPLIT_ESTIMATE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/video", context), EDIT_MENU_SPLIT_VIDEO);
	}

	OPT_SUB("Subtitle/Edit Box/Font Face", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Edit Box/Font Size", &SubsTextEditCtrl::SetStyles, this);
	Subscribe("Normal");
	Subscribe("Comment");
	Subscribe("Drawing Command");
	OPT_SUB("Colour/Subtitle/Syntax/Underline/Drawing Endpoint", &SubsTextEditCtrl::SetStyles, this);
	Subscribe("Brackets");
	Subscribe("Slashes");
	Subscribe("Tags");
	Subscribe("Error");
	Subscribe("Parameters");
	Subscribe("Line Break");
	Subscribe("Karaoke Template");
	Subscribe("Karaoke Variable");

	OPT_SUB(app_theme::ColourOption("Subtitle/Background"), &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Highlight/Syntax", &SubsTextEditCtrl::UpdateStyle, this);
	OPT_SUB("App/Call Tips", &SubsTextEditCtrl::UpdateCallTip, this);
	OPT_SUB("Subtitle/Edit Box/RTL Mode", [this](agi::OptionValue const& value) {
		SetLayoutDirection(value.GetBool() ? wxLayout_RightToLeft : wxLayout_LeftToRight);
		Refresh();
	});

	Bind(wxEVT_MENU, [this](wxCommandEvent&) {
		if (spellchecker) spellchecker->AddWord(currentWord);
		UpdateStyle();
		SetFocus();
	}, EDIT_MENU_ADD_TO_DICT);
	Bind(wxEVT_MENU, [this](wxCommandEvent&) {
		if (spellchecker) spellchecker->RemoveWord(currentWord);
		UpdateStyle();
		SetFocus();
	}, EDIT_MENU_REMOVE_FROM_DICT);

	SetLayoutDirection(OPT_GET("Subtitle/Edit Box/RTL Mode")->GetBool() ? wxLayout_RightToLeft : wxLayout_LeftToRight);
	SetStyles();
}

SubsTextEditCtrl::~SubsTextEditCtrl() {
	CloseCallTip();
}

void SubsTextEditCtrl::Subscribe(std::string const& name) {
	OPT_SUB(app_theme::ColourOption("Subtitle/Syntax/" + name), &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB(app_theme::ColourOption("Subtitle/Syntax/Background/" + name), &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Bold/" + name, &SubsTextEditCtrl::SetStyles, this);
}

wxTextAttr SubsTextEditCtrl::SyntaxStyle(int id, wxFont const& base_font, wxColour const& default_background) const {
	std::string name = StyleName(id);
	wxFont font = base_font;
	font.SetWeight(OPT_GET("Colour/Subtitle/Syntax/Bold/" + name)->GetBool() ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
	if ((id == agi::ass::SyntaxStyle::DRAWING_ENDPOINT_X || id == agi::ass::SyntaxStyle::DRAWING_ENDPOINT_Y) &&
		OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool())
		font.SetUnderlined(true);
	wxColour background = default_background;
	auto background_option = OPT_GET(app_theme::ColourOption("Subtitle/Syntax/Background/" + name));
	if (background_option->GetType() == agi::OptionType::Color)
		background = to_wx(background_option->GetColor());
	return wxTextAttr(app_theme::Colour("Subtitle/Syntax/" + name), background, font);
}

void SubsTextEditCtrl::SetStyles() {
	wxFont font = *wxNORMAL_FONT;
	font.SetEncoding(wxFONTENCODING_DEFAULT);
	auto face = FontFace("Subtitle/Edit Box");
	if (!face.empty()) font.SetFaceName(face);
	font.SetPointSize(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt());
	SetFont(font);
	SetBackgroundColour(app_theme::Colour("Subtitle/Background"));
	SetDefaultStyle(SyntaxStyle(agi::ass::SyntaxStyle::NORMAL, font, app_theme::Colour("Subtitle/Background")));
	UpdateStyle();
}

void SubsTextEditCtrl::UpdateStyle() {
	if (styling) return;
	styling = true;
	line_text = ToUtf8(GetValue());
	AssDialogue *line = context ? context->selectionController->GetActiveLine() : nullptr;
	bool template_line = line && line->Comment &&
		(boost::istarts_with(line->Effect.get(), "template") || boost::istarts_with(line->Effect.get(), "mixin"));
	tokenized_line = agi::ass::TokenizeDialogueBody(line_text, template_line);
	agi::ass::SplitWords(line_text, tokenized_line);
	cursor_pos = -1;

	wxFont font = GetFont();
	wxColour background = app_theme::Colour("Subtitle/Background");
	auto normal = SyntaxStyle(agi::ass::SyntaxStyle::NORMAL, font, background);
	SetDefaultStyle(normal);
	SetStyle(0, GetLastPosition(), normal);
	if (OPT_GET("Subtitle/Highlight/Syntax")->GetBool()) {
		size_t byte_position = 0;
		for (auto const& range : agi::ass::SyntaxHighlight(line_text, tokenized_line, spellchecker.get())) {
			int style_id = range.type == agi::ass::SyntaxStyle::SPELLING ? agi::ass::SyntaxStyle::NORMAL : range.type;
			auto attr = SyntaxStyle(style_id, font, background);
			if (range.type == agi::ass::SyntaxStyle::SPELLING) {
				auto spelling_font = attr.GetFont();
				spelling_font.SetUnderlined(true);
				attr.SetFont(spelling_font);
				attr.SetTextColour(*wxRED);
			}
			long start = TextPosition(line_text, byte_position);
			byte_position += range.length;
			SetStyle(start, TextPosition(line_text, byte_position), attr);
		}
	}
	styling = false;
	UpdateCallTip();
}

void SubsTextEditCtrl::UpdateCallTip() {
	if (!OPT_GET("App/Call Tips")->GetBool() || !HasFocus()) {
		CloseCallTip();
		return;
	}
	long native_position = GetInsertionPoint();
	if (native_position == cursor_pos) return;
	cursor_pos = native_position;
	auto new_calltip = agi::GetCalltip(tokenized_line, line_text, Utf8Position(*this, native_position));
	if (!new_calltip.text) {
		CloseCallTip();
		return;
	}
	if (!calltip || calltip_position != new_calltip.tag_position || calltip_text != new_calltip.text) {
		CloseCallTip();
		calltip_position = new_calltip.tag_position;
		calltip_text = new_calltip.text;
		calltip = new wxTipWindow(this, wxString::FromUTF8Unchecked(new_calltip.text), 500, &calltip);
	}
}

void SubsTextEditCtrl::CloseCallTip() {
	if (calltip) calltip->Close();
	calltip = nullptr;
}

void SubsTextEditCtrl::OnText(wxCommandEvent &event) {
	UpdateStyle();
	event.Skip();
}

void SubsTextEditCtrl::OnLoseFocus(wxFocusEvent &event) {
	CloseCallTip();
	event.Skip();
}

void SubsTextEditCtrl::OnKeyDown(wxKeyEvent &event) {
	if (event.GetKeyCode() == WXK_TAB) {
		Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
		return;
	}
	if (event.GetKeyCode() == WXK_RETURN && event.GetModifiers() == wxMOD_SHIFT) {
		long start, end;
		GetSelection(&start, &end);
		wxString line_break = OPT_GET("Subtitle/Edit Box/Soft Line Break")->GetBool() ? "\\n" : "\\N";
		Replace(start, end, line_break);
		SetInsertionPoint(start + line_break.length());
		return;
	}
	event.Skip();
}

void SubsTextEditCtrl::SetTextTo(std::string const& text) {
	Freeze();
	std::string prefix = ToUtf8(GetRange(0, GetInsertionPoint()));
	size_t character = agi::CharacterCount(prefix, 0);
	ChangeValue(to_wx(text));
	size_t byte_position = agi::IndexOfCharacter(text, character);
	long native_position = TextPosition(text, byte_position);
	if (context)
		context->textSelectionController->SetSelection(byte_position, byte_position);
	else
		SetSelection(native_position, native_position);
	UpdateStyle();
	Thaw();
}

void SubsTextEditCtrl::Paste() {
	std::string data = GetClipboard();
	boost::replace_all(data, "\r\n", "\\N");
	boost::replace_all(data, "\n", "\\N");
	boost::replace_all(data, "\r", "\\N");
	long start, end;
	GetSelection(&start, &end);
	wxString replacement = to_wx(data);
	Replace(start, end, replacement);
	SetInsertionPoint(start + replacement.length());
}

void SubsTextEditCtrl::OnContextMenu(wxContextMenuEvent &event) {
	long active_position = GetInsertionPoint();
	if (event.GetPosition() != wxDefaultPosition) {
		long hit_position = -1;
		HitTest(ScreenToClient(event.GetPosition()), &hit_position);
		if (hit_position >= 0) active_position = hit_position;
	}
	currentWordPos = GetBoundsOfWordAtPosition(active_position);
	currentWord = ToUtf8(GetRange(currentWordPos.first, currentWordPos.first + currentWordPos.second));

	wxMenu menu;
	if (context) {
		auto comment = cmd::get("edit/comment");
		menu.Append(EDIT_MENU_COMMENT, comment->StrMenu(context))->Enable(comment->Validate(context));
		menu.AppendSeparator();
	}
	if (spellchecker) {
		AddSpellCheckerEntries(menu);
		menu.AppendSubMenu(GetLanguagesMenu(EDIT_MENU_DIC_LANGS,
			to_wx(OPT_GET("Tool/Spell Checker/Language")->GetString()),
			to_wx(spellchecker->GetLanguageList())), _("Spell checker language"));
		menu.AppendSeparator();
	}
	AddThesaurusEntries(menu);
	menu.Append(EDIT_MENU_CUT, _("Cu&t"))->Enable(!GetStringSelection().empty());
	menu.Append(EDIT_MENU_COPY, _("&Copy"))->Enable(!GetStringSelection().empty());
	menu.Append(EDIT_MENU_PASTE, _("&Paste"))->Enable(CanPaste());
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_SELECT_ALL, _("Select &All"));
	menu.AppendSeparator();
	menu.AppendCheckItem(EDIT_MENU_RTL, _("Right to left reading order"))->Check(GetLayoutDirection() == wxLayout_RightToLeft);
	if (context) {
		menu.AppendSeparator();
		menu.Append(EDIT_MENU_SPLIT_PRESERVE, _("Split at cursor (preserve times)"));
		menu.Append(EDIT_MENU_SPLIT_ESTIMATE, _("Split at cursor (estimate times)"));
		auto split_video = cmd::get("edit/line/split/video");
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
		if (sugs.empty()) menu.Append(EDIT_MENU_SUGGESTION, _("No spell checker suggestions"))->Enable(false);
		else {
			auto submenu = new wxMenu;
			for (size_t i = 0; i < sugs.size(); ++i) submenu->Append(EDIT_MENU_SUGGESTIONS + i, to_wx(sugs[i]));
			menu.AppendSubMenu(submenu, fmt_tl("Spell checker suggestions for \"%s\"", currentWord));
		}
	}
	else {
		if (sugs.empty()) menu.Append(EDIT_MENU_SUGGESTION, _("No correction suggestions"))->Enable(false);
		for (size_t i = 0; i < sugs.size(); ++i) menu.Append(EDIT_MENU_SUGGESTIONS + i, to_wx(sugs[i]));
		menu.Append(EDIT_MENU_ADD_TO_DICT, fmt_tl("Add \"%s\" to dictionary", currentWord))->Enable(spellchecker->CanAddWord(currentWord));
	}
}

void SubsTextEditCtrl::AddThesaurusEntries(wxMenu &menu) {
	if (currentWord.empty()) return;
	auto results = thesaurus->Lookup(currentWord);
	thesSugs.clear();
	if (!results.empty()) {
		auto thesaurus_menu = new wxMenu;
		int index = 0;
		for (auto const& result : results) {
			if (result.second.empty()) {
				thesaurus_menu->Append(EDIT_MENU_THESAURUS_SUGS + index++, to_wx(result.first));
				thesSugs.push_back(result.first);
			}
			else {
				auto submenu = new wxMenu;
				for (auto const& suggestion : result.second) {
					submenu->Append(EDIT_MENU_THESAURUS_SUGS + index++, to_wx(suggestion));
					thesSugs.push_back(suggestion);
				}
				thesaurus_menu->AppendSubMenu(submenu, to_wx(result.first));
			}
		}
		menu.AppendSubMenu(thesaurus_menu, fmt_tl("Thesaurus suggestions for \"%s\"", currentWord));
	}
	else menu.Append(EDIT_MENU_THESAURUS, _("No thesaurus suggestions"))->Enable(false);
	menu.AppendSubMenu(GetLanguagesMenu(EDIT_MENU_THES_LANGS,
		to_wx(OPT_GET("Tool/Thesaurus/Language")->GetString()),
		to_wx(thesaurus->GetLanguageList())), _("Thesaurus language"));
	menu.AppendSeparator();
}

wxMenu *SubsTextEditCtrl::GetLanguagesMenu(int base_id, wxString const& current, wxArrayString const& languages) {
	auto menu = new wxMenu;
	menu->AppendRadioItem(base_id, _("Disable"))->Check(current.empty());
	for (size_t i = 0; i < languages.size(); ++i)
		menu->AppendRadioItem(base_id + i + 1, LocalizedLanguageName(languages[i]))->Check(languages[i] == current);
	return menu;
}

void SubsTextEditCtrl::OnToggleRTL(wxCommandEvent&) {
	if (context)
		cmd::call("edit/toggle_rtl_mode", context);
	else
		OPT_SET("Subtitle/Edit Box/RTL Mode")->SetBool(!OPT_GET("Subtitle/Edit Box/RTL Mode")->GetBool());
}

void SubsTextEditCtrl::OnUseSuggestion(wxCommandEvent &event) {
	std::string suggestion;
	if (event.GetId() >= EDIT_MENU_THESAURUS_SUGS) {
		size_t index = event.GetId() - EDIT_MENU_THESAURUS_SUGS;
		if (index >= thesSugs.size()) return;
		suggestion = thesSugs[index];
	}
	else {
		size_t index = event.GetId() - EDIT_MENU_SUGGESTIONS;
		if (index >= sugs.size()) return;
		suggestion = sugs[index];
	}

	size_t position;
	while ((position = suggestion.rfind('(')) != std::string::npos) {
		if (position == 0) {
			position = suggestion.find(')');
			if (position != std::string::npos) {
				if (position + 1 < suggestion.size() && suggestion[position + 1] == ' ') ++position;
				suggestion.erase(0, position + 1);
			}
			break;
		}
		suggestion.resize(position - 1);
	}
	wxString replacement = to_wx(suggestion);
	Replace(currentWordPos.first, currentWordPos.first + currentWordPos.second, replacement);
	SetSelection(currentWordPos.first, currentWordPos.first + replacement.length());
	SetFocus();
}

void SubsTextEditCtrl::OnSetDicLanguage(wxCommandEvent &event) {
	auto languages = spellchecker->GetLanguageList();
	int index = event.GetId() - EDIT_MENU_DIC_LANGS - 1;
	OPT_SET("Tool/Spell Checker/Language")->SetString(index >= 0 && static_cast<size_t>(index) < languages.size() ? languages[index] : "");
	UpdateStyle();
}

void SubsTextEditCtrl::OnSetThesLanguage(wxCommandEvent &event) {
	if (!thesaurus) return;
	auto languages = thesaurus->GetLanguageList();
	int index = event.GetId() - EDIT_MENU_THES_LANGS - 1;
	OPT_SET("Tool/Thesaurus/Language")->SetString(index >= 0 && static_cast<size_t>(index) < languages.size() ? languages[index] : "");
}

std::pair<long, long> SubsTextEditCtrl::GetBoundsOfWordAtPosition(long native_position) {
	if (native_position < 0 || native_position > GetLastPosition()) return {0, 0};
	if (line_text != ToUtf8(GetValue())) UpdateStyle();
	size_t byte_position = Utf8Position(*this, native_position);
	size_t token_start = 0;
	for (auto const& token : tokenized_line) {
		size_t token_end = token_start + token.length;
		if (token.type == agi::ass::DialogueTokenType::WORD && byte_position >= token_start && byte_position <= token_end) {
			long start = TextPosition(line_text, token_start);
			return {start, TextPosition(line_text, token_end) - start};
		}
		token_start = token_end;
	}
	return {native_position, 0};
}
