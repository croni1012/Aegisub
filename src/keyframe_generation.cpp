// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#include "keyframe_generation.h"

#include "async_video_provider.h"
#include "audio_controller.h"
#include "compat.h"
#include "dialog_progress.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/keyframe.h>
#include <libaegisub/scuisei.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <wx/msgdlg.h>

bool GenerateKeyframesFromVideo(agi::Context *c) {
    auto *provider = c->project->VideoProvider();
    if (!provider || provider->GetFrameCount() <= 0) return false;
    c->videoController->Stop();
    c->audioController->Stop();

    auto report_error = [&](std::string const& message) {
        wxMessageBox(_("Could not generate keyframes: ") + to_wx(message),
                     _("Load Keyframes from Video"), wxOK | wxICON_ERROR, c->parent);
    };
    try {
        auto const count = provider->GetFrameCount();
        auto const frame_message = _("Analyzing video: %d / %d frames");
        auto const refine_message = from_wx(_("Refining scene changes..."));
        std::vector<int> frames;
        std::exception_ptr failure;
        auto analyze = [&](agi::ProgressSink *ps) {
            try {
                agi::scuisei::Detector detector;
                auto last_update = std::chrono::steady_clock::now();
                ps->SetProgress(0, count + int64_t{1});
                // A single provider job avoids per-frame thread handoffs and display
                // cache copies. FFMS also converts directly to the analysis size.
                provider->ProcessFramesForAnalysis([&](int n, VideoFrame const& frame) {
                    detector.AddFrame(frame.data, frame.width, frame.height, frame.pitch, frame.flipped);
                    auto now = std::chrono::steady_clock::now();
                    if (n == 0 || n + 1 == count || now - last_update >= std::chrono::milliseconds(100)) {
                        ps->SetMessage(from_wx(agi::wxformat(frame_message, n + 1, count)));
                        ps->SetProgress(n + 1, count + int64_t{1});
                        last_update = now;
                    }
                }, [&] { return ps->IsCancelled(); });
                if (ps->IsCancelled()) return;
                ps->SetMessage(refine_message);
                auto result = detector.Finish();
                if (ps->IsCancelled()) return;
                if (result.empty() || result.front() != 0 || result.back() >= count ||
                    !std::is_sorted(result.begin(), result.end()) ||
                    std::adjacent_find(result.begin(), result.end()) != result.end())
                    throw std::runtime_error("The scene detector returned an invalid keyframe list.");
                frames = std::move(result);
                ps->SetProgress(1, 1);
            }
            catch (...) { failure = std::current_exception(); }
        };
        DialogProgress dialog(c->parent, _("Load Keyframes from Video"), _("Analyzing scene changes..."));
        dialog.Run(analyze);
        if (failure) std::rethrow_exception(failure);
        if (frames.empty()) return false;

        // Replace the video's persistent list only after analysis succeeds.
        // Do not touch the current list until analysis AND saving have succeeded.
        auto cache_file = c->project->KeyframeCachePath();
        if (cache_file.empty()) throw std::runtime_error("A video file is required to save scene keyframes.");
        agi::fs::CreateDirectory(cache_file.parent_path());
        agi::keyframe::Save(cache_file, frames);
        return c->project->LoadKeyframes(cache_file);
    }
    catch (agi::UserCancelException const&) { }
    catch (agi::Exception const& error) {
        report_error(error.GetMessage());
    }
    catch (std::exception const& error) {
        report_error(error.what());
    }
    return false;
}
