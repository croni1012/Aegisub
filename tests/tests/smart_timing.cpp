// Exercise the embedded Dynamo 2.1.4 algorithm with Aegisub's actual CFR/VFR
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
            aegisub = {
                register_macro = function(name, help, run, validate)
                    process, can_process = run, validate
                end,
                keyframes = function() return keys end,
                cancel = function() error('cancelled', 0) end,
                log = function() end,
                set_undo_point = function() commits = commits + 1 end,
                progress = {
                    title = function() end,
                    set = function() end,
                    is_cancelled = function() return cancel_progress or false end
                },
                dialog = { display = function(controls, buttons)
                    if cancel_settings then return false end
                    local values = {show_summary = false}
                    for _, control in ipairs(controls) do
                        if control.name then values[control.name] = control.value end
                    end
                    values.show_summary = false
                    for key, value in pairs(options) do values[key] = value end
                    return buttons[1], values
                end }
            }
            function line(start_time, end_time, text)
                return {class='dialogue', comment=false, layer=0, style='Default',
                    start_time=start_time, end_time=end_time, text=text or 'Hello',
                    extra={unrelated='preserve me'}}
            end
            function ass(...)
                return {{class='style', name='Default', align=2}, ...}
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
        cancel_settings = true
        assert(not pcall(process,subs,{2,3},2))
        cancel_settings = false
        cancel_progress = true
        assert(not pcall(process,subs,{2,3},2))
        times(subs[2],1000,2000)
        times(subs[3],2300,4000)
        assert(commits == 0)
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
}
