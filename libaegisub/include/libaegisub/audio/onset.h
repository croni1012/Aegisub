// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <functional>
#include <optional>

namespace agi {
class AudioProvider;

/// Conservative onset candidate in [start_ms, end_ms), using 5 ms DC-corrected RMS.
/// Confirms sound above threshold_db / stable background + 6 dB, tolerating short
/// dips, then retains its connected weak prefix (up to 120 ms, background + 4 dB).
/// A 300 ms history guards already-active sound. Unconfirmed earlier activity,
/// loud/changing initial audio and sparse impulses are left alone.
/// Input must be converted mono signed 16-bit PCM. Returns no onset for silence,
/// uncertain/short intervals, incomplete cache data or cancellation.
/// This detects sound energy, not speech; sustained effects can resemble speech.
std::optional<int> FindAudioOnset(AudioProvider const& provider, int start_ms, int end_ms,
    double threshold_db, int minimum_sound_ms, std::function<bool()> const& cancelled = {});
}
