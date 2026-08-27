// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "visual_tool_preview.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace agi { struct Context; }
class AssDialogue;
class AssFile;

namespace typesetting::animated_text {

enum class SplitMode { None, Words, Syllables, Characters };
enum class Tag { Fs, Fscx, Fscy, Frz, Frx, Fry, Fax, Fay, Fsp, Alpha1, Alpha3, Alpha4 };
enum class ValueMode { Fixed, Random, AddFixed, AddRandom, PercentFixed, PercentRandom };

struct Rule {
	Tag tag = Tag::Fs;
	ValueMode mode = ValueMode::AddFixed;
	double first = 10.0;
	double second = 20.0;
};

struct Animation {
	bool enabled = true;
	int duration_frames = 6;
	int repeats = 1; // Zero means repeat until the end of the line.
	std::vector<Rule> rules{{}};
};

struct Settings {
	SplitMode split = SplitMode::None;
	int unit_delay_frames = 1;
	uint32_t seed = 0x71a9b35dU;
	std::vector<Animation> animations{{}};
};

std::vector<std::string> SplitModeNames();
std::vector<std::string> TagNames();
std::vector<std::string> ValueModeNames();

bool HasUsableText(AssDialogue const& line);
bool IsEffect(AssFile const& file, AssDialogue const *line);
void ExpandSelection(AssFile const& file, std::set<AssDialogue *>& selection);
Settings LoadSettingsForSelection(agi::Context *c);
bool SettingsFromClipboard(std::string clipboard, Settings& settings);
std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line);
struct ClipboardPasteState {
	std::unordered_map<std::string, std::string> groups;
};
bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line,
	ClipboardPasteState *state = nullptr);

class PreviewSession final : public NonDestructivePreviewSession {
	struct Impl;
	std::unique_ptr<Impl> impl;
public:
	explicit PreviewSession(agi::Context *c);
	~PreviewSession() override;
	void Update(Settings const& settings);
	void Clear() override;
};

/// Apply in place. Drawing rows are ignored and never rewritten.
size_t Apply(agi::Context *c, Settings const& settings);
/// Restore the source text saved in extradata on selected rows.
bool Revert(agi::Context *c);

} // namespace typesetting::animated_text
