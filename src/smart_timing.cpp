// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#include "smart_timing.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"

#include <libaegisub/audio/onset.h>
#include <libaegisub/character_count.h>
#include <libaegisub/exception.h>
#include <libaegisub/background_runner.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace {
constexpr int LinkGap = 800;
constexpr int ForwardLimit = 750;
constexpr int BackFrames = 3;
constexpr int StartLimit = 50;
constexpr int AudioLead = 100;
constexpr int AudioMinimum = 40;

int SavedTime(int time) {
	return std::max(0, static_cast<int>((static_cast<int64_t>(time) + 5) / 10 * 10));
}

struct Entry {
	AssDialogue *line;
	int original_start, original_end, start, end;
	int alignment, duration, position = 0, lane = 0;
	std::string actor;
	Entry *previous = nullptr, *next = nullptr;
	std::vector<Entry *> overlaps;
	std::optional<int> overlap_start, overlap_end;
	bool linked_previous = false, linked_next = false;
	bool protected_overlap = false, protected_end = false, synchronized = false;
	int audio_start = 0, audio_end = 0;
};

bool SameLane(Entry const& a, Entry const& b) {
	return a.line->Layer == b.line->Layer && a.alignment == b.alignment;
}

bool PreservesEnd(Entry const& entry, int end, std::set<Entry *> const& synchronized = {}) {
	if (!entry.overlap_end || end >= *entry.overlap_end) return true;
	for (auto other : entry.overlaps)
		if (!synchronized.count(other) && end < std::min(entry.original_end, other->original_end))
			return false;
	return true;
}

bool SafeBoundary(Entry const& previous, Entry const& following, int boundary,
	std::set<Entry *> const& synchronized = {})
{
	return boundary > previous.start && boundary < following.end &&
		boundary - previous.start >= previous.duration &&
		following.end - boundary >= following.duration &&
		PreservesEnd(previous, SavedTime(boundary), synchronized) &&
		(!following.overlap_start || SavedTime(boundary) <= *following.overlap_start);
}

void Link(Entry& previous, Entry& following, int boundary) {
	previous.end = following.start = boundary;
	previous.linked_next = following.linked_previous = true;
}

class Timing {
	agi::vfr::Framerate const& fps;
	std::vector<int> keys;
	agi::ProgressSink *progress;
	std::vector<Entry> entries;

	int Frame(int time) const { return fps.FrameAtTime(std::max(0, time), agi::vfr::START); }
	int Time(int frame) const { return fps.TimeAtFrame(std::max(0, frame), agi::vfr::START); }
	bool OnKeyframe(int time) const { return std::binary_search(keys.begin(), keys.end(), Frame(time)); }
	void CheckCancelled() const {
		if (progress && progress->IsCancelled()) throw agi::UserCancelException("Smart Timing cancelled");
	}

	struct Cut { int frame, time; };
	template<class Predicate>
	std::optional<Cut> Nearest(int first, int last, int target, Predicate valid) const {
		std::optional<Cut> result;
		if (first > last) return result;
		auto begin = std::lower_bound(keys.begin(), keys.end(), Frame(first) - 2);
		int last_frame = Frame(last) + 2;
		for (auto it = begin; it != keys.end() && *it <= last_frame; ++it) {
			int time = Time(*it);
			if (time < first || time > last || !valid(*it, time)) continue;
			if (!result || std::abs(time - target) < std::abs(result->time - target))
				result = Cut{*it, time};
		}
		return result;
	}
	std::optional<Cut> Nearest(int first, int last, int target) const {
		return Nearest(first, last, target, [](int, int) { return true; });
	}
	std::optional<Cut> BackEnd(int end) const {
		return Nearest(std::min(end, Time(Frame(end) - BackFrames - 2)), end, end,
			[&](int frame, int) { int bleed = Frame(end) - frame; return bleed >= 1 && bleed <= BackFrames; });
	}
	bool CrossedCut(int end) const {
		auto cut = Nearest(std::max(0, end - 100), end, end);
		return cut && cut->time < end;
	}
	std::optional<int> ForwardEnd(int end) const {
		if (OnKeyframe(end)) return {};
		auto cut = Nearest(end, end + ForwardLimit, end);
		if (!cut) return {};
		int saved = SavedTime(cut->time);
		if (saved <= end || saved - end > ForwardLimit || Frame(saved) != cut->frame) return {};
		return saved;
	}

	void AssignLanes() {
		struct Group { std::vector<Entry *> active; std::map<int, Entry *> lanes; };
		std::map<std::pair<int, int>, Group> groups;
		for (auto& entry : entries) {
			CheckCancelled();
			auto& group = groups[{entry.line->Layer, entry.alignment}];
			std::vector<Entry *> active;
			std::set<int> blocked;
			std::optional<int> lane;
			for (auto other : group.active) {
				if (other->original_end <= entry.original_start) continue;
				active.push_back(other);
				int first = std::max(entry.original_start, other->original_start);
				int last = std::min(entry.original_end, other->original_end);
				if (first >= last || entry.actor.empty() || other->actor.empty()) continue;
				if (entry.actor == other->actor) {
					if (!lane) lane = other->lane;
				}
				else {
					blocked.insert(other->lane);
					for (auto row : {&entry, other}) {
						row->overlap_start = std::min(row->overlap_start.value_or(first), first);
						row->overlap_end = std::max(row->overlap_end.value_or(last), last);
					}
					entry.overlaps.push_back(other);
					other->overlaps.push_back(&entry);
				}
			}
			if (!lane) {
				lane = 0;
				while (blocked.count(*lane)) ++*lane;
			}
			entry.lane = *lane;
			entry.previous = group.lanes[*lane];
			if (entry.previous) entry.previous->next = &entry;
			group.lanes[*lane] = &entry;
			active.push_back(&entry);
			group.active = std::move(active);
		}
	}

	void LinkPairs() {
		for (auto& previous : entries) {
			CheckCancelled();
			auto following = previous.next;
			if (!following) continue;
			int gap = following->original_start - previous.original_end;
			if (!gap) {
				int boundary = previous.original_end;
				auto cut = Nearest(Time(Frame(boundary) - 5), boundary, boundary,
					[&](int frame, int time) {
						int bleed = Frame(boundary) - frame;
						return time <= following->original_start &&
							following->original_start - time <= StartLimit && bleed >= 1 && bleed <= BackFrames;
					});
				if (cut && SafeBoundary(previous, *following, cut->time)) boundary = cut->time;
				Link(previous, *following, boundary);
			}
			else if (gap < 0) {
				int overlap = Frame(previous.original_end) - Frame(following->original_start);
				bool linked = false;
				if (overlap >= 1 && overlap <= BackFrames) {
					int boundary = static_cast<int>(std::round((previous.original_end + following->original_start) / 2.0));
					auto cut = Nearest(std::max(0, following->original_start - 1), previous.original_end, boundary);
					if (cut && std::abs(cut->frame - Frame(boundary)) <= BackFrames &&
						cut->time <= following->original_start && following->original_start - cut->time <= StartLimit &&
						Frame(previous.original_end) - cut->frame <= BackFrames)
						boundary = cut->time;
					if (SafeBoundary(previous, *following, boundary)) {
						Link(previous, *following, boundary);
						linked = true;
					}
				}
				if (!linked) previous.protected_overlap = following->protected_overlap = true;
			}
			else if (gap <= LinkGap && previous.line->Style == following->line->Style) {
				int boundary = previous.original_end + static_cast<int>(std::round(gap * 0.8));
				auto cut = Nearest(previous.original_end, following->original_start, boundary);
				if (OnKeyframe(previous.original_end)) {
					boundary = SavedTime(previous.original_end);
					previous.end = boundary;
					previous.protected_end = true;
					int early_frames = Frame(following->original_start) - Frame(previous.original_end);
					if (following->original_start >= boundary && early_frames >= 0 && early_frames <= 6 &&
						SafeBoundary(previous, *following, boundary))
						Link(previous, *following, boundary);
				}
				else {
					if (cut) boundary = cut->time;
					if (SafeBoundary(previous, *following, boundary)) Link(previous, *following, boundary);
				}
			}
		}
		for (auto& previous : entries) {
			auto following = previous.next;
			if (!following || !previous.linked_next || !following->linked_previous ||
				previous.original_end > following->original_start || previous.end < previous.original_end ||
				following->start != previous.end || OnKeyframe(previous.end)) continue;
			auto cut = Nearest(previous.end + 1, previous.original_end + ForwardLimit, previous.end);
			if (!cut || cut->frame <= Frame(previous.end) ||
				cut->frame - Frame(following->original_start) > BackFrames) continue;
			int saved = SavedTime(cut->time);
			if (saved > previous.end && saved - previous.original_end <= ForwardLimit &&
				Frame(saved) == cut->frame && SafeBoundary(previous, *following, saved))
				Link(previous, *following, saved);
		}
	}

	void SynchronizeEnds() {
		std::vector<Entry *> ordered;
		std::set<Entry *> assigned;
		for (auto& entry : entries)
			if (!entry.actor.empty() && !entry.overlaps.empty() && !entry.protected_overlap) ordered.push_back(&entry);
		std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) {
			return std::tie(a->original_end, a->position) < std::tie(b->original_end, b->position);
		});
		for (size_t i = 0; i < ordered.size(); ++i) {
			CheckCancelled();
			auto first = ordered[i];
			if (assigned.count(first)) continue;
			std::vector<Entry *> group{first};
			std::set<std::string> actors{first->actor};
			for (size_t j = i + 1; j < ordered.size(); ++j) {
				auto other = ordered[j];
				if (other->original_end - first->original_end > LinkGap) break;
				if (!assigned.count(other) && !actors.count(other->actor) && SameLane(*first, *other) &&
					other->original_start < first->original_end) {
					group.push_back(other);
					actors.insert(other->actor);
				}
			}
			if (group.size() < 2) continue;
			std::set<Entry *> members(group.begin(), group.end());
			int boundary = 0;
			for (auto entry : group) boundary = std::max(boundary, entry->end);
			auto valid = [&](int target, std::optional<int> keyframe) {
				for (auto entry : group) {
					if (std::abs(target - entry->original_end) > LinkGap ||
						target - entry->start < entry->duration || !PreservesEnd(*entry, target, members)) return false;
					if (OnKeyframe(entry->original_end) && Frame(target) != Frame(entry->original_end)) return false;
					auto next_cut = std::upper_bound(keys.begin(), keys.end(), Frame(entry->original_end));
					if (next_cut != keys.end() && *next_cut < Frame(target)) return false;
					if (target < entry->end &&
						(!keyframe || Frame(entry->original_end) - *keyframe < 0 ||
						 Frame(entry->original_end) - *keyframe > BackFrames)) return false;
					auto following = entry->next;
					if (entry->linked_next) {
						if (!following || !SafeBoundary(*entry, *following, target, members)) return false;
					}
					else if (following && target > following->start) return false;
				}
				return true;
			};
			std::optional<int> target;
			if (OnKeyframe(boundary) && valid(boundary, Frame(boundary))) target = boundary;
			if (!target) {
				auto back = BackEnd(boundary);
				if (back && Frame(SavedTime(back->time)) == back->frame && valid(SavedTime(back->time), back->frame))
					target = SavedTime(back->time);
			}
			if (!target && !CrossedCut(boundary)) {
				auto forward = ForwardEnd(boundary);
				if (forward && std::all_of(group.begin(), group.end(),
					[&](auto entry) { return *forward - entry->original_end <= ForwardLimit; }) &&
					valid(*forward, Frame(*forward))) target = forward;
			}
			if (!target && valid(boundary, {})) target = boundary;
			if (!target) continue;
			for (auto entry : group) {
				entry->end = *target;
				entry->synchronized = true;
				assigned.insert(entry);
				if (entry->linked_next) entry->next->start = *target;
			}
		}
	}

	void AdjustEntries() {
		for (auto& entry : entries) {
			CheckCancelled();
			if (!entry.linked_previous && !entry.protected_overlap) {
				auto cut = Nearest(std::max(0, entry.original_start - StartLimit), entry.original_start, entry.original_start,
					[&](int frame, int) { return Frame(entry.original_start) - frame <= BackFrames; });
				if (cut && cut->time < entry.original_start && (!entry.previous || cut->time >= entry.previous->end))
					entry.start = cut->time;
			}
			if (entry.linked_next || entry.protected_overlap || entry.synchronized || entry.protected_end ||
				OnKeyframe(entry.original_end)) continue;
			auto back = BackEnd(entry.original_end);
			if (back && PreservesEnd(entry, SavedTime(back->time)) && back->time - entry.start >= entry.duration)
				entry.end = back->time;
			else if (!CrossedCut(entry.original_end)) {
				auto forward = ForwardEnd(entry.original_end);
				if (forward && (!entry.next || *forward <= entry.next->start)) entry.end = *forward;
			}
		}
	}

	void AlignAudio(agi::AudioProvider const& audio) {
		std::vector<Entry *> ordered;
		std::map<int, std::vector<Entry *>> ending_at;
		for (auto& entry : entries) {
			entry.audio_start = SavedTime(entry.start);
			entry.audio_end = SavedTime(entry.end);
			ordered.push_back(&entry);
			ending_at[entry.audio_end].push_back(&entry);
		}
		std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) {
			return std::tie(a->audio_start, a->position) < std::tie(b->audio_start, b->position);
		});
		int active_end = -1;
		for (size_t i = 0; i < ordered.size(); ++i) {
			CheckCancelled();
			auto entry = ordered[i];
			bool first = entry->audio_start >= active_end;
			active_end = std::max(active_end, entry->audio_end);
			int start = entry->audio_start;
			if (!first || OnKeyframe(start)) continue;
			std::vector<Entry *> continuous;
			for (auto previous : ending_at[start])
				if (SameLane(*previous, *entry)) continuous.push_back(previous);
			if (std::any_of(continuous.begin(), continuous.end(), [&](auto p) { return OnKeyframe(p->audio_end); })) continue;
			int next_start = i + 1 < ordered.size() ? ordered[i + 1]->audio_start : entry->audio_end;
			int search_end = std::min({entry->audio_end, next_start, entry->audio_end - entry->duration + AudioLead + AudioMinimum});
			if (search_end <= start) continue;
			auto onset = agi::FindAudioOnset(audio, start, search_end, -40, AudioMinimum,
				[&] { return progress && progress->IsCancelled(); });
			CheckCancelled();
			if (!onset) continue;
			int target = SavedTime(*onset - AudioLead);
			if (target <= start || target >= next_start || entry->audio_end - target < entry->duration ||
				(entry->overlap_start && target > *entry->overlap_start)) continue;
			auto last_cut = std::upper_bound(keys.begin(), keys.end(), Frame(*onset));
			if (last_cut != keys.begin() && Frame(target) < *std::prev(last_cut)) continue;
			if (!continuous.empty()) {
				auto next_cut = std::upper_bound(keys.begin(), keys.end(), Frame(start));
				if (next_cut != keys.end() && *next_cut < Frame(target)) continue;
				if (!std::all_of(continuous.begin(), continuous.end(), [&](auto p) { return SafeBoundary(*p, *entry, target); }))
					continue;
			}
			entry->start = target;
			for (auto previous : continuous) previous->end = target;
		}
	}

public:
	Timing(AssFile const& file, std::vector<AssDialogue *> const& selection,
		agi::vfr::Framerate const& fps, std::vector<int> keys, agi::ProgressSink *progress)
	: fps(fps), keys(std::move(keys)), progress(progress)
	{
		std::sort(this->keys.begin(), this->keys.end());
		this->keys.erase(std::unique(this->keys.begin(), this->keys.end()), this->keys.end());
		std::map<std::string, int> alignments;
		for (auto const& style : file.Styles) alignments[style.name] = style.alignment;
		entries.reserve(selection.size());
		std::set<AssDialogue *> seen;
		for (auto line : selection) {
			CheckCancelled();
			if (!line || !seen.insert(line).second) continue;
			int alignment = alignments.count(line->Style) ? alignments[line->Style] : 2;
			for (auto const& block : line->ParseTags())
				if (block->GetType() == AssBlockType::OVERRIDE)
					for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags)
						if (tag.Name == "\\an" && !tag.Params.empty()) alignment = tag.Params.front().Get<int>(alignment);
			int start = line->Start, end = line->End;
			int characters = static_cast<int>(agi::CharacterCount(line->Text.get(),
				agi::IGNORE_WHITESPACE | agi::IGNORE_PUNCTUATION | agi::IGNORE_BLOCKS));
			int duration = std::min(std::max(600, characters * 40), std::max(1, end - start));
			entries.push_back({line, start, end, start, end, alignment, duration});
			entries.back().actor = boost::algorithm::trim_copy(line->Actor.get());
		}
		std::stable_sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) {
			return std::tie(a.original_start, a.original_end) < std::tie(b.original_start, b.original_end);
		});
		for (size_t i = 0; i < entries.size(); ++i) entries[i].position = static_cast<int>(i);
	}

	std::vector<SmartTimingChange> Run(agi::AudioProvider const* audio) {
		AssignLanes();
		LinkPairs();
		SynchronizeEnds();
		AdjustEntries();
		if (audio) AlignAudio(*audio);
		CheckCancelled();
		std::vector<SmartTimingChange> result;
		for (auto const& entry : entries) {
			int start = SavedTime(entry.start), end = SavedTime(entry.end);
			if (end <= start) continue;
			if (start != entry.original_start || end != entry.original_end)
				result.push_back({entry.line, start, end});
		}
		return result;
	}
};
}

std::vector<SmartTimingChange> CalculateSmartTiming(
	AssFile const& file, std::vector<AssDialogue *> const& selection,
	agi::vfr::Framerate const& timecodes, std::vector<int> const& keyframes,
	agi::AudioProvider const* audio, agi::ProgressSink *progress)
{
	if (!timecodes.IsLoaded() || keyframes.empty() || selection.empty()) return {};
	return Timing(file, selection, timecodes, keyframes, progress).Run(audio);
}
