// Copyright (c) 2026 Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include <string>

#include <wx/colour.h>

class wxGauge;
class wxWindow;

namespace app_theme {

/// Record the appearance selected at application startup. The value remains
/// stable until restart so changing the preference cannot leave the running UI
/// with a mixture of light and dark controls.
void Initialize(bool dark);

bool IsDark();

/// Return a colour from the active application palette. The name is relative
/// to the Colour option group, for example "Subtitle Grid/Standard".
wxColour Colour(std::string const& name);

/// Return the complete option name for a colour in the active palette.
std::string ColourOption(std::string const& name);

/// Apply the dark palette to a progress indicator. Native gauges otherwise
/// use the system accent, which can become a low-contrast grey in dark mode.
void StyleProgress(wxGauge *gauge);

/// Replace only pure-white control text with the softer dark-palette text
/// colour. Other foreground colours are left untouched.
void SoftenWhiteText(wxWindow *window);

}
