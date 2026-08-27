// Copyright (c) 2026, Muteki Aegisub

#include "dialog_animated_text.h"

#include "ass_dialogue.h"
#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "typesetting_animated_text.h"
#include "video_controller.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace {

using typesetting::animated_text::Animation;
using typesetting::animated_text::Rule;
using typesetting::animated_text::Settings;
using typesetting::animated_text::SplitMode;
using typesetting::animated_text::Tag;
using typesetting::animated_text::ValueMode;

class DialogAnimatedText final : public wxDialog {
	agi::Context *context;
	Settings settings;
	typesetting::animated_text::PreviewSession preview;
	wxTimer playback_timer;
	bool syncing = false;
	bool loop_playback = false;
	int active_animation = -1;
	int active_rule = -1;

	wxChoice *split = nullptr;
	wxSpinCtrl *delay = nullptr;
	wxListBox *animations = nullptr;
	wxCheckBox *animation_enabled = nullptr;
	wxSpinCtrl *duration = nullptr;
	wxSpinCtrl *repeats = nullptr;
	wxCheckBox *infinite = nullptr;
	wxListBox *rules = nullptr;
	wxChoice *tag = nullptr;
	wxChoice *mode = nullptr;
	wxSpinCtrlDouble *first = nullptr;
	wxSpinCtrlDouble *second = nullptr;
	wxStaticText *first_label = nullptr;
	wxStaticText *second_label = nullptr;

	void StopPlayback() {
		loop_playback = false;
		playback_timer.Stop();
		context->videoController->Stop();
	}

	void Play(bool loop) {
		StopPlayback();
		loop_playback = loop;
		context->videoController->PlayLine();
		if (loop_playback && context->videoController->IsPlaying()) playback_timer.Start(25);
	}

	void UpdatePreview() {
		if (!syncing) preview.Update(settings);
	}

	wxString AnimationLabel(Animation const& animation, size_t index) const {
		return wxString::Format(animation.enabled ? _("Animation %zu - %d frames") :
			_("Animation %zu - %d frames (off)"), index + 1, animation.duration_frames);
	}

	wxString RuleLabel(Rule const& rule, size_t index) const {
		auto tags = typesetting::animated_text::TagNames();
		auto modes = typesetting::animated_text::ValueModeNames();
		return wxString::Format(_("%zu. %s - %s"), index + 1,
			to_wx(tags[static_cast<size_t>(rule.tag)]),
			to_wx(modes[static_cast<size_t>(rule.mode)]));
	}

	void RefreshAnimations() {
		animations->Freeze(); animations->Clear();
		for (size_t i = 0; i < settings.animations.size(); ++i)
			animations->Append(AnimationLabel(settings.animations[i], i));
		animations->Thaw();
		if (active_animation >= static_cast<int>(settings.animations.size()))
			active_animation = static_cast<int>(settings.animations.size()) - 1;
		if (active_animation >= 0) animations->SetSelection(active_animation);
	}

	void RefreshRules() {
		rules->Freeze(); rules->Clear();
		if (active_animation >= 0) {
			auto const& list = settings.animations[static_cast<size_t>(active_animation)].rules;
			for (size_t i = 0; i < list.size(); ++i) rules->Append(RuleLabel(list[i], i));
			if (active_rule >= static_cast<int>(list.size()))
				active_rule = static_cast<int>(list.size()) - 1;
		}
		rules->Thaw();
		if (active_rule >= 0) rules->SetSelection(active_rule);
	}

	void UpdateRuleLabels() {
		bool range = mode->GetSelection() == static_cast<int>(ValueMode::Random);
		bool percent = mode->GetSelection() == static_cast<int>(ValueMode::PercentFixed) ||
			mode->GetSelection() == static_cast<int>(ValueMode::PercentRandom);
		first_label->SetLabel(range ? _("Minimum:") : percent ? _("Amount (%):") : _("Value:"));
		second_label->SetLabel(_("Maximum:"));
		second_label->Show(range);
		second->Show(range);
		Layout();
	}

	void SaveRule() {
		if (syncing || active_animation < 0 || active_rule < 0) return;
		auto& list = settings.animations[static_cast<size_t>(active_animation)].rules;
		if (active_rule >= static_cast<int>(list.size())) return;
		auto& rule = list[static_cast<size_t>(active_rule)];
		rule.tag = static_cast<Tag>(std::max(0, tag->GetSelection()));
		rule.mode = static_cast<ValueMode>(std::max(0, mode->GetSelection()));
		rule.first = first->GetValue();
		rule.second = second->GetValue();
	}

	void SaveAnimation() {
		if (syncing || active_animation < 0 ||
			active_animation >= static_cast<int>(settings.animations.size())) return;
		SaveRule();
		auto& animation = settings.animations[static_cast<size_t>(active_animation)];
		animation.enabled = animation_enabled->GetValue();
		animation.duration_frames = duration->GetValue();
		animation.repeats = infinite->GetValue() ? 0 : repeats->GetValue();
	}

	void LoadRule(int selected) {
		SaveRule();
		active_rule = selected;
		syncing = true;
		bool available = active_animation >= 0 && active_rule >= 0 &&
			active_rule < static_cast<int>(settings.animations[
				static_cast<size_t>(active_animation)].rules.size());
		if (available) {
			auto const& rule = settings.animations[static_cast<size_t>(active_animation)]
				.rules[static_cast<size_t>(active_rule)];
			tag->SetSelection(static_cast<int>(rule.tag));
			mode->SetSelection(static_cast<int>(rule.mode));
			first->SetValue(rule.first); second->SetValue(rule.second);
		}
		for (wxWindow *control : std::array<wxWindow *, 4>{tag, mode, first, second})
			control->Enable(available);
		syncing = false;
		UpdateRuleLabels();
	}

	void LoadAnimation(int selected) {
		SaveAnimation();
		active_animation = selected;
		active_rule = -1;
		syncing = true;
		bool available = selected >= 0 && selected < static_cast<int>(settings.animations.size());
		if (available) {
			auto const& animation = settings.animations[static_cast<size_t>(selected)];
			animation_enabled->SetValue(animation.enabled);
			duration->SetValue(animation.duration_frames);
			infinite->SetValue(animation.repeats == 0);
			repeats->SetValue(std::max(1, animation.repeats));
		}
		for (wxWindow *control : std::array<wxWindow *, 4>{animation_enabled, duration,
			infinite, repeats}) control->Enable(available);
		repeats->Enable(available && !infinite->GetValue());
		syncing = false;
		RefreshRules();
		if (available && !settings.animations[static_cast<size_t>(selected)].rules.empty()) {
			active_rule = 0; rules->SetSelection(0); LoadRule(0);
		}
		else LoadRule(-1);
	}

	void Changed() {
		if (syncing) return;
		SaveAnimation();
		settings.split = static_cast<SplitMode>(std::max(0, split->GetSelection()));
		settings.unit_delay_frames = delay->GetValue();
		RefreshAnimations(); RefreshRules(); UpdateRuleLabels();
		UpdatePreview();
	}

	void AddAnimation() {
		SaveAnimation();
		settings.animations.emplace_back();
		active_animation = static_cast<int>(settings.animations.size()) - 1;
		RefreshAnimations(); LoadAnimation(active_animation); UpdatePreview();
	}

	void RemoveAnimation() {
		if (active_animation < 0) return;
		settings.animations.erase(settings.animations.begin() + active_animation);
		active_animation = std::min(active_animation,
			static_cast<int>(settings.animations.size()) - 1);
		RefreshAnimations(); LoadAnimation(active_animation); UpdatePreview();
	}

	void AddRule() {
		if (active_animation < 0) return;
		SaveAnimation();
		auto& list = settings.animations[static_cast<size_t>(active_animation)].rules;
		list.emplace_back(); active_rule = static_cast<int>(list.size()) - 1;
		RefreshRules(); LoadRule(active_rule); UpdatePreview();
	}

	void RemoveRule() {
		if (active_animation < 0 || active_rule < 0) return;
		auto& list = settings.animations[static_cast<size_t>(active_animation)].rules;
		list.erase(list.begin() + active_rule);
		active_rule = std::min(active_rule, static_cast<int>(list.size()) - 1);
		RefreshRules(); LoadRule(active_rule); UpdatePreview();
	}

	void Accept() {
		SaveAnimation();
		settings.split = static_cast<SplitMode>(std::max(0, split->GetSelection()));
		settings.unit_delay_frames = delay->GetValue();
		preview.Clear();
		if (!typesetting::animated_text::Apply(context, settings)) {
			wxMessageBox(_("Animated Text works only on text lines; drawing lines were skipped."),
				_("Animated Text"), wxOK | wxICON_WARNING, this);
			return;
		}
		EndModal(wxID_OK);
	}

public:
	explicit DialogAnimatedText(agi::Context *c)
	: wxDialog(c->parent, wxID_ANY, _("Animated Text"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(c)
	, settings(typesetting::animated_text::LoadSettingsForSelection(c))
	, preview(c)
	, playback_timer(this) {
		auto root = new wxBoxSizer(wxVERTICAL);
		auto general = new wxStaticBoxSizer(wxHORIZONTAL, this, _("Text segmentation"));
		general->Add(new wxStaticText(this, wxID_ANY, _("Split into:")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
		split = new wxChoice(this, wxID_ANY);
		for (auto const& name : typesetting::animated_text::SplitModeNames()) split->Append(to_wx(name));
		split->SetSelection(static_cast<int>(settings.split));
		general->Add(split, 1, wxRIGHT, FromDIP(16));
		general->Add(new wxStaticText(this, wxID_ANY, _("Delay per element (frames):")), 0,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
		delay = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
			FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS, 0, 10000, settings.unit_delay_frames);
		general->Add(delay);
		root->Add(general, 0, wxEXPAND | wxALL, FromDIP(8));

		auto body = new wxBoxSizer(wxHORIZONTAL);
		auto animation_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Animations"));
		animations = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(230, 220)));
		animation_box->Add(animations, 1, wxEXPAND | wxBOTTOM, FromDIP(6));
		auto animation_buttons = new wxBoxSizer(wxHORIZONTAL);
		auto add_animation = new wxButton(this, wxID_ANY, _("Add"));
		auto remove_animation = new wxButton(this, wxID_ANY, _("Remove"));
		animation_buttons->Add(add_animation, 1, wxRIGHT, FromDIP(4));
		animation_buttons->Add(remove_animation, 1);
		animation_box->Add(animation_buttons, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
		animation_enabled = new wxCheckBox(this, wxID_ANY, _("Enabled"));
		animation_box->Add(animation_enabled, 0, wxBOTTOM, FromDIP(6));
		auto duration_row = new wxBoxSizer(wxHORIZONTAL);
		duration_row->Add(new wxStaticText(this, wxID_ANY, _("Duration (frames):")), 1,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
		duration = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
			FromDIP(wxSize(80, -1)), wxSP_ARROW_KEYS, 1, 100000, 6);
		duration_row->Add(duration);
		animation_box->Add(duration_row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
		auto repeat_row = new wxBoxSizer(wxHORIZONTAL);
		repeat_row->Add(new wxStaticText(this, wxID_ANY, _("Repeat count:")), 1,
			wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
		repeats = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
			FromDIP(wxSize(80, -1)), wxSP_ARROW_KEYS, 1, 10000, 1);
		repeat_row->Add(repeats);
		animation_box->Add(repeat_row, 0, wxEXPAND | wxBOTTOM, FromDIP(5));
		infinite = new wxCheckBox(this, wxID_ANY, _("Repeat until line end"));
		animation_box->Add(infinite);
		body->Add(animation_box, 0, wxEXPAND | wxRIGHT, FromDIP(8));

		auto rule_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Tag changes"));
		rules = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(320, 180)));
		rule_box->Add(rules, 1, wxEXPAND | wxBOTTOM, FromDIP(6));
		auto rule_buttons = new wxBoxSizer(wxHORIZONTAL);
		auto add_rule = new wxButton(this, wxID_ANY, _("Add tag"));
		auto remove_rule = new wxButton(this, wxID_ANY, _("Remove tag"));
		rule_buttons->Add(add_rule, 1, wxRIGHT, FromDIP(4)); rule_buttons->Add(remove_rule, 1);
		rule_box->Add(rule_buttons, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
		auto grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(8));
		grid->AddGrowableCol(1, 1);
		grid->Add(new wxStaticText(this, wxID_ANY, _("Tag:")), 0, wxALIGN_CENTER_VERTICAL);
		tag = new wxChoice(this, wxID_ANY);
		for (auto const& name : typesetting::animated_text::TagNames()) tag->Append(to_wx(name));
		grid->Add(tag, 1, wxEXPAND);
		grid->Add(new wxStaticText(this, wxID_ANY, _("Change:")), 0, wxALIGN_CENTER_VERTICAL);
		mode = new wxChoice(this, wxID_ANY);
		for (auto const& name : typesetting::animated_text::ValueModeNames()) mode->Append(to_wx(name));
		grid->Add(mode, 1, wxEXPAND);
		first_label = new wxStaticText(this, wxID_ANY, _("Value:"));
		grid->Add(first_label, 0, wxALIGN_CENTER_VERTICAL);
		first = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
			wxDefaultSize, wxSP_ARROW_KEYS, -1000000, 1000000, 10, .1);
		first->SetDigits(2); grid->Add(first, 1, wxEXPAND);
		second_label = new wxStaticText(this, wxID_ANY, _("Maximum:"));
		grid->Add(second_label, 0, wxALIGN_CENTER_VERTICAL);
		second = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
			wxDefaultSize, wxSP_ARROW_KEYS, -1000000, 1000000, 20, .1);
		second->SetDigits(2); grid->Add(second, 1, wxEXPAND);
		rule_box->Add(grid, 0, wxEXPAND);
		body->Add(rule_box, 1, wxEXPAND);
		root->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

		auto bottom = new wxBoxSizer(wxHORIZONTAL);
		bottom->Add(new wxStaticText(this, wxID_ANY, _("Playback")), 0,
			wxALIGN_CENTER_VERTICAL);
		auto add_transport = [&](char const *command_name, wxString const& tooltip,
				std::function<void()> action) {
			auto button = new wxBitmapButton(this, wxID_ANY, cmd::get(command_name)->Icon(16),
				wxDefaultPosition, FromDIP(wxSize(30, 28)));
			button->SetToolTip(tooltip);
			button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) {
				action();
			});
			bottom->Add(button, 0, wxLEFT, FromDIP(3));
		};
		add_transport("video/play/line", _("Play line once"), [this] { Play(false); });
		add_transport("video/play", _("Play line repeatedly"), [this] { Play(true); });
		add_transport("video/stop", _("Stop playback"), [this] { StopPlayback(); });
		auto add_step = [&](wxString const& label, wxString const& tooltip,
				std::function<void()> action) {
			auto button = new wxButton(this, wxID_ANY, label, wxDefaultPosition,
				FromDIP(wxSize(30, 28)), wxBU_EXACTFIT);
			button->SetToolTip(tooltip);
			button->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) {
				action();
			});
			bottom->Add(button, 0, wxLEFT, FromDIP(3));
		};
		add_step("|<", _("Jump to line start"), [this] {
			StopPlayback();
			cmd::call("video/jump/start", context);
		});
		add_step("<", _("Previous frame"), [this] {
			StopPlayback();
			context->videoController->PrevFrame();
		});
		add_step(">", _("Next frame"), [this] {
			StopPlayback();
			context->videoController->NextFrame();
		});
		add_step(">|", _("Jump to line end"), [this] {
			StopPlayback();
			cmd::call("video/jump/end", context);
		});
		bottom->AddStretchSpacer();
		auto ok = new wxButton(this, wxID_OK, _("Apply"));
		auto cancel = new wxButton(this, wxID_CANCEL, _("Cancel"));
		bottom->Add(ok, 0, wxRIGHT, FromDIP(5)); bottom->Add(cancel);
		root->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
		SetSizerAndFit(root);
		SetMinSize(FromDIP(wxSize(720, 540)));

		RefreshAnimations();
		if (!settings.animations.empty()) { active_animation = 0; LoadAnimation(0); }
		else LoadAnimation(-1);
		UpdatePreview();

		split->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Changed(); });
		delay->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { Changed(); });
		animations->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& e) { LoadAnimation(e.GetSelection()); });
		rules->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& e) { LoadRule(e.GetSelection()); });
		add_animation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddAnimation(); });
		remove_animation->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveAnimation(); });
		add_rule->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddRule(); });
		remove_rule->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveRule(); });
		for (wxCheckBox *control : {animation_enabled, infinite})
			control->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
				repeats->Enable(!infinite->GetValue()); Changed();
			});
		for (wxSpinCtrl *control : {duration, repeats})
			control->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { Changed(); });
		tag->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Changed(); });
		mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Changed(); });
		first->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { Changed(); });
		second->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { Changed(); });
		ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Accept(); });
		cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
		Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (!loop_playback) return;
			if (!context->videoController->IsPlaying()) context->videoController->PlayLine();
		});
	}

	~DialogAnimatedText() override {
		StopPlayback();
		preview.Clear();
	}
};

} // namespace

void ShowAnimatedTextDialog(agi::Context *c) {
	bool usable = false;
	for (auto line : c->selectionController->GetSelectedSet())
		usable |= typesetting::animated_text::HasUsableText(*line) ||
			typesetting::animated_text::IsEffect(*c->ass, line);
	if (!usable) {
		wxMessageBox(_("Animated Text works only on text lines; drawing lines were skipped."),
			_("Animated Text"), wxOK | wxICON_WARNING, c->parent);
		return;
	}
	DialogAnimatedText(c).ShowModal();
}
