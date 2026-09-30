script_name = "Dynamo scripts/Okos időzítés"
script_description = "Konzervatív KF-igazítás és folytonos dialógushatárok."
script_author = "Dynamo"
script_version = "2.2.1-kintsugi.6"
script_namespace = "Dynamo.SmartTiming"

-- Az algoritmus mindig egy változatlan alap-időzítésből számol.
-- A saját extradata-jelölője miatt újraindítás után sem halmozódnak a módosítások.

local EXTRA_KEY = "dynamo.smart_timing"
local tr = aegisub.gettext or function(text) return text end
local AUDIO_LEAD_MS = 100
local ASS_ROUNDTRIP_TOLERANCE_MS = 9
-- Csak a már KF-en végződő előző sorhoz történő kapcsolás külön kerete.
-- Nem lazítja a szabad kezdések vagy más közös határok snap-korlátait.
local KEYFRAME_END_JOIN_FRAMES = 6
-- A végnyújtás növelése ne tágítsa vele együtt a korábbi vágás védőablakát.
local CROSSED_KEYFRAME_GUARD_MS = 100

local DEFAULTS = {
    link_gap_ms = 800,
    link_bias_percent = 80,
    start_snap_back_frames = 3,
    start_snap_max_ms = 50,
    end_snap_back_frames = 3,
    end_forward_max_ms = 750,
    overlap_fix_frames = 3,
    link_snap_frames = 3,
    link_keyframe_gaps = true,
    min_duration_ms = 600,
    max_cps = 25,
    only_bottom_dialogue = true,
    skip_complex = true,
    audio_align = true,
    audio_threshold_db = -40,
    audio_min_ms = 40
}

local function copy_table(source)
    local result = {}
    if type(source) == "table" then
        for key, value in pairs(source) do result[key] = value end
    end
    return result
end

local function round(value)
    return math.floor(value + 0.5)
end

local function canonical_ass_time(value)
    return math.max(0, math.floor((value + 5) / 10) * 10)
end

local function clamp(value, minimum, maximum)
    if value < minimum then return minimum end
    if value > maximum then return maximum end
    return value
end

local function normalize_settings(settings)
    settings.link_gap_ms = clamp(round(tonumber(settings.link_gap_ms) or DEFAULTS.link_gap_ms), 0, 2000)
    settings.link_bias_percent = clamp(round(tonumber(settings.link_bias_percent) or DEFAULTS.link_bias_percent), 0, 100)
    settings.start_snap_back_frames = clamp(round(tonumber(settings.start_snap_back_frames) or DEFAULTS.start_snap_back_frames), 0, 30)
    settings.start_snap_max_ms = clamp(round(tonumber(settings.start_snap_max_ms) or DEFAULTS.start_snap_max_ms), 0, 1000)
    settings.end_snap_back_frames = clamp(round(tonumber(settings.end_snap_back_frames) or DEFAULTS.end_snap_back_frames), 0, 30)
    settings.end_forward_max_ms = clamp(round(tonumber(settings.end_forward_max_ms) or DEFAULTS.end_forward_max_ms), 0, 2000)
    settings.overlap_fix_frames = clamp(round(tonumber(settings.overlap_fix_frames) or DEFAULTS.overlap_fix_frames), 0, 30)
    settings.link_snap_frames = clamp(round(tonumber(settings.link_snap_frames) or DEFAULTS.link_snap_frames), 0, 30)
    settings.min_duration_ms = clamp(round(tonumber(settings.min_duration_ms) or DEFAULTS.min_duration_ms), 100, 10000)
    settings.max_cps = clamp(round(tonumber(settings.max_cps) or DEFAULTS.max_cps), 0, 100)
    settings.only_bottom_dialogue = not not settings.only_bottom_dialogue
    settings.skip_complex = not not settings.skip_complex
    settings.link_keyframe_gaps = not not settings.link_keyframe_gaps
    settings.audio_align = not not settings.audio_align
    settings.audio_threshold_db = clamp(round(tonumber(settings.audio_threshold_db) or DEFAULTS.audio_threshold_db), -80, -10)
    settings.audio_min_ms = clamp(round(tonumber(settings.audio_min_ms) or DEFAULTS.audio_min_ms), 10, 500)
    return settings
end

local function settings_signature(settings)
    return table.concat({
        "v" .. script_version,
        settings.link_gap_ms,
        settings.link_bias_percent,
        settings.start_snap_back_frames,
        settings.start_snap_max_ms,
        settings.end_snap_back_frames,
        settings.end_forward_max_ms,
        settings.overlap_fix_frames,
        settings.link_snap_frames,
        settings.link_keyframe_gaps and 1 or 0,
        settings.min_duration_ms,
        settings.max_cps,
        settings.only_bottom_dialogue and 1 or 0,
        settings.skip_complex and 1 or 0,
        settings.audio_align and 1 or 0,
        settings.audio_threshold_db,
        settings.audio_min_ms
    }, ",")
end

local function keyframe_signature(keyframes)
    local hash = 0
    for index, frame in ipairs(keyframes) do
        local time = aegisub.ms_from_frame(frame) or -1
        hash = (hash * 65599 + frame * 31 + time + index) % 2147483647
    end
    return string.format("kf:%d:%d", #keyframes, hash)
end

local function lower_bound(sorted_values, value)
    local low, high = 1, #sorted_values + 1
    while low < high do
        local middle = math.floor((low + high) / 2)
        if sorted_values[middle] ~= nil and sorted_values[middle] < value then
            low = middle + 1
        else
            high = middle
        end
    end
    return low
end

local function for_keyframes_in_ms_range(keyframes, first_ms, last_ms, callback)
    if #keyframes == 0 or first_ms > last_ms then return end
    local first_frame = aegisub.frame_from_ms(math.max(0, first_ms))
    local last_frame = aegisub.frame_from_ms(math.max(0, last_ms))
    if first_frame == nil or last_frame == nil then return end

    local position = lower_bound(keyframes, first_frame - 2)
    while position <= #keyframes do
        local keyframe = keyframes[position]
        if keyframe > last_frame + 2 then break end
        local keyframe_ms = aegisub.ms_from_frame(keyframe)
        if keyframe_ms ~= nil and keyframe_ms >= first_ms and keyframe_ms <= last_ms then
            callback(keyframe, keyframe_ms)
        end
        position = position + 1
    end
end

local function nearest_keyframe_in_range(keyframes, first_ms, last_ms, target_ms, predicate)
    local best_frame, best_time, best_distance = nil, nil, nil
    for_keyframes_in_ms_range(keyframes, first_ms, last_ms, function(frame, time)
        if predicate == nil or predicate(frame, time) then
            local distance = math.abs(time - target_ms)
            if best_distance == nil or distance < best_distance or
               (distance == best_distance and time < best_time) then
                best_frame, best_time, best_distance = frame, time, distance
            end
        end
    end)
    return best_frame, best_time
end

local function frame_window_ms(time_ms, frames)
    if frames <= 0 then return time_ms, time_ms end
    local frame = aegisub.frame_from_ms(math.max(0, time_ms))
    if frame == nil then return time_ms, time_ms end
    local first = aegisub.ms_from_frame(math.max(0, frame - frames - 1)) or time_ms
    local last = aegisub.ms_from_frame(frame + frames + 1) or time_ms
    return math.min(first, time_ms), math.max(last, time_ms)
end

local function end_bleed_frames(end_time, keyframe)
    if end_time <= 0 then return 0 end
    -- frame_from_ms az Aegisub START-konvencióját használja: endnél ez az
    -- első már nem látható frame, ezért a különbség közvetlenül a bleed.
    local end_frame = aegisub.frame_from_ms(end_time)
    if end_frame == nil then return 0 end
    return end_frame - keyframe
end

local function visible_text(text)
    local clean = text or ""
    clean = clean:gsub("{[^}]*}", "")
    clean = clean:gsub("\\[Nn]", " ")
    clean = clean:gsub("\\h", " ")
    clean = clean:gsub("^%s+", ""):gsub("%s+$", "")
    return clean
end

local function visible_character_count(text)
    local clean = visible_text(text):gsub("[%s%p]", "")
    local _, count = clean:gsub("[^\128-\191]", "")
    return count
end

local function explicit_alignment(text)
    local alignment = nil
    for block in (text or ""):gmatch("{([^}]*)}") do
        for value in block:gmatch("\\an([1-9])") do
            alignment = tonumber(value)
        end
    end
    return alignment
end

local function effective_alignment(line, style_alignments)
    local explicit = explicit_alignment(line.text)
    if explicit ~= nil then return explicit end
    if line.styleref and line.styleref.align then
        return tonumber(line.styleref.align) or 2
    end
    return tonumber(style_alignments[line.style]) or 2
end

local function is_complex_line(text)
    for block in (text or ""):gmatch("{([^}]*)}") do
        if block:match("\\[kK][fot]?%d") then return true end
        if block:match("\\p[1-9]%d*") then return true end
        if block:match("\\move%s*%(") then return true end
        if block:match("\\pos%s*%(") then return true end
        if block:match("\\org%s*%(") then return true end
        if block:match("\\i?clip%s*%(") then return true end
        if block:match("\\t%s*%(") then return true end
        if block:match("\\fad[e]?%s*%(") then return true end
    end
    return false
end

local function parse_marker(line)
    if type(line.extra) ~= "table" then return nil end
    local value = line.extra[EXTRA_KEY]
    if type(value) ~= "string" then return nil end
    local original_start, original_end, applied_start, applied_end, signature =
        value:match("^2|(-?%d+)|(-?%d+)|(-?%d+)|(-?%d+)|([^|]*)$")
    if original_start == nil then
        original_start, original_end, applied_start, applied_end =
            value:match("^2|(-?%d+)|(-?%d+)|(-?%d+)|(-?%d+)$")
        signature = ""
    end
    if original_start == nil then return nil end
    return tonumber(original_start), tonumber(original_end), tonumber(applied_start), tonumber(applied_end), signature
end

local function timing_state(line, is_selected)
    local current_start = round(tonumber(line.start_time) or 0)
    local current_end = round(tonumber(line.end_time) or current_start)
    local original_start, original_end, applied_start, applied_end, marker_signature = parse_marker(line)
    local marker_matches_current = original_start ~= nil and
        math.abs(current_start - applied_start) <= ASS_ROUNDTRIP_TOLERANCE_MS and
        math.abs(current_end - applied_end) <= ASS_ROUNDTRIP_TOLERANCE_MS

    local context_start = marker_matches_current and original_start or current_start
    local context_end = marker_matches_current and original_end or current_end

    if is_selected and marker_matches_current then
        -- A contextus-signature ellenőrzése csak a szomszédok felépítése után történhet.
        return original_start, original_end, original_start, original_end,
            true, marker_signature, context_start, context_end, current_start, current_end
    end
    -- Nem kijelölt sornál mindig a tényleges, képernyőn lévő idő az akadály.
    return current_start, current_end, current_start, current_end,
        marker_matches_current, marker_signature, context_start, context_end, current_start, current_end
end

local function encode_marker(original_start, original_end, applied_start, applied_end, signature)
    return string.format("2|%d|%d|%d|%d|%s", original_start, original_end, applied_start, applied_end, signature)
end

local function required_duration(entry, settings)
    local requested = settings.min_duration_ms
    if settings.max_cps > 0 and entry.character_count > 0 then
        requested = math.max(requested, math.ceil(entry.character_count * 1000 / settings.max_cps))
    end
    -- Egy már eleve rövidebb sort nem tiltunk le, csak tovább nem rövidítjük.
    return math.min(requested, math.max(1, entry.original_end - entry.original_start))
end

local function preserves_speaker_end(entry, end_time, synchronized)
    if entry.speaker_overlap_end == nil or end_time >= entry.speaker_overlap_end then return true end
    if synchronized == nil then return false end
    for _, other in ipairs(entry.speaker_overlaps or {}) do
        if not synchronized[other] and
           end_time < math.min(entry.context_original_end, other.context_original_end) then return false end
    end
    return true
end

local function safe_boundary(previous, following, boundary, settings, synchronized)
    if boundary <= previous.start_time or boundary >= following.end_time then return false end
    if boundary - previous.start_time < required_duration(previous, settings) then return false end
    if following.end_time - boundary < required_duration(following, settings) then return false end
    local saved_boundary = canonical_ass_time(boundary)
    if not preserves_speaker_end(previous, saved_boundary, synchronized) then return false end
    if following.speaker_overlap_start and saved_boundary > following.speaker_overlap_start then return false end
    return true
end

local function same_spatial_lane(previous, following)
    return (previous.line.layer or 0) == (following.line.layer or 0) and
           previous.alignment == following.alignment
end

local function same_positive_link_lane(previous, following)
    return same_spatial_lane(previous, following) and
        tostring(previous.line.style or "") == tostring(following.line.style or "")
end

local function stable_hash(value)
    local hash = 0
    local text = tostring(value or "")
    for index = 1, #text do
        hash = (hash * 257 + text:byte(index) + index) % 2147483647
    end
    return hash
end

local function record_identity(record)
    if record == nil then return "-" end
    return string.format(
        "%d:%d:%d:%d:%d:%d:%d:%d",
        stable_hash(record.line.text),
        stable_hash(record.line.style),
        tonumber(record.line.layer) or 0,
        tonumber(record.alignment) or 2,
        tonumber(record.context_original_start) or 0,
        tonumber(record.context_original_end) or 0,
        stable_hash(record.actor),
        record.speech_lane or 1
    )
end

local function record_context_signature(record, current_signature)
    return table.concat({
        current_signature,
        record.speaker_context,
        record_identity(record.previous_speech),
        record_identity(record),
        record_identity(record.next_speech)
    }, ",")
end

local function build_style_alignments(subtitles)
    local styles = {}
    for index = 1, #subtitles do
        local line = subtitles[index]
        if line.class == "style" then
            styles[line.name] = tonumber(line.align) or 2
        end
    end
    return styles
end

local function assign_speech_lanes(speech)
    local groups = {}
    for position, record in ipairs(speech) do
        local group_key = tostring(record.line.layer or 0) .. ":" .. tostring(record.alignment)
        local group = groups[group_key]
        if group == nil then
            group = {active = {}, lanes = {}, records = {}, signature = 0}
            groups[group_key] = group
        end

        -- A marker eredeti idejeivel a kijelöléstől és az előző igazítástól
        -- függetlenül ugyanazokat a párhuzamos beszédsávokat építjük fel.
        local active, blocked, lane = {}, {}, nil
        for _, other in ipairs(group.active) do
            if other.context_original_end > record.context_original_start then
                active[#active + 1] = other
                local first = math.max(record.context_original_start, other.context_original_start)
                local last = math.min(record.context_original_end, other.context_original_end)
                if first < last and record.actor ~= "" and other.actor ~= "" then
                    if record.actor == other.actor then
                        -- Egy szereplő kis időzítési átfedése továbbra is javítható.
                        lane = lane or other.speech_lane
                    else
                        blocked[other.speech_lane] = true
                        -- A szándékos átfedés teljes eredeti szakaszát megőrizzük:
                        -- közös határ vagy KF-igazítás sem vághatja le.
                        record.speaker_overlap_start = math.min(record.speaker_overlap_start or first, first)
                        record.speaker_overlap_end = math.max(record.speaker_overlap_end or last, last)
                        other.speaker_overlap_start = math.min(other.speaker_overlap_start or first, first)
                        other.speaker_overlap_end = math.max(other.speaker_overlap_end or last, last)
                        record.speaker_overlaps = record.speaker_overlaps or {}
                        other.speaker_overlaps = other.speaker_overlaps or {}
                        record.speaker_overlaps[#record.speaker_overlaps + 1] = other
                        other.speaker_overlaps[#other.speaker_overlaps + 1] = record
                    end
                end
            end
        end

        -- Az első felszabaduló sávot bármelyik következő szereplő használhatja.
        -- Üres névből nem következtetünk külön beszélőre: marad a hagyományos javítás.
        if lane == nil then
            lane = 1
            while blocked[lane] do lane = lane + 1 end
        end
        record.speech_lane = lane
        record.speech_position = position
        record.previous_speech = group.lanes[lane]
        if record.previous_speech then record.previous_speech.next_speech = record end
        group.lanes[lane] = record
        active[#active + 1] = record
        group.active = active
        group.records[#group.records + 1] = record
        group.signature = stable_hash(tostring(group.signature) .. ":" .. record_identity(record))
    end

    -- A többi sáv szereplői is befolyásolják a besorolást és az átfedésvédelmet.
    -- Átnevezés vagy módosított szomszéd esetén a mentett eredményt újraszámoljuk.
    for _, group in pairs(groups) do
        for _, record in ipairs(group.records) do
            record.speaker_context = tostring(group.signature)
        end
    end
end

local function collect_speech(subtitles, selected_lines, settings, current_signature)
    local selected = {}
    for _, index in ipairs(selected_lines) do selected[index] = true end

    local styles = build_style_alignments(subtitles)
    local speech = {}
    local entries = {}
    local skipped = {comments = 0, alignment = 0, complex = 0, non_dialogue = 0}

    for index = 1, #subtitles do
        local line = subtitles[index]
        if line.class == "dialogue" and not line.comment then
            local alignment = effective_alignment(line, styles)
            local accepted_alignment = not settings.only_bottom_dialogue or alignment == 2
            local accepted_complexity = not settings.skip_complex or not is_complex_line(line.text)

            if accepted_alignment then
                local record_selected = selected[index] == true and accepted_complexity
                local original_start, original_end, initial_start, initial_end,
                    marker_valid, marker_signature, context_start, context_end,
                    current_start, current_end = timing_state(line, record_selected)
                local record = {
                    index = index,
                    line = line,
                    selected = record_selected,
                    modifiable = accepted_complexity,
                    alignment = alignment,
                    actor = tostring(line.actor or ""):match("^%s*(.-)%s*$"),
                    original_start = original_start,
                    original_end = original_end,
                    start_time = initial_start,
                    end_time = initial_end,
                    current_start = current_start,
                    current_end = current_end,
                    context_original_start = context_start,
                    context_original_end = context_end,
                    character_count = visible_character_count(line.text),
                    marker_valid = marker_valid,
                    marker_signature = marker_signature,
                    linked_previous = false,
                    linked_next = false,
                    protected_overlap = false
                }
                speech[#speech + 1] = record
                if record.selected then entries[#entries + 1] = record end
                if selected[index] and not accepted_complexity then
                    skipped.complex = skipped.complex + 1
                end
            elseif selected[index] then
                skipped.alignment = skipped.alignment + 1
            end
        elseif selected[index] then
            if line.class == "dialogue" and line.comment then
                skipped.comments = skipped.comments + 1
            else
                skipped.non_dialogue = skipped.non_dialogue + 1
            end
        end
    end

    table.sort(speech, function(left, right)
        if left.context_original_start ~= right.context_original_start then
            return left.context_original_start < right.context_original_start
        end
        if left.context_original_end ~= right.context_original_end then
            return left.context_original_end < right.context_original_end
        end
        return left.index < right.index
    end)

    assign_speech_lanes(speech)

    for _, record in ipairs(speech) do
        record.context_signature = record_context_signature(record, current_signature)
        if record.selected and record.marker_valid and
           record.marker_signature == record.context_signature then
            record.start_time = record.current_start
            record.end_time = record.current_end
        end
    end

    -- Kijelölési szélen egy már érintkező/átfedő, nem kijelölt szomszéd
    -- időpontját nem mozdíthatjuk: különben rés vagy új átfedés keletkezne.
    for _, record in ipairs(speech) do
        if record.selected then
            local previous = record.previous_speech
            local following = record.next_speech
            record.edge_lock_start = previous ~= nil and not previous.selected and
                same_spatial_lane(previous, record) and previous.end_time >= record.start_time
            record.edge_lock_end = following ~= nil and not following.selected and
                same_spatial_lane(record, following) and following.start_time <= record.end_time

            -- Verzió/KF-lista váltásakor is a ténylegesen érintkező kijelölési
            -- szélt őrizzük meg, ne a markerből visszaállított rést hozzuk vissza.
            if record.marker_valid then
                if previous ~= nil and not previous.selected and same_spatial_lane(previous, record) and
                   previous.end_time == record.current_start then
                    record.start_time = record.current_start
                    record.edge_lock_start = true
                end
                if following ~= nil and not following.selected and same_spatial_lane(record, following) and
                   following.start_time == record.current_end then
                    record.end_time = record.current_end
                    record.edge_lock_end = true
                end
            end
        end
    end

    table.sort(entries, function(left, right)
        return left.speech_position < right.speech_position
    end)

    return entries, skipped, speech
end

local function find_start_keyframe(entry, settings, keyframes)
    if settings.start_snap_back_frames <= 0 then return nil end
    local first_ms = frame_window_ms(entry.original_start, settings.start_snap_back_frames)
    local original_frame = aegisub.frame_from_ms(entry.original_start)
    if original_frame == nil then return nil end

    local _, time = nearest_keyframe_in_range(
        keyframes,
        first_ms,
        entry.original_start,
        entry.original_start,
        function(frame, keyframe_ms)
            -- A hatframe-es összekötési korlátot az egyedi kezdésigazítás sem
            -- kerülheti meg, még csak a következő sor újrafuttatásakor sem.
            local previous = entry.previous_speech
            local too_far_join = previous ~= nil and same_positive_link_lane(previous, entry) and
                previous.original_end < entry.original_start and
                aegisub.frame_from_ms(previous.original_end) == frame and
                original_frame - frame > KEYFRAME_END_JOIN_FRAMES
            return not too_far_join and keyframe_ms <= entry.original_start and
                   entry.original_start - keyframe_ms <= settings.start_snap_max_ms and
                   original_frame - frame >= 0 and
                   original_frame - frame <= settings.start_snap_back_frames
        end
    )
    return time
end

local function find_back_end_keyframe(entry, settings, keyframes)
    if settings.end_snap_back_frames <= 0 then return nil, nil end
    local first_ms = frame_window_ms(entry.original_end, settings.end_snap_back_frames + 1)
    local frame, time = nearest_keyframe_in_range(
        keyframes,
        first_ms,
        entry.original_end,
        entry.original_end,
        function(keyframe, keyframe_ms)
            if keyframe_ms > entry.original_end then return false end
            local bleed = end_bleed_frames(entry.original_end, keyframe)
            return bleed >= 1 and bleed <= settings.end_snap_back_frames
        end
    )
    if frame == nil then return nil, nil end
    return time, end_bleed_frames(entry.original_end, frame)
end

local function find_crossed_keyframe(entry, settings, keyframes)
    if settings.end_forward_max_ms <= 0 then return nil end
    local _, time = nearest_keyframe_in_range(
        keyframes,
        math.max(0, entry.original_end - math.min(settings.end_forward_max_ms, CROSSED_KEYFRAME_GUARD_MS)),
        entry.original_end,
        entry.original_end
    )
    return time
end

local function find_forward_end_keyframe(entry, settings, keyframes)
    if settings.end_forward_max_ms <= 0 then return nil end
    local boundary_frame = aegisub.frame_from_ms(entry.original_end)
    if boundary_frame == nil or keyframes[lower_bound(keyframes, boundary_frame)] == boundary_frame then return nil end
    local frame, time = nearest_keyframe_in_range(
        keyframes,
        entry.original_end,
        entry.original_end + settings.end_forward_max_ms,
        entry.original_end
    )
    if frame == nil then return nil end
    local saved_time = canonical_ass_time(time)
    if saved_time <= entry.original_end or saved_time - entry.original_end > settings.end_forward_max_ms or
       aegisub.frame_from_ms(saved_time) ~= frame then return nil end
    return saved_time
end

local function keyframe_between(keyframes, first_ms, last_ms, target_ms)
    local frame, time = nearest_keyframe_in_range(
        keyframes, first_ms, last_ms, target_ms or last_ms
    )
    return frame, time
end

local function shared_back_keyframe(previous, following, settings, keyframes)
    if settings.link_snap_frames <= 0 or settings.end_snap_back_frames <= 0 then return nil end
    local boundary = previous.original_end
    local first_ms = frame_window_ms(boundary, math.max(settings.link_snap_frames, settings.end_snap_back_frames) + 1)
    local _, time = nearest_keyframe_in_range(
        keyframes,
        first_ms,
        boundary,
        boundary,
        function(frame, keyframe_ms)
            local bleed = end_bleed_frames(boundary, frame)
            local boundary_frame = aegisub.frame_from_ms(boundary)
            return keyframe_ms <= following.original_start and
                   following.original_start - keyframe_ms <= settings.start_snap_max_ms and
                   bleed >= 1 and bleed <= settings.end_snap_back_frames and
                   boundary_frame ~= nil and boundary_frame - frame <= settings.link_snap_frames
        end
    )
    return time
end

local function shared_forward_keyframe(previous, following, settings, keyframes)
    if settings.link_snap_frames <= 0 or settings.end_forward_max_ms <= 0 then return nil end
    local boundary = previous.end_time
    local boundary_frame = aegisub.frame_from_ms(boundary)
    if boundary_frame == nil then return nil end

    -- Az ASS-re kerekített idő már lehet a KF-en akkor is, ha néhány ms-mal
    -- korábbi, mint ms_from_frame(KF). Ilyenkor nem keresünk újabb vágást.
    if keyframes[lower_bound(keyframes, boundary_frame)] == boundary_frame then return nil end
    local frame, time = nearest_keyframe_in_range(
        keyframes, boundary + 1, previous.original_end + settings.end_forward_max_ms, boundary
    )
    if frame == nil then return nil end
    local following_frame = aegisub.frame_from_ms(following.original_start)
    -- A rés kitöltése nem beszédlevágás. Az eredeti következő kezdéshez mérjük
    -- a késést, nem a rés 80%-ánál frissen létrehozott közös határhoz.
    if frame <= boundary_frame or following_frame == nil or
       frame - following_frame > settings.link_snap_frames then return nil end

    -- A ténylegesen mentett, centiszekundumos határral vizsgáljuk a védelmeket.
    local saved_time = canonical_ass_time(time)
    if saved_time <= boundary or saved_time - previous.original_end > settings.end_forward_max_ms or
       aegisub.frame_from_ms(saved_time) ~= frame then return nil end
    return saved_time
end

local function original_end_on_keyframe(entry, keyframes)
    local frame = aegisub.frame_from_ms(entry.original_end)
    if frame == nil or keyframes[lower_bound(keyframes, frame)] ~= frame then return nil end
    local saved_time = canonical_ass_time(entry.original_end)
    if aegisub.frame_from_ms(saved_time) ~= frame then return nil end
    return frame, saved_time
end

local function can_join_keyframe_end(previous, following, frame, boundary, settings)
    if not settings.link_keyframe_gaps then return false end
    local next_frame = aegisub.frame_from_ms(following.original_start)
    if next_frame == nil then return false end
    local early_ms = following.original_start - boundary
    local early_frames = next_frame - frame
    return early_ms >= 0 and early_frames >= 0 and
        early_frames <= KEYFRAME_END_JOIN_FRAMES and
        safe_boundary(previous, following, boundary, settings)
end

local function link_pairs(entries, settings, keyframes, stats)
    for _, previous in ipairs(entries) do
        local following = previous.next_speech
        if following ~= nil and following.selected and same_spatial_lane(previous, following) then
            local gap = following.original_start - previous.original_end

            if gap == 0 then
                local boundary = previous.original_end
                local keyframe_boundary = shared_back_keyframe(previous, following, settings, keyframes)
                if keyframe_boundary ~= nil and safe_boundary(previous, following, keyframe_boundary, settings) then
                    boundary = keyframe_boundary
                    stats.shared_keyframe = stats.shared_keyframe + 1
                else
                    stats.shared_preserved = stats.shared_preserved + 1
                end

                previous.end_time = boundary
                following.start_time = boundary
                previous.linked_next = true
                following.linked_previous = true

            elseif gap < 0 then
                local previous_end_frame = aegisub.frame_from_ms(previous.original_end)
                local following_start_frame = aegisub.frame_from_ms(following.original_start)
                local overlap_frames = nil
                if previous_end_frame ~= nil and following_start_frame ~= nil then
                    overlap_frames = previous_end_frame - following_start_frame
                end

                if overlap_frames ~= nil and overlap_frames >= 1 and
                   overlap_frames <= settings.overlap_fix_frames then
                    local boundary = round((previous.original_end + following.original_start) / 2)
                    local frame, keyframe_time = keyframe_between(
                        keyframes,
                        math.max(0, following.original_start - 1),
                        previous.original_end,
                        boundary
                    )
                    local boundary_frame = aegisub.frame_from_ms(boundary)
                    if settings.link_snap_frames > 0 and frame ~= nil and
                       boundary_frame ~= nil and
                       math.abs(frame - boundary_frame) <= settings.link_snap_frames and
                       keyframe_time <= following.original_start and
                       following.original_start - keyframe_time <= settings.start_snap_max_ms and
                       end_bleed_frames(previous.original_end, frame) <= settings.end_snap_back_frames then
                        boundary = keyframe_time
                    end

                    if safe_boundary(previous, following, boundary, settings) then
                        previous.end_time = boundary
                        following.start_time = boundary
                        previous.linked_next = true
                        following.linked_previous = true
                        stats.overlap_fixed = stats.overlap_fixed + 1
                    else
                        previous.protected_overlap = true
                        following.protected_overlap = true
                        stats.overlap_protected = stats.overlap_protected + 1
                    end
                else
                    previous.protected_overlap = true
                    following.protected_overlap = true
                    stats.overlap_protected = stats.overlap_protected + 1
                end

            else
                local linkable = same_positive_link_lane(previous, following) and
                    gap <= settings.link_gap_ms

                if linkable then
                    local preferred_boundary = round(
                        previous.original_end + gap * settings.link_bias_percent / 100
                    )
                    local end_keyframe, end_boundary = original_end_on_keyframe(previous, keyframes)
                    local _, scene_cut = keyframe_between(
                        keyframes,
                        previous.original_end,
                        following.original_start,
                        preferred_boundary
                    )
                    if end_keyframe ~= nil then
                        -- A KF időpontját az ASS 10 ms-ra kerekítheti. Frame alapján
                        -- felismerjük a már vágáson levő véget, akkor is, ha a nyers
                        -- ms_from_frame(KF) pár ms-mal a keresett rés elé esik.
                        previous.end_time = end_boundary
                        previous.protected_keyframe_end = true
                        following.start_time = following.original_start
                        if can_join_keyframe_end(previous, following, end_keyframe, end_boundary, settings) then
                            following.start_time = end_boundary
                            previous.linked_next = true
                            following.linked_previous = true
                            stats.shared_keyframe = stats.shared_keyframe + 1
                        else
                            stats.keyframe_gap_preserved = stats.keyframe_gap_preserved + 1
                        end
                    elseif scene_cut == nil then
                        local boundary = preferred_boundary
                        if safe_boundary(previous, following, boundary, settings) then
                            previous.end_time = boundary
                            following.start_time = boundary
                            previous.linked_next = true
                            following.linked_previous = true
                            stats.continuity_linked = stats.continuity_linked + 1
                        end
                    else
                        -- Közeli párnál a jelenetváltást nem hidaljuk át: mindkét
                        -- feliratoldal ugyanarra a KF-re kerül, így folytonos marad.
                        if settings.link_keyframe_gaps and
                           safe_boundary(previous, following, scene_cut, settings) then
                            previous.end_time = scene_cut
                            following.start_time = scene_cut
                            previous.linked_next = true
                            following.linked_previous = true
                            stats.shared_keyframe = stats.shared_keyframe + 1
                        end
                    end
                end
            end
        end
    end

    -- Az eleve érintkező ÉS a most réskitöltéssel összekötött határok ellenőrzése.
    -- A visszavágási/átfedési döntések és a már KF-en levő határok maradnak. E második
    -- körben a következő sor már a saját páros döntése utáni végével szerepel,
    -- így két szomszédos igazítás együtt sem kerüli meg a minimum/CPS-védelmet.
    for _, previous in ipairs(entries) do
        local following = previous.next_speech
        if following ~= nil and following.selected and same_spatial_lane(previous, following) and
           previous.original_end <= following.original_start and
           previous.linked_next and following.linked_previous and
           previous.end_time >= previous.original_end and
           following.start_time == previous.end_time then
            local boundary = shared_forward_keyframe(previous, following, settings, keyframes)
            if boundary ~= nil and safe_boundary(previous, following, boundary, settings) then
                previous.end_time = boundary
                following.start_time = boundary
                if previous.original_end == following.original_start then
                    stats.shared_preserved = stats.shared_preserved - 1
                else
                    stats.continuity_linked = stats.continuity_linked - 1
                end
                stats.shared_keyframe = stats.shared_keyframe + 1
            end
        end
    end
end

local function synchronize_speaker_ends(entries, settings, keyframes, stats)
    if settings.link_gap_ms <= 0 then return end
    local ordered, assigned = {}, {}
    for _, entry in ipairs(entries) do
        if entry.actor ~= "" and entry.speaker_overlaps and not entry.protected_overlap then
            ordered[#ordered + 1] = entry
        end
    end
    table.sort(ordered, function(left, right)
        if left.original_end ~= right.original_end then return left.original_end < right.original_end end
        return left.speech_position < right.speech_position
    end)

    local function synchronize(group)
        local members, boundary = {}, 0
        for _, entry in ipairs(group) do
            members[entry] = true
            boundary = math.max(boundary, entry.end_time)
        end

        local function valid(target, keyframe)
            for _, entry in ipairs(group) do
                if math.abs(target - entry.original_end) > settings.link_gap_ms or
                   target - entry.start_time < required_duration(entry, settings) or
                   not preserves_speaker_end(entry, target, members) then return false end
                if entry.edge_lock_end and target ~= entry.end_time then return false end
                local original_frame = original_end_on_keyframe(entry, keyframes)
                if original_frame and aegisub.frame_from_ms(target) ~= original_frame then return false end
                local end_frame = aegisub.frame_from_ms(entry.original_end)
                local target_frame = aegisub.frame_from_ms(target)
                if end_frame == nil or target_frame == nil then return false end
                local next_cut = keyframes[lower_bound(keyframes, end_frame + 1)]
                -- A közös vég keresése sem ugorhat át egy korábbi jelenetváltást.
                if next_cut and next_cut < target_frame then return false end
                if target < entry.end_time then
                    -- Közös visszavágás csak valódi, közeli KF-re történhet.
                    local bleed = keyframe and end_bleed_frames(entry.original_end, keyframe) or -1
                    if bleed < 0 or bleed > settings.end_snap_back_frames then return false end
                end
                local following = entry.next_speech
                if entry.linked_next then
                    if following == nil or not following.selected or following.edge_lock_start or
                       not safe_boundary(entry, following, target, settings, members) then return false end
                elseif following and target > following.start_time then
                    return false
                end
            end
            return true
        end

        local target
        local boundary_frame = aegisub.frame_from_ms(boundary)
        if boundary_frame and keyframes[lower_bound(keyframes, boundary_frame)] == boundary_frame and
           valid(boundary, boundary_frame) then
            target = boundary
        end
        if target == nil then
            local back = find_back_end_keyframe({original_end = boundary}, settings, keyframes)
            if back then
                local saved = canonical_ass_time(back)
                local frame = aegisub.frame_from_ms(back)
                if aegisub.frame_from_ms(saved) == frame and valid(saved, frame) then target = saved end
            end
        end
        if target == nil then
            local crossed = find_crossed_keyframe({original_end = boundary}, settings, keyframes)
            if crossed == nil or crossed >= boundary then
                local forward = find_forward_end_keyframe({original_end = boundary}, settings, keyframes)
                if forward then
                    local within_forward_limit = true
                    for _, entry in ipairs(group) do
                        if forward - entry.original_end > settings.end_forward_max_ms then within_forward_limit = false end
                    end
                    if within_forward_limit and valid(forward, aegisub.frame_from_ms(forward)) then target = forward end
                end
            end
        end
        if target == nil and valid(boundary, nil) then target = boundary end
        if target == nil then return false end

        for _, entry in ipairs(group) do
            entry.end_time = target
            entry.synchronized_end = true
            assigned[entry] = true
            if entry.linked_next then entry.next_speech.start_time = target end
        end
        stats.speaker_ends_synced = stats.speaker_ends_synced + 1
        return true
    end

    for index, first in ipairs(ordered) do
        if not assigned[first] then
            local group, actors = {first}, {[first.actor] = true}
            for other_index = index + 1, #ordered do
                local other = ordered[other_index]
                -- A teljes csoport végideje férjen a keretbe, ne csak páronként
                -- láncolódjanak össze egyre távolabbi sorok.
                if other.original_end - first.original_end > settings.link_gap_ms then break end
                if not assigned[other] and not actors[other.actor] and same_spatial_lane(first, other) and
                   other.original_start < first.original_end then
                    group[#group + 1] = other
                    actors[other.actor] = true
                end
            end
            if #group > 1 then synchronize(group) end
        end
    end
end

local function adjust_individual_entries(entries, settings, keyframes, stats)
    for position, entry in ipairs(entries) do
        aegisub.progress.title(string.format("Időzítés — %d/%d", position, #entries))
        if aegisub.progress.is_cancelled() then aegisub.cancel() end

        if not entry.linked_previous and not entry.protected_overlap and not entry.edge_lock_start then
            local keyframe_start = find_start_keyframe(entry, settings, keyframes)
            if keyframe_start ~= nil and keyframe_start < entry.original_start then
                local previous = entry.previous_speech
                if previous == nil or not same_spatial_lane(previous, entry) or
                   keyframe_start >= previous.end_time then
                    entry.start_time = keyframe_start
                    stats.start_keyframe = stats.start_keyframe + 1
                end
            end
        end

        if not entry.linked_next and not entry.protected_overlap and not entry.edge_lock_end and not entry.synchronized_end and
           not entry.protected_keyframe_end and original_end_on_keyframe(entry, keyframes) == nil then
            local back_keyframe = find_back_end_keyframe(entry, settings, keyframes)
            if back_keyframe ~= nil and
               (entry.speaker_overlap_end == nil or canonical_ass_time(back_keyframe) >= entry.speaker_overlap_end) and
               back_keyframe - entry.start_time >= required_duration(entry, settings) then
                entry.end_time = back_keyframe
                stats.end_keyframe_back = stats.end_keyframe_back + 1
            else
                local crossed_keyframe = find_crossed_keyframe(entry, settings, keyframes)
                if crossed_keyframe ~= nil and crossed_keyframe < entry.original_end then
                    -- 4+ frame-mel a KF után végződő beszédet nem találgatjuk és nem nyújtjuk tovább.
                    stats.end_crossed_guard = stats.end_crossed_guard + 1
                else
                    local forward_keyframe = find_forward_end_keyframe(entry, settings, keyframes)
                    local next_speech = entry.next_speech
                    local next_start = next_speech and same_spatial_lane(entry, next_speech) and
                        next_speech.start_time or nil
                    if forward_keyframe ~= nil and
                       (next_start == nil or forward_keyframe <= next_start) then
                        entry.end_time = forward_keyframe
                        stats.end_keyframe_forward = stats.end_keyframe_forward + 1
                    end
                end
            end
        end
    end
end

local function align_starts_to_audio(speech, settings, keyframes, stats)
    if not settings.audio_align or not stats.audio_available then return end
    local ordered, ending_at = {}, {}
    for _, entry in ipairs(speech) do
        ordered[#ordered + 1] = entry
        entry.before_audio_start = canonical_ass_time(entry.start_time)
        entry.before_audio_end = canonical_ass_time(entry.end_time)
        local endings = ending_at[entry.before_audio_end] or {}
        endings[#endings + 1] = entry
        ending_at[entry.before_audio_end] = endings
    end
    table.sort(ordered, function(left, right)
        if left.before_audio_start ~= right.before_audio_start then return left.before_audio_start < right.before_audio_start end
        return left.speech_position < right.speech_position
    end)

    local active_end = -1
    for position, entry in ipairs(ordered) do
        local first_in_overlap = entry.before_audio_start >= active_end
        active_end = math.max(active_end, entry.before_audio_end)
        if aegisub.progress.is_cancelled() then aegisub.cancel() end
        aegisub.progress.title(string.format(tr("Audio onset - %d/%d"), position, #ordered))
        aegisub.progress.set(position * 100 / #ordered)

        local start = entry.before_audio_start
        local start_frame = aegisub.frame_from_ms(start)
        local on_keyframe = start_frame and keyframes[lower_bound(keyframes, start_frame)] == start_frame
        if entry.selected and first_in_overlap and not entry.edge_lock_start and not on_keyframe then
            local continuous, movable = {}, true
            -- A korábban közös végre igazított átfedő sorok együtt maradnak.
            for _, previous in ipairs(ending_at[start] or {}) do
                if same_spatial_lane(previous, entry) then
                    continuous[#continuous + 1] = previous
                    local end_frame = aegisub.frame_from_ms(previous.before_audio_end)
                    local end_on_keyframe = end_frame and keyframes[lower_bound(keyframes, end_frame)] == end_frame
                    if not previous.selected or previous.edge_lock_end or end_on_keyframe then movable = false end
                end
            end
            if movable then
                local next_entry = ordered[position + 1]
                local next_start = next_entry and next_entry.before_audio_start or entry.before_audio_end
                local latest_onset = entry.before_audio_end - required_duration(entry, settings) + AUDIO_LEAD_MS
                local search_end = math.min(entry.before_audio_end, next_start, latest_onset + settings.audio_min_ms)
                if search_end > start then
                    local onset = aegisub._audio_onset(start, search_end, settings.audio_threshold_db, settings.audio_min_ms)
                    if aegisub.progress.is_cancelled() then aegisub.cancel() end
                    if onset ~= nil then
                        local target = canonical_ass_time(onset - AUDIO_LEAD_MS)
                        local target_frame = aegisub.frame_from_ms(target)
                        local onset_frame = aegisub.frame_from_ms(onset)
                        local last_cut = onset_frame and keyframes[lower_bound(keyframes, onset_frame + 1) - 1]
                        local allowed = target > start and target < next_start and
                            entry.before_audio_end - target >= required_duration(entry, settings) and
                            target_frame ~= nil and (last_cut == nil or target_frame >= last_cut) and
                            (entry.speaker_overlap_start == nil or target <= entry.speaker_overlap_start)
                        if #continuous > 0 and allowed then
                            local next_cut = start_frame and keyframes[lower_bound(keyframes, start_frame + 1)]
                            allowed = next_cut == nil or next_cut >= target_frame
                            for _, previous in ipairs(continuous) do
                                allowed = allowed and safe_boundary(previous, entry, target, settings)
                            end
                        end
                        if allowed then
                            entry.start_time = target
                            for _, previous in ipairs(continuous) do previous.end_time = target end
                            stats.audio_starts = stats.audio_starts + 1
                        end
                    end
                end
            end
        end
    end
end

local function show_result(modified)
    aegisub._timing_result(modified)
end

local function process(subtitles, selected_lines, active_line, overrides)
    -- The menu invokes this with three arguments and always uses fresh defaults.
    local settings = copy_table(DEFAULTS)
    for key, value in pairs(overrides or {}) do settings[key] = value end
    settings = normalize_settings(settings)
    local keyframes = aegisub.keyframes() or {}

    if #keyframes == 0 or aegisub.frame_from_ms(0) == nil then
        return selected_lines, active_line
    end
    aegisub.progress.set(0)
    table.sort(keyframes, function(left, right) return left < right end)
    local audio_identity = settings.audio_align and aegisub._audio_timing_info and aegisub._audio_timing_info()
    local current_signature = settings_signature(settings) .. "," .. keyframe_signature(keyframes) ..
        ",audio:" .. stable_hash(audio_identity or "none")

    local entries, _, speech = collect_speech(subtitles, selected_lines, settings, current_signature)
    if #entries == 0 then
        show_result(0)
        return selected_lines, active_line
    end

    local stats = {
        processed = #entries,
        shared_preserved = 0,
        continuity_linked = 0,
        overlap_fixed = 0,
        shared_keyframe = 0,
        speaker_ends_synced = 0,
        keyframe_gap_preserved = 0,
        start_keyframe = 0,
        end_keyframe_back = 0,
        end_keyframe_forward = 0,
        end_crossed_guard = 0,
        audio_available = audio_identity ~= nil and audio_identity ~= false and aegisub._audio_onset ~= nil,
        audio_starts = 0,
        overlap_protected = 0,
        modified = 0
    }

    -- Minden páros döntés a változatlan eredeti időkből készül.
    link_pairs(entries, settings, keyframes, stats)
    synchronize_speaker_ends(entries, settings, keyframes, stats)
    adjust_individual_entries(entries, settings, keyframes, stats)
    align_starts_to_audio(speech, settings, keyframes, stats)

    if aegisub.progress.is_cancelled() then aegisub.cancel() end
    for position, entry in ipairs(entries) do
        aegisub.progress.set(position * 100 / #entries)
        local new_start = canonical_ass_time(entry.start_time)
        local new_end = canonical_ass_time(entry.end_time)
        if new_end <= new_start then
            new_start = canonical_ass_time(entry.original_start)
            new_end = canonical_ass_time(entry.original_end)
        end

        local current_start = round(tonumber(entry.line.start_time) or 0)
        local current_end = round(tonumber(entry.line.end_time) or current_start)
        local timing_changed = current_start ~= new_start or current_end ~= new_end
        local extra = copy_table(entry.line.extra)
        local old_marker = extra[EXTRA_KEY]

        if new_start == entry.original_start and new_end == entry.original_end then
            extra[EXTRA_KEY] = nil
        else
            extra[EXTRA_KEY] = encode_marker(
                entry.original_start, entry.original_end, new_start, new_end, entry.context_signature
            )
        end

        if timing_changed or old_marker ~= extra[EXTRA_KEY] then
            entry.line.start_time = new_start
            entry.line.end_time = new_end
            entry.line.extra = extra
            subtitles[entry.index] = entry.line
        end
        if timing_changed then stats.modified = stats.modified + 1 end
    end

    aegisub.set_undo_point(script_name)
    show_result(stats.modified)
    return selected_lines, active_line
end

local function validate(subtitles, selected_lines)
    for _, index in ipairs(selected_lines) do
        local line = subtitles[index]
        if line.class == "dialogue" and not line.comment then return true end
    end
    return false
end

aegisub.register_macro(
    "Dynamo scripts/Okos időzítés",
    "Konzervatív KF-snap, folytonosság és 1–3 frame-es bleed-javítás.",
    process,
    validate
)
