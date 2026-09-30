// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/audio/onset.h>
#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace agi {
std::optional<int> FindAudioOnset(AudioProvider const& provider, int start_ms, int end_ms,
    double threshold_db, int minimum_sound_ms, std::function<bool()> const& cancelled) {
    const int rate = provider.GetSampleRate();
    if (rate <= 0 || rate > 768000 || provider.GetChannels() != 1 ||
        provider.GetBytesPerSample() != 2 || provider.AreSamplesFloat())
        throw std::invalid_argument("Audio onset detection requires converted mono 16-bit PCM");
    if (!std::isfinite(threshold_db) || threshold_db < -96 || threshold_db > 0 ||
        minimum_sound_ms < 5 || minimum_sound_ms > 1000)
        throw std::invalid_argument("Invalid audio onset threshold or duration");
    if (start_ms < 0 || end_ms <= start_ms) return {};

    auto sample_at = [rate](int64_t ms) { return (ms * rate + 999) / 1000; };
    int64_t end = end_ms;
    if (sample_at(end) > provider.GetNumSamples())
        end = provider.GetNumSamples() * 1000 / rate;
    // A cache's undecoded tail is zero-filled by GetAudio; it is not silence.
    if (end <= start_ms || sample_at(end) > provider.GetDecodedSamples()) return {};

    std::vector<int16_t> buffer;
    auto energies = [&](int64_t chunk_start, int64_t chunk_end) {
        const int64_t first_sample = sample_at(chunk_start);
        buffer.resize(static_cast<size_t>(sample_at(chunk_end) - first_sample));
        provider.GetAudio(buffer.data(), first_sample, static_cast<int64_t>(buffer.size()));
        std::vector<double> result;
        result.reserve(static_cast<size_t>((chunk_end - chunk_start + 4) / 5));
        for (int64_t ms = chunk_start; ms < chunk_end; ms += 5) {
            const int64_t window_end = std::min(ms + 5, chunk_end);
            const int64_t first = sample_at(ms) - first_sample;
            const int64_t last = sample_at(window_end) - first_sample;
            if (last <= first) { result.push_back(0); continue; }
            double sum = 0, squares = 0;
            for (int64_t sample = first; sample < last; ++sample) {
                const double value = buffer[static_cast<size_t>(sample)];
                sum += value;
                squares += value * value;
            }
            const double mean = sum / (last - first);
            result.push_back(std::max(0.0, squares / (last - first) - mean * mean));
        }
        return result;
    };
    auto decibels = [](std::vector<double> values) {
        for (auto& value : values)
            value = 10 * std::log10(std::max(1e-12, value / (32768.0 * 32768.0)));
        std::sort(values.begin(), values.end());
        return values;
    };
    auto percentile = [](std::vector<double> const& sorted, double fraction) {
        const double index = (sorted.size() - 1) * fraction;
        const size_t first = static_cast<size_t>(index);
        const size_t last = std::min(first + 1, sorted.size() - 1);
        return sorted[first] + (sorted[last] - sorted[first]) * (index - first);
    };

    // Keep the complete initial background window inside the search interval.
    if (end - start_ms < 60 || (cancelled && cancelled())) return {};
    const auto initial = decibels(energies(start_ms, start_ms + 60));
    const double floor_db = percentile(initial, 0.5);
    const auto preceding = decibels(energies(std::max(0, start_ms - 300), start_ms));
    // A weak voice may have started before the subtitle and filled the old
    // 120 ms lookbehind. Compare against the quieter part of a longer history
    // before mistaking that voice for background and seeking a louder syllable.
    if (floor_db >= threshold_db - 12 && preceding.size() >= 12 &&
        floor_db > percentile(preceding, 0.2) + 6) return {};
    constexpr double noise_margin_db = 6;
    if (floor_db >= threshold_db - noise_margin_db) {
        // Only adapt to a quiet, stable background already present before the
        // subtitle. Loud/variable sound or a new attack may already be speech
        // (or combat effects); do not search for a later, louder syllable.
        if (preceding.size() < 12 || floor_db > -32 ||
            percentile(initial, 0.8) - percentile(initial, 0.2) > 6) return {};
        if (std::abs(floor_db - percentile(preceding, 0.5)) > 4) return {};
        threshold_db = std::max(threshold_db, floor_db + noise_margin_db);
    }

    const double threshold_squared = std::pow(10.0, threshold_db / 10.0) * 32768.0 * 32768.0;
    // Confirm with the stronger threshold, but retain a short, connected quiet
    // consonant before it. The weaker threshold still needs background contrast.
    const double weak_db = std::max(threshold_db - 8, floor_db + 4);
    const double weak_squared = std::pow(10.0, weak_db / 10.0) * 32768.0 * 32768.0;
    int64_t weak_start = -1, strong_start = -1;
    int64_t weak_ms = 0, strong_ms = 0, weak_gap = 0, strong_gap = 0;
    for (int64_t chunk_start = start_ms; chunk_start < end;) {
        if (cancelled && cancelled()) return {};
        const int64_t chunk_end = std::min(end, chunk_start + 200);
        const auto windows = energies(chunk_start, chunk_end);
        for (size_t index = 0; index < windows.size(); ++index) {
            const int64_t ms = chunk_start + static_cast<int64_t>(index) * 5;
            const int64_t window_end = std::min(ms + 5, chunk_end);
            const int64_t duration = window_end - ms;
            const double energy = windows[index];
            if (energy >= weak_squared) {
                if (weak_start < 0) weak_start = ms;
                weak_ms += duration;
                weak_gap = 0;
            }
            else if (weak_start >= 0) {
                weak_gap += duration;
                if (weak_gap > 10) {
                    // A meaningful earlier sound did not obtain a strong
                    // confirmation. Do not discard it and jump to a later word.
                    if (weak_ms >= std::max(40, minimum_sound_ms)) return {};
                    weak_start = -1;
                    weak_ms = 0;
                }
            }
            if (energy >= threshold_squared) {
                if (strong_start < 0) strong_start = ms;
                strong_ms += duration;
                strong_gap = 0;
                const int64_t span = window_end - strong_start;
                // Brief dips are normal in a voice. Require actual strong
                // support and 75% occupancy so sparse clicks cannot accumulate.
                if (strong_ms >= minimum_sound_ms && strong_ms * 4 >= span * 3) {
                    if (weak_start >= 0 && strong_start - weak_start > 120) return {};
                    return static_cast<int>(weak_start >= 0 ? weak_start : strong_start);
                }
                if (span > minimum_sound_ms * 2) return {};
            }
            else if (strong_start >= 0) {
                strong_gap += duration;
                if (strong_gap > 10) {
                    strong_start = -1;
                    strong_ms = 0;
                }
            }
        }
        chunk_start = chunk_end;
    }
    return {};
}
}
