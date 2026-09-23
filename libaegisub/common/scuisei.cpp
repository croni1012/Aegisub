// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/scuisei.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "scuisei_licenses.h"

extern "C" {
void *aegisub_scuisei_create();
void aegisub_scuisei_destroy(void *handle);
int aegisub_scuisei_push(void *handle, unsigned char const *bytes, size_t length,
                       uint32_t width, uint32_t height, size_t stride, bool flipped);
int aegisub_scuisei_finish(void *handle, uint32_t const **frames, size_t *count);
}

namespace agi::scuisei {
Detector::Detector() : handle(aegisub_scuisei_create()) {
    if (!handle) throw std::runtime_error("Could not initialize the scuisei scene detector.");
}
Detector::~Detector() { aegisub_scuisei_destroy(handle); }

void Detector::AddFrame(std::span<unsigned char const> pixels, int width, int height, int pitch, bool flipped) {
    if (width <= 0 || height <= 0 || pitch <= 0)
        throw std::invalid_argument("Invalid video frame dimensions.");
    auto result = aegisub_scuisei_push(handle, pixels.data(), pixels.size(),
                                     width, height, static_cast<size_t>(pitch), flipped);
    if (result == 1) throw std::invalid_argument("Invalid video frame or completed scene detector.");
    if (result) throw std::runtime_error("scuisei failed while analyzing a video frame.");
}

std::vector<int> Detector::Finish() {
    uint32_t const *frames = nullptr;
    size_t count = 0;
    if (aegisub_scuisei_finish(handle, &frames, &count))
        throw std::runtime_error("scuisei failed while refining scene changes.");
    std::vector<int> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (frames[i] > static_cast<uint32_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Scene-change frame index is out of range.");
        result.push_back(static_cast<int>(frames[i]));
    }
    return result;
}

std::string_view License() { return ScuiseiLicenseText(); }
}
