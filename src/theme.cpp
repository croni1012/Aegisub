// Copyright (c) 2026 Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "theme.h"

#include "compat.h"
#include "options.h"

#include <wx/gauge.h>
#include <wx/window.h>

namespace app_theme {
namespace {
bool dark_mode = false;
}

void Initialize(bool dark) {
	dark_mode = dark;
}

bool IsDark() {
	return dark_mode;
}

std::string ColourOption(std::string const& name) {
	return dark_mode ? "Colour/Dark/" + name : "Colour/" + name;
}

wxColour Colour(std::string const& name) {
	return to_wx(OPT_GET(ColourOption(name))->GetColor());
}

void StyleProgress(wxGauge *gauge) {
	if (!dark_mode || !gauge) return;
	gauge->SetForegroundColour(Colour("UI/Progress"));
	gauge->SetBackgroundColour(Colour("UI/Progress Background"));
}

void SoftenWhiteText(wxWindow *window) {
	if (!dark_mode || !window) return;

	auto const foreground = window->GetForegroundColour();
	if (foreground.IsOk() && foreground.Red() == 255 &&
		foreground.Green() == 255 && foreground.Blue() == 255)
	{
		window->SetForegroundColour(Colour("UI/Text"));
		window->Refresh();
	}

	for (auto *child : window->GetChildren())
		SoftenWhiteText(child);
}

}
