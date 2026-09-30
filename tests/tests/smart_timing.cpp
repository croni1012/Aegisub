// Exercise the embedded smart timing algorithm with Aegisub's actual CFR/VFR
// conversions. GUI/undo integration is exercised through the built-in command.
#include <gtest/gtest.h>
#include <libaegisub/lua/script_reader.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/vfr.h>
#include <memory>
#include <vector>

namespace {
class SmartTiming : public testing::Test {
protected:
    std::unique_ptr<lua_State, decltype(&lua_close)> state{luaL_newstate(), lua_close};
    agi::vfr::Framerate rate = agi::vfr::Framerate(25.0);

    void Run(const char *code) {
        ASSERT_EQ(0, luaL_dostring(state.get(), code))
            << agi::lua::get_string_or_default(state.get(), -1);
    }

    void SetUp() override {
        ASSERT_NE(nullptr, state);
        auto L = state.get();
        luaL_openlibs(L);
        ASSERT_NO_FATAL_FAILURE(Run(R"lua(
            keys = {0}
            commits = 0
            options = {}
            results = {}
            aegisub = {
                register_macro = function(name, help, run, validate)
                    default_process, can_process = run, validate
                    process = function(subs, selected, active) return run(subs, selected, active, options) end
                end,
                keyframes = function() return keys end,
                _audio_timing_info = function() return audio_identity end,
                _audio_onset = function(first, last, threshold, duration)
                    audio_queries = (audio_queries or 0) + 1
                    if audio_detector then return audio_detector(first, last, threshold, duration) end
                    for _, onset in ipairs(audio_onsets or {}) do
                        if onset >= first and onset + duration <= last then return onset end
                    end
                end,
                cancel = function() error('cancelled', 0) end,
                log = function() error('Unexpected summary output') end,
                set_undo_point = function() commits = commits + 1 end,
                progress = {
                    title = function() end,
                    set = function() end,
                    is_cancelled = function() return cancel_progress or false end
                },
                _timing_result = function(modified)
                    results[#results + 1] = string.format('Lines with corrected timing: %d', modified)
                end,
                dialog = { display = function() error('Unexpected settings dialog') end }
            }
            function line(start_time, end_time, text)
                return {class='dialogue', comment=false, layer=0, style='Default',
                    start_time=start_time, end_time=end_time, text=text or 'Hello',
                    extra={unrelated='preserve me'}}
            end
            function ass(...)
                return {{class='style', name='Default', align=2}, ...}
            end
            function named(start_time, end_time, actor)
                local row = line(start_time, end_time)
                row.actor = actor
                row.effect = 'preserve effect'
                return row
            end
            function times(row, first, last)
                assert(row.start_time == first, row.start_time .. ' ~= ' .. first)
                assert(row.end_time == last, row.end_time .. ' ~= ' .. last)
                assert(row.extra.unrelated == 'preserve me')
            end
        )lua"));
        lua_getglobal(L, "aegisub");
        lua_pushlightuserdata(L, &rate);
        lua_pushcclosure(L, [](lua_State *L) {
            auto rate = static_cast<agi::vfr::Framerate *>(lua_touserdata(L, lua_upvalueindex(1)));
            lua_pushinteger(L, rate->FrameAtTime(luaL_checkinteger(L, 1), agi::vfr::START));
            return 1;
        }, 1);
        lua_setfield(L, -2, "frame_from_ms");
        lua_pushlightuserdata(L, &rate);
        lua_pushcclosure(L, [](lua_State *L) {
            auto rate = static_cast<agi::vfr::Framerate *>(lua_touserdata(L, lua_upvalueindex(1)));
            lua_pushinteger(L, rate->TimeAtFrame(luaL_checkinteger(L, 1), agi::vfr::START));
            return 1;
        }, 1);
        lua_setfield(L, -2, "ms_from_frame");
        lua_pop(L, 1);
        ASSERT_TRUE(agi::lua::LoadFile(L, SMART_TIMING_TEST_SCRIPT_PATH));
        ASSERT_EQ(0, lua_pcall(L, 0, 0, 0)) << agi::lua::get_string_or_default(L, -1);
    }
};

TEST_F(SmartTiming, LinksShortGapAndIsIdempotent) {
    Run(R"lua(
        local subs = ass(line(1000,2000), line(2300,4000), line(5000,6000))
        local sel = {2,3}
        for i=1,5 do
            local result, active = process(subs, sel, 2)
            times(subs[2],1000,2240)
            times(subs[3],2240,4000)
            times(subs[4],5000,6000)
            assert(result == sel and active == 2)
        end
        assert(subs[2].extra['dynamo.smart_timing'])
        assert(commits == 5)
    )lua");
}

TEST_F(SmartTiming, SnapsOnlyWithinFrameAndMillisecondLimits) {
    Run(R"lua(
        keys = {0,25,50}
        local subs = ass(line(1030,2070))
        process(subs,{2},2)
        times(subs[2],980,1980)
        local outside = ass(line(1040,2130))
        process(outside,{2},2)
        times(outside[2],1040,2130)
    )lua");
}

TEST_F(SmartTiming, Includes800MillisecondContinuityGapButExcludes810) {
    Run(R"lua(
        local subs = ass(line(1000,2000),line(2800,4200))
        for i=1,3 do
            process(subs,{2,3},2)
            times(subs[2],1000,2640)
            times(subs[3],2640,4200)
        end
        local outside = ass(line(1000,2000),line(2810,4200))
        process(outside,{2,3},2)
        times(outside[2],1000,2000)
        times(outside[3],2810,4200)
    )lua");
}

TEST_F(SmartTiming, SnapsNewlyLinkedBoundaryToNextKeyframe) {
    Run(R"lua(
        keys = {0,64}
        local subs = ass(line(1000,2000),line(2500,4000))
        for i=1,3 do
            process(subs,{2,3},2)
            times(subs[2],1000,2540)
            times(subs[3],2540,4000)
        end
    )lua");
}

TEST_F(SmartTiming, MeasuresLinkedEndExtensionFromOriginalEnd) {
    Run(R"lua(
        keys = {0,70}
        local subs = ass(line(1000,2000),line(2700,4000))
        process(subs,{2,3},2)
        -- The cut is 220 ms from the new boundary but 780 ms from the old end.
        times(subs[2],1000,2560)
        times(subs[3],2560,4000)
    )lua");
}

TEST_F(SmartTiming, PreservesFollowingStartDelayAndDurationLimits) {
    Run(R"lua(
        keys = {0,67}
        local late = ass(line(1000,2000),line(2500,4000))
        process(late,{2,3},2)
        times(late[2],1000,2400)
        times(late[3],2400,4000)
        keys = {0,64}
        local short = ass(line(1000,2000),line(2500,3100))
        process(short,{2,3},2)
        times(short[2],1000,2400)
        times(short[3],2400,3100)
    )lua");
}

TEST_F(SmartTiming, KeepsCrossedKeyframeGuardAt100Milliseconds) {
    rate = agi::vfr::Framerate(100.0);
    Run(R"lua(
        keys = {0,192,254}
        local close = ass(line(1000,2000))
        process(close,{2},2)
        times(close[2],1000,2000)
        keys = {0,189,254}
        local outside = ass(line(1000,2000))
        process(outside,{2},2)
        times(outside[2],1000,2540)
    )lua");
}

TEST_F(SmartTiming, PreservesUnselectedTouchingNeighbor) {
    Run(R"lua(
        keys = {0,50}
        local subs = ass(line(1000,2000),line(2000,4000))
        process(subs,{2},2)
        times(subs[2],1000,2000)
        times(subs[3],2000,4000)
        assert(not subs[3].extra['dynamo.smart_timing'])
    )lua");
}

TEST_F(SmartTiming, ExtendsEnd542MillisecondsToNextKeyframe) {
    std::vector<int> timecodes{0};
    for (int i = 1; i < 100; ++i) timecodes.push_back(i * 40 + 2);
    rate = agi::vfr::Framerate(std::move(timecodes));
    Run(R"lua(
        keys = {0,40,64}
        assert(aegisub.ms_from_frame(64) - 2000 == 542)
        -- A previous cut 418 ms earlier must not block the forward extension.
        local subs = ass(line(1000,2000),line(2800,4000))
        for i=1,5 do
            process(subs,{2},2)
            times(subs[2],1000,2540)
            times(subs[3],2800,4000)
            assert(aegisub.frame_from_ms(subs[2].end_time) == 64)
        end
        assert(not subs[3].extra['dynamo.smart_timing'])
    )lua");
}

TEST_F(SmartTiming, Includes750MillisecondsButExcludes751) {
    for (int offset : {10, 11}) {
        std::vector<int> timecodes{0};
        for (int i = 1; i < 100; ++i) timecodes.push_back(i * 40 + offset);
        rate = agi::vfr::Framerate(std::move(timecodes));
        Run(R"lua(
            keys = {0,69}
            local cut = aegisub.ms_from_frame(69)
            assert(cut == 2750 or cut == 2751)
            local subs = ass(line(1000,2000))
            process(subs,{2},2)
            times(subs[2],1000,cut == 2750 and 2750 or 2000)
        )lua");
    }
}

TEST_F(SmartTiming, HonorsForwardLimitChangesAndDisabling) {
    Run(R"lua(
        keys = {0,64}
        local subs = ass(line(1000,2000))
        options.end_forward_max_ms = 539
        process(subs,{2},2)
        times(subs[2],1000,2000)
        options.end_forward_max_ms = 540
        process(subs,{2},2)
        times(subs[2],1000,2540)
        options.end_forward_max_ms = 0
        process(subs,{2},2)
        times(subs[2],1000,2000)
    )lua");
}

TEST_F(SmartTiming, BackwardSnapTakesPriorityOverForwardExtension) {
    Run(R"lua(
        keys = {0,50,64}
        local subs = ass(line(1000,2070))
        process(subs,{2},2)
        times(subs[2],1000,1980)
    )lua");
}

TEST_F(SmartTiming, PreservesEndAlreadyOnKeyframe) {
    Run(R"lua(
        keys = {0,49,50,64}
        for _, finish in ipairs({1980,2000}) do
            local subs = ass(line(1000,finish))
            process(subs,{2},2)
            times(subs[2],1000,finish)
        end
    )lua");
}

TEST_F(SmartTiming, DoesNotExtendAcrossUnselectedNeighbor) {
    Run(R"lua(
        keys = {0,64}
        local subs = ass(line(1000,2000),line(2400,4000))
        process(subs,{2},2)
        times(subs[2],1000,2000)
        times(subs[3],2400,4000)
        assert(not subs[3].extra['dynamo.smart_timing'])
    )lua");
}

TEST_F(SmartTiming, SkipsCommentsSignsAndComplexLines) {
    Run(R"lua(
        keys = {0,25,50}
        local subs = ass(line(1030,2070),line(1030,2070,'{\\an7}Sign'),
            line(1030,2070,'{\\pos(100,100)}Text'),line(1030,2070,'{\\k20}Song'),
            line(5000,6000))
        subs[2].comment = true
        process(subs,{2,3,4,5,6},6)
        for i=2,5 do times(subs[i],1030,2070) end
        times(subs[6],5000,6000)
        assert(not can_process(subs,{2}))
    )lua");
}

TEST_F(SmartTiming, FixesSmallButPreservesLargeOverlaps) {
    Run(R"lua(
        local small = ass(line(1000,2200),line(2100,3500))
        process(small,{2,3},2)
        times(small[2],1000,2150)
        times(small[3],2150,3500)
        local large = ass(line(1000,2600),line(2200,4000))
        process(large,{2,3},2)
        times(large[2],1000,2600)
        times(large[3],2200,4000)
    )lua");
}

TEST_F(SmartTiming, PreservesShortLineDuration) {
    Run(R"lua(
        keys = {0,32}
        local subs = ass(line(1000,1370))
        process(subs,{2},2)
        times(subs[2],1000,1370)
    )lua");
}

TEST_F(SmartTiming, RecomputesFromOriginalAfterKeyframesChange) {
    Run(R"lua(
        keys = {0,25,50}
        local subs = ass(line(1030,2070))
        process(subs,{2},2)
        times(subs[2],980,1980)
        keys = {0,26,50}
        process(subs,{2},2)
        times(subs[2],1020,1980)
    )lua");
}

TEST_F(SmartTiming, CancelsBeforeWritingOrCreatingUndoPoint) {
    Run(R"lua(
        local subs = ass(line(1000,2000),line(2300,4000))
        cancel_progress = true
        assert(not pcall(process,subs,{2,3},2))
        times(subs[2],1000,2000)
        times(subs[3],2300,4000)
        assert(commits == 0)
        assert(#results == 0)
    )lua");
}

TEST_F(SmartTiming, MenuUsesFreshDefaultsAndReportsOnlyChangedTimings) {
    Run(R"lua(
        local subs = ass(line(1000,2000),line(2800,4000))
        options.link_gap_ms = 0
        process(subs,{2,3},2)
        times(subs[2],1000,2000)
        assert(#results == 1 and results[1] == 'Lines with corrected timing: 0')
        default_process(subs,{2,3},2)
        times(subs[2],1000,2640)
        times(subs[3],2640,4000)
        assert(#results == 2 and results[2] == 'Lines with corrected timing: 2')
        default_process(subs,{2,3},2)
        assert(#results == 3 and results[3] == 'Lines with corrected timing: 0')
    )lua");
}

TEST_F(SmartTiming, MissingKeyframesAreSilentAndSkippedLinesReportZero) {
    Run(R"lua(
        local subs = ass(line(1000,2000))
        keys = {}
        default_process(subs,{2},2)
        times(subs[2],1000,2000)
        assert(#results == 0)
        keys = {0}
        subs[2].comment = true
        default_process(subs,{2},2)
        times(subs[2],1000,2000)
        assert(commits == 0)
        assert(#results == 1 and results[1] == 'Lines with corrected timing: 0')
    )lua");
}

TEST_F(SmartTiming, UsesLoadedVariableFrameRateTimecodes) {
    std::vector<int> times{0};
    for (int i = 1; i < 200; ++i) times.push_back(times.back() + (i % 2 ? 30 : 50));
    rate = agi::vfr::Framerate(std::move(times));
    Run(R"lua(
        keys = {0,25,50}
        local start = aegisub.ms_from_frame(25)
        local finish = aegisub.ms_from_frame(50)
        local subs = ass(line(start+20,finish+40))
        process(subs,{2},2)
        times(subs[2],math.floor((start+5)/10)*10,math.floor((finish+5)/10)*10)
    )lua");
}

TEST_F(SmartTiming, CancelsAfterCalculatingBeforeWriting) {
    Run(R"lua(
        keys = {0,64}
        local subs = ass(line(1000,2000))
        local checks = 0
        aegisub.progress.is_cancelled = function()
            checks = checks + 1
            return checks >= 2
        end
        assert(not pcall(process,subs,{2},2))
        times(subs[2],1000,2000)
        assert(not subs[2].extra['dynamo.smart_timing'])
        assert(commits == 0)
        assert(#results == 0)
    )lua");
}

TEST_F(SmartTiming, PreservesSmallOverlapBetweenNamedSpeakers) {
    Run(R"lua(
        local subs = ass(named(1000,2200,'Anna'),named(2100,3500,'Andras'))
        for i=1,3 do
            process(subs,{2,3},2)
            times(subs[2],1000,2200)
            times(subs[3],2100,3500)
        end
        assert(subs[2].actor == 'Anna' and subs[3].actor == 'Andras')
        assert(subs[2].effect == 'preserve effect' and subs[3].effect == 'preserve effect')
    )lua");
}

TEST_F(SmartTiming, ReusesPrimaryLaneForNextSpeakerWhileAnotherContinues) {
    Run(R"lua(
        local subs = ass(named(1000,3000,'Anna'),named(2000,5000,'Andras'),named(3500,4500,'Eva'))
        for i=1,3 do
            process(subs,{2,3,4},2)
            times(subs[2],1000,3400)
            times(subs[3],2000,5000)
            times(subs[4],3400,5000)
        end
    )lua");
}

TEST_F(SmartTiming, PreservesThreeSimultaneousSpeakersWithUnsortedRows) {
    Run(R"lua(
        local subs = ass(named(2000,6000,'Cecil'),named(1000,3000,'Anna'),
            named(3500,4800,'Dora'),named(1500,5000,'Andras'))
        for i=1,3 do
            process(subs,{2,3,4,5},3)
            times(subs[2],2000,6000)
            times(subs[3],1000,3400)
            times(subs[4],3400,5000)
            times(subs[5],1500,5000)
        end
    )lua");
}

TEST_F(SmartTiming, ReusesOverlapLaneWhilePrimarySpeakerContinues) {
    Run(R"lua(
        local subs = ass(named(1000,6000,'Anna'),named(1500,3000,'Andras'),named(3500,5000,'Cecil'))
        process(subs,{2,3,4},2)
        times(subs[2],1000,6000)
        times(subs[3],1500,3400)
        times(subs[4],3400,5000)
    )lua");
}

TEST_F(SmartTiming, KeepsLegacyOverlapRepairForSameOrUnknownSpeaker) {
    Run(R"lua(
        for _, actor in ipairs({'Anna',' Anna ','','   '}) do
            local subs = ass(named(1000,2200,'Anna'),named(2100,3500,actor))
            process(subs,{2,3},2)
            times(subs[2],1000,2150)
            times(subs[3],2150,3500)
            assert(subs[3].actor == actor)
        end
    )lua");
}

TEST_F(SmartTiming, KeyframeTrimPreservesIntentionalSpeakerOverlap) {
    Run(R"lua(
        keys = {0,54}
        local subs = ass(named(1000,2200,'Anna'),named(2100,3500,'Andras'))
        process(subs,{2,3},2)
        times(subs[2],1000,2200)
        times(subs[3],2100,3500)
    )lua");
}

TEST_F(SmartTiming, SharedKeyframeDoesNotDelayOverlappingSpeaker) {
    Run(R"lua(
        keys = {0,64}
        local subs = ass(named(1000,2000,'Anna'),named(1500,4500,'Beth'),named(2500,3500,'Cecil'))
        process(subs,{2,3,4},2)
        times(subs[2],1000,2400)
        times(subs[3],1500,4500)
        times(subs[4],2400,3500)
    )lua");
}

TEST_F(SmartTiming, LinksSelectedLaneAroundUnselectedSpeaker) {
    Run(R"lua(
        local subs = ass(named(1000,3000,'Anna'),named(2000,5000,'Andras'),named(3500,4500,'Eva'))
        process(subs,{2,4},2)
        times(subs[2],1000,3400)
        times(subs[3],2000,5000)
        times(subs[4],3400,4500)
        assert(not subs[3].extra['dynamo.smart_timing'])
        for i=1,3 do
            process(subs,{2},2)
            times(subs[2],1000,3400)
            times(subs[3],2000,5000)
            times(subs[4],3400,4500)
        end
    )lua");
}

TEST_F(SmartTiming, RecomputesOriginalOverlapAfterActorChanges) {
    Run(R"lua(
        local subs = ass(named(1000,2200,'Anna'),named(2100,3500,'Anna'))
        process(subs,{2,3},2)
        times(subs[2],1000,2150)
        times(subs[3],2150,3500)
        subs[3].actor = 'Andras'
        process(subs,{2,3},2)
        times(subs[2],1000,2200)
        times(subs[3],2100,3500)
        subs[3].actor = 'Anna'
        process(subs,{2,3},2)
        times(subs[2],1000,2150)
        times(subs[3],2150,3500)
    )lua");
}

TEST_F(SmartTiming, LinksConsecutiveDifferentSpeakersWithoutOverlap) {
    Run(R"lua(
        local subs = ass(named(1000,2000,'Anna'),named(2300,4000,'Andras'))
        process(subs,{2,3},2)
        times(subs[2],1000,2240)
        times(subs[3],2240,4000)
    )lua");
}

TEST_F(SmartTiming, SynchronizesOverlappingEndsWithin800Milliseconds) {
    Run(R"lua(
        for _, difference in ipairs({0,500,800,810}) do
            local subs = ass(named(1000,3000,'Anna'),named(2000,3000+difference,'Andras'))
            for i=1,3 do
                process(subs,{2,3},2)
                times(subs[2],1000,difference <= 800 and 3000+difference or 3000)
                times(subs[3],2000,3000+difference)
            end
        end
    )lua");
}

TEST_F(SmartTiming, SnapsSynchronizedEndsToForwardKeyframe) {
    Run(R"lua(
        keys = {0,93}
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'))
        for i=1,3 do
            process(subs,{2,3},2)
            times(subs[2],1000,3700)
            times(subs[3],2000,3700)
        end
    )lua");
}

TEST_F(SmartTiming, SnapsSynchronizedEndsBackToKeyframe) {
    Run(R"lua(
        keys = {0,87}
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'))
        process(subs,{2,3},2)
        times(subs[2],1000,3460)
        times(subs[3],2000,3460)
    )lua");
}

TEST_F(SmartTiming, SynchronizesThreeEndsWithoutChainingBeyondLimit) {
    Run(R"lua(
        local together = ass(named(1000,3000,'Anna'),named(1500,3500,'Andras'),named(2000,3800,'Cecil'))
        process(together,{2,3,4},2)
        times(together[2],1000,3800)
        times(together[3],1500,3800)
        times(together[4],2000,3800)
        local chain = ass(named(1000,3000,'Anna'),named(1500,3700,'Andras'),named(2000,4400,'Cecil'))
        process(chain,{2,3,4},2)
        times(chain[2],1000,3700)
        times(chain[3],1500,3700)
        times(chain[4],2000,4400)
    )lua");
}

TEST_F(SmartTiming, SynchronizedEndsPreserveNextSubtitleAndItsOverlap) {
    Run(R"lua(
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'),named(3200,5000,'Eva'))
        process(subs,{2,3,4},2)
        times(subs[2],1000,3160)
        times(subs[3],2000,3500)
        times(subs[4],3160,5000)
    )lua");
}

TEST_F(SmartTiming, SynchronizedEndsMoveLinkedNextStartSafely) {
    Run(R"lua(
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'),named(3600,5000,'Eva'))
        for i=1,3 do
            process(subs,{2,3,4},2)
            times(subs[2],1000,3500)
            times(subs[3],2000,3500)
            times(subs[4],3500,5000)
        end
    )lua");
}

TEST_F(SmartTiming, SynchronizedEndsHonorForwardLimitAndUnselectedRows) {
    Run(R"lua(
        keys = {0,103}
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'))
        process(subs,{2,3},2)
        times(subs[2],1000,3500)
        times(subs[3],2000,3500)
        keys = {0}
        local partial = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'))
        process(partial,{2},2)
        times(partial[2],1000,3000)
        times(partial[3],2000,3500)
        assert(not partial[3].extra['dynamo.smart_timing'])
    )lua");
}

TEST_F(SmartTiming, SynchronizedEndsDoNotSkipInterveningKeyframe) {
    Run(R"lua(
        keys = {0,83,93}
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'))
        process(subs,{2,3},2)
        times(subs[2],1000,3300)
        times(subs[3],2000,3700)
    )lua");
}

TEST_F(SmartTiming, EndSynchronizationUsesContinuitySetting) {
    Run(R"lua(
        local subs = ass(named(1000,3000,'Anna'),named(2000,3500,'Andras'))
        options.link_gap_ms = 400
        process(subs,{2,3},2)
        times(subs[2],1000,3000)
        times(subs[3],2000,3500)
        options.link_gap_ms = 500
        process(subs,{2,3},2)
        times(subs[2],1000,3500)
        times(subs[3],2000,3500)
        options.link_gap_ms = 0
        process(subs,{2,3},2)
        times(subs[2],1000,3000)
        times(subs[3],2000,3500)
    )lua");
}

TEST_F(SmartTiming, AudioMovesOnlyStartRightWith100MillisecondLead) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {1400}
        local subs = ass(line(1000,3000))
        for i=1,3 do
            process(subs,{2},2)
            times(subs[2],1300,3000)
        end
    )lua");
}

TEST_F(SmartTiming, AudioDoesNotMoveLeftOrMoveKeyframeStart) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {1020}
        local early = ass(line(1000,3000))
        process(early,{2},2)
        times(early[2],1000,3000)
        keys = {0,25}
        audio_onsets = {1400}
        local keyframe = ass(line(980,3000)) -- Canonical frame-25 START time.
        process(keyframe,{2},2)
        times(keyframe[2],980,3000)
        assert(audio_queries == 1) -- The keyframe-aligned line is never analyzed.
    )lua");
}

TEST_F(SmartTiming, AudioLeadCannotLandBeforeSceneCut) {
    Run(R"lua(
        audio_identity = 'audio'
        keys = {0,30}
        audio_onsets = {1220}
        local subs = ass(line(1000,3000))
        process(subs,{2},2)
        times(subs[2],1000,3000)
        audio_onsets = {1280}
        process(subs,{2},2)
        times(subs[2],1180,3000)
    )lua");
}

TEST_F(SmartTiming, AudioKeepsSynchronizedPreviousEndsTogether) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {3400}
        local subs = ass(named(1000,3000,'Anna'),named(1800,3000,'Andras'),named(3000,5000,'Kata'))
        for i=1,3 do
            process(subs,{2,3,4},4)
            times(subs[2],1000,3300)
            times(subs[3],1800,3300)
            times(subs[4],3300,5000)
        end
    )lua");
}

TEST_F(SmartTiming, AudioMovesContinuousSelectedPreviousEnd) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {2400}
        local subs = ass(line(1000,2000),line(2000,4000))
        for i=1,3 do
            process(subs,{2,3},3)
            times(subs[2],1000,2300)
            times(subs[3],2300,4000)
        end
    )lua");
}

TEST_F(SmartTiming, AudioPreservesUnselectedPreviousAndNoncontinuousEnds) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {2400}
        local touching = ass(line(1000,2000),line(2000,4000))
        process(touching,{3},3)
        times(touching[2],1000,2000)
        times(touching[3],2000,4000)
        options.link_gap_ms = 0
        local gap = ass(line(1000,1900),line(2000,4000))
        process(gap,{2,3},3)
        times(gap[2],1000,1900)
        times(gap[3],2300,4000)
    )lua");
}

TEST_F(SmartTiming, AudioAdjustsOnlyFirstOverlappingSpeaker) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {1400,2000,2400}
        options.link_gap_ms = 0
        local subs = ass(named(1000,3500,'Anna'),named(1800,4000,'Andras'),named(2200,5000,'Cecil'))
        process(subs,{2,3,4},2)
        times(subs[2],1300,3500)
        times(subs[3],1800,4000)
        times(subs[4],2200,5000)
        assert(audio_queries == 1)
    )lua");
}

TEST_F(SmartTiming, AudioDoesNotSearchIntoNextSpeakerOrCutReadingTime) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {1600}
        options.link_gap_ms = 0
        local overlap = ass(named(1000,3000,'Anna'),named(1500,4000,'Andras'))
        process(overlap,{2,3},2)
        times(overlap[2],1000,3000)
        times(overlap[3],1500,4000)
        audio_onsets = {1900}
        local short = ass(line(1000,2200))
        process(short,{2},2)
        times(short[2],1000,2200)
    )lua");
}

TEST_F(SmartTiming, AudioPreservesContinuousSharedEndsOnKeyframes) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {2400}
        keys = {0,50}
        local subs = ass(named(1000,1980,'Anna'),named(1500,1980,'Andras'),named(1980,4000,'Kata'))
        process(subs,{2,3,4},4)
        times(subs[2],1000,1980)
        times(subs[3],1500,1980)
        times(subs[4],1980,4000)
        assert(audio_queries == 1) -- Only the first, non-keyframe start is examined.
    )lua");
}

TEST_F(SmartTiming, AudioCanBeDisabledAndCancelledWithoutWriting) {
    Run(R"lua(
        audio_identity = 'audio'
        audio_onsets = {1400}
        options.audio_align = false
        local subs = ass(line(1000,3000))
        process(subs,{2},2)
        times(subs[2],1000,3000)
        options.audio_align = true
        audio_detector = function() cancel_progress = true; return 1400 end
        local old_commits = commits
        assert(not pcall(process,subs,{2},2))
        times(subs[2],1000,3000)
        assert(commits == old_commits)
    )lua");
}
}
