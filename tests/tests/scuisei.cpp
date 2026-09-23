// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#include <libaegisub/scuisei.h>
#include <main.h>
#include <stdexcept>

namespace {
std::vector<unsigned char> Frame(unsigned char value, int width = 160, int height = 90, int padding = 0) {
    return std::vector<unsigned char>((width * 4 + padding) * height, value);
}
}

TEST(lagi_scuisei, StaticVideoAndEmptyVideo) {
    agi::scuisei::Detector empty;
    EXPECT_TRUE(empty.Finish().empty());
    agi::scuisei::Detector detector;
    auto frame = Frame(127);
    for (int i = 0; i < 80; ++i) detector.AddFrame(frame, 160, 90, 640, false);
    EXPECT_EQ(detector.Finish(), (std::vector<int>{0}));
    EXPECT_THROW(detector.AddFrame(frame, 160, 90, 640, false), std::invalid_argument);
}

TEST(lagi_scuisei, ExactHardCutAndPaddedFrames) {
    agi::scuisei::Detector detector;
    auto black = Frame(0, 160, 90, 16), white = Frame(255, 160, 90, 16);
    for (int i = 0; i < 100; ++i)
        detector.AddFrame(i < 50 ? black : white, 160, 90, 656, false);
    EXPECT_EQ(detector.Finish(), (std::vector<int>{0, 50}));
    EXPECT_EQ(detector.Finish(), (std::vector<int>{0, 50}));
}

TEST(lagi_scuisei, RejectsInvalidBuffersBeforeReading) {
    agi::scuisei::Detector detector;
    auto frame = Frame(0);
    EXPECT_THROW(detector.AddFrame({}, 160, 90, 640, false), std::invalid_argument);
    EXPECT_THROW(detector.AddFrame(frame, 160, 90, 639, false), std::invalid_argument);
    EXPECT_THROW(detector.AddFrame(frame, 0, 90, 640, false), std::invalid_argument);
    EXPECT_THROW(detector.AddFrame(frame, 160, -1, 640, false), std::invalid_argument);
    detector.AddFrame(frame, 160, 90, 640, false);
    EXPECT_EQ(detector.Finish(), (std::vector<int>{0}));
}

TEST(lagi_scuisei, FlippedStorageDoesNotCreateCuts) {
    agi::scuisei::Detector detector;
    auto normal = Frame(0), upside_down = Frame(0);
    for (int y = 0; y < 90; ++y)
        for (int x = 0; x < 160 * 4; ++x) {
            auto value = static_cast<unsigned char>((x / 4 + y * 3) % 256);
            normal[y * 640 + x] = value;
            upside_down[(89 - y) * 640 + x] = value;
        }
    for (int n = 0; n < 80; ++n)
        detector.AddFrame(n % 2 ? upside_down : normal, 160, 90, 640, n % 2 != 0);
    EXPECT_EQ(detector.Finish(), (std::vector<int>{0}));
}
