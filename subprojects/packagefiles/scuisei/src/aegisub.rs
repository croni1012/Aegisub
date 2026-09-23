// Copyright (c) 2026. SPDX-License-Identifier: MIT
// Streaming adapter for scuisei-rs e4de7913c93e2068b88db3045ccd0144b547e5b8.
// The detector, metrics and postprocessing modules are unchanged upstream code.
mod detector;
mod error;
mod postprocess;
mod simd_metrics;
mod validation;
pub use error::{SCuiseiError, SCuiseiResult};

// Upstream CLI defaults, without depending on its CLI or FFmpeg decoder.
mod cli {
    pub const DEFAULT_WINDOW_SIZE: usize = 30;
    pub const DEFAULT_SIGMA: f64 = 3.0;
    pub const DEFAULT_BASE_THRESHOLD: f64 = 0.20;
    pub const DEFAULT_MIN_HIST_DISTANCE: f64 = 0.03;
    pub const DEFAULT_MIN_SCORE: f64 = 0.0;
    pub const DEFAULT_SAD_WEIGHT: f64 = 0.70;
    pub const DEFAULT_HIST_WEIGHT: f64 = 0.30;
    pub const DEFAULT_ME_SEARCH_RADIUS: i32 = 4;
    pub const DEFAULT_ME_INTRA_THRESH: i32 = 2000;
    pub const DEFAULT_ME_INTRA_THRESH2: f64 = 90.0;
}

use std::panic::{catch_unwind, AssertUnwindSafe};

pub struct SceneDetector {
    xvid: detector::XvidDetector,
    adaptive: detector::Detector,
    previous: Vec<u8>,
    previous_hist: [u32; 16],
    previous_grid: [u32; simd_metrics::GRID_HIST_LEN],
    dimensions: (usize, usize),
    frame_count: usize,
    stats: Vec<postprocess::FrameCutStats>,
    result: Vec<u32>,
    finished: bool,
}

impl SceneDetector {
    fn new() -> Self {
        Self {
            xvid: detector::XvidDetector::new(detector::XvidDetectorConfig::default()),
            adaptive: detector::Detector::new(detector::DetectorConfig::default()),
            previous: Vec::new(), previous_hist: [0; 16],
            previous_grid: [0; simd_metrics::GRID_HIST_LEN],
            dimensions: (0, 0), frame_count: 0, stats: Vec::new(),
            result: Vec::new(), finished: false,
        }
    }

    fn push_gray(&mut self, pixels: Vec<u8>, width: usize, height: usize) {
        let same_dimensions = self.dimensions == (width, height);
        let metrics = simd_metrics::GridHistogramPlan::new(width, height)
            .accumulate_frame_metrics(same_dimensions.then_some(self.previous.as_slice()), &pixels);
        if self.frame_count != 0 {
            let record = if !same_dimensions {
                self.xvid.reset();
                self.adaptive.reset();
                postprocess::FrameCutStats {
                    frame_index: self.frame_count, is_cut: true, score: f64::INFINITY,
                    hist_distance: 1.0, grid_hist_distance: 1.0, grid_hist_median: 1.0,
                }
            } else {
                let decision = self.xvid.decide(&self.previous, &pixels, width, height);
                let hist = simd_metrics::histogram_distance_16_from_hists(
                    &self.previous_hist, &metrics.hist, pixels.len());
                let (grid, median) = simd_metrics::grid_histogram_distances_16_from_hists(
                    &self.previous_grid, &metrics.grid_hist, pixels.len());
                let score = self.adaptive.blended_score(
                    simd_metrics::normalize_sad(metrics.sad, width, height), hist);
                let (adaptive_cut, _) = self.adaptive.decide_with_threshold(score, hist);
                self.adaptive.observe(score);
                let near_cut = decision.threshold.is_finite() && decision.threshold > 0.0
                    && decision.score >= decision.threshold * 0.85;
                postprocess::FrameCutStats {
                    frame_index: self.frame_count,
                    is_cut: decision.is_cut || (near_cut && adaptive_cut),
                    score: decision.score, hist_distance: hist,
                    grid_hist_distance: grid, grid_hist_median: median,
                }
            };
            self.stats.push(record);
        }
        self.previous = pixels;
        self.previous_hist = metrics.hist;
        self.previous_grid = metrics.grid_hist;
        self.dimensions = (width, height);
        self.frame_count += 1;
    }

    fn finish(&mut self) {
        if self.finished { return; }
        if self.frame_count > 0 {
            self.result = postprocess::refine_frame_keyframes_with_config(
                &self.stats, &postprocess::PostprocessConfig::default())
                .into_iter().filter(|&n| n < self.frame_count).map(|n| n as u32).collect();
            // Frame zero is a valid scene boundary, including a one-frame video.
            self.result.push(0);
            self.result.sort_unstable();
            self.result.dedup();
        }
        self.finished = true;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn aegisub_scuisei_create() -> *mut SceneDetector {
    catch_unwind(|| Box::into_raw(Box::new(SceneDetector::new())))
        .unwrap_or(std::ptr::null_mut())
}

// A handle is exclusively owned by one C++ SceneDetector. No unwinding is
// allowed across these ABI boundaries. The caller must pass live buffers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn aegisub_scuisei_push(
    handle: *mut SceneDetector, bytes: *const u8, len: usize,
    width: u32, height: u32, stride: usize, flipped: bool,
) -> i32 {
    if handle.is_null() || bytes.is_null() || width == 0 || height == 0 { return 1; }
    let (width, height) = (width as usize, height as usize);
    let Some(row) = width.checked_mul(4) else { return 1; };
    let Some(required) = (height - 1).checked_mul(stride).and_then(|v| v.checked_add(row)) else { return 1; };
    if stride < row || len < required || required > isize::MAX as usize { return 1; }
    catch_unwind(AssertUnwindSafe(|| {
        let detector = unsafe { &mut *handle };
        if detector.finished || detector.frame_count >= i32::MAX as usize { return 1; }
        let source = unsafe { std::slice::from_raw_parts(bytes, required) };
        // Same nearest-neighbour sampling positions and bounds as upstream.
        let mut w = width.min(160);
        let mut h = (height * w / width).max(1);
        if h > 96 { h = height.min(96); w = (width * h / height).max(1); }
        let mut gray = vec![0; w * h];
        for y in 0..h {
            let sy = y * height / h;
            let sy = if flipped { height - 1 - sy } else { sy };
            for x in 0..w {
                let at = sy * stride + (x * width / w) * 4;
                // Aegisub supplies BGRX. Use full-range BT.601 luma, ignoring X.
                gray[y * w + x] = ((29 * u32::from(source[at])
                    + 150 * u32::from(source[at + 1])
                    + 77 * u32::from(source[at + 2]) + 128) >> 8) as u8;
            }
        }
        detector.push_gray(gray, w, h);
        0
    })).unwrap_or(2)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aegisub_scuisei_finish(
    handle: *mut SceneDetector, output: *mut *const u32, count: *mut usize,
) -> i32 {
    if handle.is_null() || output.is_null() || count.is_null() { return 1; }
    catch_unwind(AssertUnwindSafe(|| {
        let detector = unsafe { &mut *handle };
        detector.finish();
        unsafe { *output = detector.result.as_ptr(); *count = detector.result.len(); }
        0
    })).unwrap_or(2)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aegisub_scuisei_destroy(handle: *mut SceneDetector) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(handle)); }));
    }
}

#[cfg(test)]
mod adapter_tests {
    use super::*;
    #[test]
    fn static_clip_has_only_first_frame() {
        let mut detector = SceneDetector::new();
        for _ in 0..80 { detector.push_gray(vec![127; 160 * 90], 160, 90); }
        detector.finish();
        assert_eq!(detector.result, [0]);
    }
    #[test]
    fn single_frame_and_empty_clip() {
        let mut detector = SceneDetector::new(); detector.finish();
        assert!(detector.result.is_empty());
        let mut detector = SceneDetector::new();
        detector.push_gray(vec![0; 16], 4, 4); detector.finish();
        assert_eq!(detector.result, [0]);
    }
    #[test]
    fn hard_cut_is_frame_aligned() {
        let mut detector = SceneDetector::new();
        for n in 0..100 { detector.push_gray(vec![if n < 50 { 0 } else { 255 }; 160 * 90], 160, 90); }
        detector.finish();
        assert_eq!(detector.result, [0, 50]);
    }
}
