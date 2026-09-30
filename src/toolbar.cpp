// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file toolbar.cpp
/// @brief Dynamic menu toolbar generator.
/// @ingroup toolbar menu

#include "include/aegisub/toolbar.h"

#include "command/command.h"
#include "compat.h"
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "include/aegisub/hotkey.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "selection_controller.h"
#include "theme.h"
#include "utils.h"

#include <libaegisub/hotkey.h>
#include <libaegisub/json.h>
#include <libaegisub/log.h>
#include <libaegisub/signal.h>
#include <libaegisub/string.h>

#include <boost/interprocess/streams/bufferstream.hpp>
#include <vector>

#include <wx/frame.h>
#include <wx/button.h>
#include <wx/tglbtn.h>
#include <wx/msgdlg.h>
#include <wx/stattext.h>
#include <wx/toolbar.h>

#ifdef __WXMSW__
#include <commctrl.h>
#endif

namespace {
	json::Object const& get_root() {
		static json::Object root;
		if (root.empty()) {
			boost::interprocess::ibufferstream stream((const char *)default_toolbar, sizeof(default_toolbar));
			root = std::move(static_cast<json::Object&>(agi::json_util::parse(stream)));
		}
		return root;
	}

	class ThemedToolbar : public wxToolBar {
		bool const use_flat_palette;
		agi::signal::Connection toolbar_background_slot;
		agi::signal::Connection toolbar_active_background_slot;

		bool UsesDarkFlatBackground() const {
			return app_theme::IsDark() && use_flat_palette;
		}

		void ApplyDarkFlatBackground() {
			if (!UsesDarkFlatBackground()) return;
			SetBackgroundColour(app_theme::Colour("UI/Toolbar Background"));
			Refresh(false);
		}

#ifdef __WXMSW__
		bool MSWOnNotify(int idCtrl, WXLPARAM lParam, WXLPARAM *result) override {
			bool const handled = wxToolBar::MSWOnNotify(idCtrl, lParam, result);
			if (!app_theme::IsDark()) return handled;

			auto *header = reinterpret_cast<NMHDR *>(lParam);
			if (!header || header->code != NM_CUSTOMDRAW) return handled;

			auto *draw = reinterpret_cast<NMTBCUSTOMDRAW *>(lParam);
			if (draw->nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return handled;

			auto const background = GetBackgroundColour();
			COLORREF const background_ref = RGB(background.Red(), background.Green(), background.Blue());
			draw->clrBtnFace = background_ref;
			draw->clrBtnHighlight = background_ref;
			auto const hover = background.ChangeLightness(115);
			draw->clrHighlightHotTrack = RGB(hover.Red(), hover.Green(), hover.Blue());

			if (UsesDarkFlatBackground()) {
				auto const state = draw->nmcd.uItemState;
				bool const checked_without_hover =
					(state & (CDIS_CHECKED | CDIS_HOT)) == CDIS_CHECKED;
				bool const pressed = (state & CDIS_SELECTED) != 0;
				if (checked_without_hover || pressed) {
					auto const active = app_theme::Colour("UI/Toolbar Active Background");
					HBRUSH const brush = CreateSolidBrush(
						RGB(active.Red(), active.Green(), active.Blue()));
					FillRect(draw->nmcd.hdc, &draw->nmcd.rc, brush);
					DeleteObject(brush);
					*result |= TBCDRF_NOBACKGROUND;
				}
			}

			return handled;
		}
#endif

	public:
		ThemedToolbar(wxWindow *parent, long style, bool use_flat_palette)
		: wxToolBar(parent, -1, wxDefaultPosition, wxDefaultSize, style)
		, use_flat_palette(use_flat_palette)
		{
			ApplyDarkFlatBackground();
			Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
				event.Skip();
				if (UsesDarkFlatBackground()) Refresh(true);
			});
			if (UsesDarkFlatBackground()) {
				toolbar_background_slot = OPT_SUB("Colour/Dark/UI/Toolbar Background",
					[this](agi::OptionValue const&) { ApplyDarkFlatBackground(); });
				toolbar_active_background_slot = OPT_SUB("Colour/Dark/UI/Toolbar Active Background",
					[this](agi::OptionValue const&) { Refresh(false); });
			}
		}
	};

	bool uses_dark_flat_background(std::string const& name) {
		return name == "audio" || name == "video" || name == "visual_tools";
	}

	class Toolbar final : public ThemedToolbar {
		/// Window ID of first toolbar control
		static const int TOOL_ID_BASE = 5000;

		/// Toolbar name in config file
		std::string name;
		/// Project context
		agi::Context *context;
		/// Commands for each of the buttons
		std::vector<cmd::Command *> commands;
		/// Commands whose toolbar presence follows their validation state
		std::vector<std::pair<cmd::Command *, bool>> conditional_commands;
		/// Hotkey context
		std::string ht_context;

		/// Current icon size
		int icon_size;

		/// Listener for icon size change signal
		agi::signal::Connection icon_size_slot;

		/// Listener for hotkey change signal
		agi::signal::Connection hotkeys_changed_slot;
		/// Listener for changes which can show or hide contextual toolbar commands
		agi::signal::Connection active_line_slot;
		wxButton *dark_mode_button = nullptr;
		wxToggleButton *top_bar_button = nullptr;

		void OnToggleTopBar(wxCommandEvent&) {
			auto *option = OPT_SET("App/Show Top Bar");
			option->SetBool(!option->GetBool());
			config::opt->Flush();
			if (top_bar_button) top_bar_button->SetValue(option->GetBool());
		}

		wxString DarkModeButtonLabel() const {
			return OPT_GET("App/Dark Mode")->GetBool()
				? _("Disable Dark Mode")
				: _("Enable Dark Mode");
		}

		void UpdateDarkModeButtonLabel() {
			if (!dark_mode_button) return;
			dark_mode_button->SetLabel(DarkModeButtonLabel());
			Realize();
			if (GetParent()) GetParent()->Layout();
		}

		void OnToggleDarkMode(wxCommandEvent&) {
			auto *option = OPT_SET("App/Dark Mode");
			option->SetBool(!option->GetBool());
			config::opt->Flush();
			UpdateDarkModeButtonLabel();

			if (wxYES != wxMessageBox(
				_("Aegisub needs to be restarted so that the new appearance can be applied. Restart now?"),
				_("Restart Aegisub?"), wxYES_NO | wxICON_QUESTION | wxCENTER, this))
				return;

			if (context->frame->Close())
				RestartAegisub();
		}

		void AddDarkModeButton() {
			if (name != "main") return;

			AddStretchableSpace();

			// Explicit side padding on both buttons rather than the platform default. Windows
			// uses five pixels per side; the portable fallback keeps the existing six. The
			// widest possible label fixes the width when the dark-mode label changes.
			int horizontal_padding = FromDIP(6);
#ifdef __WXMSW__
			horizontal_padding = FromDIP(5);
#endif
			auto padded_size = [this](wxWindow *button,
					std::initializer_list<wxString> labels, int padding) {
				int widest = 0;
				for (auto const& label : labels)
					widest = std::max(widest, button->GetTextExtent(label).GetWidth());
				return wxSize(widest + 2 * padding, button->GetBestSize().GetHeight());
			};

			// Pressed in, the bar above the video stays put even where the tool in force has
			// nothing to put in it - which keeps everything below from shifting up and down as
			// tools come and go.
			top_bar_button = new wxToggleButton(this, wxID_ANY, _("Top Bar"),
				wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
			top_bar_button->SetValue(OPT_GET("App/Show Top Bar")->GetBool());
			top_bar_button->SetToolTip(_("Always show the bar above the video"));
			top_bar_button->Bind(wxEVT_TOGGLEBUTTON, &Toolbar::OnToggleTopBar, this);

			dark_mode_button = new wxButton(this, wxID_ANY, DarkModeButtonLabel(),
				wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
			dark_mode_button->SetToolTip(_("Enable or disable dark mode and restart Aegisub"));
			dark_mode_button->Bind(wxEVT_BUTTON, &Toolbar::OnToggleDarkMode, this);

			auto top_size = padded_size(top_bar_button, {_("Top Bar")}, horizontal_padding);
			auto dark_size = padded_size(dark_mode_button,
				{_("Disable Dark Mode"), _("Enable Dark Mode")}, horizontal_padding);
			int shared_height = std::max(top_size.GetHeight(), dark_size.GetHeight());
			top_size.SetHeight(shared_height);
			dark_size.SetHeight(shared_height);
			top_bar_button->SetSize(top_size);
			dark_mode_button->SetSize(dark_size);
			top_bar_button->SetMinSize(top_size);
			dark_mode_button->SetMinSize(dark_size);

			AddControl(top_bar_button);
			AddControl(dark_mode_button);
		}

		bool RefreshConditionalVisibility() {
			for (auto const& [command, shown] : conditional_commands) {
				if (shown != command->Validate(context)) {
					RegenerateToolbar();
					return true;
				}
			}
			return false;
		}

		/// Enable/disable the toolbar buttons
		void OnIdle(wxIdleEvent &) {
			if (RefreshConditionalVisibility()) return;
			for (size_t i = 0; i < commands.size(); ++i) {
				if (commands[i]->Type() & cmd::COMMAND_VALIDATE)
					EnableTool(TOOL_ID_BASE + i, commands[i]->Validate(context));
				if (commands[i]->Type() & cmd::COMMAND_TOGGLE || commands[i]->Type() & cmd::COMMAND_RADIO)
					ToggleTool(TOOL_ID_BASE + i, commands[i]->IsActive(context));
			}
		}

		void OnActiveLineChanged(AssDialogue *) {
			RefreshConditionalVisibility();
		}

		void BindConditionalContextUpdates() {
			if (!conditional_commands.empty())
				active_line_slot = context->selectionController->AddActiveLineListener(
					&Toolbar::OnActiveLineChanged, this);
		}

		/// Toolbar button click handler
		void OnClick(wxCommandEvent &evt) {
			(*commands[evt.GetId() - TOOL_ID_BASE])(context);
		}

		/// Regenerate the toolbar when the icon size changes
		void OnIconSizeChange(agi::OptionValue const& opt) {
			icon_size = opt.GetInt();
			RegenerateToolbar();
		}

		/// Clear the toolbar and recreate it
		void RegenerateToolbar() {
			Unbind(wxEVT_IDLE, &Toolbar::OnIdle, this);
			ClearTools();
			dark_mode_button = nullptr;
			commands.clear();
			conditional_commands.clear();
			Populate();
		}

		/// Populate the toolbar with buttons
		void Populate() {
			json::Object const& root = get_root();
			auto root_it = root.find(name);
			if (root_it == root.end()) {
				// Toolbar names are all hardcoded so this should never happen
				throw agi::InternalError("Toolbar named " + name + " not found.");
			}

			json::Array const& arr = root_it->second;
			commands.reserve(arr.size());
			bool needs_onidle = false;
			bool have_tool = false;
			bool pending_separator = false;

			for (json::String const& command_name : arr) {
				if (command_name == "spacer/40") {
					// A fixed, non-focusable gap without a separator line.
					auto size = FromDIP(wxSize(40, 1));
					auto spacer = new wxStaticText(this, wxID_ANY, wxEmptyString,
						wxDefaultPosition, size, wxST_NO_AUTORESIZE);
					spacer->SetMinSize(size);
					AddControl(spacer);
					pending_separator = false;
					continue;
				}
				if (command_name.empty()) {
					pending_separator = have_tool;
					continue;
				}

				cmd::Command *command;
				try {
					command = cmd::get(command_name);
				}
				catch (cmd::CommandNotFound const&) {
					LOG_W("toolbar/command/not_found") << "Command '" << command_name << "' not found; skipping";
					continue;
				}

				int flags = command->Type();
				if (flags & cmd::COMMAND_HIDE_INVALID) {
					bool shown = command->Validate(context);
					conditional_commands.emplace_back(command, shown);
					needs_onidle = true;
					if (!shown) continue;
				}
				if (pending_separator && have_tool) AddSeparator();
				pending_separator = false;
				wxItemKind kind =
					flags & cmd::COMMAND_RADIO ? wxITEM_RADIO :
					flags & cmd::COMMAND_TOGGLE ? wxITEM_CHECK :
					wxITEM_NORMAL;

				AddTool(TOOL_ID_BASE + commands.size(), command->StrDisplay(context), command->Icon(icon_size, GetLayoutDirection()), command->GetTooltip(ht_context), kind);

				commands.push_back(command);
				have_tool = true;
				needs_onidle = needs_onidle || flags != cmd::COMMAND_NORMAL;
			}

			AddDarkModeButton();

			// Only bind the update function if there are actually any dynamic tools
			if (needs_onidle) {
				Bind(wxEVT_IDLE, &Toolbar::OnIdle, this);
			}

			Realize();
			if (GetParent()) GetParent()->Layout();
		}

	public:
		Toolbar(wxWindow *parent, std::string name, agi::Context *c, std::string ht_context, bool vertical)
		: ThemedToolbar(parent, wxTB_NODIVIDER | wxTB_FLAT | (vertical ? wxTB_VERTICAL : wxTB_HORIZONTAL),
			uses_dark_flat_background(name))
		, name(std::move(name))
		, context(c)
		, ht_context(std::move(ht_context))
		, icon_size(OPT_GET("App/Toolbar Icon Size")->GetInt())
		, icon_size_slot(OPT_SUB("App/Toolbar Icon Size", &Toolbar::OnIconSizeChange, this))
		, hotkeys_changed_slot(hotkey::inst->AddHotkeyChangeListener(&Toolbar::RegenerateToolbar, this))
		{
			Populate();
			BindConditionalContextUpdates();
			Bind(wxEVT_TOOL, &Toolbar::OnClick, this);
			Bind(wxEVT_DPI_CHANGED, [this] (wxDPIChangedEvent &e) { RegenerateToolbar(); e.Skip(); });
		}

		Toolbar(wxFrame *parent, std::string name, agi::Context *c, std::string ht_context)
		: ThemedToolbar(parent, wxTB_FLAT | wxTB_HORIZONTAL, uses_dark_flat_background(name))
		, name(std::move(name))
		, context(c)
		, ht_context(std::move(ht_context))
#ifndef __WXMAC__
		, icon_size(OPT_GET("App/Toolbar Icon Size")->GetInt())
		, icon_size_slot(OPT_SUB("App/Toolbar Icon Size", &Toolbar::OnIconSizeChange, this))
#else
		, icon_size(32)
#endif
		, hotkeys_changed_slot(hotkey::inst->AddHotkeyChangeListener(&Toolbar::RegenerateToolbar, this))
		{
			parent->SetToolBar(this);
			Populate();
			BindConditionalContextUpdates();
			Bind(wxEVT_TOOL, &Toolbar::OnClick, this);
			Bind(wxEVT_DPI_CHANGED, [this] (wxDPIChangedEvent &e) { RegenerateToolbar(); e.Skip(); });
		}
	};
}

namespace toolbar {
	void AttachToolbar(wxFrame *frame, std::string const& name, agi::Context *c, std::string const& hotkey) {
		new Toolbar(frame, name, c, hotkey);
	}

	wxToolBar *GetToolbar(wxWindow *parent, std::string const& name, agi::Context *c, std::string const& hotkey, bool vertical) {
		return new Toolbar(parent, name, c, hotkey, vertical);
	}

	wxToolBar *GetVisualSubToolbar(wxWindow *parent) {
		return new ThemedToolbar(parent,
			wxTB_VERTICAL | wxTB_BOTTOM | wxTB_NODIVIDER | wxTB_FLAT, true);
	}
}
