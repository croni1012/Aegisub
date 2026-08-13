// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "floating_tag_windows.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "auto4_base.h"
#include "colour_button.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "value_event.h"

#include <libaegisub/signal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinbutt.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/tglbtn.h>
#include <wx/utils.h>

namespace {

constexpr int palette_width = 520;
constexpr int minimum_palette_width = 190;
constexpr double epsilon = 0.0001;

std::string FormatNumber(double value) {
	value = std::round(value * 1000.0) / 1000.0;
	std::string text = agi::format("%.3f", value);
	while (text.find('.') != std::string::npos && text.back() == '0') text.pop_back();
	if (!text.empty() && text.back() == '.') text.pop_back();
	if (text == "-0") text = "0";
	return text;
}

struct SliderConfig {
	double minimum = 0.0;
	double maximum = 100.0;
	double step = 1.0;
	double reset = 0.0;
	std::vector<double> presets;
};

bool IsAlphaTag(std::string const& tag) {
	return tag == "\\alpha" || tag == "\\1a" || tag == "\\3a" || tag == "\\4a";
}

SliderConfig ConfigFor(std::string const& tag) {
	if (tag == "\\blur")
		return {0, 50, .1, 0, {0, .4, .5, .6, .7, .8, .9, 1, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2, 3, 5, 10}};
	if (IsAlphaTag(tag))
		return {0, 255, 1, 0, {0, 32, 64, 96, 128, 160, 192, 224, 255}};
	if (tag == "\\bord" || tag == "\\xbord" || tag == "\\ybord")
		return {0, 100, .1, 0, {0, .5, 1, 1.5, 2, 3, 4, 5, 8, 10, 15, 20}};
	if (tag == "\\shad")
		return {0, 100, .1, 0, {0, .5, 1, 1.5, 2, 3, 5, 10, 20}};
	if (tag == "\\xshad" || tag == "\\yshad")
		return {-100, 100, .1, 0, {-20, -10, -5, -3, -2, -1, 0, 1, 2, 3, 5, 10, 20}};
	if (tag == "\\fs")
		return {1, 500, 1, 48, {8, 12, 16, 20, 24, 28, 32, 36, 40, 45, 48, 50, 60, 72, 80, 100, 120, 150, 200, 250}};
	if (tag == "\\fscx" || tag == "\\fscy")
		return {0, 1000, 1, 100, {0, 10, 25, 50, 75, 90, 100, 105, 110, 115, 125, 150, 200, 250, 500}};
	if (tag == "\\fsp")
		return {-100, 100, .1, 0, {-20, -10, -5, -2, -1, 0, 1, 2, 3, 5, 10, 20, 25}};
	if (tag == "\\frz" || tag == "\\frx" || tag == "\\fry")
		return {-360, 360, 1, 0, {-360, -270, -180, -90, -45, -25, -20, -15, -10, -5, 0, 5, 10, 15, 20, 25, 45, 90, 180, 270, 360}};
	if (tag == "\\fax" || tag == "\\fay")
		return {-1, 1, .01, 0, {-1, -.75, -.5, -.25, -.1, -.07, -.05, -.02, 0, .02, .05, .07, .1, .25, .5, .75, 1}};
	return {};
}

SliderConfig PercentageConfig(std::string const& tag) {
	if (IsAlphaTag(tag))
		return {0, 100, 1, 100, {0, 25, 50, 75, 100}};
	if (tag == "\\blur" || tag == "\\fs" || tag == "\\fscx" || tag == "\\fscy" ||
		tag == "\\bord" || tag == "\\xbord" || tag == "\\ybord")
		return {0, 500, 1, 100, {0, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400, 500}};
	return {-1000, 1000, 1, 100, {-200, -150, -100, -50, 0, 25, 50, 75, 100, 125, 150, 200, 300, 400, 500}};
}

double ClampFor(std::string const& tag, double value) {
	auto config = ConfigFor(tag);
	return std::clamp(value, config.minimum, config.maximum);
}

class ValueSlider final : public wxPanel {
	std::string tag;
	bool proportional;
	SliderConfig config;
	wxTextCtrl *text = nullptr;
	wxSpinButton *spin = nullptr;
	wxSlider *slider = nullptr;
	double value = 0.0;
	double reset_value = 0.0;
	bool updating = false;
	std::function<void(double)> callback;
	std::function<void()> reset_callback;

	static constexpr int resolution = 100000;

	double Step(bool control, bool shift) const {
		if (control && shift) return config.step / 1000.0;
		if (control) return config.step / 100.0;
		if (shift) return config.step / 10.0;
		return config.step;
	}

	void UpdateSlider() {
		double position = (value - config.minimum) / (config.maximum - config.minimum);
		slider->SetValue(std::clamp(static_cast<int>(std::lround(position * resolution)), 0, resolution));
	}

	void SetUserValue(double new_value) {
		value = std::clamp(std::round(new_value * 1000.0) / 1000.0, config.minimum, config.maximum);
		updating = true;
		text->ChangeValue(to_wx(FormatNumber(value)));
		text->SetInsertionPointEnd();
		UpdateSlider();
		updating = false;
		callback(value);
	}

	void CommitText() {
		double entered = 0.0;
		if (text->GetValue().ToDouble(&entered)) SetUserValue(entered);
		else ShowValue(value, true, false);
	}

	void OnWheel(wxMouseEvent& event) {
		double direction = event.GetWheelRotation() > 0 ? 1.0 : -1.0;
		SetUserValue(value + direction * Step(event.ControlDown(), event.ShiftDown()));
	}

	void ShowPresets() {
		if (config.presets.empty()) return;
		wxMenu menu;
		for (size_t index = 0; index < config.presets.size(); ++index) {
			int id = wxID_HIGHEST + 1700 + static_cast<int>(index);
			menu.AppendCheckItem(id, to_wx(FormatNumber(config.presets[index])));
			menu.Check(id, std::abs(config.presets[index] - value) < epsilon);
			menu.Bind(wxEVT_MENU, [this, index](wxCommandEvent&) {
				SetUserValue(config.presets[index]);
			}, id);
		}
		PopupMenu(&menu);
	}

public:
	ValueSlider(wxWindow *parent, std::string tag, bool proportional, std::function<void(double)> callback,
		std::function<void()> reset_callback = {})
	: wxPanel(parent)
	, tag(std::move(tag))
	, proportional(proportional)
	, config(proportional ? PercentageConfig(this->tag) : ConfigFor(this->tag))
	, value(config.reset)
	, reset_value(config.reset)
	, callback(std::move(callback))
	, reset_callback(std::move(reset_callback))
	{
		auto main = new wxBoxSizer(wxVERTICAL);
		auto row = new wxBoxSizer(wxHORIZONTAL);
		auto label = new wxStaticText(this, wxID_ANY, to_wx(this->tag.substr(1)) + ":", wxDefaultPosition, FromDIP(wxSize(38, -1)));
		text = new wxTextCtrl(this, wxID_ANY, to_wx(FormatNumber(value)), wxDefaultPosition,
			FromDIP(wxSize(65, 22)), wxTE_PROCESS_ENTER);
		spin = new wxSpinButton(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(18, 22)));
		spin->SetRange(-100000, 100000);
		row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
		row->Add(text, 0, wxRIGHT, FromDIP(2));
		row->Add(spin, 0);
		if (proportional)
			row->Add(new wxStaticText(this, wxID_ANY, "%"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(2));
		main->Add(row, 0, wxBOTTOM, FromDIP(2));
		slider = new wxSlider(this, wxID_ANY, 0, 0, resolution);
		main->Add(slider, 0, wxEXPAND);
		SetSizer(main);

		slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			double ratio = slider->GetValue() / static_cast<double>(resolution);
			double raw = config.minimum + ratio * (config.maximum - config.minimum);
			SetUserValue(std::round(raw / config.step) * config.step);
		});
		for (auto control : std::array<wxWindow *, 4>{label, text, spin, slider}) {
			control->Bind(wxEVT_MOUSEWHEEL, &ValueSlider::OnWheel, this);
			control->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent&) {
				if (!this->reset_callback) {
					SetUserValue(reset_value);
					return;
				}
				value = reset_value;
				updating = true;
				text->ChangeValue(to_wx(FormatNumber(value)));
				UpdateSlider();
				updating = false;
				this->reset_callback();
			});
		}
		text->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { CommitText(); });
		text->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
			if (!updating && text->IsModified()) CommitText();
			event.Skip();
		});
		text->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) { ShowPresets(); });
		spin->Bind(wxEVT_SPIN_UP, [this](wxSpinEvent&) {
			SetUserValue(value + Step(wxGetKeyState(WXK_CONTROL), wxGetKeyState(WXK_SHIFT)));
		});
		spin->Bind(wxEVT_SPIN_DOWN, [this](wxSpinEvent&) {
			SetUserValue(value - Step(wxGetKeyState(WXK_CONTROL), wxGetKeyState(WXK_SHIFT)));
		});

		wxString precise = "- " + _("Right click to reset") + "\n- " +
			_("Hold CTRL, SHIFT or both to be more precise while using mouse wheel");
		slider->SetToolTip(precise);
		spin->SetToolTip(precise);
		text->SetToolTip(precise + "\n- " + _("Double click to see options"));
	}

	void ShowValue(double new_value, bool uniform, bool update_reset) {
		value = std::clamp(new_value, config.minimum, config.maximum);
		if (update_reset) reset_value = proportional && !IsAlphaTag(tag) ? 100.0 : value;
		updating = true;
		text->ChangeValue(uniform ? to_wx(FormatNumber(value)) : wxString::FromUTF8("\xE2\x80\x94"));
		UpdateSlider();
		updating = false;
	}
};

struct LineState {
	double blur = 0;
	double font_size = 48;
	double scale_x = 100;
	double scale_y = 100;
	double spacing = 0;
	double border_x = 2;
	double border_y = 2;
	double shadow_x = 2;
	double shadow_y = 2;
	double rotation_z = 0;
	double rotation_x = 0;
	double rotation_y = 0;
	double shear_x = 0;
	double shear_y = 0;
	int alignment = 2;
	agi::Color primary{255, 255, 255};
	agi::Color outline{0, 0, 0};
	agi::Color shadow{0, 0, 0};
	std::string font = "Arial";
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	int encoding = 1;
	std::array<int, 3> margins{{10, 10, 10}};
	bool has_position = false;
	double position_x = 0;
	double position_y = 0;
	bool has_move = false;
	double move_x1 = 0;
	double move_y1 = 0;
	double move_x2 = 0;
	double move_y2 = 0;
	bool move_has_times = false;
	int move_t1 = 0;
	int move_t2 = 0;
};

void ApplyStyle(LineState& state, AssStyle const& style, bool include_alignment) {
	state.font_size = style.fontsize;
	state.scale_x = style.scalex;
	state.scale_y = style.scaley;
	state.spacing = style.spacing;
	state.border_x = state.border_y = style.outline_w;
	state.shadow_x = state.shadow_y = style.shadow_w;
	state.rotation_z = style.angle;
	state.rotation_x = state.rotation_y = 0;
	state.shear_x = state.shear_y = 0;
	state.blur = 0;
	state.primary = style.primary;
	state.outline = style.outline;
	state.shadow = style.shadow;
	state.font = style.font;
	state.bold = style.bold;
	state.italic = style.italic;
	state.underline = style.underline;
	state.strikeout = style.strikeout;
	state.encoding = style.encoding;
	if (include_alignment) state.alignment = style.alignment;
}

LineState ReadLine(agi::Context *context, AssDialogue const *line) {
	LineState state;
	AssStyle fallback;
	AssStyle const *base = line ? context->ass->GetStyle(line->Style.get()) : nullptr;
	if (!base) base = &fallback;
	ApplyStyle(state, *base, true);
	state.margins = base->Margin;
	if (!line) return state;
	for (size_t i = 0; i < state.margins.size(); ++i)
		if (line->Margin[i]) state.margins[i] = line->Margin[i];

	auto number = [](AssOverrideTag const& tag, double current) {
		return tag.Params.empty() ? current : tag.Params.front().Get<double>(current);
	};
	auto set_colour = [](agi::Color& target, AssOverrideTag const& tag) {
		if (tag.Params.empty() || tag.Params.front().omitted) return;
		auto colour = tag.Params.front().Get<agi::Color>(target);
		target.r = colour.r; target.g = colour.g; target.b = colour.b;
	};
	auto set_alpha = [](agi::Color& target, AssOverrideTag const& tag) {
		if (!tag.Params.empty() && !tag.Params.front().omitted)
			target.a = static_cast<unsigned char>(std::clamp(tag.Params.front().Get<int>(target.a), 0, 255));
	};

	for (auto& block : line->ParseTags()) {
		if ((block->GetType() == AssBlockType::PLAIN || block->GetType() == AssBlockType::DRAWING) &&
			!block->GetText().empty()) break;
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags) {
			if (tag.Name == "\\r") {
				std::string name = tag.Params.empty() ? std::string() : tag.Params.front().Get<std::string>(std::string());
				auto reset = context->ass->GetStyle(name.empty() ? line->Style.get() : name);
				ApplyStyle(state, reset ? *reset : fallback, false);
			}
			else if (tag.Name == "\\blur") state.blur = number(tag, state.blur);
			else if (tag.Name == "\\fs") state.font_size = number(tag, state.font_size);
			else if (tag.Name == "\\fscx") state.scale_x = number(tag, state.scale_x);
			else if (tag.Name == "\\fscy") state.scale_y = number(tag, state.scale_y);
			else if (tag.Name == "\\fsp") state.spacing = number(tag, state.spacing);
			else if (tag.Name == "\\bord") state.border_x = state.border_y = number(tag, state.border_x);
			else if (tag.Name == "\\xbord") state.border_x = number(tag, state.border_x);
			else if (tag.Name == "\\ybord") state.border_y = number(tag, state.border_y);
			else if (tag.Name == "\\shad") state.shadow_x = state.shadow_y = number(tag, state.shadow_x);
			else if (tag.Name == "\\xshad") state.shadow_x = number(tag, state.shadow_x);
			else if (tag.Name == "\\yshad") state.shadow_y = number(tag, state.shadow_y);
			else if (tag.Name == "\\fr" || tag.Name == "\\frz") state.rotation_z = number(tag, state.rotation_z);
			else if (tag.Name == "\\frx") state.rotation_x = number(tag, state.rotation_x);
			else if (tag.Name == "\\fry") state.rotation_y = number(tag, state.rotation_y);
			else if (tag.Name == "\\fax") state.shear_x = number(tag, state.shear_x);
			else if (tag.Name == "\\fay") state.shear_y = number(tag, state.shear_y);
			else if (tag.Name == "\\fn" && !tag.Params.empty()) state.font = tag.Params.front().Get<std::string>(state.font);
			else if (tag.Name == "\\b") state.bold = number(tag, state.bold ? 1 : 0) != 0;
			else if (tag.Name == "\\i") state.italic = number(tag, state.italic ? 1 : 0) != 0;
			else if (tag.Name == "\\u") state.underline = number(tag, state.underline ? 1 : 0) != 0;
			else if (tag.Name == "\\s") state.strikeout = number(tag, state.strikeout ? 1 : 0) != 0;
			else if (tag.Name == "\\c" || tag.Name == "\\1c") set_colour(state.primary, tag);
			else if (tag.Name == "\\3c") set_colour(state.outline, tag);
			else if (tag.Name == "\\4c") set_colour(state.shadow, tag);
			else if (tag.Name == "\\alpha") {
				set_alpha(state.primary, tag); set_alpha(state.outline, tag); set_alpha(state.shadow, tag);
			}
			else if (tag.Name == "\\1a") set_alpha(state.primary, tag);
			else if (tag.Name == "\\3a") set_alpha(state.outline, tag);
			else if (tag.Name == "\\4a") set_alpha(state.shadow, tag);
			else if (tag.Name == "\\an") state.alignment = std::clamp(static_cast<int>(number(tag, state.alignment)), 1, 9);
			else if (tag.Name == "\\a") state.alignment = AssStyle::SsaToAss(static_cast<int>(number(tag, 2)));
			else if (tag.Name == "\\pos" && tag.Params.size() >= 2) {
				state.has_position = true;
				state.position_x = tag.Params[0].Get<double>(state.position_x);
				state.position_y = tag.Params[1].Get<double>(state.position_y);
			}
			else if (tag.Name == "\\move" && tag.Params.size() >= 4) {
				state.has_move = true;
				state.move_x1 = tag.Params[0].Get<double>(); state.move_y1 = tag.Params[1].Get<double>();
				state.move_x2 = tag.Params[2].Get<double>(); state.move_y2 = tag.Params[3].Get<double>();
				state.move_has_times = tag.Params.size() >= 6 && !tag.Params[4].omitted && !tag.Params[5].omitted;
				if (state.move_has_times) {
					state.move_t1 = tag.Params[4].Get<int>(); state.move_t2 = tag.Params[5].Get<int>();
				}
			}
		}
	}
	return state;
}

double NumericValue(LineState const& state, std::string const& tag) {
	if (tag == "\\blur") return state.blur;
	if (tag == "\\fs") return state.font_size;
	if (tag == "\\fscx") return state.scale_x;
	if (tag == "\\fscy") return state.scale_y;
	if (tag == "\\fsp") return state.spacing;
	if (tag == "\\bord") return (state.border_x + state.border_y) / 2;
	if (tag == "\\xbord") return state.border_x;
	if (tag == "\\ybord") return state.border_y;
	if (tag == "\\shad") return (state.shadow_x + state.shadow_y) / 2;
	if (tag == "\\xshad") return state.shadow_x;
	if (tag == "\\yshad") return state.shadow_y;
	if (tag == "\\frz") return state.rotation_z;
	if (tag == "\\frx") return state.rotation_x;
	if (tag == "\\fry") return state.rotation_y;
	if (tag == "\\fax") return state.shear_x;
	if (tag == "\\fay") return state.shear_y;
	if (tag == "\\alpha") return (state.primary.a + state.outline.a + state.shadow.a) / 3.0;
	if (tag == "\\1a") return state.primary.a;
	if (tag == "\\3a") return state.outline.a;
	if (tag == "\\4a") return state.shadow.a;
	return 0;
}

bool NumericDefined(LineState const& state, std::string const& tag) {
	if (tag == "\\bord") return std::abs(state.border_x - state.border_y) < epsilon;
	if (tag == "\\shad") return std::abs(state.shadow_x - state.shadow_y) < epsilon;
	if (tag == "\\alpha")
		return state.primary.a == state.outline.a && state.primary.a == state.shadow.a;
	return true;
}

agi::Color ColourValue(LineState const& state, std::string const& tag) {
	if (tag == "\\3c") return state.outline;
	if (tag == "\\4c") return state.shadow;
	return state.primary;
}

void SetOverrideAtStart(AssDialogue *line, std::string const& tag, std::string const& value) {
	if (!line) return;
	auto aliases = [&](std::string const& existing) {
		if (existing == tag) return true;
		if ((tag == "\\c" || tag == "\\1c") && (existing == "\\c" || existing == "\\1c")) return true;
		if (tag == "\\frz" && existing == "\\fr") return true;
		if (tag == "\\pos" && existing == "\\move") return true;
		if (tag == "\\move" && existing == "\\pos") return true;
		if (tag == "\\clip" && existing == "\\iclip") return true;
		if (tag == "\\iclip" && existing == "\\clip") return true;
		if (tag == "\\bord" && (existing == "\\xbord" || existing == "\\ybord")) return true;
		if (tag == "\\shad" && (existing == "\\xshad" || existing == "\\yshad")) return true;
		if (tag == "\\alpha" && (existing == "\\1a" || existing == "\\3a" || existing == "\\4a")) return true;
		return false;
	};

	auto blocks = line->ParseTags();
	AssDialogueBlockOverride *target = nullptr;
	for (auto& block : blocks) {
		if ((block->GetType() == AssBlockType::PLAIN || block->GetType() == AssBlockType::DRAWING) &&
			!block->GetText().empty()) break;
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		target = static_cast<AssDialogueBlockOverride *>(block.get());
		target->Tags.erase(std::remove_if(target->Tags.begin(), target->Tags.end(),
			[&](AssOverrideTag const& existing) { return aliases(existing.Name); }), target->Tags.end());
	}
	if (target) {
		target->AddTag(tag + value);
		line->UpdateText(blocks);
	}
	else {
		line->Text = "{" + tag + value + "}" + line->Text.get();
	}
}

void RemoveOverrides(AssDialogue *line, std::vector<std::string> const& tags) {
	auto blocks = line->ParseTags();
	for (auto it = blocks.begin(); it != blocks.end();) {
		if ((*it)->GetType() != AssBlockType::OVERRIDE) {
			++it;
			continue;
		}
		auto block = static_cast<AssDialogueBlockOverride *>(it->get());
		block->Tags.erase(std::remove_if(block->Tags.begin(), block->Tags.end(), [&](AssOverrideTag const& tag) {
			return std::find(tags.begin(), tags.end(), tag.Name) != tags.end();
		}), block->Tags.end());
		if (block->Tags.empty()) it = blocks.erase(it);
		else ++it;
	}
	if (blocks.empty()) line->Text = "";
	else line->UpdateText(blocks);
}

struct Hsl {
	double h = 0;
	double s = 0;
	double l = 0;
};

Hsl ToHsl(agi::Color colour) {
	double r = colour.r / 255.0, g = colour.g / 255.0, b = colour.b / 255.0;
	double maximum = std::max({r, g, b}), minimum = std::min({r, g, b});
	double delta = maximum - minimum;
	Hsl hsl;
	hsl.l = (maximum + minimum) / 2;
	if (delta == 0) return hsl;
	hsl.s = delta / (1 - std::abs(2 * hsl.l - 1));
	if (maximum == r) hsl.h = std::fmod((g - b) / delta, 6.0);
	else if (maximum == g) hsl.h = (b - r) / delta + 2;
	else hsl.h = (r - g) / delta + 4;
	hsl.h *= 60;
	if (hsl.h < 0) hsl.h += 360;
	return hsl;
}

agi::Color FromHsl(Hsl hsl, unsigned char alpha) {
	double chroma = (1 - std::abs(2 * hsl.l - 1)) * hsl.s;
	double x = chroma * (1 - std::abs(std::fmod(hsl.h / 60.0, 2.0) - 1));
	double r = 0, g = 0, b = 0;
	if (hsl.h < 60) r = chroma, g = x;
	else if (hsl.h < 120) r = x, g = chroma;
	else if (hsl.h < 180) g = chroma, b = x;
	else if (hsl.h < 240) g = x, b = chroma;
	else if (hsl.h < 300) r = x, b = chroma;
	else r = chroma, b = x;
	double match = hsl.l - chroma / 2;
	return agi::Color(
		static_cast<unsigned char>(std::lround((r + match) * 255)),
		static_cast<unsigned char>(std::lround((g + match) * 255)),
		static_cast<unsigned char>(std::lround((b + match) * 255)), alpha);
}

agi::Color AdjustLightness(agi::Color colour, double percentage_points) {
	auto hsl = ToHsl(colour);
	hsl.l = std::clamp(hsl.l + percentage_points / 100.0, 0.0, 1.0);
	return FromHsl(hsl, colour.a);
}

class ColourEditor final : public wxPanel {
	ColourButton *button = nullptr;
	wxStaticText *mixed = nullptr;
	wxSlider *adjustment = nullptr;
	agi::Color reset_colour;
	std::function<void(agi::Color)> colour_callback;
	std::function<void(double)> adjustment_callback;
	std::function<void()> reset_callback;

public:
	ColourEditor(wxWindow *parent, wxString const& label,
		std::function<void(agi::Color)> colour_callback,
		std::function<void(double)> adjustment_callback,
		std::function<void()> reset_callback)
	: wxPanel(parent)
	, colour_callback(std::move(colour_callback))
	, adjustment_callback(std::move(adjustment_callback))
	, reset_callback(std::move(reset_callback))
	{
		auto main = new wxBoxSizer(wxVERTICAL);
		auto row = new wxBoxSizer(wxHORIZONTAL);
		row->Add(new wxStaticText(this, wxID_ANY, label + ":", wxDefaultPosition, FromDIP(wxSize(65, -1))),
			0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
		button = new ColourButton(this, FromDIP(wxSize(58, 16)), false);
		mixed = new wxStaticText(this, wxID_ANY, wxString::FromUTF8("\xE2\x80\x94"));
		mixed->SetToolTip(_("Multiple values; the active line is shown"));
		row->Add(mixed, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
		row->Add(button, 0);
		main->Add(row, 0, wxEXPAND);

		auto adjustment_row = new wxBoxSizer(wxHORIZONTAL);
		adjustment_row->Add(new wxStaticText(this, wxID_ANY, _("Darken")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
		adjustment = new wxSlider(this, wxID_ANY, 0, -100, 100);
		adjustment_row->Add(adjustment, 1, wxEXPAND);
		adjustment_row->Add(new wxStaticText(this, wxID_ANY, _("Lighten")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(3));
		main->Add(adjustment_row, 0, wxEXPAND | wxTOP, FromDIP(2));
		SetSizer(main);
		button->Bind(EVT_COLOR, [this](ValueEvent<agi::Color>& event) { this->colour_callback(event.Get()); });
		adjustment->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
			int position = adjustment->GetValue();
			this->adjustment_callback(position);
		});
		auto reset = [this](wxMouseEvent&) {
			adjustment->SetValue(0);
			this->reset_callback();
		};
		adjustment->Bind(wxEVT_RIGHT_UP, reset);
		button->Bind(wxEVT_RIGHT_UP, reset);
		adjustment->SetToolTip(_("SCSS-style HSL adjustment in percentage points"));
	}

	void ShowValue(agi::Color colour, bool uniform, bool update_reset) {
		if (update_reset) {
			reset_colour = colour;
			adjustment->SetValue(0);
		}
		button->SetColor(colour);
		mixed->Show(!uniform);
		Layout();
	}
};

class TagPanel : public wxPanel {
protected:
	agi::Context *context;
	bool applying = false;
	bool *floating_edit_in_progress;
	int commit_id = -1;
	std::string last_action;
	std::map<AssDialogue const *, LineState> baselines;

private:
	agi::signal::Connection selection_connection;
	agi::signal::Connection active_line_connection;
	agi::signal::Connection commit_connection;

	void SelectionChanged() {
		commit_id = -1;
		last_action.clear();
		baselines.clear();
		for (auto line : context->selectionController->GetSelectedSet())
			baselines.emplace(line, ReadLine(context, line));
		RefreshFromSelection(true);
	}
	void ActiveLineChanged(AssDialogue *) { SelectionChanged(); }
	void FileCommitted(int, AssDialogue const *changed_line) {
		if (*floating_edit_in_progress) {
			if (!applying) RefreshFromSelection(false);
			return;
		}

		commit_id = -1;
		last_action.clear();
		if (changed_line && context->selectionController->GetSelectedSet().count(const_cast<AssDialogue *>(changed_line)))
			baselines[changed_line] = ReadLine(context, changed_line);
		else if (!changed_line) {
			baselines.clear();
			for (auto line : context->selectionController->GetSelectedSet())
				baselines.emplace(line, ReadLine(context, line));
		}
		RefreshFromSelection(true);
	}

protected:
	AssDialogue *ReferenceLine() const {
		auto active = context->selectionController->GetActiveLine();
		auto const& selection = context->selectionController->GetSelectedSet();
		if (active && selection.count(active)) return active;
		return selection.empty() ? nullptr : *selection.begin();
	}

	LineState const& BaselineFor(AssDialogue const *line) {
		auto found = baselines.find(line);
		if (found != baselines.end()) return found->second;
		return baselines.emplace(line, ReadLine(context, line)).first->second;
	}

	double SelectionPercentage(std::string const& tag) {
		AssDialogue const *largest = nullptr;
		double largest_baseline = 0.0;
		for (auto line : context->selectionController->GetSelectedSet()) {
			double baseline = NumericValue(BaselineFor(line), tag);
			if (!largest || std::abs(baseline) > std::abs(largest_baseline)) {
				largest = line;
				largest_baseline = baseline;
			}
		}
		if (!largest) return IsAlphaTag(tag) ? 0.0 : 100.0;
		if (IsAlphaTag(tag))
			return NumericValue(ReadLine(context, largest), tag) * 100.0 / 255.0;
		if (std::abs(largest_baseline) <= epsilon) return 100.0;
		return NumericValue(ReadLine(context, largest), tag) * 100.0 / largest_baseline;
	}

	void Commit(wxString const& description, std::string action) {
		if (action != last_action) commit_id = -1;
		last_action = std::move(action);
		auto const& selected = context->selectionController->GetSelectedSet();
		AssDialogue *single = selected.size() == 1 ? *selected.begin() : nullptr;
		applying = true;
		*floating_edit_in_progress = true;
		try {
			commit_id = context->ass->Commit(description, AssFile::COMMIT_DIAG_TEXT, commit_id, single);
		}
		catch (...) {
			*floating_edit_in_progress = false;
			applying = false;
			throw;
		}
		*floating_edit_in_progress = false;
		applying = false;
		RefreshFromSelection(false);
	}

	void ApplyNumeric(std::string const& tag, double requested, bool proportional) {
		if (!ReferenceLine()) return;
		for (auto line : context->selectionController->GetSelectedSet()) {
			double target = proportional && IsAlphaTag(tag)
				? requested * 255.0 / 100.0
				: proportional ? NumericValue(BaselineFor(line), tag) * requested / 100.0 : requested;
			target = ClampFor(tag, target);
			std::string value = IsAlphaTag(tag)
				? agi::format("&H%02X&", static_cast<int>(std::lround(target)))
				: FormatNumber(target);
			SetOverrideAtStart(line, tag, value);
		}
		Commit(_("change floating tag value"), tag + (proportional ? "/proportional" : "/fixed"));
	}

	void ResetNumeric(std::string const& tag) {
		if (!ReferenceLine()) return;
		for (auto line : context->selectionController->GetSelectedSet()) {
			double target = ClampFor(tag, NumericValue(BaselineFor(line), tag));
			std::string value = IsAlphaTag(tag)
				? agi::format("&H%02X&", static_cast<int>(std::lround(target)))
				: FormatNumber(target);
			SetOverrideAtStart(line, tag, value);
		}
		Commit(_("change floating tag value"), tag + "/reset");
	}

	void ApplyColour(std::string const& tag, agi::Color requested, bool proportional) {
		auto reference_line = ReferenceLine();
		if (!reference_line) return;
		auto reference = ColourValue(BaselineFor(reference_line), tag);
		for (auto line : context->selectionController->GetSelectedSet()) {
			auto target = requested;
			if (proportional) {
				auto baseline = ColourValue(BaselineFor(line), tag);
				target.r = static_cast<unsigned char>(std::clamp<int>(baseline.r + requested.r - reference.r, 0, 255));
				target.g = static_cast<unsigned char>(std::clamp<int>(baseline.g + requested.g - reference.g, 0, 255));
				target.b = static_cast<unsigned char>(std::clamp<int>(baseline.b + requested.b - reference.b, 0, 255));
			}
			SetOverrideAtStart(line, tag, target.GetAssOverrideFormatted());
		}
		Commit(_("change floating tag colour"), tag + (proportional ? "/colour-proportional" : "/colour-fixed"));
	}

	void ApplyColourAdjustment(std::string const& tag, double percentage_points) {
		if (!ReferenceLine()) return;
		for (auto line : context->selectionController->GetSelectedSet()) {
			auto target = AdjustLightness(ColourValue(BaselineFor(line), tag), percentage_points);
			SetOverrideAtStart(line, tag, target.GetAssOverrideFormatted());
		}
		Commit(_("change floating tag colour"), tag + "/lightness");
	}

	void ResetColour(std::string const& tag) {
		if (!ReferenceLine()) return;
		for (auto line : context->selectionController->GetSelectedSet())
			SetOverrideAtStart(line, tag, ColourValue(BaselineFor(line), tag).GetAssOverrideFormatted());
		Commit(_("change floating tag colour"), tag + "/reset");
	}

	virtual void RefreshFromSelection(bool update_reset) = 0;

public:
	TagPanel(wxWindow *parent, agi::Context *context, bool *floating_edit_in_progress)
	: wxPanel(parent)
	, context(context)
	, floating_edit_in_progress(floating_edit_in_progress)
	, selection_connection(context->selectionController->AddSelectionListener(&TagPanel::SelectionChanged, this))
	, active_line_connection(context->selectionController->AddActiveLineListener(&TagPanel::ActiveLineChanged, this))
	, commit_connection(context->ass->AddCommitListener(&TagPanel::FileCommitted, this))
	{
	}
};

struct NumericBinding {
	std::string tag;
	bool proportional = false;
	ValueSlider *control = nullptr;
};

class ResponsiveGrid final : public wxPanel {
	std::vector<wxWindow *> controls;
	int columns = 0;
	bool last_at_row_end;
	bool rebuilding = false;

	void Rebuild(int width) {
		if (rebuilding) return;
		int gap = FromDIP(8);
		int wanted = std::clamp((std::max(width, FromDIP(minimum_palette_width)) + gap) /
			(FromDIP(150) + gap), 1, 3);
		if (wanted == columns) return;

		rebuilding = true;
		if (auto old_grid = GetSizer()) {
			old_grid->Clear(false);
			SetSizer(nullptr, true);
		}
		auto grid = new wxFlexGridSizer(wanted, gap, gap);
		for (int column = 0; column < wanted; ++column) grid->AddGrowableCol(column, 1);
		for (size_t index = 0; index < controls.size(); ++index) {
			if (last_at_row_end && index + 1 == controls.size()) {
				int used = static_cast<int>(index % wanted);
				int spacers = (wanted - 1 - used + wanted) % wanted;
				for (int spacer = 0; spacer < spacers; ++spacer) grid->AddSpacer(1);
			}
			grid->Add(controls[index], 1, wxEXPAND);
		}
		columns = wanted;
		SetSizer(grid, true);
		SetMinSize(wxSize(FromDIP(minimum_palette_width - 16), grid->GetMinSize().y));
		InvalidateBestSize();
		Layout();
		if (auto page = dynamic_cast<wxScrolledWindow *>(GetParent())) {
			page->SetMinSize(wxDefaultSize);
			if (auto page_sizer = page->GetSizer())
				page->SetMinSize(wxSize(-1, page_sizer->GetMinSize().y));
			page->InvalidateBestSize();
			page->Layout();
			page->FitInside();
			if (auto notebook = page->GetParent()) {
				notebook->InvalidateBestSize();
				if (auto panel = notebook->GetParent()) panel->InvalidateBestSize();
			}
		}
		rebuilding = false;
	}

public:
	ResponsiveGrid(wxWindow *parent, bool last_at_row_end = false)
	: wxPanel(parent)
	, last_at_row_end(last_at_row_end)
	{
		SetMinSize(FromDIP(wxSize(minimum_palette_width - 16, -1)));
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			Rebuild(event.GetSize().x);
			event.Skip();
		});
	}

	void AddControl(wxWindow *control) { controls.push_back(control); }
	void Finalize() { Rebuild(FromDIP(palette_width - 16)); }
};

class NumericTagPanel final : public TagPanel {
	std::vector<NumericBinding> bindings;

	void RefreshFromSelection(bool update_reset) override {
		auto reference = ReferenceLine();
		Enable(reference != nullptr);
		if (!reference) return;
		auto reference_state = ReadLine(context, reference);
		for (auto const& binding : bindings) {
			double value = binding.proportional
				? SelectionPercentage(binding.tag)
				: NumericValue(reference_state, binding.tag);
			bool uniform = NumericDefined(reference_state, binding.tag);
			for (auto line : context->selectionController->GetSelectedSet()) {
				auto state = ReadLine(context, line);
				uniform = uniform && NumericDefined(state, binding.tag) &&
					(binding.proportional || std::abs(NumericValue(state, binding.tag) - value) < epsilon);
			}
			binding.control->ShowValue(value, uniform, update_reset);
		}
	}

public:
	NumericTagPanel(wxWindow *parent, agi::Context *context, bool *floating_edit_in_progress,
		std::vector<std::string> const& tags)
	: TagPanel(parent, context, floating_edit_in_progress)
	{
		auto main = new wxBoxSizer(wxVERTICAL);
		auto notebook = new wxNotebook(this, wxID_ANY);
		for (int page_index = 0; page_index < 2; ++page_index) {
			bool proportional = page_index == 1;
			auto page = new wxScrolledWindow(notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
			page->SetScrollRate(0, FromDIP(12));
			auto page_sizer = new wxBoxSizer(wxVERTICAL);
			auto grid = new ResponsiveGrid(page);
			for (auto const& tag : tags) {
				auto control = new ValueSlider(grid, tag, proportional, [this, tag, proportional](double value) {
					ApplyNumeric(tag, value, proportional);
				});
				bindings.push_back({tag, proportional, control});
				grid->AddControl(control);
			}
			grid->Finalize();
			page_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));
			page->SetSizer(page_sizer);
			page->SetMinSize(wxSize(-1, page_sizer->GetMinSize().y));
			notebook->AddPage(page, proportional ? _("Proportional") : _("Fixed value"));
		}
		main->Add(notebook, 1, wxEXPAND);
		SetSizer(main);
		RefreshFromSelection(true);
	}
};

struct ColourBinding {
	std::string tag;
	bool proportional = false;
	ColourEditor *control = nullptr;
};

class BasicTagPanel final : public TagPanel {
	std::vector<NumericBinding> numeric;
	std::vector<ColourBinding> colours;

	void RefreshFromSelection(bool update_reset) override {
		auto reference = ReferenceLine();
		Enable(reference != nullptr);
		if (!reference) return;
		auto reference_state = ReadLine(context, reference);
		for (auto const& binding : numeric) {
			double value = binding.proportional
				? SelectionPercentage(binding.tag)
				: NumericValue(reference_state, binding.tag);
			bool uniform = NumericDefined(reference_state, binding.tag);
			for (auto line : context->selectionController->GetSelectedSet()) {
				auto state = ReadLine(context, line);
				uniform = uniform && NumericDefined(state, binding.tag) &&
					(binding.proportional || std::abs(NumericValue(state, binding.tag) - value) < epsilon);
			}
			binding.control->ShowValue(value, uniform, update_reset);
		}
		for (auto const& binding : colours) {
			binding.control->Enable(!binding.proportional);
			auto value = ColourValue(reference_state, binding.tag);
			bool uniform = true;
			for (auto line : context->selectionController->GetSelectedSet()) {
				auto other = ColourValue(ReadLine(context, line), binding.tag);
				uniform = uniform && other.r == value.r && other.g == value.g && other.b == value.b;
			}
			binding.control->ShowValue(value, uniform, update_reset);
		}
	}

public:
	BasicTagPanel(wxWindow *parent, agi::Context *context, bool *floating_edit_in_progress)
	: TagPanel(parent, context, floating_edit_in_progress)
	{
		auto main = new wxBoxSizer(wxVERTICAL);
		auto notebook = new wxNotebook(this, wxID_ANY);
		for (int page_index = 0; page_index < 2; ++page_index) {
			bool proportional = page_index == 1;
			auto page = new wxScrolledWindow(notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
			page->SetScrollRate(0, FromDIP(12));
			auto page_sizer = new wxBoxSizer(wxVERTICAL);
			auto grid = new ResponsiveGrid(page, true);
			auto add_numeric = [&](std::string tag) {
				auto control = new ValueSlider(grid, tag, proportional, [this, tag, proportional](double value) {
					ApplyNumeric(tag, value, proportional);
				}, proportional && IsAlphaTag(tag) ? std::function<void()>([this, tag] { ResetNumeric(tag); }) : std::function<void()>());
				numeric.push_back({tag, proportional, control});
				grid->AddControl(control);
			};
			struct ColourDefinition { const char *tag; const char *label; };
			for (auto const& definition : std::array<ColourDefinition, 3>{{
				{"\\c", "Primary"}, {"\\3c", "Outline"}, {"\\4c", "Shadow"}}}) {
				auto control = new ColourEditor(grid, wxGetTranslation(definition.label),
					[this, tag = std::string(definition.tag), proportional](agi::Color colour) {
						ApplyColour(tag, colour, proportional);
					},
					[this, tag = std::string(definition.tag)](double percentage_points) {
						ApplyColourAdjustment(tag, percentage_points);
					},
					[this, tag = std::string(definition.tag)] { ResetColour(tag); });
				control->Enable(!proportional);
				colours.push_back({definition.tag, proportional, control});
				grid->AddControl(control);
			}
			for (auto const& tag : {"\\alpha", "\\1a", "\\3a", "\\4a"}) add_numeric(tag);
			add_numeric("\\blur");
			grid->Finalize();
			page_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));
			page->SetSizer(page_sizer);
			page->SetMinSize(wxSize(-1, page_sizer->GetMinSize().y));
			notebook->AddPage(page, proportional ? _("Proportional") : _("Fixed value"));
		}
		main->Add(notebook, 1, wxEXPAND);
		SetSizer(main);
		RefreshFromSelection(true);
	}
};

std::pair<double, double> TextExtents(LineState const& state, AssDialogue const& line) {
	AssStyle style;
	style.font = state.font;
	style.fontsize = state.font_size;
	style.scalex = state.scale_x;
	style.scaley = state.scale_y;
	style.spacing = state.spacing;
	style.bold = state.bold;
	style.italic = state.italic;
	style.underline = state.underline;
	style.strikeout = state.strikeout;
	style.encoding = state.encoding;
	std::string text = line.GetStrippedText();
	for (size_t position = 0; (position = text.find("\\N", position)) != std::string::npos;)
		text.replace(position, 2, "\n");
	for (size_t position = 0; (position = text.find("\\n", position)) != std::string::npos;)
		text.replace(position, 2, " ");
	double total_height = 0, maximum_width = 0;
	std::stringstream lines(text);
	std::string row;
	bool found = false;
	while (std::getline(lines, row)) {
		found = true;
		double width = 0, height = 0, descent = 0, lead = 0;
		if (!Automation4::CalculateTextExtents(&style, row, width, height, descent, lead)) {
			width = state.font_size * state.scale_x / 100.0 * row.size() * .55;
			height = state.font_size * state.scale_y / 100.0;
		}
		maximum_width = std::max(maximum_width, width);
		total_height += height;
	}
	if (!found) total_height = state.font_size * state.scale_y / 100.0;
	return {maximum_width, total_height};
}

std::pair<double, double> AnchorOffset(int alignment, double width, double height) {
	int horizontal = (alignment - 1) % 3;
	int vertical = (alignment - 1) / 3;
	double x = horizontal == 0 ? 0 : horizontal == 1 ? width / 2 : width;
	double y = vertical == 0 ? height : vertical == 1 ? height / 2 : 0;
	return {x, y};
}

struct DrawingBounds {
	double minimum_x = 0.0;
	double minimum_y = 0.0;
	double maximum_x = 0.0;
	double maximum_y = 0.0;
	bool valid = false;
};

std::vector<double> DrawingNumbers(std::string const& drawing) {
	static std::regex const number_pattern(R"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)");
	std::vector<double> numbers;
	for (std::sregex_iterator match(drawing.begin(), drawing.end(), number_pattern), end; match != end; ++match) {
		try { numbers.push_back(std::stod(match->str())); }
		catch (...) { }
	}
	return numbers;
}

void AddDrawingBounds(DrawingBounds& bounds, AssDialogueBlockDrawing const& drawing) {
	if (drawing.Scale != 1) return;
	auto numbers = DrawingNumbers(drawing.text);
	for (size_t index = 0; index + 1 < numbers.size(); index += 2) {
		double x = numbers[index];
		double y = numbers[index + 1];
		if (!bounds.valid) {
			bounds.minimum_x = bounds.maximum_x = x;
			bounds.minimum_y = bounds.maximum_y = y;
			bounds.valid = true;
		}
		else {
			bounds.minimum_x = std::min(bounds.minimum_x, x);
			bounds.minimum_y = std::min(bounds.minimum_y, y);
			bounds.maximum_x = std::max(bounds.maximum_x, x);
			bounds.maximum_y = std::max(bounds.maximum_y, y);
		}
	}
}

std::string TranslateDrawing(std::string const& drawing, double x_offset, double y_offset) {
	static std::regex const number_pattern(R"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)");
	std::string result;
	size_t previous = 0;
	size_t coordinate = 0;
	for (std::sregex_iterator match(drawing.begin(), drawing.end(), number_pattern), end; match != end; ++match) {
		auto position = static_cast<size_t>(match->position());
		result.append(drawing, previous, position - previous);
		double value = std::stod(match->str()) + (coordinate++ % 2 == 0 ? x_offset : y_offset);
		result += FormatNumber(value);
		previous = position + static_cast<size_t>(match->length());
	}
	result.append(drawing, previous, std::string::npos);
	return result;
}

bool RepositionDrawingPoints(AssDialogue *line, int old_alignment, int new_alignment) {
	auto blocks = line->ParseTags();
	DrawingBounds bounds;
	for (auto const& block : blocks) {
		if (block->GetType() == AssBlockType::PLAIN) {
			if (!static_cast<AssDialogueBlockPlain const&>(*block).text.empty()) return false;
		}
		else if (block->GetType() == AssBlockType::DRAWING) {
			auto const& drawing = static_cast<AssDialogueBlockDrawing const&>(*block);
			if (drawing.Scale != 1 && !drawing.text.empty()) return false;
			AddDrawingBounds(bounds, drawing);
		}
	}
	if (!bounds.valid) return false;

	auto old_anchor = AnchorOffset(old_alignment,
		bounds.maximum_x - bounds.minimum_x, bounds.maximum_y - bounds.minimum_y);
	auto new_anchor = AnchorOffset(new_alignment,
		bounds.maximum_x - bounds.minimum_x, bounds.maximum_y - bounds.minimum_y);
	double x_offset = new_anchor.first - old_anchor.first;
	double y_offset = new_anchor.second - old_anchor.second;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::DRAWING) continue;
		auto drawing = static_cast<AssDialogueBlockDrawing *>(block.get());
		if (drawing->Scale == 1)
			drawing->text = TranslateDrawing(drawing->text, x_offset, y_offset);
	}
	line->UpdateText(blocks);
	return true;
}

std::pair<double, double> RepositionDelta(LineState const& state, AssDialogue const& line, int new_alignment) {
	auto [width, height] = TextExtents(state, line);
	auto old_anchor = AnchorOffset(state.alignment, width, height);
	auto new_anchor = AnchorOffset(new_alignment, width, height);
	double dx = new_anchor.first - old_anchor.first;
	double dy = new_anchor.second - old_anchor.second;
	double sheared_x = dx + state.shear_x * dy;
	double sheared_y = dy + state.shear_y * dx;
	double radians = state.rotation_z * 3.14159265358979323846 / 180.0;
	return {
		std::cos(radians) * sheared_x + std::sin(radians) * sheared_y,
		-std::sin(radians) * sheared_x + std::cos(radians) * sheared_y
	};
}

std::pair<double, double> DefaultPosition(agi::Context *context, LineState const& state) {
	int width = 0, height = 0;
	context->ass->GetResolution(width, height);
	int horizontal = (state.alignment - 1) % 3;
	int vertical = (state.alignment - 1) / 3;
	double x = horizontal == 0 ? state.margins[0]
		: horizontal == 1 ? (width + state.margins[0] - state.margins[1]) / 2.0
		: width - state.margins[1];
	double y = vertical == 0 ? height - state.margins[2]
		: vertical == 1 ? height / 2.0 : state.margins[2];
	return {x, y};
}

class AlignmentTagPanel final : public TagPanel {
	std::array<wxToggleButton *, 9> buttons{};
	wxCheckBox *reposition = nullptr;

	void RefreshFromSelection(bool) override {
		auto reference = ReferenceLine();
		Enable(reference != nullptr);
		if (!reference) return;
		int alignment = ReadLine(context, reference).alignment;
		bool uniform = true;
		for (auto line : context->selectionController->GetSelectedSet())
			uniform = uniform && ReadLine(context, line).alignment == alignment;
		for (int value = 1; value <= 9; ++value)
			buttons[value - 1]->SetValue(uniform && value == alignment);
	}

	void ApplyAlignment(int alignment) {
		for (auto line : context->selectionController->GetSelectedSet()) {
			auto state = ReadLine(context, line);
			if (reposition->IsChecked()) {
				if (RepositionDrawingPoints(line, state.alignment, alignment)) {
					// Unpositioned drawings first need an explicit copy of their old
					// margin-based anchor; the point shift then preserves their image.
					if (!state.has_position && !state.has_move) {
						auto position = DefaultPosition(context, state);
						SetOverrideAtStart(line, "\\pos", "(" + FormatNumber(position.first) +
							"," + FormatNumber(position.second) + ")");
					}
				}
				else {
					auto delta = RepositionDelta(state, *line, alignment);
					if (state.has_move) {
						std::string move = "(" + FormatNumber(state.move_x1 + delta.first) + "," +
							FormatNumber(state.move_y1 + delta.second) + "," +
							FormatNumber(state.move_x2 + delta.first) + "," +
							FormatNumber(state.move_y2 + delta.second);
						if (state.move_has_times)
							move += "," + std::to_string(state.move_t1) + "," + std::to_string(state.move_t2);
						SetOverrideAtStart(line, "\\move", move + ")");
					}
					else {
						auto position = state.has_position
							? std::pair<double, double>{state.position_x, state.position_y}
							: DefaultPosition(context, state);
						SetOverrideAtStart(line, "\\pos", "(" + FormatNumber(position.first + delta.first) +
							"," + FormatNumber(position.second + delta.second) + ")");
					}
				}
			}
			SetOverrideAtStart(line, "\\an", std::to_string(alignment));
		}
		Commit(_("change subtitle alignment"), "alignment");
	}

public:
	AlignmentTagPanel(wxWindow *parent, agi::Context *context, bool *floating_edit_in_progress)
	: TagPanel(parent, context, floating_edit_in_progress)
	{
		auto main = new wxBoxSizer(wxVERTICAL);
		auto grid = new wxGridSizer(3, 3, FromDIP(5), FromDIP(5));
		std::array<wxString, 9> labels{{
			wxString::FromUTF8("└"), wxString::FromUTF8("─"), wxString::FromUTF8("┘"),
			wxString::FromUTF8("│"), "+", wxString::FromUTF8("│"),
			wxString::FromUTF8("┌"), wxString::FromUTF8("─"), wxString::FromUTF8("┐")
		}};
		for (int row = 0; row < 3; ++row) {
			for (int column = 0; column < 3; ++column) {
				int alignment = (2 - row) * 3 + column + 1;
				auto button = new wxToggleButton(this, wxID_ANY, labels[alignment - 1]);
				auto font = button->GetFont();
				font.SetPointSize(font.GetPointSize() + 4);
				button->SetFont(font);
				button->SetToolTip(to_wx(agi::format("\\an%d", alignment)));
				buttons[alignment - 1] = button;
				button->Bind(wxEVT_TOGGLEBUTTON, [this, alignment](wxCommandEvent&) { ApplyAlignment(alignment); });
				grid->Add(button, 1, wxEXPAND);
			}
		}
		main->Add(grid, 1, wxEXPAND | wxALL, FromDIP(10));
		reposition = new wxCheckBox(this, wxID_ANY, _("Re-position"));
		main->Add(reposition, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
		SetSizer(main);
		RefreshFromSelection(true);
	}
};

class DeleteTagsPanel final : public TagPanel {
	void RefreshFromSelection(bool) override { Enable(ReferenceLine() != nullptr); }

public:
	DeleteTagsPanel(wxWindow *parent, agi::Context *context, bool *floating_edit_in_progress)
	: TagPanel(parent, context, floating_edit_in_progress)
	{
		struct DeleteDefinition { const char *label; std::vector<std::string> tags; };
		std::vector<DeleteDefinition> definitions = {
			{"\\blur", {"\\blur"}}, {"\\c", {"\\c", "\\1c"}}, {"\\3c", {"\\3c"}}, {"\\4c", {"\\4c"}},
			{"\\alpha", {"\\alpha"}}, {"\\1a", {"\\1a"}}, {"\\3a", {"\\3a"}}, {"\\4a", {"\\4a"}},
			{"\\bord", {"\\bord"}}, {"\\xbord", {"\\xbord"}}, {"\\ybord", {"\\ybord"}},
			{"\\shad", {"\\shad"}}, {"\\xshad", {"\\xshad"}}, {"\\yshad", {"\\yshad"}},
			{"\\fs", {"\\fs"}}, {"\\fscx", {"\\fscx"}}, {"\\fscy", {"\\fscy"}}, {"\\fsp", {"\\fsp"}},
			{"\\an", {"\\an", "\\a"}}, {"\\frz", {"\\frz", "\\fr"}}, {"\\frx", {"\\frx"}}, {"\\fry", {"\\fry"}},
			{"\\fax", {"\\fax"}}, {"\\fay", {"\\fay"}}, {"\\clip / \\iclip", {"\\clip", "\\iclip"}}, {"\\fad", {"\\fad"}}
		};
		auto main = new wxBoxSizer(wxVERTICAL);
		auto grid = new ResponsiveGrid(this);
		for (auto definition : definitions) {
			auto button = new wxButton(grid, wxID_ANY, to_wx(definition.label));
			button->Bind(wxEVT_BUTTON, [this, definition = std::move(definition)](wxCommandEvent&) {
				for (auto line : this->context->selectionController->GetSelectedSet()) RemoveOverrides(line, definition.tags);
				Commit(_("delete override tag"), "delete/" + std::string(definition.label));
			});
			grid->AddControl(button);
		}
		grid->Finalize();
		main->Add(grid, 0, wxEXPAND | wxALL, FromDIP(5));
		SetSizer(main);
		RefreshFromSelection(true);
	}
};

constexpr std::array<FloatingTagWindow, 6> all_palettes = {
	FloatingTagWindow::Basic,
	FloatingTagWindow::BorderShadow,
	FloatingTagWindow::Font,
	FloatingTagWindow::Alignment,
	FloatingTagWindow::Transform,
	FloatingTagWindow::DeleteTags
};

class PaletteItem final : public wxPanel {
	wxPanel *header;
	wxStaticText *title;
	wxButton *move_up;
	wxButton *move_down;
	wxWindow *body = nullptr;
	wxBoxSizer *main_sizer;
	wxPoint drag_origin;
	bool mouse_down = false;
	bool dragging = false;
	int last_width = -1;
	bool updating_height = false;
	std::function<void()> close_callback;
	std::function<void()> move_up_callback;
	std::function<void()> move_down_callback;
	std::function<void()> content_size_callback;
	std::function<void()> drag_begin_callback;
	std::function<void(wxPoint)> drag_motion_callback;
	std::function<void(wxPoint, wxPoint)> drag_end_callback;
	std::function<void()> drag_cancel_callback;
	wxColour header_background;
	wxColour title_background;
	wxColour title_foreground;

	void BindDrag(wxWindow *window) {
		window->Bind(wxEVT_LEFT_DOWN, [this, window](wxMouseEvent& event) {
			drag_origin = window->ClientToScreen(event.GetPosition());
			mouse_down = true;
			dragging = false;
			window->CaptureMouse();
		});
		window->Bind(wxEVT_MOTION, [this, window](wxMouseEvent& event) {
			if (!mouse_down || !event.LeftIsDown()) return;
			auto current = window->ClientToScreen(event.GetPosition());
			if (!dragging && std::abs(current.x - drag_origin.x) + std::abs(current.y - drag_origin.y) >= FromDIP(6)) {
				dragging = true;
				window->SetCursor(wxCursor(wxCURSOR_SIZING));
				drag_begin_callback();
			}
			if (dragging) drag_motion_callback(current);
		});
		window->Bind(wxEVT_LEFT_UP, [this, window](wxMouseEvent& event) {
			auto current = window->ClientToScreen(event.GetPosition());
			bool was_dragging = dragging;
			mouse_down = false;
			dragging = false;
			window->SetCursor(wxNullCursor);
			if (window->HasCapture()) window->ReleaseMouse();
			if (was_dragging) drag_end_callback(drag_origin, current);
		});
		window->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this, window](wxMouseCaptureLostEvent&) {
			bool was_dragging = dragging;
			mouse_down = false;
			dragging = false;
			window->SetCursor(wxNullCursor);
			if (was_dragging) drag_cancel_callback();
		});
	}

	void UpdateContentHeight() {
		if (!body || updating_height) return;
		updating_height = true;
		body->InvalidateBestSize();
		InvalidateBestSize();
		auto body_size = body->GetSizer() ? body->GetSizer()->GetMinSize() : body->GetBestSize();
		auto height = body_size.y + (header->IsShown() ? FromDIP(27) : 0);
		SetMinSize(wxSize(FromDIP(minimum_palette_width), height));
		Layout();
		updating_height = false;
	}

public:
	FloatingTagWindow const kind;
	wxString const caption;

	PaletteItem(wxWindow *parent, FloatingTagWindow kind, wxString caption,
		std::function<void()> close_callback,
		std::function<void()> move_up_callback,
		std::function<void()> move_down_callback,
		std::function<void()> content_size_callback,
		std::function<void()> drag_begin_callback,
		std::function<void(wxPoint)> drag_motion_callback,
		std::function<void(wxPoint, wxPoint)> drag_end_callback,
		std::function<void()> drag_cancel_callback)
	: wxPanel(parent)
	, header(new wxPanel(this))
	, title(new wxStaticText(header, wxID_ANY, caption))
	, move_up(new wxButton(header, wxID_ANY, wxString::FromUTF8("\xE2\x86\x91"), wxDefaultPosition, FromDIP(wxSize(24, 22)), wxBU_EXACTFIT))
	, move_down(new wxButton(header, wxID_ANY, wxString::FromUTF8("\xE2\x86\x93"), wxDefaultPosition, FromDIP(wxSize(24, 22)), wxBU_EXACTFIT))
	, main_sizer(new wxBoxSizer(wxVERTICAL))
	, close_callback(std::move(close_callback))
	, move_up_callback(std::move(move_up_callback))
	, move_down_callback(std::move(move_down_callback))
	, content_size_callback(std::move(content_size_callback))
	, drag_begin_callback(std::move(drag_begin_callback))
	, drag_motion_callback(std::move(drag_motion_callback))
	, drag_end_callback(std::move(drag_end_callback))
	, drag_cancel_callback(std::move(drag_cancel_callback))
	, kind(kind)
	, caption(std::move(caption))
	{
		header_background = header->GetBackgroundColour();
		title_background = title->GetBackgroundColour();
		title_foreground = title->GetForegroundColour();
		auto close = new wxButton(header, wxID_ANY, wxS("×"), wxDefaultPosition, FromDIP(wxSize(24, 22)), wxBU_EXACTFIT);
		close->SetToolTip(_("Close"));
		close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { this->close_callback(); });
		move_up->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			CallAfter([this] { this->move_up_callback(); });
		});
		move_down->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			CallAfter([this] { this->move_down_callback(); });
		});

		auto header_sizer = new wxBoxSizer(wxHORIZONTAL);
		header_sizer->Add(title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
		header_sizer->Add(move_up, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
		header_sizer->Add(move_down, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
		header_sizer->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
		header->SetSizer(header_sizer);
		header->SetMinSize(FromDIP(wxSize(minimum_palette_width, 27)));
		main_sizer->Add(header, 0, wxEXPAND);
		SetSizer(main_sizer);
		BindDrag(header);
		BindDrag(title);
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			event.Skip();
			if (!body || event.GetSize().x == last_width) return;
			last_width = event.GetSize().x;
			CallAfter([this] {
				UpdateContentHeight();
				this->content_size_callback();
			});
		});
	}

	void SetBody(wxWindow *new_body) {
		body = new_body;
		body->SetMinSize(FromDIP(wxSize(minimum_palette_width, -1)));
		main_sizer->Add(body, 0, wxEXPAND);
		UpdateContentHeight();
	}

	void ShowCategoryHeader(bool show) {
		header->Show(show);
		UpdateContentHeight();
	}

	void SetMoveButtonsEnabled(bool can_move_up, bool can_move_down) {
		move_up->Enable(can_move_up);
		move_down->Enable(can_move_down);
	}

	void SetDragHighlight(bool highlight) {
		header->SetBackgroundColour(highlight
			? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
			: header_background);
		title->SetBackgroundColour(highlight
			? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
			: title_background);
		title->SetForegroundColour(highlight
			? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
			: title_foreground);
		header->Refresh();
		title->Refresh();
	}
};

class PaletteGroupFrame final : public wxFrame {
	wxScrolledWindow *scroll;
	wxBoxSizer *item_sizer;
	std::vector<PaletteItem *> items;
	wxTimer dock_timer;
	std::function<void(PaletteGroupFrame *)> close_callback;
	std::function<void(PaletteGroupFrame *)> move_progress_callback;
	std::function<void(PaletteGroupFrame *, bool)> settled_callback;
	bool settle_was_move = false;
	bool automatic_size = true;
	int transparency = 255;

public:
	PaletteGroupFrame(wxFrame *owner, wxPoint position, wxSize saved_size,
		std::function<void(PaletteGroupFrame *)> close_callback,
		std::function<void(PaletteGroupFrame *)> move_progress_callback,
		std::function<void(PaletteGroupFrame *, bool)> settled_callback)
	: wxFrame(owner, wxID_ANY, _("Floating tag windows"), position, wxDefaultSize,
		wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxMAXIMIZE_BOX |
		wxFRAME_TOOL_WINDOW | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxCLIP_CHILDREN)
	, scroll(new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL))
	, item_sizer(new wxBoxSizer(wxVERTICAL))
	, dock_timer(this)
	, close_callback(std::move(close_callback))
	, move_progress_callback(std::move(move_progress_callback))
	, settled_callback(std::move(settled_callback))
	{
		SetMinSize(FromDIP(wxSize(minimum_palette_width + 20, 120)));
		if (saved_size.x > 0 && saved_size.y > 0) {
			SetClientSize(saved_size);
			automatic_size = false;
		}
		scroll->SetSizer(item_sizer);
		scroll->SetScrollRate(0, FromDIP(12));
		auto frame_sizer = new wxBoxSizer(wxVERTICAL);
		frame_sizer->Add(scroll, 1, wxEXPAND);
		SetSizer(frame_sizer);

		Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
			if (!event.CanVeto()) {
				event.Skip();
				return;
			}
			this->close_callback(this);
			event.Veto();
		});
		Bind(wxEVT_MOVE, [this](wxMoveEvent& event) {
			if (IsShown()) {
				settle_was_move = true;
				this->move_progress_callback(this);
				dock_timer.StartOnce(300);
			}
			event.Skip();
		});
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			if (IsShown()) {
				settle_was_move = false;
				dock_timer.StartOnce(300);
			}
			event.Skip();
		});
		Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			if (wxGetMouseState().LeftIsDown()) {
				dock_timer.StartOnce(150);
				return;
			}
			this->settled_callback(this, settle_was_move);
			settle_was_move = false;
		});
	}

	void AddItem(PaletteItem *item) {
		item->Reparent(scroll);
		items.push_back(item);
		item_sizer->Add(item, 0, wxEXPAND);
	}

	void RemoveItem(PaletteItem *item) {
		item_sizer->Detach(item);
		items.erase(std::remove(items.begin(), items.end(), item), items.end());
	}

	bool MoveVisibleItem(PaletteItem *item, int direction) {
		auto current = std::find(items.begin(), items.end(), item);
		if (current == items.end() || direction == 0) return false;
		auto target = current;
		if (direction < 0) {
			while (target != items.begin()) {
				--target;
				if ((*target)->IsShown()) break;
			}
			if (target == current || !(*target)->IsShown()) return false;
			std::rotate(target, current, current + 1);
		}
		else {
			while (++target != items.end() && !(*target)->IsShown()) { }
			if (target == items.end()) return false;
			std::rotate(current, current + 1, target + 1);
		}

		item_sizer->Clear(false);
		for (auto palette : items) item_sizer->Add(palette, 0, wxEXPAND);
		RefreshGroup();
		return true;
	}

	std::vector<PaletteItem *> const& Items() const { return items; }
	bool Empty() const { return items.empty(); }

	void RefreshGroup(bool fit_content_height = false) {
		if (items.empty()) {
			Hide();
			return;
		}

		bool grouped = items.size() > 1;
		for (auto item : items) item->ShowCategoryHeader(grouped);
		bool have_visible_before = false;
		for (size_t index = 0; index < items.size(); ++index) {
			bool have_visible_after = std::any_of(items.begin() + index + 1, items.end(),
				[](PaletteItem *item) { return item->IsShown(); });
			items[index]->SetMoveButtonsEnabled(items[index]->IsShown() && have_visible_before,
				items[index]->IsShown() && have_visible_after);
			have_visible_before = have_visible_before || items[index]->IsShown();
		}
		SetTitle(grouped ? _("Floating tag windows") : items.front()->caption);

		bool any_visible = std::any_of(items.begin(), items.end(), [](PaletteItem *item) { return item->IsShown(); });
		if (!any_visible) {
			Hide();
			return;
		}

		item_sizer->Layout();
		auto content_size = item_sizer->GetMinSize();
		if (automatic_size || fit_content_height || GetClientSize().y < content_size.y) {
			int width = automatic_size ? FromDIP(palette_width + 20) : GetClientSize().x;
			SetClientSize(width, std::max(content_size.y, FromDIP(40)));
			automatic_size = false;
		}
		scroll->SetVirtualSize(std::max(scroll->GetClientSize().x, FromDIP(minimum_palette_width)), content_size.y);
		scroll->FitInside();
		Layout();
		Show();
	}

	void SetDragVisual(bool target) {
		int wanted = target ? 205 : 255;
		if (wanted == transparency) return;
		transparency = wanted;
		SetTransparent(static_cast<unsigned char>(wanted));
	}

	void StopDockTimer() { dock_timer.Stop(); }
};

} // namespace

class FloatingTagWindowManager::Impl {
	wxFrame *owner;
	agi::Context *context;
	std::vector<PaletteGroupFrame *> groups;
	std::map<FloatingTagWindow, PaletteItem *> items;
	bool floating_edit_in_progress = false;
	bool destroying = false;
	bool loading = true;

	struct SavedItem {
		FloatingTagWindow kind;
		bool shown;
	};
	struct SavedGroup {
		wxPoint position;
		wxSize size = wxDefaultSize;
		std::vector<SavedItem> items;
	};

	PaletteGroupFrame *CreateGroup(wxPoint position, wxSize size = wxDefaultSize) {
		auto group = new PaletteGroupFrame(owner, position, size,
			[this](PaletteGroupFrame *group) { HideGroup(group); },
			[this](PaletteGroupFrame *group) { GroupMoveProgress(group); },
			[this](PaletteGroupFrame *group, bool moved) { GroupSettled(group, moved); });
		groups.push_back(group);
		return group;
	}

	PaletteGroupFrame *GroupFor(PaletteItem *item) const {
		for (auto group : groups) {
			auto const& group_items = group->Items();
			if (std::find(group_items.begin(), group_items.end(), item) != group_items.end()) return group;
		}
		return nullptr;
	}

	PaletteGroupFrame *DropTarget(PaletteGroupFrame *source, wxPoint point) const {
		for (auto group : groups)
			if (group != source && group->IsShown() && group->GetScreenRect().Contains(point)) return group;
		return nullptr;
	}

	void ClearDragVisuals() {
		for (auto group : groups) group->SetDragVisual(false);
		for (auto const& [kind, item] : items) item->SetDragHighlight(false);
	}

	void ShowDragVisuals(PaletteGroupFrame *source, PaletteItem *dragged_item, wxPoint point) {
		auto target = DropTarget(source, point);
		for (auto group : groups) group->SetDragVisual(group == target);
		for (auto const& [kind, item] : items) item->SetDragHighlight(item == dragged_item);
	}

	void RemoveEmptyGroup(PaletteGroupFrame *group) {
		if (!group || !group->Empty()) return;
		group->StopDockTimer();
		groups.erase(std::remove(groups.begin(), groups.end(), group), groups.end());
		group->Destroy();
	}

	void MoveItem(PaletteItem *item, PaletteGroupFrame *target) {
		auto source = GroupFor(item);
		if (!source || !target || source == target) return;
		source->RemoveItem(item);
		target->AddItem(item);
		source->RefreshGroup(true);
		target->RefreshGroup(true);
		target->Raise();
		RemoveEmptyGroup(source);
		Save();
	}

	void BeginItemDrag(FloatingTagWindow kind) {
		auto found = items.find(kind);
		if (found == items.end()) return;
		auto source = GroupFor(found->second);
		if (source) ShowDragVisuals(source, found->second, wxGetMousePosition());
	}

	void UpdateItemDrag(FloatingTagWindow kind, wxPoint current) {
		auto found = items.find(kind);
		if (found == items.end()) return;
		auto source = GroupFor(found->second);
		if (source) ShowDragVisuals(source, found->second, current);
	}

	void CancelItemDrag() { ClearDragVisuals(); }

	void EndItemDrag(FloatingTagWindow kind, wxPoint origin, wxPoint current) {
		auto found = items.find(kind);
		if (found == items.end()) return;
		auto item = found->second;
		auto source = GroupFor(item);
		if (!source) return;

		auto target = DropTarget(source, current);
		ClearDragVisuals();
		if (target) {
			MoveItem(item, target);
			return;
		}

		if (source->Items().size() > 1) {
			auto position = current - owner->FromDIP(wxPoint(40, 12));
			auto new_group = CreateGroup(position);
			MoveItem(item, new_group);
		}
		else {
			source->Move(source->GetPosition() + current - origin);
			Save();
		}
	}

	void GroupMoveProgress(PaletteGroupFrame *source) {
		if (loading || destroying || !source || !source->IsShown() || !wxGetMouseState().LeftIsDown()) return;
		auto rect = source->GetScreenRect();
		ShowDragVisuals(source, nullptr, rect.GetTopLeft() + wxPoint(rect.width / 2, owner->FromDIP(12)));
	}

	void GroupSettled(PaletteGroupFrame *source, bool moved) {
		if (loading || destroying || !source || !source->IsShown()) return;
		ClearDragVisuals();
		if (!moved) {
			Save();
			return;
		}
		auto rect = source->GetScreenRect();
		auto probe = rect.GetTopLeft() + wxPoint(rect.width / 2, owner->FromDIP(12));
		auto target = DropTarget(source, probe);
		if (!target) {
			Save();
			return;
		}

		auto moving = source->Items();
		for (auto item : moving) {
			source->RemoveItem(item);
			target->AddItem(item);
		}
		target->RefreshGroup(true);
		target->Raise();
		RemoveEmptyGroup(source);
		Save();
	}

	void HideGroup(PaletteGroupFrame *group) {
		if (destroying) return;
		for (auto item : group->Items()) item->Show(false);
		group->RefreshGroup(group->Items().size() > 1);
		Save();
	}

	void ItemContentSizeChanged(FloatingTagWindow kind) {
		auto found = items.find(kind);
		if (found == items.end()) return;
		if (auto group = GroupFor(found->second)) group->RefreshGroup(true);
	}

	void MoveItemVertically(FloatingTagWindow kind, int direction) {
		auto found = items.find(kind);
		if (found == items.end()) return;
		if (auto group = GroupFor(found->second); group && group->MoveVisibleItem(found->second, direction))
			Save();
	}

	void AddPalette(PaletteGroupFrame *group, FloatingTagWindow kind, bool shown) {
		wxString caption;
		std::function<wxWindow *(wxWindow *)> create_panel;
		switch (kind) {
			case FloatingTagWindow::Basic:
				caption = _("Basic");
				create_panel = [this](wxWindow *parent) { return new BasicTagPanel(parent, context, &floating_edit_in_progress); };
				break;
			case FloatingTagWindow::BorderShadow:
				caption = _("Bord & Shad");
				create_panel = [this](wxWindow *parent) { return new NumericTagPanel(parent, context, &floating_edit_in_progress, {"\\bord", "\\xbord", "\\ybord", "\\shad", "\\xshad", "\\yshad"}); };
				break;
			case FloatingTagWindow::Font:
				caption = _("Font");
				create_panel = [this](wxWindow *parent) { return new NumericTagPanel(parent, context, &floating_edit_in_progress, {"\\fs", "\\fscx", "\\fscy", "\\fsp"}); };
				break;
			case FloatingTagWindow::Alignment:
				caption = _("Alignment");
				create_panel = [this](wxWindow *parent) { return new AlignmentTagPanel(parent, context, &floating_edit_in_progress); };
				break;
			case FloatingTagWindow::Transform:
				caption = _("Transformation");
				create_panel = [this](wxWindow *parent) { return new NumericTagPanel(parent, context, &floating_edit_in_progress, {"\\frz", "\\frx", "\\fry", "\\fax", "\\fay"}); };
				break;
			case FloatingTagWindow::DeleteTags:
				caption = _("Tag deletion");
				create_panel = [this](wxWindow *parent) { return new DeleteTagsPanel(parent, context, &floating_edit_in_progress); };
				break;
		}

		auto item = new PaletteItem(group, kind, caption,
			[this, kind] { SetShown(kind, false); },
			[this, kind] { MoveItemVertically(kind, -1); },
			[this, kind] { MoveItemVertically(kind, 1); },
			[this, kind] { ItemContentSizeChanged(kind); },
			[this, kind] { BeginItemDrag(kind); },
			[this, kind](wxPoint current) { UpdateItemDrag(kind, current); },
			[this, kind](wxPoint origin, wxPoint current) { EndItemDrag(kind, origin, current); },
			[this] { CancelItemDrag(); });
		item->SetBody(create_panel(item));
		item->Show(shown);
		items[kind] = item;
		group->AddItem(item);
	}

	std::vector<SavedGroup> LoadLayout() const {
		std::vector<SavedGroup> result;
		auto layout = OPT_GET("Tool/Floating Tags/Layout")->GetString();
		bool version_two = layout.rfind("v2;", 0) == 0;
		if (!version_two && layout.rfind("v1;", 0) != 0) return result;
		std::array<bool, all_palettes.size()> seen{};
		std::stringstream stream(layout.substr(3));
		std::string group_text;
		while (std::getline(stream, group_text, ';')) {
			auto colon = group_text.find(':');
			if (colon == std::string::npos) continue;
			SavedGroup group;
			try {
				std::vector<int> geometry;
				std::stringstream geometry_stream(group_text.substr(0, colon));
				std::string value;
				while (std::getline(geometry_stream, value, ',')) geometry.push_back(std::stoi(value));
				if (geometry.size() < 2) continue;
				group.position = wxPoint(geometry[0], geometry[1]);
				if (version_two && geometry.size() >= 4) group.size = wxSize(geometry[2], geometry[3]);
			}
			catch (...) { continue; }

			std::stringstream item_stream(group_text.substr(colon + 1));
			std::string item_text;
			while (std::getline(item_stream, item_text, ',')) {
				if (item_text.size() < 2) continue;
				int index = item_text[0] - '0';
				if (index < 0 || index >= static_cast<int>(all_palettes.size()) || seen[index]) continue;
				seen[index] = true;
				group.items.push_back({all_palettes[index], item_text[1] == '+'});
			}
			if (!group.items.empty()) result.push_back(std::move(group));
		}
		return result;
	}

public:
	Impl(wxFrame *frame, agi::Context *context)
	: owner(frame)
	, context(context)
	{
		auto saved = LoadLayout();
		std::array<bool, all_palettes.size()> created{};
		for (auto const& saved_group : saved) {
			auto group = CreateGroup(saved_group.position, saved_group.size);
			for (auto const& saved_item : saved_group.items) {
				AddPalette(group, saved_item.kind, saved_item.shown);
				created[static_cast<size_t>(saved_item.kind)] = true;
			}
			group->RefreshGroup(true);
		}

		auto base_position = owner->GetScreenPosition() + owner->FromDIP(wxPoint(35, 65));
		for (size_t i = 0; i < all_palettes.size(); ++i) {
			if (created[i]) continue;
			auto group = CreateGroup(base_position + owner->FromDIP(wxPoint(static_cast<int>(i) * 26, static_cast<int>(i) * 22)));
			AddPalette(group, all_palettes[i], false);
			group->RefreshGroup();
		}
		loading = false;
	}

	~Impl() {
		destroying = true;
		Save();
		for (auto group : groups) {
			group->StopDockTimer();
			group->Destroy();
		}
	}

	void Save() {
		if (loading) return;
		std::string layout = "v2;";
		for (auto group : groups) {
			if (group->Empty()) continue;
			auto position = group->GetPosition();
			auto size = group->GetClientSize();
			layout += agi::format("%d,%d,%d,%d:", position.x, position.y, size.x, size.y);
			bool first = true;
			for (auto item : group->Items()) {
				if (!first) layout += ',';
				first = false;
				layout += agi::format("%d%c", static_cast<int>(item->kind), item->IsShown() ? '+' : '-');
			}
			layout += ';';
		}
		OPT_SET("Tool/Floating Tags/Layout")->SetString(layout);
	}

	void SetShown(FloatingTagWindow window, bool shown) {
		auto found = items.find(window);
		if (found == items.end()) return;
		auto item = found->second;
		item->Show(shown);
		auto group = GroupFor(item);
		if (!group) return;
		group->RefreshGroup(true);
		if (shown) group->Raise();
		Save();
	}

	void Toggle(FloatingTagWindow window) {
		auto found = items.find(window);
		if (found == items.end()) return;
		SetShown(window, !found->second->IsShown());
	}

	void ShowAll() {
		auto target = GroupFor(items.at(FloatingTagWindow::Basic));
		if (!target) {
			auto position = owner->GetScreenPosition() + owner->FromDIP(wxPoint(35, 65));
			target = CreateGroup(position);
		}

		auto previous_groups = groups;
		for (auto kind : all_palettes) {
			auto item = items.at(kind);
			if (auto source = GroupFor(item)) source->RemoveItem(item);
		}
		for (auto kind : all_palettes) {
			auto item = items.at(kind);
			item->Show(true);
			target->AddItem(item);
		}
		for (auto group : previous_groups)
			if (group != target) RemoveEmptyGroup(group);

		target->RefreshGroup(true);
		target->Raise();
		Save();
	}

	bool IsShown(FloatingTagWindow window) const {
		auto found = items.find(window);
		return found != items.end() && found->second->IsShown();
	}
};

FloatingTagWindowManager::FloatingTagWindowManager(wxFrame *frame, agi::Context *context)
: impl(std::make_unique<Impl>(frame, context))
{
}

FloatingTagWindowManager::~FloatingTagWindowManager() = default;

void FloatingTagWindowManager::Toggle(FloatingTagWindow window) { impl->Toggle(window); }
void FloatingTagWindowManager::ShowAll() { impl->ShowAll(); }
bool FloatingTagWindowManager::IsShown(FloatingTagWindow window) const { return impl->IsShown(window); }
void FloatingTagWindowManager::SaveLayout() { impl->Save(); }
