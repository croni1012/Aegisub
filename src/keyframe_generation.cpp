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
#include <libaegisub/path.h>
#include <libaegisub/scuisei.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <wx/filename.h>
#include <wx/msgdlg.h>

bool GenerateKeyframesFromVideo(agi::Context *c) {
    auto *provider = c->project->VideoProvider();
    if (!provider || provider->GetFrameCount() <= 0) return false;
    c->videoController->Stop();
    c->audioController->Stop();

    agi::fs::path cache_file;
    try {
        auto const count = provider->GetFrameCount();
        auto const frame_message = from_wx(_("Analyzing video: %d / %d frames"));
        auto const refine_message = from_wx(_("Refining scene changes..."));
        std::vector<int> frames;
        std::exception_ptr failure;
        DialogProgress dialog(c->parent, _("Load Keyframes from Video"), _("Analyzing scene changes..."));
        // Run() calls ShowModal(): all other application windows stay disabled
        // until the worker has finished, including after the user presses Cancel.
        dialog.Run([&](agi::ProgressSink *ps) {
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
                        ps->SetMessage(agi::format(frame_message, n + 1, count));
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
        });
        if (failure) std::rethrow_exception(failure);
        if (frames.empty()) return false;

        // Keep a persistent, unique list so saved ASS projects can reopen it.
        // Do not touch the current list until analysis AND saving have succeeded.
        auto cache_dir = c->path->Decode("?user/scuisei-keyframes");
        agi::fs::CreateDirectory(cache_dir);
        auto filename = wxFileName::CreateTempFileName(to_wx((cache_dir / "scuisei-").string()));
        if (filename.empty()) throw std::runtime_error("Could not create the generated keyframe file.");
        cache_file = from_wx(filename);
        agi::keyframe::Save(cache_file, frames);
        if (c->project->LoadKeyframes(cache_file)) {
            cache_file.clear(); // This file is now part of the project's persistent state.
            return true;
        }
    }
    catch (agi::UserCancelException const&) { }
    catch (agi::Exception const& error) {
        wxMessageBox(_("Could not generate keyframes: ") + to_wx(error.GetMessage()),
                     _("Load Keyframes from Video"), wxOK | wxICON_ERROR, c->parent);
    }
    catch (std::exception const& error) {
        wxMessageBox(_("Could not generate keyframes: ") + to_wx(error.what()),
                     _("Load Keyframes from Video"), wxOK | wxICON_ERROR, c->parent);
    }
    if (!cache_file.empty()) {
        try { agi::fs::Remove(cache_file); } catch (...) { }
    }
    return false;
}
