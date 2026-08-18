// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "dialog_ai_karaoke.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "theme.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/option.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/timer.h>

namespace {

constexpr int max_karaoke_duration_ms = 10 * 60 * 1000;
constexpr wxFileOffset max_audio_file_bytes = 24 * 1024 * 1024;

wxDEFINE_EVENT(EVT_AI_KARAOKE_DONE, wxThreadEvent);

struct AudioTimingFeatures {
	double first_frame_ms = 0.0;
	double frame_step_ms = 10.0;
	std::vector<float> change;

	bool empty() const { return change.empty(); }

	float At(double time_ms) const {
		if (change.empty()) return 0.0f;
		auto index = static_cast<long long>(std::llround((time_ms - first_frame_ms) /
			frame_step_ms));
		index = std::clamp<long long>(index, 0, static_cast<long long>(change.size()) - 1);
		return change[static_cast<size_t>(index)];
	}
};

uint16_t read_u16(std::istream& stream) {
	std::array<unsigned char, 2> bytes{};
	stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
	if (!stream) throw ai::Error("A karaoke hangmintája nem olvasható.");
	return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t read_u32(std::istream& stream) {
	std::array<unsigned char, 4> bytes{};
	stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
	if (!stream) throw ai::Error("A karaoke hangmintája nem olvasható.");
	return static_cast<uint32_t>(bytes[0]) |
		(static_cast<uint32_t>(bytes[1]) << 8) |
		(static_cast<uint32_t>(bytes[2]) << 16) |
		(static_cast<uint32_t>(bytes[3]) << 24);
}

AudioTimingFeatures analyze_audio_timing(agi::fs::path const& path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream) throw ai::Error("A karaoke hangmintája nem nyitható meg.");
	std::array<char, 4> id{};
	stream.read(id.data(), id.size());
	if (!stream || std::string(id.data(), id.size()) != "RIFF")
		throw ai::Error("A karaoke hangmintája nem RIFF WAV fájl.");
	read_u32(stream);
	stream.read(id.data(), id.size());
	if (!stream || std::string(id.data(), id.size()) != "WAVE")
		throw ai::Error("A karaoke hangmintája nem WAV fájl.");

	uint16_t format = 0;
	uint16_t channels = 0;
	uint16_t bits_per_sample = 0;
	uint32_t sample_rate = 0;
	std::vector<unsigned char> audio_bytes;
	while (stream && audio_bytes.empty()) {
		stream.read(id.data(), id.size());
		if (!stream) break;
		auto size = read_u32(stream);
		auto chunk = std::string(id.data(), id.size());
		if (chunk == "fmt ") {
			if (size < 16) throw ai::Error("A karaoke WAV formátumblokkja érvénytelen.");
			format = read_u16(stream);
			channels = read_u16(stream);
			sample_rate = read_u32(stream);
			read_u32(stream);
			read_u16(stream);
			bits_per_sample = read_u16(stream);
			stream.seekg(size - 16, std::ios::cur);
		}
		else if (chunk == "data") {
			audio_bytes.resize(size);
			stream.read(reinterpret_cast<char *>(audio_bytes.data()), size);
		}
		else stream.seekg(size, std::ios::cur);
		if (size & 1) stream.seekg(1, std::ios::cur);
	}
	if (format != 1 || !channels || !sample_rate || bits_per_sample != 16 || audio_bytes.empty())
		throw ai::Error("A karaoke időzítő csak 16 bites PCM WAV hangmintát tud elemezni.");

	auto source_frames = audio_bytes.size() / (sizeof(int16_t) * channels);
	auto downsample = std::max<uint32_t>(1, sample_rate / 16000);
	double effective_rate = static_cast<double>(sample_rate) / downsample;
	std::vector<float> samples;
	samples.reserve((source_frames + downsample - 1) / downsample);
	for (size_t frame = 0; frame < source_frames; frame += downsample) {
		auto stop = std::min(source_frames, frame + downsample);
		double sum = 0.0;
		size_t count = 0;
		for (auto current = frame; current < stop; ++current) {
			for (uint16_t channel = 0; channel < channels; ++channel) {
				auto offset = (current * channels + channel) * sizeof(int16_t);
				auto encoded = static_cast<uint16_t>(audio_bytes[offset] |
					(audio_bytes[offset + 1] << 8));
				sum += static_cast<int16_t>(encoded);
				++count;
			}
		}
		samples.push_back(static_cast<float>(sum / (32768.0 * count)));
	}

	constexpr size_t fft_size = 512;
	if (samples.size() < fft_size) return {};
	std::array<size_t, fft_size> reversed{};
	for (size_t value = 0; value < fft_size; ++value) {
		size_t source_value = value;
		size_t target = 0;
		for (int bit = 0; bit < 9; ++bit) {
			target = (target << 1) | (source_value & 1);
			source_value >>= 1;
		}
		reversed[value] = target;
	}
	std::array<std::complex<float>, fft_size / 2> twiddle{};
	constexpr double pi = 3.14159265358979323846;
	for (size_t i = 0; i < twiddle.size(); ++i) {
		auto angle = -2.0 * pi * i / fft_size;
		twiddle[i] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
	}
	std::array<float, fft_size> window{};
	for (size_t i = 0; i < fft_size; ++i)
		window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * pi * i / (fft_size - 1)));
	std::array<std::complex<float>, fft_size> spectrum{};
	std::array<float, fft_size / 2 + 1> previous{};
	std::array<float, fft_size / 2 + 1> current{};
	auto hop = std::max<size_t>(1, static_cast<size_t>(std::llround(effective_rate / 100.0)));
	AudioTimingFeatures features;
	features.first_frame_ms = fft_size * 500.0 / effective_rate;
	features.frame_step_ms = hop * 1000.0 / effective_rate;
	features.change.reserve((samples.size() - fft_size) / hop + 1);
	bool first = true;
	for (size_t start = 0; start + fft_size <= samples.size(); start += hop) {
		for (size_t i = 0; i < fft_size; ++i)
			spectrum[reversed[i]] = {samples[start + i] * window[i], 0.0f};
		for (size_t length = 2; length <= fft_size; length <<= 1) {
			auto half = length / 2;
			auto twiddle_step = fft_size / length;
			for (size_t block = 0; block < fft_size; block += length) {
				for (size_t i = 0; i < half; ++i) {
					auto right = spectrum[block + i + half] * twiddle[i * twiddle_step];
					auto left = spectrum[block + i];
					spectrum[block + i] = left + right;
					spectrum[block + i + half] = left - right;
				}
			}
		}
		for (size_t i = 0; i < current.size(); ++i)
			current[i] = std::log1p(std::abs(spectrum[i]) * 20.0f);
		float positive_flux = 0.0f;
		float absolute_change = 0.0f;
		if (!first) {
			for (size_t i = 0; i < current.size(); ++i) {
				auto difference = current[i] - previous[i];
				positive_flux += std::max(0.0f, difference);
				absolute_change += std::abs(difference);
			}
			positive_flux /= current.size();
			absolute_change /= current.size();
		}
		features.change.push_back(positive_flux + 0.18f * absolute_change);
		previous = current;
		first = false;
	}
	return features;
}

struct KaraokeOutcome {
	bool cancelled = false;
	std::string error;
	ai::KaraokeResult result;
	AudioTimingFeatures timing_features;
};

struct TemporaryFile final {
	wxString path;
	~TemporaryFile() {
		if (!path.empty() && wxFileExists(path)) wxRemoveFile(path);
	}
};

std::string remove_karaoke_tags(std::string const& text) {
	std::string result;
	for (size_t i = 0; i < text.size();) {
		if (text[i] != '{') {
			result += text[i++];
			continue;
		}
		auto end = text.find('}', i + 1);
		if (end == std::string::npos) {
			result += text.substr(i);
			break;
		}
		std::string block;
		for (size_t p = i + 1; p < end;) {
			if (text[p] == '\\' && p + 2 < end &&
				(text[p + 1] == 'k' || text[p + 1] == 'K')) {
				size_t q = p + 2;
				if (q < end && (text[q] == 'f' || text[q] == 'F' ||
					text[q] == 'o' || text[q] == 'O')) ++q;
				auto digits = q;
				while (q < end && text[q] >= '0' && text[q] <= '9') ++q;
				if (q > digits) {
					p = q;
					continue;
				}
			}
			block += text[p++];
		}
		if (!block.empty()) result += "{" + block + "}";
		i = end + 1;
	}
	return result;
}

std::string visible_text(std::string const& ass_text) {
	std::string result;
	for (size_t i = 0; i < ass_text.size();) {
		if (ass_text[i] == '{') {
			auto end = ass_text.find('}', i + 1);
			if (end != std::string::npos) {
				i = end + 1;
				continue;
			}
		}
		result += ass_text[i++];
	}
	return result;
}

struct TokenizedText {
	std::vector<std::string> tokens;
	std::vector<size_t> offsets{0};
};

TokenizedText tokenize_text(std::string const& text) {
	TokenizedText result;
	for (size_t pos = 0; pos < text.size();) {
		auto start = pos;
		if (text[pos] == '\\' && pos + 1 < text.size() &&
			(text[pos + 1] == 'N' || text[pos + 1] == 'n' || text[pos + 1] == 'h'))
			pos += 2;
		else {
			auto lead = static_cast<unsigned char>(text[pos]);
			size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 :
				(lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
			pos += std::min(length, text.size() - pos);
		}
		result.tokens.emplace_back(text.substr(start, pos - start));
		result.offsets.push_back(pos);
	}
	return result;
}

std::vector<size_t> align_token_boundaries(std::vector<std::string> const& source,
	std::vector<std::string> const& target) {
	auto source_size = source.size();
	auto target_size = target.size();
	std::vector<size_t> mapping(source_size + 1, 0);
	if (!source_size) return mapping;

	// Subtitle lines are normally short. Avoid quadratic memory use for an
	// unexpectedly large pasted line while retaining a deterministic fallback.
	if (source_size * target_size > 2000000) {
		for (size_t i = 0; i <= source_size; ++i)
			mapping[i] = (i * target_size + source_size / 2) / source_size;
		return mapping;
	}

	auto width = target_size + 1;
	std::vector<int> distance((source_size + 1) * width);
	auto at = [&](size_t i, size_t j) -> int& { return distance[i * width + j]; };
	for (size_t i = 0; i <= source_size; ++i) at(i, 0) = static_cast<int>(i);
	for (size_t j = 0; j <= target_size; ++j) at(0, j) = static_cast<int>(j);
	for (size_t i = 1; i <= source_size; ++i) {
		for (size_t j = 1; j <= target_size; ++j) {
			auto substitution = at(i - 1, j - 1) + (source[i - 1] == target[j - 1] ? 0 : 1);
			at(i, j) = std::min({substitution, at(i - 1, j) + 1, at(i, j - 1) + 1});
		}
	}

	auto i = source_size;
	auto j = target_size;
	mapping[i] = j;
	while (i || j) {
		if (i && j && at(i, j) == at(i - 1, j - 1) +
			(source[i - 1] == target[j - 1] ? 0 : 1)) {
			--i;
			--j;
			mapping[i] = j;
		}
		else if (i && at(i, j) == at(i - 1, j) + 1) {
			--i;
			mapping[i] = j;
		}
		else {
			--j;
			mapping[i] = j;
		}
	}
	return mapping;
}

void merge_syllables(std::vector<ai::KaraokeSyllable>& syllables, size_t index) {
	auto& left = syllables[index];
	auto& right = syllables[index + 1];
	left.end_ms = std::max(left.end_ms, right.end_ms);
	left.text += right.text;
	left.kanji += right.kanji;
	left.romaji += right.romaji;
	syllables.erase(syllables.begin() + index + 1);
}

using SyllableTextField = std::string ai::KaraokeSyllable::*;

void align_syllable_text(std::vector<ai::KaraokeSyllable>& syllables,
	std::string const& target, SyllableTextField field, bool require_nonempty = true) {
	auto target_text = tokenize_text(target);
	if (target_text.tokens.empty()) throw ai::Error("A karaoke cél-szövege üres.");
	while (require_nonempty && syllables.size() > target_text.tokens.size()) {
		size_t best = 0;
		size_t best_size = std::numeric_limits<size_t>::max();
		for (size_t i = 0; i + 1 < syllables.size(); ++i) {
			auto size = tokenize_text(syllables[i].*field).tokens.size() +
				tokenize_text(syllables[i + 1].*field).tokens.size();
			if (size < best_size) {
				best = i;
				best_size = size;
			}
		}
		merge_syllables(syllables, best);
	}

	std::vector<std::string> source_tokens;
	std::vector<size_t> source_cuts{0};
	for (auto const& syllable : syllables) {
		auto tokenized = tokenize_text(syllable.*field);
		source_tokens.insert(source_tokens.end(), tokenized.tokens.begin(), tokenized.tokens.end());
		source_cuts.push_back(source_tokens.size());
	}
	auto mapping = align_token_boundaries(source_tokens, target_text.tokens);
	std::vector<size_t> target_cuts(syllables.size() + 1, 0);
	target_cuts.back() = target_text.tokens.size();
	for (size_t i = 1; i < syllables.size(); ++i) {
		auto proposed = source_tokens.empty()
			? (i * target_text.tokens.size() + syllables.size() / 2) / syllables.size()
			: mapping[source_cuts[i]];
		auto minimum = target_cuts[i - 1] + (require_nonempty ? 1 : 0);
		auto maximum = require_nonempty
			? target_text.tokens.size() - (syllables.size() - i)
			: target_text.tokens.size();
		target_cuts[i] = std::clamp(proposed, minimum, maximum);
	}
	for (size_t i = 0; i < syllables.size(); ++i) {
		auto begin = target_text.offsets[target_cuts[i]];
		auto end = target_text.offsets[target_cuts[i + 1]];
		syllables[i].*field = target.substr(begin, end - begin);
	}
}

std::string remove_middle_dots(std::string text) {
	for (auto dot : {"・", "･", "·"}) {
		for (auto pos = text.find(dot); pos != std::string::npos; pos = text.find(dot, pos))
			text.erase(pos, std::char_traits<char>::length(dot));
	}
	return text;
}

char ascii_lower(std::string const& token) {
	if (token.size() != 1) return 0;
	auto value = static_cast<unsigned char>(token[0]);
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') :
		value < 0x80 ? static_cast<char>(value) : 0;
}

bool is_romaji_vowel(std::string const& token) {
	auto value = ascii_lower(token);
	if (value == 'a' || value == 'i' || value == 'u' || value == 'e' || value == 'o')
		return true;
	return token == "ā" || token == "ī" || token == "ū" || token == "ē" || token == "ō" ||
		token == "Ā" || token == "Ī" || token == "Ū" || token == "Ē" || token == "Ō";
}

bool is_ascii_letter(std::string const& token) {
	auto value = ascii_lower(token);
	return value >= 'a' && value <= 'z';
}

std::vector<std::string> split_japanese_romaji_mora(std::string const& text) {
	auto tokenized = tokenize_text(text);
	auto const& tokens = tokenized.tokens;
	std::vector<std::string> result;
	std::string prefix;
	for (size_t i = 0; i < tokens.size();) {
		auto value = ascii_lower(tokens[i]);
		if (!is_ascii_letter(tokens[i]) && !is_romaji_vowel(tokens[i])) {
			if (result.empty()) prefix += tokens[i];
			else result.back() += tokens[i];
			++i;
			continue;
		}

		auto start = i;
		if (is_romaji_vowel(tokens[i])) ++i;
		else if (value == 'n') {
			if (i + 2 < tokens.size() && ascii_lower(tokens[i + 1]) == 'y' &&
				is_romaji_vowel(tokens[i + 2]))
				i += 3;
			else if (i + 1 < tokens.size() && is_romaji_vowel(tokens[i + 1])) i += 2;
			else {
				++i;
				if (i < tokens.size() && (tokens[i] == "'" || tokens[i] == "’")) ++i;
			}
		}
		else if (i + 1 < tokens.size() && ascii_lower(tokens[i + 1]) == value && value != 'n')
			++i;
		else {
			++i;
			if (i < tokens.size() &&
				((value == 's' && ascii_lower(tokens[i]) == 'h') ||
				 (value == 'c' && ascii_lower(tokens[i]) == 'h') ||
				 (value == 't' && ascii_lower(tokens[i]) == 's')))
				++i;
			if (i + 1 < tokens.size() && ascii_lower(tokens[i]) == 'y' &&
				is_romaji_vowel(tokens[i + 1]))
				i += 2;
			else if (i < tokens.size() && is_romaji_vowel(tokens[i])) ++i;
		}

		std::string mora = std::move(prefix);
		prefix.clear();
		for (auto p = start; p < i; ++p) mora += tokens[p];
		result.push_back(std::move(mora));
	}
	if (!prefix.empty()) {
		if (result.empty()) result.push_back(std::move(prefix));
		else result.back() += prefix;
	}
	return result;
}

bool looks_english(std::string const& word) {
	std::string lower;
	for (auto value : word) {
		auto ch = static_cast<unsigned char>(value);
		if (ch >= 'A' && ch <= 'Z') lower += static_cast<char>(ch - 'A' + 'a');
		else if (ch < 0x80) lower += static_cast<char>(ch);
	}
	if (lower.size() <= 3) return false;
	for (auto marker : {"l", "v", "x", "q", "th", "ph", "ck", "tion", "ing", "ed", "er", "ly"})
		if (lower.find(marker) != std::string::npos) return true;
	auto last = lower.empty() ? 0 : lower.back();
	return last >= 'a' && last <= 'z' && last != 'a' && last != 'i' && last != 'u' &&
		last != 'e' && last != 'o' && last != 'n';
}

std::vector<std::string> split_english_word(std::string const& word) {
	if (word.size() < 4) return {word};
	std::string lower = word;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
		return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') :
			static_cast<char>(value);
	});
	auto vowel = [&](size_t index) {
		auto value = lower[index];
		return value == 'a' || value == 'e' || value == 'i' || value == 'o' || value == 'u' ||
			(value == 'y' && index > 0);
	};
	std::vector<std::pair<size_t, size_t>> nuclei;
	for (size_t i = 0; i < lower.size();) {
		if (!vowel(i)) { ++i; continue; }
		auto start = i++;
		while (i < lower.size() && vowel(i)) ++i;
		nuclei.emplace_back(start, i);
	}
	if (nuclei.size() > 1 && nuclei.back().first + 1 == lower.size() && lower.back() == 'e')
		nuclei.pop_back();
	if (nuclei.size() <= 1) return {word};

	std::vector<size_t> cuts{0};
	for (size_t i = 0; i + 1 < nuclei.size(); ++i) {
		auto cluster_start = nuclei[i].second;
		auto cluster_end = nuclei[i + 1].first;
		auto cluster = lower.substr(cluster_start, cluster_end - cluster_start);
		size_t cut = cluster_start;
		if (cluster.size() > 1) {
			if (cluster == "ck" || cluster == "ng") cut = cluster_end;
			else if (cluster != "ch" && cluster != "sh" && cluster != "th" &&
				cluster != "ph" && cluster != "wh")
				cut = cluster_start + 1;
		}
		cuts.push_back(cut);
	}
	cuts.push_back(word.size());
	std::vector<std::string> result;
	for (size_t i = 0; i + 1 < cuts.size(); ++i)
		if (cuts[i + 1] > cuts[i]) result.push_back(word.substr(cuts[i], cuts[i + 1] - cuts[i]));
	return result.empty() ? std::vector<std::string>{word} : result;
}

std::vector<std::string> split_english_syllables(std::string const& text) {
	std::vector<std::string> result;
	std::string word;
	auto flush_word = [&] {
		if (word.empty()) return;
		auto syllables = split_english_word(word);
		result.insert(result.end(), std::make_move_iterator(syllables.begin()),
			std::make_move_iterator(syllables.end()));
		word.clear();
	};
	for (auto value : text) {
		auto ch = static_cast<unsigned char>(value);
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || value == '\'')
			word += value;
		else {
			flush_word();
			if (result.empty()) word += value;
			else result.back() += value;
		}
	}
	flush_word();
	return result.empty() ? std::vector<std::string>{text} : result;
}

bool is_probably_romaji(std::string const& text) {
	auto tokens = tokenize_text(text).tokens;
	size_t letters = 0;
	size_t other = 0;
	for (auto const& token : tokens) {
		if (is_ascii_letter(token) || is_romaji_vowel(token)) ++letters;
		else if (token != " " && token != "\t" && token != "\\N" && token != "\\n") ++other;
	}
	return letters >= 2 && letters >= other * 3;
}

bool contains_japanese(std::string const& text) {
	for (size_t pos = 0; pos < text.size();) {
		auto lead = static_cast<unsigned char>(text[pos++]);
		unsigned codepoint = lead;
		int continuation = 0;
		if ((lead & 0xE0) == 0xC0) { codepoint = lead & 0x1F; continuation = 1; }
		else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0F; continuation = 2; }
		else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07; continuation = 3; }
		for (int i = 0; i < continuation && pos < text.size(); ++i)
			codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[pos++]) & 0x3F);
		if ((codepoint >= 0x3040 && codepoint <= 0x30FF) ||
			(codepoint >= 0x3400 && codepoint <= 0x9FFF) ||
			(codepoint >= 0xFF66 && codepoint <= 0xFF9D))
			return true;
	}
	return false;
}

bool is_incomplete_recognition_line(ai::KaraokeLine const& line) {
	return line.romaji.empty() || line.syllables.empty();
}

void split_romaji_syllables(ai::KaraokeLine& line, SyllableTextField field) {
	std::vector<ai::KaraokeSyllable> expanded;
	for (size_t index = 0; index < line.syllables.size(); ++index) {
		auto const& syllable = line.syllables[index];
		auto english = syllable.language == "en" ||
			(syllable.language != "ja" && looks_english(syllable.*field));
		auto parts = english ? split_english_syllables(syllable.*field) :
			split_japanese_romaji_mora(syllable.*field);
		if (parts.size() <= 1) {
			expanded.push_back(syllable);
			continue;
		}
		auto end = syllable.end_ms > syllable.start_ms ? syllable.end_ms :
			index + 1 < line.syllables.size() ? line.syllables[index + 1].start_ms : line.end_ms;
		auto duration = std::max(static_cast<int>(parts.size()), end - syllable.start_ms);
		for (size_t part = 0; part < parts.size(); ++part) {
			auto split = syllable;
			split.start_ms = syllable.start_ms + static_cast<int>(duration * part / parts.size());
			split.end_ms = syllable.start_ms + static_cast<int>(duration * (part + 1) / parts.size());
			split.*field = parts[part];
			if (field == &ai::KaraokeSyllable::romaji) split.text = parts[part];
			if (part) split.kanji.clear();
			expanded.push_back(std::move(split));
		}
	}
	line.syllables = std::move(expanded);
}

void normalize_syllable_times(ai::KaraokeLine& line) {
	auto duration = line.end_ms - line.start_ms;
	while (line.syllables.size() >= static_cast<size_t>(duration) && line.syllables.size() > 1)
		merge_syllables(line.syllables, line.syllables.size() - 2);
	auto count = line.syllables.size();
	int previous = line.start_ms - 1;
	for (size_t i = 0; i < count; ++i) {
		auto minimum = std::max(line.start_ms + static_cast<int>(i), previous + 1);
		auto maximum = line.end_ms - static_cast<int>(count - i);
		line.syllables[i].start_ms = std::clamp(line.syllables[i].start_ms, minimum, maximum);
		previous = line.syllables[i].start_ms;
	}
	for (size_t i = 0; i < count; ++i) {
		auto minimum = line.syllables[i].start_ms + 1;
		auto maximum = i + 1 < count ? line.syllables[i + 1].start_ms : line.end_ms;
		line.syllables[i].end_ms = std::clamp(line.syllables[i].end_ms, minimum, maximum);
	}
}

bool has_valid_syllable_timing(ai::KaraokeLine const& line) {
	int previous_start = line.start_ms - 1;
	for (auto const& syllable : line.syllables) {
		if (syllable.text.empty() || syllable.start_ms <= previous_start ||
			syllable.end_ms <= syllable.start_ms || syllable.start_ms < line.start_ms ||
			syllable.end_ms > line.end_ms)
			return false;
		previous_start = syllable.start_ms;
	}
	return true;
}

struct FeatureNormalization {
	double median = 0.0;
	double deviation = 1.0;
};

FeatureNormalization feature_normalization(AudioTimingFeatures const& features,
	int start_ms, int end_ms) {
	std::vector<float> values;
	auto first = std::max<double>(start_ms, features.first_frame_ms);
	auto last_available = features.first_frame_ms +
		(features.change.size() - 1) * features.frame_step_ms;
	auto last = std::min<double>(end_ms, last_available);
	for (double time = first; time <= last; time += features.frame_step_ms)
		values.push_back(features.At(time));
	if (values.empty()) return {};
	auto middle = values.begin() + values.size() / 2;
	std::nth_element(values.begin(), middle, values.end());
	auto median = *middle;
	auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
	double variance = 0.0;
	for (auto value : values) variance += (value - mean) * (value - mean);
	return {median, std::max(1e-6, std::sqrt(variance / values.size()))};
}

void refine_syllable_times_from_audio(ai::KaraokeLine& line,
	AudioTimingFeatures const& features) {
	auto count = line.syllables.size();
	if (features.empty() || count < 2) return;
	auto normalization = feature_normalization(features,
		std::max(0, line.start_ms - 250), line.end_ms + 250);
	auto z_score = [&](double time) {
		return (features.At(time) - normalization.median) / normalization.deviation;
	};

	std::vector<int> original_starts;
	std::vector<int> original_ends;
	original_starts.reserve(count);
	original_ends.reserve(count);
	for (auto const& syllable : line.syllables) {
		original_starts.push_back(syllable.start_ms);
		original_ends.push_back(syllable.end_ms);
	}
	std::vector<double> corrections(count, 0.0);

	// The reference karaokes show that a bad automatic line commonly accumulates
	// error towards its end. Correct only low-confidence lines, and only with one
	// smooth offset plus drift so individual syllables cannot be rearranged.
	if (count >= 5) {
		double current_mean = 0.0;
		for (auto start : original_starts) current_mean += z_score(start);
		current_mean /= count;
		if (current_mean < 0.20) {
			auto root_count = std::sqrt(static_cast<double>(count));
			double baseline = current_mean * count / root_count;
			double best_score = baseline;
			int best_offset = 0;
			int best_drift = 0;
			for (int offset = -120; offset <= 120; offset += 10) {
				for (int drift = -180; drift <= 180; drift += 10) {
					double score = 0.0;
					int previous = line.start_ms - 1;
					bool valid = true;
					for (size_t i = 0; i < count; ++i) {
						auto fraction = static_cast<double>(i) / (count - 1);
						auto adjusted = static_cast<int>(std::llround(
							original_starts[i] + offset + drift * fraction));
						if (adjusted <= previous || adjusted < line.start_ms || adjusted >= line.end_ms) {
							valid = false;
							break;
						}
						score += z_score(adjusted);
						previous = adjusted;
					}
					if (!valid) continue;
					score /= root_count;
					score -= 0.5 * std::pow(offset / 60.0, 2.0);
					score -= 0.5 * std::pow(drift / 90.0, 2.0);
					if (score > best_score) {
						best_score = score;
						best_offset = offset;
						best_drift = drift;
					}
				}
			}
			if (best_score >= baseline + 0.30) {
				for (size_t i = 0; i < count; ++i) {
					auto fraction = static_cast<double>(i) / (count - 1);
					corrections[i] = best_offset + best_drift * fraction;
				}
			}
		}
	}

	std::vector<int> adjusted_starts(count);
	for (size_t i = 0; i < count; ++i)
		adjusted_starts[i] = static_cast<int>(std::llround(original_starts[i] + corrections[i]));

	// Manual timing in the supplied fast and slow references falls near spectral
	// changes, normally within a few centiseconds. Snap only when the candidate is
	// clearly stronger, and reserve a third of each neighbouring interval so dense
	// fast singing can never lose or reorder a syllable.
	for (size_t i = 0; i < count; ++i) {
		auto left = i ? adjusted_starts[i - 1] : line.start_ms;
		auto right = i + 1 < count ? adjusted_starts[i + 1] : line.end_ms;
		auto max_shift = std::min({40, std::max(0, (adjusted_starts[i] - left - 1) / 3),
			std::max(0, (right - adjusted_starts[i] - 1) / 3)});
		auto center_score = z_score(adjusted_starts[i]);
		if (center_score >= 0.0) continue;
		auto best_objective = center_score;
		auto best_raw_score = center_score;
		int best_delta = 0;
		for (int delta = -40; delta <= 40; delta += 10) {
			if (std::abs(delta) > max_shift) continue;
			auto raw_score = z_score(adjusted_starts[i] + delta);
			auto objective = raw_score - 0.5 * std::pow(delta / 20.0, 2.0);
			if (raw_score >= center_score + 0.25 && objective > best_objective) {
				best_objective = objective;
				best_raw_score = raw_score;
				best_delta = delta;
			}
		}
		if (best_raw_score >= center_score + 0.25) adjusted_starts[i] += best_delta;
	}

	std::vector<double> total_shift(count);
	for (size_t i = 0; i < count; ++i)
		total_shift[i] = adjusted_starts[i] - original_starts[i];
	for (size_t i = 0; i < count; ++i) {
		line.syllables[i].start_ms = adjusted_starts[i];
		if (i + 1 < count && original_ends[i] >= original_starts[i + 1] - 20) {
			line.syllables[i].end_ms = adjusted_starts[i + 1];
			continue;
		}
		double end_shift = total_shift[i];
		if (i + 1 < count && original_starts[i + 1] > original_starts[i]) {
			auto fraction = std::clamp(static_cast<double>(original_ends[i] - original_starts[i]) /
				(original_starts[i + 1] - original_starts[i]), 0.0, 1.0);
			end_shift += (total_shift[i + 1] - total_shift[i]) * fraction;
		}
		line.syllables[i].end_ms = original_ends[i] + static_cast<int>(std::llround(end_shift));
	}
}

void retime_syllables(ai::KaraokeLine& line, int new_start, int new_end) {
	auto old_start = line.start_ms;
	auto old_end = line.end_ms;
	if (old_end > old_start) {
		auto old_duration = static_cast<long long>(old_end - old_start);
		auto new_duration = static_cast<long long>(new_end - new_start);
		auto convert = [&](int value) {
			return new_start + static_cast<int>(((static_cast<long long>(value) - old_start) *
				new_duration + old_duration / 2) / old_duration);
		};
		for (auto& syllable : line.syllables) {
			syllable.start_ms = convert(syllable.start_ms);
			syllable.end_ms = convert(syllable.end_ms);
		}
	}
	line.start_ms = new_start;
	line.end_ms = new_end;
}

struct KaraokeTagTiming {
	int total_cs = 0;
	std::vector<int> gap_before_cs;
	std::vector<int> syllable_cs;
};

KaraokeTagTiming make_tag_timing(ai::KaraokeLine const& line) {
	KaraokeTagTiming result;
	auto count = line.syllables.size();
	if (!count) return result;
	result.gap_before_cs.assign(count, 0);
	result.syllable_cs.assign(count, 1);
	auto to_centiseconds = [&](int value) {
		return std::max(0, (value - line.start_ms + 5) / 10);
	};
	int cursor = 0;
	for (size_t i = 0; i < count; ++i) {
		auto start = std::max(cursor, to_centiseconds(line.syllables[i].start_ms));
		result.gap_before_cs[i] = start - cursor;
		auto end = std::max(start + 1, to_centiseconds(line.syllables[i].end_ms));
		result.syllable_cs[i] = end - start;
		cursor = end;
	}
	result.total_cs = cursor;
	return result;
}

template<typename TextGetter>
std::string make_plain_karaoke(ai::KaraokeLine const& line, TextGetter get_text) {
	std::string result;
	if (line.syllables.empty()) return {};
	auto timing = make_tag_timing(line);
	for (size_t i = 0; i < line.syllables.size(); ++i) {
		if (timing.gap_before_cs[i] > 0)
			result += "{\\k" + std::to_string(timing.gap_before_cs[i]) + "}";
		result += "{\\k" + std::to_string(timing.syllable_cs[i]) + "}";
		result += get_text(line.syllables[i]);
	}
	return result;
}

std::string make_existing_karaoke(std::string const& original,
	ai::KaraokeLine const& result_line) {
	auto raw = remove_karaoke_tags(original);
	std::string pieces;
	for (auto const& syllable : result_line.syllables) pieces += syllable.text;
	if (pieces != visible_text(raw))
		throw ai::Error("Az AI szótagokra bontott szövege nem egyezik az eredeti feliratsorral.");

	std::string output;
	size_t raw_pos = 0;
	auto copy_override_blocks = [&] {
		while (raw_pos < raw.size() && raw[raw_pos] == '{') {
			auto end = raw.find('}', raw_pos + 1);
			if (end == std::string::npos) break;
			output += raw.substr(raw_pos, end - raw_pos + 1);
			raw_pos = end + 1;
		}
	};
	copy_override_blocks();
	auto timing = make_tag_timing(result_line);
	for (size_t index = 0; index < result_line.syllables.size(); ++index) {
		if (timing.gap_before_cs[index] > 0)
			output += "{\\k" + std::to_string(timing.gap_before_cs[index]) + "}";
		output += "{\\k" + std::to_string(timing.syllable_cs[index]) + "}";
		auto remaining = result_line.syllables[index].text.size();
		while (remaining && raw_pos < raw.size()) {
			if (raw[raw_pos] == '{') {
				copy_override_blocks();
				continue;
			}
			output += raw[raw_pos++];
			--remaining;
		}
		if (remaining) throw ai::Error("A karaoke szótaghatárai nem illeszthetők az eredeti ASS-szövegre.");
	}
	output += raw.substr(raw_pos);
	return output;
}

class AIKaraokeDialog final : public wxDialog {
	agi::Context *context;
	ai::KaraokeMode mode;
	std::vector<AssDialogue *> subtitle_lines;
	std::vector<ai::KaraokeInputLine> input_lines;
	agi::fs::path audio_file;
	int clip_start;
	int clip_end;
	ai::KaraokeResult result;
	AudioTimingFeatures timing_features;

	wxStaticText *status;
	wxGauge *progress;
	wxPanel *style_panel;
	wxChoice *kanji_style;
	wxButton *cancel_button;
	wxButton *start_button;
	wxTimer pulse_timer;
	std::atomic_bool cancelled{false};
	bool busy = false;
	bool close_when_idle = false;

	void PostOutcome(std::shared_ptr<KaraokeOutcome> outcome) {
		auto event = new wxThreadEvent(EVT_AI_KARAOKE_DONE);
		event->SetPayload(std::move(outcome));
		wxQueueEvent(this, event);
	}

	void StartRequest() {
		if (busy) return;
		busy = true;
		status->SetLabel(mode == ai::KaraokeMode::KanjiGeneration
			? _("Creating kanji lines from the selected romaji...")
			: _("Recognizing the Japanese audio and calculating romaji karaoke timing..."));
		progress->Show();
		progress->Pulse();
		start_button->Hide();
		if (style_panel) style_panel->Disable();
		cancel_button->SetLabel(_("Cancel request"));
		Layout();
		pulse_timer.Start(120);
		auto key = ai::GetApiKey();
		auto model = OPT_GET("AI/OpenAI/Model")->GetString();
		// Karaoke timing uses the timestamp-capable transcription model automatically.
		auto transcription_model = std::string("whisper-1");
		constexpr bool advanced_timing = true;
		auto mode_copy = mode;
		auto lines = input_lines;
		auto audio = audio_file;
		agi::dispatch::Background().Async([this, key = std::move(key), model = std::move(model),
			transcription_model = std::move(transcription_model), advanced_timing, mode_copy,
			lines = std::move(lines), audio = std::move(audio)]() mutable {
			auto outcome = std::make_shared<KaraokeOutcome>();
			try {
				ai::OpenAIClient client(std::move(key), std::move(model),
					std::move(transcription_model), {}, &cancelled);
				if (mode_copy == ai::KaraokeMode::KanjiGeneration) {
					ai::KaraokeResult kanji;
					constexpr size_t kanji_batch_size = 40;
					for (size_t begin = 0; begin < lines.size(); begin += kanji_batch_size) {
						if (cancelled.load()) throw ai::Error("A kérés megszakítva.");
						auto end = std::min(lines.size(), begin + kanji_batch_size);
						std::vector<ai::KaraokeInputLine> batch(lines.begin() + begin, lines.begin() + end);
						auto partial = client.CreateKanji(batch);
						kanji.lines.insert(kanji.lines.end(),
							std::make_move_iterator(partial.lines.begin()),
							std::make_move_iterator(partial.lines.end()));
					}
					outcome->result = std::move(kanji);
				}
				else {
				try { outcome->timing_features = analyze_audio_timing(audio); }
				catch (...) { outcome->timing_features = {}; }
				auto transcript = client.TranscribeTimed(audio);
				if (cancelled.load()) throw ai::Error("A kérés megszakítva.");
				ai::KaraokeResult karaoke;
				auto append_result = [&](ai::KaraokeResult partial) {
					karaoke.lines.insert(karaoke.lines.end(),
						std::make_move_iterator(partial.lines.begin()),
						std::make_move_iterator(partial.lines.end()));
				};
				auto is_retryable_batch_error = [](std::exception const& error) {
					std::string message = error.what();
					return message.find("Unexpected end of token stream") != std::string::npos ||
						message.find("Timeout was reached") != std::string::npos;
				};
				std::function<void(size_t, size_t)> request_range;
				request_range = [&](size_t begin, size_t end) {
					if (cancelled.load()) throw ai::Error("A kérés megszakítva.");
					std::vector<ai::KaraokeInputLine> batch(lines.begin() + begin, lines.begin() + end);
					try {
						append_result(client.CreateKaraoke(mode_copy, batch, transcript, advanced_timing));
					}
					catch (std::exception const& error) {
						if (end - begin <= 1 || !is_retryable_batch_error(error)) throw;
						auto middle = begin + (end - begin) / 2;
						request_range(begin, middle);
						request_range(middle, end);
					}
				};
				constexpr size_t karaoke_batch_size = 10;
				constexpr int karaoke_batch_duration_ms = 35 * 1000;
				for (size_t begin = 0; begin < lines.size();) {
					auto end = begin + 1;
					while (end < lines.size() && end - begin < karaoke_batch_size &&
						lines[end].end_ms - lines[begin].start_ms <= karaoke_batch_duration_ms)
						++end;
					request_range(begin, end);
					begin = end;
				}
				std::unordered_set<int> returned_ids;
				for (auto const& line : karaoke.lines) returned_ids.insert(line.source_line_id);
				for (auto const& line : lines) {
					if (returned_ids.count(line.id)) continue;
					try {
						auto retry = client.CreateKaraoke(mode_copy, {line}, transcript, advanced_timing);
						auto match = std::find_if(retry.lines.begin(), retry.lines.end(), [&](auto const& item) {
							return item.source_line_id == line.id;
						});
						if (match != retry.lines.end()) {
							karaoke.lines.push_back(std::move(*match));
							returned_ids.insert(line.id);
						}
					}
					catch (...) {
						if (mode_copy == ai::KaraokeMode::AudioRecognition) throw;
					}
				}
				if (mode_copy == ai::KaraokeMode::AudioRecognition) {
					for (auto const& input : lines) {
						auto existing = std::find_if(karaoke.lines.begin(), karaoke.lines.end(),
							[&](auto const& item) { return item.source_line_id == input.id; });
						if (existing == karaoke.lines.end() ||
							!is_incomplete_recognition_line(*existing))
							continue;
						auto retry = client.CreateKaraoke(mode_copy, {input}, transcript, advanced_timing);
						auto replacement = std::find_if(retry.lines.begin(), retry.lines.end(),
							[&](auto const& item) { return item.source_line_id == input.id; });
						if (replacement != retry.lines.end()) *existing = std::move(*replacement);
					}
				}
				outcome->result = std::move(karaoke);
				}
			}
			catch (std::exception const& error) {
				outcome->cancelled = cancelled.load();
				outcome->error = error.what();
			}
			PostOutcome(std::move(outcome));
		});
	}

	void ValidateResult() {
		if (mode == ai::KaraokeMode::KanjiGeneration) {
			std::unordered_map<int, ai::KaraokeLine *> by_id;
			for (auto& line : result.lines)
				if (!by_id.emplace(line.source_line_id, &line).second)
					throw ai::Error("Az AI ismételt feliratsor-azonosítót adott.");
			std::vector<ai::KaraokeLine> ordered;
			for (auto const& input : input_lines) {
				auto it = by_id.find(input.id);
				if (it == by_id.end())
					throw ai::Error("Az AI kihagyott egy kijelölt feliratsort.");
				it->second->kanji = remove_middle_dots(std::move(it->second->kanji));
				ordered.push_back(std::move(*it->second));
			}
			if (result.lines.size() != ordered.size())
				throw ai::Error("Az AI ismeretlen feliratsorra hivatkozott.");
			result.lines = std::move(ordered);
			return;
		}
		if (result.lines.empty() && mode == ai::KaraokeMode::AudioRecognition)
			throw ai::Error("Az AI nem adott karaoke-sorokat.");
		std::unordered_map<int, ai::KaraokeLine *> by_id;
		for (auto& line : result.lines)
			if (!by_id.emplace(line.source_line_id, &line).second)
				throw ai::Error("Az AI ismételt feliratsor-azonosítót adott.");
		std::vector<ai::KaraokeLine> ordered;
		size_t matched_lines = 0;
		for (auto const& input : input_lines) {
			auto it = by_id.find(input.id);
			if (it != by_id.end()) {
				ordered.push_back(std::move(*it->second));
				++matched_lines;
				continue;
			}
			if (mode == ai::KaraokeMode::AudioRecognition)
				throw ai::Error("Az AI kihagyott egy kijelölt feliratsort.");
			ai::KaraokeLine fallback;
			fallback.source_line_id = input.id;
			fallback.start_ms = input.start_ms;
			fallback.end_ms = input.end_ms;
			fallback.kanji = input.text;
			fallback.romaji = input.text;
			fallback.syllables.push_back({input.start_ms, input.end_ms, input.text,
				input.text, input.text, "other"});
			ordered.push_back(std::move(fallback));
		}
		if (result.lines.size() > matched_lines)
			throw ai::Error("Az AI ismeretlen feliratsorra hivatkozott.");
		result.lines = std::move(ordered);

		if (mode == ai::KaraokeMode::AudioRecognition) {
			for (size_t i = 0; i < input_lines.size(); ++i) {
				auto& line = result.lines[i];
				line.kanji = remove_middle_dots(std::move(line.kanji));
				for (auto& syllable : line.syllables)
					syllable.kanji = remove_middle_dots(std::move(syllable.kanji));
				if (is_incomplete_recognition_line(line))
					throw ai::Error("Az AI hiányos hangfelismeréses karaoke-sort adott.");
				retime_syllables(line, input_lines[i].start_ms, input_lines[i].end_ms);
				if (!input_lines[i].text.empty()) line.romaji = input_lines[i].text;
				align_syllable_text(line.syllables, line.romaji, &ai::KaraokeSyllable::romaji);
				split_romaji_syllables(line, &ai::KaraokeSyllable::romaji);
				line.kanji.clear();
				for (auto& syllable : line.syllables) syllable.kanji.clear();
				for (auto& syllable : line.syllables) syllable.text = syllable.romaji;
			}
		}
		if (mode == ai::KaraokeMode::SyllableTiming) {
			for (size_t i = 0; i < input_lines.size(); ++i) {
				auto& line = result.lines[i];
				retime_syllables(line, input_lines[i].start_ms, input_lines[i].end_ms);
				align_syllable_text(line.syllables, input_lines[i].text,
					&ai::KaraokeSyllable::text);
				if (is_probably_romaji(input_lines[i].text))
					split_romaji_syllables(line, &ai::KaraokeSyllable::text);
				for (auto& syllable : line.syllables) syllable.kanji = syllable.text;
				line.kanji = input_lines[i].text;
			}
		}

		for (size_t i = 0; i < result.lines.size(); ++i) {
			auto& line = result.lines[i];
			if (line.start_ms < 0 || line.end_ms <= line.start_ms || line.end_ms > clip_end - clip_start)
				throw ai::Error("Az AI a megadott hangintervallumon kívüli időzítést adott.");
			if (line.syllables.empty()) throw ai::Error("Az AI nem adott szótagidőzítést.");
			normalize_syllable_times(line);
			if (!has_valid_syllable_timing(line))
				throw ai::Error("Az AI érvénytelen szótagidőzítést adott.");
			auto timing_before_audio_refinement = line.syllables;
			refine_syllable_times_from_audio(line, timing_features);
			normalize_syllable_times(line);
			if (!has_valid_syllable_timing(line))
				line.syllables = std::move(timing_before_audio_refinement);
		}
		if (mode == ai::KaraokeMode::SyllableTiming) {
			for (size_t i = 0; i < input_lines.size(); ++i) {
				if (result.lines[i].start_ms != input_lines[i].start_ms ||
					result.lines[i].end_ms != input_lines[i].end_ms)
					throw ai::Error("Az AI megváltoztatta a szótagolandó sor időtartamát.");
				std::string text;
				for (auto const& syllable : result.lines[i].syllables) text += syllable.text;
				if (text != input_lines[i].text)
					throw ai::Error("Az AI megváltoztatta a szótagolandó feliratszöveget.");
			}
		}
	}

	void OnRequestDone(wxThreadEvent& event) {
		pulse_timer.Stop();
		auto outcome = event.GetPayload<std::shared_ptr<KaraokeOutcome>>();
		busy = false;
		if (close_when_idle) {
			EndModal(wxID_CANCEL);
			return;
		}
		if (!outcome->error.empty()) {
			status->SetLabel(outcome->cancelled ? _("Request cancelled.") : _("The karaoke request failed."));
			if (!outcome->cancelled)
				wxMessageBox(to_wx(outcome->error), _("AI karaoke failed"), wxOK | wxICON_ERROR, this);
			cancel_button->SetLabel(_("Close"));
			return;
		}
		result = std::move(outcome->result);
		timing_features = std::move(outcome->timing_features);
		try { ValidateResult(); }
		catch (std::exception const& error) {
			status->SetLabel(_("The karaoke result could not be validated."));
			wxMessageBox(to_wx(error.what()), _("AI karaoke failed"), wxOK | wxICON_ERROR, this);
			cancel_button->SetLabel(_("Close"));
			return;
		}
		progress->SetValue(100);
		Apply();
	}

	std::string selected_style(wxChoice *choice, std::string const& fallback) const {
		return choice->GetSelection() == wxNOT_FOUND ? fallback : from_wx(choice->GetStringSelection());
	}

	void ApplyRecognition() {
		for (size_t i = 0; i < subtitle_lines.size(); ++i) {
			auto line = subtitle_lines[i];
			auto tag_timing = make_tag_timing(result.lines[i]);
			line->Text = make_plain_karaoke(result.lines[i],
				[](ai::KaraokeSyllable const& syllable) { return syllable.romaji; });
			line->End = std::max(static_cast<int>(line->End),
				static_cast<int>(line->Start) + tag_timing.total_cs * 10);
			line->Effect = "";
		}
		context->ass->Commit(_("fill karaoke lines from audio recognition"),
			AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_TIME);
	}

	void ApplyKanji() {
		auto style = selected_style(kanji_style, subtitle_lines.front()->Style.get());
		auto insert_pos = std::next(context->ass->iterator_to(*subtitle_lines.back()));
		Selection selection;
		AssDialogue *active = nullptr;
		for (size_t i = 0; i < result.lines.size(); ++i) {
			if (!contains_japanese(result.lines[i].kanji)) continue;
			auto created = new AssDialogue(*subtitle_lines[i]);
			created->Text = result.lines[i].kanji;
			created->Style = style;
			created->Effect = "";
			created->SourceLineText = result.lines[i].kanji;
			created->Comment = false;
			context->ass->Events.insert(insert_pos, *created);
			selection.insert(created);
			if (!active) active = created;
		}
		if (!active) throw ai::Error("Az AI nem adott létrehozható japán kanji sorokat.");
		context->selectionController->SetSelectionAndActive(std::move(selection), active);
		context->ass->Commit(_("create kanji lines with AI"),
			AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
	}

	void ApplyExisting() {
		for (size_t i = 0; i < subtitle_lines.size(); ++i) {
			auto line = subtitle_lines[i];
			auto tag_timing = make_tag_timing(result.lines[i]);
			line->Text = make_existing_karaoke(line->Text.get(), result.lines[i]);
			line->End = std::max(static_cast<int>(line->End),
				static_cast<int>(line->Start) + tag_timing.total_cs * 10);
		}
		context->ass->Commit(_("time karaoke lines with AI"),
			AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_TIME);
	}

	void Apply() {
		try {
			if (mode == ai::KaraokeMode::AudioRecognition) ApplyRecognition();
			else if (mode == ai::KaraokeMode::KanjiGeneration) ApplyKanji();
			else ApplyExisting();
			EndModal(wxID_OK);
		}
		catch (std::exception const& error) {
			status->SetLabel(_("AI karaoke could not be applied"));
			wxMessageBox(to_wx(error.what()), _("AI karaoke could not be applied"),
				wxOK | wxICON_ERROR, this);
			cancel_button->SetLabel(_("Close"));
		}
	}

	void Close() {
		if (!busy) {
			EndModal(wxID_CANCEL);
			return;
		}
		close_when_idle = true;
		cancelled.store(true);
		status->SetLabel(_("Cancelling the request..."));
		cancel_button->Disable();
	}

	void SelectMatchingStyle(wxChoice *choice, std::initializer_list<wxString> needles,
		wxString const& fallback) {
		for (auto const& needle : needles) {
			for (unsigned i = 0; i < choice->GetCount(); ++i) {
				auto lower = choice->GetString(i).Lower();
				if (lower.Contains(needle)) {
					choice->SetSelection(i);
					return;
				}
			}
		}
		choice->SetStringSelection(fallback);
		if (choice->GetSelection() == wxNOT_FOUND && choice->GetCount()) choice->SetSelection(0);
	}

public:
	AIKaraokeDialog(agi::Context *context, ai::KaraokeMode mode,
		std::vector<AssDialogue *> subtitle_lines,
		std::vector<ai::KaraokeInputLine> input_lines,
		agi::fs::path audio_file, int clip_start, int clip_end)
	: wxDialog(context->parent, wxID_ANY, _("AI karaoke"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, context(context), mode(mode), subtitle_lines(std::move(subtitle_lines))
	, input_lines(std::move(input_lines)), audio_file(std::move(audio_file))
	, clip_start(clip_start), clip_end(clip_end), pulse_timer(this) {
		auto main = new wxBoxSizer(wxVERTICAL);
		wxString title_text;
		if (mode == ai::KaraokeMode::AudioRecognition)
			title_text = _("Fill selected lines from audio recognition");
		else if (mode == ai::KaraokeMode::KanjiGeneration)
			title_text = _("Create kanji lines from selected romaji");
		else
			title_text = _("Time selected karaoke lines from audio");
		auto title = new wxStaticText(this, wxID_ANY, title_text);
		auto font = title->GetFont();
		font.SetWeight(wxFONTWEIGHT_BOLD);
		font.SetPointSize(font.GetPointSize() + 2);
		title->SetFont(font);
		main->Add(title, wxSizerFlags().Border(wxALL, 12));
		status = new wxStaticText(this, wxID_ANY,
			mode == ai::KaraokeMode::KanjiGeneration
				? _("Choose the kanji style, then create the lines.")
				: _("Recognizing the Japanese audio and calculating romaji karaoke timing..."));
		main->Add(status, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 12));
		progress = new wxGauge(this, wxID_ANY, 100);
		app_theme::StyleProgress(progress);
		main->Add(progress, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 12));

		style_panel = new wxPanel(this);
		auto styles = new wxStaticBoxSizer(wxVERTICAL, style_panel, _("Output style"));
		style_panel->SetSizer(styles);
		auto form = new wxFlexGridSizer(2, 6, 8);
		form->AddGrowableCol(1, 1);
		wxArrayString style_names;
		for (auto const& name : context->ass->GetStyles()) style_names.Add(to_wx(name));
		form->Add(new wxStaticText(styles->GetStaticBox(), wxID_ANY, _("Kanji style:")),
			wxSizerFlags().CenterVertical());
		kanji_style = new wxChoice(styles->GetStaticBox(), wxID_ANY, wxDefaultPosition,
			wxDefaultSize, style_names);
		form->Add(kanji_style, wxSizerFlags().Expand());
		styles->Add(form, wxSizerFlags().Expand().Border(wxALL, 8));
		auto fallback = to_wx(this->subtitle_lines.front()->Style.get());
		SelectMatchingStyle(kanji_style, {"kanji"}, fallback);
		if (mode != ai::KaraokeMode::KanjiGeneration) style_panel->Hide();
		main->Add(style_panel, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 12));

		auto buttons = new wxBoxSizer(wxHORIZONTAL);
		cancel_button = new wxButton(this, wxID_ANY, _("Cancel request"));
		buttons->Add(cancel_button);
		buttons->AddStretchSpacer();
		start_button = new wxButton(this, wxID_ANY, _("Create kanji lines"));
		if (mode != ai::KaraokeMode::KanjiGeneration) start_button->Hide();
		buttons->Add(start_button);
		main->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 12));
		SetSizerAndFit(main);
		SetSizeHints(FromDIP(wxSize(520, 200)));
		CenterOnParent();

		Bind(EVT_AI_KARAOKE_DONE, &AIKaraokeDialog::OnRequestDone, this);
		Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (busy) progress->Pulse(); });
		Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
			if (busy) { Close(); event.Veto(); }
			else EndModal(wxID_CANCEL);
		});
		cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
		start_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartRequest(); });
		if (mode == ai::KaraokeMode::KanjiGeneration) {
			progress->Hide();
			cancel_button->SetLabel(_("Cancel"));
			Fit();
		}
		else CallAfter([this] { StartRequest(); });
	}
};

} // namespace

void ShowAIKaraokeDialog(agi::Context *context, ai::KaraokeMode mode,
	std::vector<AssDialogue *> lines) {
	if (mode != ai::KaraokeMode::KanjiGeneration && !context->project->AudioProvider()) return;
	lines.erase(std::remove_if(lines.begin(), lines.end(), [mode](AssDialogue *line) {
		return !line || line->Comment ||
			(mode != ai::KaraokeMode::AudioRecognition && visible_text(line->Text.get()).empty());
	}), lines.end());
	if (lines.empty()) {
		wxMessageBox(_("The selection contains no dialogue lines."),
			_("AI karaoke"), wxOK | wxICON_WARNING, context->parent);
		return;
	}

	if (mode == ai::KaraokeMode::KanjiGeneration) {
		std::vector<ai::KaraokeInputLine> input;
		input.reserve(lines.size());
		for (auto line : lines)
			input.push_back({line->Id, 0, 0,
				visible_text(remove_karaoke_tags(line->Text.get()))});
		AIKaraokeDialog dialog(context, mode, std::move(lines), std::move(input),
			{}, 0, 0);
		dialog.ShowModal();
		return;
	}

	int clip_start = static_cast<int>(lines.front()->Start);
	int clip_end = static_cast<int>(lines.back()->End);
	for (auto line : lines) {
		clip_start = std::min(clip_start, static_cast<int>(line->Start));
		clip_end = std::max(clip_end, static_cast<int>(line->End));
	}
	if (clip_start < 0 || clip_end <= clip_start) {
		wxMessageBox(_("The karaoke audio interval is invalid."), _("AI karaoke"),
			wxOK | wxICON_WARNING, context->parent);
		return;
	}
	if (clip_end - clip_start > max_karaoke_duration_ms) {
		wxMessageBox(_("AI karaoke can process at most 10 minutes at once. Choose a shorter interval."),
			_("AI karaoke"), wxOK | wxICON_WARNING, context->parent);
		return;
	}

	TemporaryFile temporary;
	auto base = wxFileName::CreateTempFileName("aegisub-ai-karaoke-");
	if (base.empty()) {
		wxMessageBox(_("A temporary audio file could not be created."), _("AI karaoke"),
			wxOK | wxICON_ERROR, context->parent);
		return;
	}
	temporary.path = base + ".wav";
	if (!wxRenameFile(base, temporary.path, true)) {
		wxRemoveFile(base);
		wxMessageBox(_("The temporary audio file could not be prepared."), _("AI karaoke"),
			wxOK | wxICON_ERROR, context->parent);
		return;
	}
	try {
		agi::SaveAudioClip(*context->project->AudioProvider(),
			agi::fs::path(from_wx(temporary.path)), clip_start, clip_end);
	}
	catch (std::exception const& error) {
		wxMessageBox(to_wx(error.what()), _("The audio clip could not be created"),
			wxOK | wxICON_ERROR, context->parent);
		return;
	}
	if (wxFileName(temporary.path).GetSize() > wxULongLong(max_audio_file_bytes)) {
		wxMessageBox(_("The generated WAV file is too large for the AI request. Choose a shorter interval."),
			_("AI karaoke"), wxOK | wxICON_WARNING, context->parent);
		return;
	}

	std::vector<ai::KaraokeInputLine> input;
	input.reserve(lines.size());
	for (auto line : lines) {
		input.push_back({line->Id,
			static_cast<int>(line->Start) - clip_start,
			static_cast<int>(line->End) - clip_start,
			mode == ai::KaraokeMode::AudioRecognition ? std::string{} :
				visible_text(remove_karaoke_tags(line->Text.get()))});
	}
	AIKaraokeDialog dialog(context, mode, std::move(lines), std::move(input),
		agi::fs::path(from_wx(temporary.path)), clip_start, clip_end);
	dialog.ShowModal();
}
