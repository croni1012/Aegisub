// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_animated_text.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"

#include <libaegisub/ass/uuencode.h>
#include <libaegisub/format.h>
#include <libaegisub/unicode.h>

#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <wx/intl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

namespace typesetting::animated_text {
namespace {

constexpr char const *data_key = "aegisub/animated-text";
constexpr char const *source_key = "aegisub/animated-text-source";
constexpr char const *group_key = "aegisub/animated-text-group";
constexpr std::string_view clipboard_settings = "{:Aegisub Animated Text Settings:";
constexpr std::string_view clipboard_source = "{:Aegisub Animated Text Source:";
constexpr std::string_view clipboard_group = "{:Aegisub Animated Text Group:";

std::vector<std::string> Split(std::string const& value, char separator) {
	std::vector<std::string> out;
	size_t at = 0;
	for (;;) {
		auto end = value.find(separator, at);
		out.push_back(value.substr(at, end == std::string::npos ? end : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	return out;
}

std::string Number(double value) {
	if (std::abs(value) < .0000001) value = 0.0;
	std::ostringstream out;
	out << std::setprecision(8) << std::defaultfloat << value;
	return out.str();
}

std::string SerializeSettings(Settings const& settings) {
	std::string out = agi::format("2|%d|%d|%u|%d", static_cast<int>(settings.split),
		settings.unit_delay_frames, settings.seed, settings.animations.size());
	for (auto const& animation : settings.animations) {
		out += agi::format("|%d,%d,%d,%d", animation.enabled,
			animation.duration_frames, animation.repeats, animation.rules.size());
		for (auto const& rule : animation.rules)
			out += agi::format(";%d,%d,%s,%s", static_cast<int>(rule.tag),
				static_cast<int>(rule.mode), Number(rule.first), Number(rule.second));
	}
	return out;
}

std::optional<Settings> DeserializeSettings(std::string const& value) {
	try {
		auto fields = Split(value, '|');
		if (fields.size() < 5 || (fields[0] != "1" && fields[0] != "2"))
			return std::nullopt;
		Settings out;
		int split = std::stoi(fields[1]);
		if (fields[0] == "1") ++split;
		out.split = static_cast<SplitMode>(std::clamp(split, 0, 3));
		out.unit_delay_frames = std::clamp(std::stoi(fields[2]), 0, 10000);
		out.seed = static_cast<uint32_t>(std::stoul(fields[3]));
		int count = std::clamp(std::stoi(fields[4]), 0, 100);
		if (fields.size() != static_cast<size_t>(count + 5)) return std::nullopt;
		out.animations.clear();
		for (int i = 0; i < count; ++i) {
			auto items = Split(fields[static_cast<size_t>(i + 5)], ';');
			auto head = Split(items.front(), ',');
			if (head.size() != 4) return std::nullopt;
			Animation animation;
			animation.enabled = std::stoi(head[0]) != 0;
			animation.duration_frames = std::clamp(std::stoi(head[1]), 1, 100000);
			animation.repeats = std::clamp(std::stoi(head[2]), 0, 10000);
			int rules = std::clamp(std::stoi(head[3]), 0, 100);
			if (items.size() != static_cast<size_t>(rules + 1)) return std::nullopt;
			animation.rules.clear();
			for (int r = 0; r < rules; ++r) {
				auto parts = Split(items[static_cast<size_t>(r + 1)], ',');
				if (parts.size() != 4) return std::nullopt;
				Rule rule;
				rule.tag = static_cast<Tag>(std::clamp(std::stoi(parts[0]), 0, 11));
				rule.mode = static_cast<ValueMode>(std::clamp(std::stoi(parts[1]), 0, 5));
				rule.first = std::clamp(std::stod(parts[2]), -1000000.0, 1000000.0);
				rule.second = std::clamp(std::stod(parts[3]), -1000000.0, 1000000.0);
				animation.rules.push_back(rule);
			}
			out.animations.push_back(std::move(animation));
		}
		return out;
	}
	catch (...) { return std::nullopt; }
}

std::optional<std::string> Extra(AssFile const& file, AssDialogue const& line,
		char const *key) {
	for (auto const& extra : file.GetExtradata(line.ExtradataIds))
		if (extra.key == key) return extra.value;
	return std::nullopt;
}

std::string Marker(std::string_view prefix, std::string const& value) {
	return std::string(prefix) + agi::ass::UUEncode(value.data(),
		value.data() + value.size(), false) + "}";
}

std::optional<std::string> TakeMarker(std::string& text, std::string_view prefix) {
	auto start = text.find(prefix);
	if (start == std::string::npos) return std::nullopt;
	auto end = text.find('}', start + prefix.size());
	if (end == std::string::npos) return std::nullopt;
	auto encoded = std::string_view(text).substr(start + prefix.size(),
		end - start - prefix.size());
	auto decoded = agi::ass::UUDecode(encoded.data(), encoded.data() + encoded.size());
	text.erase(start, end - start + 1);
	return std::string(decoded.begin(), decoded.end());
}

std::string NewGroupId() {
	static std::atomic<uint64_t> counter{};
	auto now = static_cast<uint64_t>(std::chrono::steady_clock::now()
		.time_since_epoch().count());
	return agi::format("%llu-%llu", static_cast<unsigned long long>(now),
		static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)));
}

uint32_t Mix(uint32_t value) {
	value ^= value >> 16; value *= 0x7feb352dU;
	value ^= value >> 15; value *= 0x846ca68bU;
	return value ^ (value >> 16);
}

double Random01(uint32_t seed) {
	return static_cast<double>(Mix(seed)) /
		static_cast<double>(std::numeric_limits<uint32_t>::max());
}

struct Values {
	double fs = 48.0, fscx = 100.0, fscy = 100.0;
	double frz = 0.0, frx = 0.0, fry = 0.0;
	double fax = 0.0, fay = 0.0, fsp = 0.0;
	double alpha1 = 0.0, alpha3 = 0.0, alpha4 = 0.0;

	double& Get(Tag tag) {
		switch (tag) {
			case Tag::Fs: return fs; case Tag::Fscx: return fscx; case Tag::Fscy: return fscy;
			case Tag::Frz: return frz; case Tag::Frx: return frx; case Tag::Fry: return fry;
			case Tag::Fax: return fax; case Tag::Fay: return fay; case Tag::Fsp: return fsp;
			case Tag::Alpha1: return alpha1; case Tag::Alpha3: return alpha3;
			case Tag::Alpha4: return alpha4;
		}
		return fs;
	}
	double Get(Tag tag) const {
		return const_cast<Values *>(this)->Get(tag);
	}
};

Values StyleValues(AssFile& file, AssDialogue const& line) {
	AssStyle fallback;
	auto style = file.GetStyle(line.Style.get());
	if (!style) style = &fallback;
	return {style->fontsize, style->scalex, style->scaley, style->angle, 0, 0, 0, 0,
		style->spacing, static_cast<double>(style->primary.a),
		static_cast<double>(style->outline.a), static_cast<double>(style->shadow.a)};
}

void ApplyOverride(AssFile& file, AssDialogue const& line,
		AssDialogueBlockOverride const& block, Values& values) {
	for (auto const& tag : block.Tags) {
		if (tag.Name == "\\r") {
			std::string name = tag.Params.empty() ? std::string() :
				tag.Params.front().Get<std::string>(std::string());
			AssDialogue reset(line);
			if (!name.empty()) reset.Style = name;
			values = StyleValues(file, reset);
			continue;
		}
		if (tag.Params.empty() || tag.Params.front().omitted) continue;
		auto number = [&] { return tag.Params.front().Get<double>(0.0); };
		if (tag.Name == "\\fs") values.fs = number();
		else if (tag.Name == "\\fscx") values.fscx = number();
		else if (tag.Name == "\\fscy") values.fscy = number();
		else if (tag.Name == "\\frz" || tag.Name == "\\fr") values.frz = number();
		else if (tag.Name == "\\frx") values.frx = number();
		else if (tag.Name == "\\fry") values.fry = number();
		else if (tag.Name == "\\fax") values.fax = number();
		else if (tag.Name == "\\fay") values.fay = number();
		else if (tag.Name == "\\fsp") values.fsp = number();
		else if (tag.Name == "\\alpha") {
			values.alpha1 = values.alpha3 = values.alpha4 =
				tag.Params.front().Get<int>(0);
		}
		else if (tag.Name == "\\1a") values.alpha1 = tag.Params.front().Get<int>(0);
		else if (tag.Name == "\\3a") values.alpha3 = tag.Params.front().Get<int>(0);
		else if (tag.Name == "\\4a") values.alpha4 = tag.Params.front().Get<int>(0);
	}
}

bool IsSpace(std::string_view text) {
	int32_t at = 0;
	while (at < static_cast<int32_t>(text.size())) {
		UChar32 cp;
		U8_NEXT(text.data(), at, static_cast<int32_t>(text.size()), cp);
		if (cp >= 0 && !u_isUWhiteSpace(cp)) return false;
	}
	return true;
}

std::vector<std::string> Characters(std::string_view text) {
	agi::BreakIterator iterator;
	iterator.set_text(text);
	std::vector<std::string> out;
	while (!iterator.done()) {
		out.emplace_back(iterator.current());
		iterator.next();
	}
	return out;
}

bool IsVowel(std::string_view grapheme) {
	int32_t at = 0; UChar32 cp;
	U8_NEXT(grapheme.data(), at, static_cast<int32_t>(grapheme.size()), cp);
	cp = u_tolower(cp);
	static std::u32string const vowels = U"aeiouyáéíóöőúüűàèìòùâêîôûäëïüãõåæœ";
	return vowels.find(static_cast<char32_t>(cp)) != std::u32string::npos ||
		(cp >= 0x3040 && cp <= 0x30ff);
}

std::vector<std::pair<std::string, bool>> Units(std::string const& text, SplitMode mode) {
	std::vector<std::pair<std::string, bool>> out;
	if (mode == SplitMode::None) {
		out.emplace_back(text, !IsSpace(text));
		return out;
	}
	auto chars = Characters(text);
	if (mode == SplitMode::Characters) {
		for (auto& part : chars) out.emplace_back(std::move(part), !IsSpace(part));
		return out;
	}
	std::vector<std::string> words;
	for (auto& part : chars) {
		bool space = IsSpace(part);
		if (words.empty() || IsSpace(words.back()) != space) words.push_back(std::move(part));
		else words.back() += part;
	}
	if (mode == SplitMode::Words) {
		for (auto& part : words) out.emplace_back(std::move(part), !IsSpace(part));
		return out;
	}
	for (auto& word : words) {
		if (IsSpace(word)) { out.emplace_back(std::move(word), false); continue; }
		auto graphemes = Characters(word);
		if (graphemes.size() < 3) { out.emplace_back(std::move(word), true); continue; }
		std::string syllable;
		bool has_vowel = false;
		for (size_t i = 0; i < graphemes.size(); ++i) {
			syllable += graphemes[i];
			has_vowel |= IsVowel(graphemes[i]);
			bool next_vowel = i + 1 < graphemes.size() && IsVowel(graphemes[i + 1]);
			bool vowel_after_next = i + 2 < graphemes.size() && IsVowel(graphemes[i + 2]);
			if (has_vowel && (next_vowel || (!IsVowel(graphemes[i]) && vowel_after_next))) {
				out.emplace_back(std::move(syllable), true);
				syllable.clear(); has_vowel = false;
			}
		}
		if (!syllable.empty()) out.emplace_back(std::move(syllable), true);
	}
	return out;
}

std::string TagName(Tag tag) {
	static constexpr char const *names[] = {"fs", "fscx", "fscy", "frz", "frx", "fry",
		"fax", "fay", "fsp", "1a", "3a", "4a"};
	return names[static_cast<size_t>(tag)];
}

double Target(Rule const& rule, double base, uint32_t seed) {
	double low = std::min(rule.first, rule.second);
	double high = std::max(rule.first, rule.second);
	double ranged = low + (high - low) * Random01(seed);
	double signed_random = (Random01(seed) * 2.0 - 1.0) * std::abs(rule.first);
	switch (rule.mode) {
		case ValueMode::Fixed: return rule.first;
		case ValueMode::Random: return ranged;
		case ValueMode::AddFixed: return base + rule.first;
		case ValueMode::AddRandom: return base + signed_random;
		case ValueMode::PercentFixed: return base * (1.0 + rule.first / 100.0);
		case ValueMode::PercentRandom: return base * (1.0 + signed_random / 100.0);
	}
	return base;
}

std::string ValueTag(Tag tag, double value) {
	if (tag == Tag::Alpha1 || tag == Tag::Alpha3 || tag == Tag::Alpha4) {
		int alpha = std::clamp(static_cast<int>(std::lround(value)), 0, 255);
		std::ostringstream out;
		out << "\\" << TagName(tag) << "&H" << std::uppercase << std::hex
			<< std::setw(2) << std::setfill('0') << alpha << "&";
		return out.str();
	}
	return "\\" + TagName(tag) + Number(value);
}

std::string BaseTags(Settings const& settings, Values const& values) {
	std::array<bool, 12> written{};
	std::string out;
	for (auto const& animation : settings.animations) {
		if (!animation.enabled) continue;
		for (auto const& rule : animation.rules) {
			auto index = static_cast<size_t>(rule.tag);
			if (written[index]) continue;
			written[index] = true;
			out += ValueTag(rule.tag, values.Get(rule.tag));
		}
	}
	return out;
}

int RelativeTime(agi::Context *c, AssDialogue const& line, int frame_offset) {
	int first = c->videoController->FrameAtTime(line.Start, agi::vfr::START);
	int absolute = c->videoController->TimeAtFrame(first + std::max(0, frame_offset),
		agi::vfr::START);
	return std::clamp(absolute - static_cast<int>(line.Start), 0,
		std::max(0, static_cast<int>(line.End) - static_cast<int>(line.Start)));
}

std::string Transforms(agi::Context *c, AssDialogue const& line, Settings const& settings,
		Values const& values, size_t unit) {
	std::string out;
	int cursor = static_cast<int>(unit) * settings.unit_delay_frames;
	int duration_ms = std::max(0, static_cast<int>(line.End) - static_cast<int>(line.Start));
	for (size_t ai = 0; ai < settings.animations.size(); ++ai) {
		auto const& animation = settings.animations[ai];
		if (!animation.enabled || animation.rules.empty()) continue;
		int repeats = animation.repeats;
		if (!repeats) {
			int start_ms = RelativeTime(c, line, cursor);
			int cycle_ms = std::max(1, RelativeTime(c, line,
				cursor + animation.duration_frames) - start_ms);
			repeats = std::max(1, (duration_ms - start_ms + cycle_ms - 1) / cycle_ms);
		}
		for (int repeat = 0; repeat < repeats; ++repeat) {
			int start = RelativeTime(c, line, cursor + repeat * animation.duration_frames);
			int end = RelativeTime(c, line, cursor + (repeat + 1) * animation.duration_frames);
			if (start >= duration_ms || end <= start) break;
			int middle = start + std::max(1, (end - start) / 2);
			middle = std::min(middle, end);
			std::string target, base;
			for (size_t ri = 0; ri < animation.rules.size(); ++ri) {
				auto const& rule = animation.rules[ri];
				double initial = values.Get(rule.tag);
				uint32_t seed = settings.seed ^ static_cast<uint32_t>(line.Row * 0x9e3779b9U) ^
					static_cast<uint32_t>(unit * 0x85ebca6bU) ^ static_cast<uint32_t>(ai * 7919) ^
					static_cast<uint32_t>(repeat * 104729) ^ static_cast<uint32_t>(ri * 65537);
				target += ValueTag(rule.tag, Target(rule, initial, seed));
				base += ValueTag(rule.tag, initial);
			}
			out += agi::format("\\t(%d,%d,%s)\\t(%d,%d,%s)", start, middle,
				target, middle, end, base);
		}
		if (animation.repeats)
			cursor += animation.duration_frames * animation.repeats;
		else break;
	}
	return out;
}

std::string AlphaTags(Values const& values, bool visible) {
	if (!visible) return "\\1a&HFF&\\3a&HFF&\\4a&HFF&";
	return ValueTag(Tag::Alpha1, values.alpha1) + ValueTag(Tag::Alpha3, values.alpha3) +
		ValueTag(Tag::Alpha4, values.alpha4);
}

std::vector<std::string> GenerateRows(agi::Context *c, AssDialogue const& source,
		Settings const& settings) {
	if (!HasUsableText(source)) return {};

	if (settings.split == SplitMode::None) {
		Values values = StyleValues(*c->ass, source);
		std::string output;
		bool found_text = false;
		for (auto& block : source.ParseTags()) {
			if (block->GetType() == AssBlockType::OVERRIDE) {
				auto const& override = static_cast<AssDialogueBlockOverride const&>(*block);
				ApplyOverride(*c->ass, source, override, values);
				output += block->GetText();
			}
			else if (block->GetType() == AssBlockType::PLAIN) {
				auto const& plain = static_cast<AssDialogueBlockPlain const&>(*block);
				if (!IsSpace(plain.text)) {
					auto transforms = Transforms(c, source, settings, values, 0);
					if (!transforms.empty())
						output += "{" + BaseTags(settings, values) + transforms + "}";
					found_text = true;
				}
				output += plain.text;
			}
			else output += block->GetText();
		}
		return found_text ? std::vector<std::string>{std::move(output)} :
			std::vector<std::string>{};
	}

	size_t count = 0;
	for (auto& block : source.ParseTags()) {
		if (block->GetType() != AssBlockType::PLAIN) continue;
		for (auto const& unit : Units(
			static_cast<AssDialogueBlockPlain const&>(*block).text, settings.split))
			if (unit.second) ++count;
	}
	std::vector<std::string> rows;
	rows.reserve(count);
	for (size_t target = 0; target < count; ++target) {
		Values values = StyleValues(*c->ass, source);
		std::string output;
		size_t unit_index = 0;
		for (auto& block : source.ParseTags()) {
			if (block->GetType() == AssBlockType::OVERRIDE) {
				auto const& override = static_cast<AssDialogueBlockOverride const&>(*block);
				ApplyOverride(*c->ass, source, override, values);
				output += block->GetText();
			}
			else if (block->GetType() == AssBlockType::PLAIN) {
				auto const& plain = static_cast<AssDialogueBlockPlain const&>(*block);
				for (auto& [text, animate] : Units(plain.text, settings.split)) {
					if (animate) {
						bool visible = unit_index == target;
						std::string tags = AlphaTags(values, visible);
						if (visible)
							tags += BaseTags(settings, values) +
								Transforms(c, source, settings, values, unit_index);
						output += "{" + tags + "}";
						++unit_index;
					}
					output += text;
				}
			}
			else output += block->GetText();
		}
		rows.push_back(std::move(output));
	}
	return rows;
}

AssDialogue StoredSource(AssDialogue const& representative, std::string const& encoded) {
	if (encoded.starts_with("Dialogue:") || encoded.starts_with("Comment:"))
		return AssDialogue(encoded);
	AssDialogue source(representative);
	source.Text = encoded; // Compatibility with the first Animated Text metadata format.
	source.ExtradataIds = std::vector<uint32_t>{};
	return source;
}

struct SourceGroup {
	AssDialogue source;
	std::vector<AssDialogue *> existing;
	std::string group;
	bool editing = false;
};

std::vector<SourceGroup> CollectGroups(agi::Context *c) {
	std::vector<SourceGroup> groups;
	std::set<std::string> seen;
	for (auto selected : c->selectionController->GetSelectedSet()) {
		auto group = Extra(*c->ass, *selected, group_key);
		auto source = Extra(*c->ass, *selected, source_key);
		auto data = Extra(*c->ass, *selected, data_key);
		if (!source || !data) {
			groups.push_back({AssDialogue(*selected), {selected}, NewGroupId(), false});
			continue;
		}
		if (!group) group = agi::format("legacy-%d", selected->Id);
		if (!seen.insert(*group).second) continue;
		SourceGroup collected{StoredSource(*selected, *source), {}, *group, true};
		for (auto& line : c->ass->Events) {
			auto line_group = Extra(*c->ass, line, group_key);
			if ((line_group && line_group == group) || (!line_group && &line == selected))
				collected.existing.push_back(&line);
		}
		if (collected.existing.empty()) collected.existing.push_back(selected);
		groups.push_back(std::move(collected));
	}
	return groups;
}

} // namespace

std::vector<std::string> SplitModeNames() {
	return {from_wx(_("No split")), from_wx(_("Words")), from_wx(_("Syllables")),
		from_wx(_("Characters"))};
}

std::vector<std::string> TagNames() {
	return {"fs", "fscx", "fscy", "frz", "frx", "fry", "fax", "fay", "fsp",
		"1a", "3a", "4a"};
}

std::vector<std::string> ValueModeNames() {
	return {from_wx(_("Set to fixed value")), from_wx(_("Set to random range")),
		from_wx(_("Add/subtract fixed value")), from_wx(_("Add/subtract random value")),
		from_wx(_("Add/subtract fixed percent")),
		from_wx(_("Add/subtract random percent"))};
}

bool HasUsableText(AssDialogue const& line) {
	bool text = false;
	for (auto const& block : line.ParseTags()) {
		if (block->GetType() == AssBlockType::DRAWING) return false;
		if (block->GetType() == AssBlockType::PLAIN &&
			!IsSpace(static_cast<AssDialogueBlockPlain const&>(*block).text)) text = true;
	}
	return text;
}

bool IsEffect(AssFile const& file, AssDialogue const *line) {
	return line && Extra(file, *line, data_key).has_value() &&
		Extra(file, *line, source_key).has_value();
}

void ExpandSelection(AssFile const& file, std::set<AssDialogue *>& selection) {
	std::set<std::string> groups;
	for (auto line : selection)
		if (auto group = Extra(file, *line, group_key)) groups.insert(*group);
	if (groups.empty()) return;
	for (auto const& line : file.Events) {
		auto group = Extra(file, line, group_key);
		if (group && groups.count(*group)) selection.insert(const_cast<AssDialogue *>(&line));
	}
}

Settings LoadSettingsForSelection(agi::Context *c) {
	for (auto line : c->selectionController->GetSelectedSet()) {
		auto data = Extra(*c->ass, *line, data_key);
		if (data) if (auto parsed = DeserializeSettings(*data)) return *parsed;
	}
	return {};
}

bool SettingsFromClipboard(std::string clipboard, Settings& settings) {
	try {
		auto encoded = TakeMarker(clipboard, clipboard_settings);
		if (!encoded) return false;
		auto parsed = DeserializeSettings(*encoded);
		if (!parsed) return false;
		settings = std::move(*parsed);
		return true;
	}
	catch (...) { return false; }
}

std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line) {
	auto settings = Extra(file, line, data_key);
	auto source = Extra(file, line, source_key);
	auto group = Extra(file, line, group_key);
	if (!settings || !source) return {};
	if (!group) group = agi::format("legacy-%d", line.Id);
	return Marker(clipboard_settings, *settings) + Marker(clipboard_source, *source) +
		Marker(clipboard_group, *group);
}

bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line, ClipboardPasteState *state) {
	std::string text = line.Text.get();
	auto settings = TakeMarker(text, clipboard_settings);
	auto source = TakeMarker(text, clipboard_source);
	auto group = TakeMarker(text, clipboard_group);
	line.Text = std::move(text);
	if (!settings || !source || !group || !DeserializeSettings(*settings)) return false;
	std::string restored_group;
	if (state) {
		auto [it, inserted] = state->groups.emplace(*group, std::string());
		if (inserted) it->second = NewGroupId();
		restored_group = it->second;
	}
	else restored_group = NewGroupId();
	file.SetExtradataValue(line, data_key, *settings);
	file.SetExtradataValue(line, source_key, *source);
	file.SetExtradataValue(line, group_key, restored_group);
	return true;
}

struct PreviewSession::Impl {
	agi::Context *context;
	std::vector<SourceGroup> groups;
	explicit Impl(agi::Context *c) : context(c), groups(CollectGroups(c)) {}
	void Clear() {
		std::vector<AssDialogue const *> originals;
		for (auto const& group : groups)
			for (auto line : group.existing) originals.push_back(line);
		if (!originals.empty()) context->videoController->PreviewSubtitles(originals);
	}
	void Update(Settings const& settings) {
		std::vector<AssDialogue> silenced;
		std::vector<AssDialogue> added;
		for (auto const& group : groups) {
			for (auto line : group.existing) {
				silenced.emplace_back(*line);
				silenced.back().Comment = true;
			}
			for (auto& text : GenerateRows(context, group.source, settings)) {
				added.emplace_back(group.source);
				added.back().Comment = false;
				added.back().Text = std::move(text);
			}
		}
		if (added.empty()) { Clear(); return; }
		std::vector<AssDialogue const *> changed, extras;
		for (auto const& line : silenced) changed.push_back(&line);
		for (auto const& line : added) extras.push_back(&line);
		context->videoController->PreviewSubtitles(changed, extras);
	}
};

PreviewSession::PreviewSession(agi::Context *c) : impl(std::make_unique<Impl>(c)) {}
PreviewSession::~PreviewSession() = default;
void PreviewSession::Update(Settings const& settings) { impl->Update(settings); }
void PreviewSession::Clear() { impl->Clear(); }

size_t Apply(agi::Context *c, Settings const& settings) {
	auto groups = CollectGroups(c);
	Selection selection;
	AssDialogue *active = nullptr;
	auto old_active = c->selectionController->GetActiveLine();
	std::vector<std::unique_ptr<AssDialogue>> removed;
	size_t count = 0;
	for (auto& group : groups) {
		auto generated = GenerateRows(c, group.source, settings);
		if (generated.empty() || group.existing.empty()) continue;
		bool was_active = std::find(group.existing.begin(), group.existing.end(), old_active) !=
			group.existing.end();
		auto insert_at = c->ass->Events.iterator_to(*group.existing.front());
		std::string stored_source = group.source.GetEntryData(false);
		for (auto& text : generated) {
			auto line = new AssDialogue(group.source);
			line->Comment = false;
			line->Text = std::move(text);
			c->ass->SetExtradataValue(*line, data_key, SerializeSettings(settings));
			c->ass->SetExtradataValue(*line, source_key, stored_source);
			c->ass->SetExtradataValue(*line, group_key, group.group);
			c->ass->Events.insert(insert_at, *line);
			selection.insert(line);
			if (!active || was_active) active = line;
			++count;
		}
		for (auto line : group.existing) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
	}
	if (!count) return 0;
	c->selectionController->SetSelectionAndActive(std::move(selection), active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("apply animated text"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return count;
}

bool Revert(agi::Context *c) {
	auto groups = CollectGroups(c);
	Selection selection;
	AssDialogue *active = nullptr;
	auto old_active = c->selectionController->GetActiveLine();
	std::vector<std::unique_ptr<AssDialogue>> removed;
	for (auto& group : groups) {
		if (!group.editing || group.existing.empty()) continue;
		bool was_active = std::find(group.existing.begin(), group.existing.end(), old_active) !=
			group.existing.end();
		auto insert_at = c->ass->Events.iterator_to(*group.existing.front());
		auto original = new AssDialogue(group.source);
		c->ass->Events.insert(insert_at, *original);
		selection.insert(original);
		if (!active || was_active) active = original;
		for (auto line : group.existing) {
			c->ass->Events.erase(c->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
	}
	if (selection.empty()) return false;
	c->selectionController->SetSelectionAndActive(std::move(selection), active);
	c->ass->CleanExtradata();
	c->ass->Commit(_("remove animated text"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

} // namespace typesetting::animated_text
