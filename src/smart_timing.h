// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <vector>

class AssDialogue;
class AssFile;
namespace agi {
class AudioProvider;
class ProgressSink;
namespace vfr { class Framerate; }
}

struct SmartTimingChange {
	AssDialogue *line;
	int start;
	int end;
};

/// Calculate from the current times of every selected row. Unselected events
/// and extradata are never consulted. No changes are applied by this function.
std::vector<SmartTimingChange> CalculateSmartTiming(
	AssFile const& file, std::vector<AssDialogue *> const& selection,
	agi::vfr::Framerate const& timecodes, std::vector<int> const& keyframes,
	agi::AudioProvider const* audio = nullptr, agi::ProgressSink *progress = nullptr);
