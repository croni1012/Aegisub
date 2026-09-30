// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#include <gtest/gtest.h>
#include <libaegisub/audio/onset.h>
#include <libaegisub/audio/provider.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
class OnsetAudio : public agi::AudioProvider {
public:
    std::vector<int16_t> samples;
    explicit OnsetAudio(int rate = 48000) {
        channels = 1;
        sample_rate = rate;
        bytes_per_sample = 2;
        num_samples = rate * 2;
        decoded_samples = num_samples;
        samples.resize(static_cast<size_t>(num_samples));
    }
    void Sound(int first_ms, int last_ms, int amplitude = 4000) {
        for (int64_t i = static_cast<int64_t>(first_ms) * sample_rate / 1000;
             i < static_cast<int64_t>(last_ms) * sample_rate / 1000; ++i)
            samples[static_cast<size_t>(i)] = static_cast<int16_t>(i % 2 ? amplitude : -amplitude);
    }
    void Incomplete() { decoded_samples = sample_rate / 2; }
    void Unsupported() { float_samples = true; }
    void FillBuffer(void *buf, int64_t start, int64_t count) const override {
        auto out = static_cast<int16_t *>(buf);
        for (int64_t i = 0; i < count; ++i) out[i] = samples[static_cast<size_t>(start + i)];
    }
};
}

TEST(AudioOnset, FindsSustainedSoundAtDifferentSampleRates) {
    for (int rate : {32000,44100,48000,96000}) {
        OnsetAudio audio(rate);
        audio.Sound(450,1000);
        EXPECT_EQ(450, agi::FindAudioOnset(audio, 100, 1500, -40, 40));
    }
}

TEST(AudioOnset, RejectsSilenceQuietNoiseAndShortClicks) {
    OnsetAudio audio;
    audio.Sound(0,2000,100);
    audio.Sound(100,105,20000);
    audio.Sound(250,275,10000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -40, 40));
    audio.Sound(600,900);
    EXPECT_EQ(600, agi::FindAudioOnset(audio, 0, 1000, -40, 40));
}

TEST(AudioOnset, SustainedSoundCrossesReadChunkBoundary) {
    OnsetAudio audio;
    audio.Sound(190,300);
    EXPECT_EQ(190, agi::FindAudioOnset(audio, 0, 1000, -40, 40));
}

TEST(AudioOnset, LeavesAlreadyActiveLoudSoundUnchanged) {
    OnsetAudio audio;
    audio.Sound(0,1000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 250, 1500, -40, 40));
}

TEST(AudioOnset, FindsRiseAboveStableBackgroundExceedingAbsoluteThreshold) {
    OnsetAudio audio;
    audio.Sound(0,2000,500); // -36.3 dBFS background, already above -40.
    audio.Sound(500,800,2000);
    EXPECT_EQ(500, agi::FindAudioOnset(audio, 200, 1000, -40, 40));
    EXPECT_FALSE(agi::FindAudioOnset(audio, 200, 450, -40, 40));
}

TEST(AudioOnset, StableBackgroundStillRequiresSustainedContrast) {
    OnsetAudio audio;
    audio.Sound(0,2000,500);
    audio.Sound(300,500,750); // A small fluctuation is not a new onset.
    audio.Sound(600,625,4000); // Neither is a short click.
    EXPECT_FALSE(agi::FindAudioOnset(audio, 200, 1000, -40, 40));
    audio.Sound(800,1000,1200);
    EXPECT_EQ(800, agi::FindAudioOnset(audio, 200, 1100, -40, 40));
}

TEST(AudioOnset, RejectsLoudAndUnstableInitialBackground) {
    OnsetAudio audio;
    audio.Sound(0,2000,2000);
    audio.Sound(500,800,8000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 200, 1000, -40, 40));
    audio.Sound(0,2000,500);
    for (int ms = 200; ms < 260; ms += 10) audio.Sound(ms, ms+5, 1500);
    audio.Sound(500,800,4000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 200, 1000, -40, 40));
}

TEST(AudioOnset, DoesNotMistakeAnInitialAttackForBackground) {
    OnsetAudio audio;
    audio.Sound(200,500,500);
    audio.Sound(500,1000,4000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 200, 1000, -40, 40));
    EXPECT_FALSE(agi::FindAudioOnset(audio, 200, 250, -40, 10));
    audio.Sound(0,2000,500);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -40, 40));
}

TEST(AudioOnset, HonorsThresholdAndConfirmationDuration) {
    OnsetAudio audio;
    audio.Sound(500,530,500);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -40, 40));
    EXPECT_EQ(500, agi::FindAudioOnset(audio, 0, 1000, -40, 20));
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -30, 20));
}

TEST(AudioOnset, DetectsSpeechAlreadyFillingTheShortLookbehind) {
    OnsetAudio audio;
    audio.Sound(0,2000,100);
    audio.Sound(380,850,500);
    audio.Sound(900,1600,4000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 500, 1600, -40, 40));
}

TEST(AudioOnset, RetainsAConnectedQuietConsonant) {
    for (int rate : {32000,44100,48000,96000}) {
        OnsetAudio audio(rate);
        audio.Sound(400,450,200);
        audio.Sound(450,1000);
        EXPECT_EQ(400, agi::FindAudioOnset(audio, 100, 1500, -40, 40));
    }
}

TEST(AudioOnset, DoesNotSkipEarlierUnconfirmedQuietSpeech) {
    OnsetAudio audio;
    audio.Sound(250,350,200);
    audio.Sound(700,1200);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 100, 1500, -40, 40));
}

TEST(AudioOnset, LeavesAnAmbiguousLongQuietPrefixAlone) {
    OnsetAudio audio;
    audio.Sound(400,600,200);
    audio.Sound(600,1000);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 100, 1500, -40, 40));
}

TEST(AudioOnset, ConfirmsModulatedVoiceAcrossBriefDips) {
    for (int rate : {44100,48000}) {
        OnsetAudio audio(rate);
        audio.Sound(390,700,600);
        for (int ms = 410; ms < 700; ms += 20) audio.Sound(ms,ms+5,250);
        EXPECT_EQ(390, agi::FindAudioOnset(audio, 200, 1000, -40, 40));
    }
}

TEST(AudioOnset, SparseImpulsesCannotAccumulateIntoSpeech) {
    OnsetAudio audio;
    for (int ms = 400; ms < 1000; ms += 15) audio.Sound(ms,ms+5);
    EXPECT_FALSE(agi::FindAudioOnset(audio, 100, 1500, -40, 40));
}

TEST(AudioOnset, RejectsDCAndIncompleteCacheData) {
    OnsetAudio audio;
    for (auto& sample : audio.samples) sample = 3000;
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -40, 40));
    audio.Sound(600,900);
    audio.Incomplete();
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -40, 40));
}

TEST(AudioOnset, ClipsAtFileEndAndHonorsSearchBounds) {
    OnsetAudio audio;
    audio.Sound(1900,2000);
    EXPECT_EQ(1900, agi::FindAudioOnset(audio, 1800, 3000, -40, 40));
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1900, -40, 40));
    EXPECT_FALSE(agi::FindAudioOnset(audio, -10, 100, -40, 40));
}

TEST(AudioOnset, CancellationAndInvalidOptions) {
    OnsetAudio audio;
    audio.Sound(600,900);
    int polls = 0;
    EXPECT_FALSE(agi::FindAudioOnset(audio, 0, 1000, -40, 40, [&] { return ++polls == 2; }));
    EXPECT_EQ(2, polls);
    EXPECT_THROW(agi::FindAudioOnset(audio, 0, 1000, -200, 40), std::invalid_argument);
    audio.Unsupported();
    EXPECT_THROW(agi::FindAudioOnset(audio, 0, 1000, -40, 40), std::invalid_argument);
}
