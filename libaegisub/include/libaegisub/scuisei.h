// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace agi::scuisei {
/// Streaming scene detection on the exact BGRX frames used by the video provider.
class Detector final {
    void *handle;
public:
    Detector();
    ~Detector();
    Detector(Detector const&) = delete;
    Detector& operator=(Detector const&) = delete;
    void AddFrame(std::span<unsigned char const> pixels, int width, int height, int pitch, bool flipped);
    std::vector<int> Finish();
};

/// Notices are embedded in the executable and displayed in the About dialog.
std::string_view License();
}
