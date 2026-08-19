// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_textbox.h"

#include "typesetting_perspective.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "auto4_base.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "text_to_shape.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/ass/uuencode.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <map>
#include <sstream>

namespace typesetting::textbox {
namespace {

constexpr std::string_view clipboard_settings = "{:Aegisub Textbox Settings:";

std::string ClipboardMarker(std::string const& value) {
	return std::string(clipboard_settings) + agi::ass::UUEncode(value.data(),
		value.data() + value.size(), false) + "}";
}

std::optional<std::string> TakeClipboardMarker(std::string& text) {
	auto start = text.find(clipboard_settings);
	if (start == std::string::npos) return std::nullopt;
	auto end = text.find('}', start + clipboard_settings.size());
	if (end == std::string::npos) return std::nullopt;
	auto encoded = std::string_view(text).substr(start + clipboard_settings.size(),
		end - start - clipboard_settings.size());
	auto decoded = agi::ass::UUDecode(encoded.data(), encoded.data() + encoded.size());
	text.erase(start, end - start + 1);
	return std::string(decoded.begin(), decoded.end());
}

double Number(json::UnknownElement const& value, double fallback) {
	try { return static_cast<json::Double const&>(value); }
	catch (json::Exception const&) {
		try { return static_cast<double>(static_cast<json::Integer const&>(value)); }
		catch (json::Exception const&) { return fallback; }
	}
}

double Field(json::Object const& object, char const *name, double fallback) {
	auto it = object.find(name);
	return it == object.end() ? fallback : Number(it->second, fallback);
}

bool BoolField(json::Object const& object, char const *name, bool fallback) {
	auto it = object.find(name);
	if (it == object.end()) return fallback;
	try { return static_cast<json::Boolean const&>(it->second); }
	catch (json::Exception const&) { return fallback; }
}

std::string StringField(json::Object const& object, char const *name,
	std::string fallback = {}) {
	auto it = object.find(name);
	if (it == object.end()) return fallback;
	try { return static_cast<json::String const&>(it->second); }
	catch (json::Exception const&) { return fallback; }
}

std::string FormatNumber(double value) {
	if (std::abs(value) < .0005) value = 0.0;
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(3) << value;
	std::string text = stream.str();
	while (text.size() > 1 && text.back() == '0') text.pop_back();
	if (!text.empty() && text.back() == '.') text.pop_back();
	return text;
}

bool SameColour(agi::Color const& left, agi::Color const& right) {
	return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

json::Object ColourObject(agi::Color const& colour) {
	json::Object object;
	object["r"] = static_cast<int>(colour.r);
	object["g"] = static_cast<int>(colour.g);
	object["b"] = static_cast<int>(colour.b);
	object["a"] = static_cast<int>(colour.a);
	return object;
}

agi::Color ParseColour(json::Object const& object, char const *name,
	agi::Color fallback) {
	auto it = object.find(name);
	if (it == object.end()) return fallback;
	try {
		auto const& colour = static_cast<json::Object const&>(it->second);
		fallback.r = static_cast<unsigned char>(std::clamp(Field(colour, "r", fallback.r), 0.0, 255.0));
		fallback.g = static_cast<unsigned char>(std::clamp(Field(colour, "g", fallback.g), 0.0, 255.0));
		fallback.b = static_cast<unsigned char>(std::clamp(Field(colour, "b", fallback.b), 0.0, 255.0));
		fallback.a = static_cast<unsigned char>(std::clamp(Field(colour, "a", fallback.a), 0.0, 255.0));
	}
	catch (json::Exception const&) { }
	return fallback;
}

json::Object StyleObject(TextStyle const& style) {
	json::Object object;
	object["fn"] = style.font;
	object["fs"] = style.size;
	object["fscx"] = style.scale_x;
	object["fscy"] = style.scale_y;
	object["fsp"] = style.spacing;
	object["bord"] = style.border;
	object["shad"] = style.shadow;
	object["b"] = style.bold;
	object["i"] = style.italic;
	object["u"] = style.underline;
	object["s"] = style.strikeout;
	object["c1"] = ColourObject(style.primary);
	object["c3"] = ColourObject(style.outline);
	object["c4"] = ColourObject(style.shadow_colour);
	return object;
}

TextStyle ParseStyle(json::Object const& object, TextStyle fallback) {
	fallback.font = StringField(object, "fn", fallback.font);
	fallback.size = Field(object, "fs", fallback.size);
	fallback.scale_x = Field(object, "fscx", fallback.scale_x);
	fallback.scale_y = Field(object, "fscy", fallback.scale_y);
	fallback.spacing = Field(object, "fsp", fallback.spacing);
	fallback.border = Field(object, "bord", fallback.border);
	fallback.shadow = Field(object, "shad", fallback.shadow);
	fallback.bold = BoolField(object, "b", fallback.bold);
	fallback.italic = BoolField(object, "i", fallback.italic);
	fallback.underline = BoolField(object, "u", fallback.underline);
	fallback.strikeout = BoolField(object, "s", fallback.strikeout);
	fallback.primary = ParseColour(object, "c1", fallback.primary);
	fallback.outline = ParseColour(object, "c3", fallback.outline);
	fallback.shadow_colour = ParseColour(object, "c4", fallback.shadow_colour);
	return fallback;
}

TextStyle FromAssStyle(AssStyle const& style) {
	TextStyle out;
	out.font = style.font;
	out.size = style.fontsize;
	out.scale_x = style.scalex;
	out.scale_y = style.scaley;
	out.spacing = style.spacing;
	out.border = style.outline_w;
	out.shadow = style.shadow_w;
	out.bold = style.bold;
	out.italic = style.italic;
	out.underline = style.underline;
	out.strikeout = style.strikeout;
	out.primary = style.primary;
	out.outline = style.outline;
	out.shadow_colour = style.shadow;
	return out;
}

void SetColour(agi::Color& target, AssOverrideTag const& tag) {
	if (tag.Params.empty() || tag.Params.front().omitted) return;
	auto colour = tag.Params.front().Get<agi::Color>(target);
	target.r = colour.r;
	target.g = colour.g;
	target.b = colour.b;
}

void SetAlpha(agi::Color& target, AssOverrideTag const& tag) {
	if (!tag.Params.empty() && !tag.Params.front().omitted)
		target.a = static_cast<unsigned char>(tag.Params.front().Get<int>(target.a));
}

TextStyle InitialStyle(agi::Context *c, AssDialogue const& line) {
	AssStyle fallback;
	AssStyle const *ass_style = c->ass->GetStyle(line.Style.get());
	if (!ass_style) ass_style = &fallback;
	TextStyle out = FromAssStyle(*ass_style);

	for (auto& block : line.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags) {
			auto number = [&](double current) {
				return tag.Params.empty() ? current : tag.Params.front().Get<double>(current);
			};
			if (tag.Name == "\\r") {
				std::string name = tag.Params.empty() ? std::string() :
					tag.Params.front().Get<std::string>(std::string());
				AssStyle const *reset = c->ass->GetStyle(name.empty() ? line.Style.get() : name);
				out = FromAssStyle(reset ? *reset : fallback);
			}
			else if (tag.Name == "\\fn" && !tag.Params.empty())
				out.font = tag.Params.front().Get<std::string>(out.font);
			else if (tag.Name == "\\fs") out.size = number(out.size);
			else if (tag.Name == "\\fscx") out.scale_x = number(out.scale_x);
			else if (tag.Name == "\\fscy") out.scale_y = number(out.scale_y);
			else if (tag.Name == "\\fsp") out.spacing = number(out.spacing);
			else if (tag.Name == "\\bord") out.border = number(out.border);
			else if (tag.Name == "\\shad") out.shadow = number(out.shadow);
			else if (tag.Name == "\\b") out.bold = number(out.bold ? 1 : 0) != 0;
			else if (tag.Name == "\\i") out.italic = number(out.italic ? 1 : 0) != 0;
			else if (tag.Name == "\\u") out.underline = number(out.underline ? 1 : 0) != 0;
			else if (tag.Name == "\\s") out.strikeout = number(out.strikeout ? 1 : 0) != 0;
			else if (tag.Name == "\\c" || tag.Name == "\\1c") SetColour(out.primary, tag);
			else if (tag.Name == "\\3c") SetColour(out.outline, tag);
			else if (tag.Name == "\\4c") SetColour(out.shadow_colour, tag);
			else if (tag.Name == "\\alpha") {
				SetAlpha(out.primary, tag); SetAlpha(out.outline, tag); SetAlpha(out.shadow_colour, tag);
			}
			else if (tag.Name == "\\1a") SetAlpha(out.primary, tag);
			else if (tag.Name == "\\3a") SetAlpha(out.outline, tag);
			else if (tag.Name == "\\4a") SetAlpha(out.shadow_colour, tag);
		}
		break;
	}
	return out;
}

wxString VisibleText(AssDialogue const& line) {
	wxString text = to_wx(line.GetStrippedText());
	text.Replace("\\N", "\n");
	text.Replace("\\n", " ");
	text.Replace("\\h", " ");
	return text;
}

bool IsSpace(wxUniChar character) {
	return std::iswspace(static_cast<wint_t>(character.GetValue())) != 0 && character != '\n';
}

TextStyle const& StyleAt(Document const& document, size_t index) {
	return index < document.styles.size() ? document.styles[index] : document.base_style;
}

std::string ColourTags(int channel, agi::Color const& colour) {
	return agi::format("\\%dc%s\\%da&H%02X&", channel,
		colour.GetAssOverrideFormatted(), channel, static_cast<int>(colour.a));
}

std::string StyleTags(TextStyle const& style, double spacing_add = 0.0) {
	return "\\fn" + style.font +
		"\\fs" + FormatNumber(style.size) +
		"\\fscx" + FormatNumber(style.scale_x) +
		"\\fscy" + FormatNumber(style.scale_y) +
		"\\fsp" + FormatNumber(style.spacing + spacing_add) +
		"\\bord" + FormatNumber(style.border) +
		"\\shad" + FormatNumber(style.shadow) +
		"\\b" + (style.bold ? "1" : "0") +
		"\\i" + (style.italic ? "1" : "0") +
		"\\u" + (style.underline ? "1" : "0") +
		"\\s" + (style.strikeout ? "1" : "0") +
		ColourTags(1, style.primary) + ColourTags(3, style.outline) +
		ColourTags(4, style.shadow_colour);
}

std::string StyleDeltaTags(TextStyle const& from, TextStyle const& to) {
	std::string tags;
	if (from.font != to.font) tags += "\\fn" + to.font;
	if (from.size != to.size) tags += "\\fs" + FormatNumber(to.size);
	if (from.scale_x != to.scale_x) tags += "\\fscx" + FormatNumber(to.scale_x);
	if (from.scale_y != to.scale_y) tags += "\\fscy" + FormatNumber(to.scale_y);
	if (from.spacing != to.spacing) tags += "\\fsp" + FormatNumber(to.spacing);
	if (from.border != to.border) tags += "\\bord" + FormatNumber(to.border);
	if (from.shadow != to.shadow) tags += "\\shad" + FormatNumber(to.shadow);
	if (from.bold != to.bold) tags += std::string("\\b") + (to.bold ? "1" : "0");
	if (from.italic != to.italic) tags += std::string("\\i") + (to.italic ? "1" : "0");
	if (from.underline != to.underline) tags += std::string("\\u") + (to.underline ? "1" : "0");
	if (from.strikeout != to.strikeout) tags += std::string("\\s") + (to.strikeout ? "1" : "0");
	if (!SameColour(from.primary, to.primary)) tags += ColourTags(1, to.primary);
	if (!SameColour(from.outline, to.outline)) tags += ColourTags(3, to.outline);
	if (!SameColour(from.shadow_colour, to.shadow_colour)) tags += ColourTags(4, to.shadow_colour);
	return tags;
}

bool TextBoxOwnsTag(std::string const& name) {
	for (char const *owned : {"\\an", "\\q", "\\pos", "\\move", "\\r", "\\fn", "\\fs",
		"\\fscx", "\\fscy", "\\fsp", "\\bord", "\\xbord", "\\ybord", "\\shad",
		"\\xshad", "\\yshad", "\\b", "\\i", "\\u", "\\s", "\\c", "\\1c",
		"\\3c", "\\4c", "\\alpha", "\\1a", "\\3a", "\\4a", "\\org", "\\fr",
		"\\frz", "\\frx", "\\fry", "\\fax", "\\fay", "\\clip", "\\iclip"})
		if (name == owned) return true;
	return false;
}

std::string PreservedTags(AssDialogue const& prototype) {
	for (auto& block : prototype.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		std::string tags;
		for (auto const& tag : static_cast<AssDialogueBlockOverride*>(block.get())->Tags)
			if (!TextBoxOwnsTag(tag.Name)) tags += static_cast<std::string>(tag);
		return tags;
	}
	return {};
}

struct Metric {
	double width = 0;
	double height = 0;
};

struct ContentBox {
	double left = 0;
	double right = 0;
	double top = 0;
};

ContentBox GetContentBox(Document const& document) {
	double left = std::min(document.top_left.X(), document.bottom_right.X());
	double right = std::max(document.top_left.X(), document.bottom_right.X());
	double top = std::min(document.top_left.Y(), document.bottom_right.Y());
	double bottom = std::max(document.top_left.Y(), document.bottom_right.Y());
	double padding_x = std::min(std::max(0.0, document.padding),
		std::max(0.0, (right - left - 1.0) * .5));
	double padding_y = std::min(std::max(0.0, document.padding),
		std::max(0.0, (bottom - top - 1.0) * .5));
	return {left + padding_x, right - padding_x, top + padding_y};
}

std::string MetricKey(TextStyle const& style, wxString const& character) {
	return style.font + "|" + FormatNumber(style.size) + "|" +
		FormatNumber(style.scale_x) + "|" + FormatNumber(style.scale_y) + "|" +
		FormatNumber(style.spacing) + "|" + (style.bold ? "1" : "0") +
		(style.italic ? "1" : "0") + (style.underline ? "1" : "0") +
		(style.strikeout ? "1" : "0") + "|" + from_wx(character);
}

Metric Measure(agi::Context *c, AssDialogue const& prototype, TextStyle const& style,
	wxString const& character, std::map<std::string, Metric>& cache,
	std::map<std::string, bool>& synthetic_bold_cache) {
	std::string key = MetricKey(style, character);
	auto found = cache.find(key);
	if (found != cache.end()) return found->second;

	AssStyle fallback;
	AssStyle measured = c->ass->GetStyle(prototype.Style.get()) ?
		AssStyle(c->ass->GetStyle(prototype.Style.get())->GetEntryData()) : fallback;
	measured.font = style.font;
	measured.fontsize = std::max(.1, style.size);
	measured.scalex = style.scale_x;
	measured.scaley = style.scale_y;
	measured.spacing = style.spacing;
	bool synthetic_bold = false;
	if (style.bold) {
		std::string bold_key = style.font + (style.italic ? "|i" : "|r");
		auto [it, inserted] = synthetic_bold_cache.try_emplace(std::move(bold_key), false);
		if (inserted) it->second = text_to_shape::UsesSyntheticBold(style.font, style.italic);
		synthetic_bold = it->second;
	}
	measured.bold = style.bold && !synthetic_bold;
	measured.italic = style.italic;
	measured.underline = style.underline;
	measured.strikeout = style.strikeout;

	double width = 0, height = 0, descent = 0, external_leading = 0;
	wxString sample = character.empty() ? wxString("Mg") : character;
	if (!Automation4::CalculateTextExtents(&measured, from_wx(sample), width, height,
		descent, external_leading)) {
		width = character.empty() ? 0.0 : style.size * style.scale_x / 100.0;
		height = style.size * style.scale_y / 100.0;
	}
	if (character.empty()) width = 0.0;
	Metric metric{std::max(0.0, width), std::max(1.0, height)};
	cache.emplace(std::move(key), metric);
	return metric;
}

std::vector<LayoutRow> Layout(agi::Context *c, AssDialogue const& prototype,
	Document const& document) {
	auto box = GetContentBox(document);
	double right_outset = 0.0;
	auto include_style_outset = [&](TextStyle const& style) {
		double border = std::max(0.0, style.border);
		right_outset = std::max(right_outset, border + std::max(0.0, style.shadow));
	};
	include_style_outset(document.base_style);
	for (auto const& style : document.styles) include_style_outset(style);
	double left = box.left;
	double right = box.right;
	double top = box.top;
	double maximum_width = std::max(1.0, right - left);
	// Word wrapping must use the full content width. Taking the outline out of
	// this value can move a borderline word to the next row and cascade through
	// the whole paragraph. Only the expanded justified row stops slightly early.
	double justified_width = std::max(1.0, maximum_width - right_outset);
	constexpr double minimum_justified_width_ratio = .60;
	constexpr double maximum_word_stretch_em = .25;
	std::map<std::string, Metric> cache;
	std::map<std::string, bool> synthetic_bold_cache;

	std::vector<double> advances(document.text.length());
	std::vector<double> heights(document.text.length());
	for (size_t i = 0; i < document.text.length(); ++i) {
		if (document.text[i] == '\n') continue;
		auto metric = Measure(c, prototype, StyleAt(document, i), document.text.Mid(i, 1),
			cache, synthetic_bold_cache);
		advances[i] = metric.width;
		heights[i] = metric.height;
	}

	std::vector<LayoutRow> rows;
	double y = top;
	auto add_row = [&](size_t start, size_t end, bool paragraph_last) {
		LayoutRow row;
		row.start = start;
		row.end = end;
		row.visible_end = end;
		while (row.visible_end > start && IsSpace(document.text[row.visible_end - 1]))
			--row.visible_end;
		row.y = y;
		row.paragraph_last = paragraph_last;
		row.height = 0.0;
		for (size_t i = start; i < end; ++i) row.height = std::max(row.height, heights[i]);
		if (row.height <= 0.0)
			row.height = Measure(c, prototype, StyleAt(document, start), wxString(), cache,
				synthetic_bold_cache).height;

		double natural_width = 0.0;
		for (size_t i = start; i < row.visible_end; ++i) natural_width += advances[i];
		size_t spaces = 0;
		if (document.alignment == Alignment::Justified && !paragraph_last) {
			for (size_t i = start; i < row.visible_end; ++i)
				if (IsSpace(document.text[i])) ++spaces;
		}
		// Very short rows become unreadable when their few spaces are stretched
		// across the whole box. Keep them left-aligned until the natural text
		// occupies at least 60% of the usable line width.
		if (spaces && natural_width < justified_width &&
			natural_width >= maximum_width * minimum_justified_width_ratio) {
			double extra_width = justified_width - natural_width;
			// Put a bounded part of the expansion into word gaps. Long rows use only
			// this natural-looking path; rows with a few long words distribute the
			// remainder gently between letters instead of producing huge blank gaps.
			double maximum_word_spacing = std::numeric_limits<double>::max();
			for (size_t i = start; i < row.visible_end; ++i) {
				if (!IsSpace(document.text[i])) continue;
				auto const& style = StyleAt(document, i);
				double em_width = std::max(0.0, style.size) *
					std::abs(style.scale_x) / 100.0;
				maximum_word_spacing = std::min(maximum_word_spacing,
					em_width * maximum_word_stretch_em);
			}
			row.word_spacing = std::min(extra_width / spaces, maximum_word_spacing);
			double remaining = extra_width - row.word_spacing * spaces;
			size_t letter_gaps = row.visible_end > start ? row.visible_end - start - 1 : 0;
			if (letter_gaps) row.letter_spacing = remaining / letter_gaps;
		}
		bool justified = row.word_spacing > 0.0 || row.letter_spacing > 0.0;
		row.width = justified ? justified_width : natural_width;

		double offset = document.alignment == Alignment::Centre ?
			(maximum_width - row.width) * .5 : document.alignment == Alignment::Right ?
			maximum_width - row.width : 0.0;
		row.carets.push_back(left + offset);
		double x = left + offset;
		for (size_t i = start; i < end; ++i) {
			if (i < row.visible_end) {
				x += advances[i];
				if (i + 1 < row.visible_end) x += row.letter_spacing;
				if (IsSpace(document.text[i])) x += row.word_spacing;
			}
			row.carets.push_back(x);
		}
		rows.push_back(std::move(row));
		y += rows.back().height + document.line_spacing;
	};

	auto layout_line = [&](size_t line_start, size_t line_end, bool block_last) {
		if (line_start != line_end) {
			size_t cursor = line_start;
			while (cursor < line_end) {
				double width = 0.0;
				size_t last_break = wxString::npos;
				size_t cut = line_end;
				for (size_t i = cursor; i < line_end; ++i) {
					double next = width + advances[i];
					if (next > maximum_width && i > cursor) {
						cut = last_break != wxString::npos && last_break > cursor ? last_break : i;
						break;
					}
					width = next;
					if (IsSpace(document.text[i])) last_break = i + 1;
				}
				if (cut <= cursor) cut = cursor + 1;
				add_row(cursor, cut, block_last && cut == line_end);
				cursor = cut;
			}
		}
		else add_row(line_start, line_end, block_last);
	};

	// A single explicit line break remains part of the same paragraph block, so
	// its row is justified just like an automatically wrapped row. Two or more
	// consecutive breaks end the block; only that block's final row is left-aligned.
	auto layout_block = [&](size_t block_start, size_t block_end) {
		size_t line_start = block_start;
		while (line_start <= block_end) {
			size_t line_end = document.text.find('\n', line_start);
			if (line_end == wxString::npos || line_end >= block_end) line_end = block_end;
			bool block_last = line_end == block_end;
			layout_line(line_start, line_end, block_last);
			if (block_last) break;
			line_start = line_end + 1;
		}
	};

	size_t block_start = 0;
	while (block_start <= document.text.length()) {
		size_t separator = wxString::npos;
		for (size_t i = block_start; i + 1 < document.text.length(); ++i) {
			if (document.text[i] == '\n' && document.text[i + 1] == '\n') {
				separator = i;
				break;
			}
		}
		size_t block_end = separator == wxString::npos ? document.text.length() : separator;
		layout_block(block_start, block_end);
		if (separator == wxString::npos) break;
		size_t next_block = separator + 2;
		while (next_block < document.text.length() && document.text[next_block] == '\n')
			++next_block;
		// Preserve the vertical blank rows represented by the extra separators.
		for (size_t blank = separator + 1; blank < next_block; ++blank)
			add_row(blank, blank, true);
		block_start = next_block;
	}
	return rows;
}

std::string EscapeCharacter(wxString const& character) {
	if (character == "{") return "\\{";
	if (character == " ") return "\\h";
	return from_wx(character);
}

double JustifiedSpacing(TextStyle const& style, double visual_spacing) {
	// ASS applies fsp before fscx, while layout coordinates are already in script
	// pixels. Compensate so the rendered word positions match the caret geometry.
	return style.spacing + visual_spacing * 100.0 /
		std::max(.001, std::abs(style.scale_x));
}

std::optional<std::string> Data(AssFile const& file, AssDialogue const& line) {
	for (auto const& extra : file.GetExtradata(line.ExtradataIds))
		if (extra.key == data_key) return extra.value;
	return std::nullopt;
}

struct ProjectedStyle {
	TextStyle style;
	double shear_x = 0.0;
	double shear_y = 0.0;
};

ProjectedStyle ProjectStyle(TextStyle const& source, PerspectiveTags const& plane) {
	// The textbox plane acts after the character's own fscx/fscy. Express that combined
	// matrix as the scale and shear tags ASS understands for every styled span.
	double a = plane.scale.X() / 100.0;
	double b = a * plane.shear_x;
	double c = plane.scale.Y() / 100.0 * plane.shear_y;
	double d = plane.scale.Y() / 100.0;
	double sx = source.scale_x / 100.0;
	double sy = source.scale_y / 100.0;
	ProjectedStyle out{source};
	out.style.scale_x = a * sx * 100.0;
	out.style.scale_y = d * sy * 100.0;
	out.shear_x = std::abs(a * sx) > 1e-9 ? b * sy / (a * sx) : 0.0;
	out.shear_y = std::abs(d * sy) > 1e-9 ? c * sx / (d * sy) : 0.0;
	return out;
}

std::string ShearDelta(ProjectedStyle const& from, ProjectedStyle const& to) {
	std::string tags;
	if (std::abs(from.shear_x - to.shear_x) > .0005)
		tags += "\\fax" + FormatNumber(to.shear_x);
	if (std::abs(from.shear_y - to.shear_y) > .0005)
		tags += "\\fay" + FormatNumber(to.shear_y);
	return tags;
}

} // namespace

bool operator==(TextStyle const& left, TextStyle const& right) {
	return left.font == right.font && left.size == right.size &&
		left.scale_x == right.scale_x && left.scale_y == right.scale_y &&
		left.spacing == right.spacing && left.border == right.border &&
		left.shadow == right.shadow && left.bold == right.bold &&
		left.italic == right.italic && left.underline == right.underline &&
		left.strikeout == right.strikeout && SameColour(left.primary, right.primary) &&
		SameColour(left.outline, right.outline) && SameColour(left.shadow_colour, right.shadow_colour);
}

bool operator==(Document const& left, Document const& right) {
	return left.text == right.text && left.base_style == right.base_style &&
		left.styles == right.styles && left.top_left == right.top_left &&
	left.bottom_right == right.bottom_right && left.alignment == right.alignment &&
		left.line_spacing == right.line_spacing && left.padding == right.padding &&
		left.transformed == right.transformed && (!left.transformed || left.quad == right.quad);
}

void Corners(Document const& document, Vector2D out[4]) {
	if (document.transformed) {
		std::copy(document.quad.begin(), document.quad.end(), out);
		return;
	}
	float left = std::min(document.top_left.X(), document.bottom_right.X());
	float right = std::max(document.top_left.X(), document.bottom_right.X());
	float top = std::min(document.top_left.Y(), document.bottom_right.Y());
	float bottom = std::max(document.top_left.Y(), document.bottom_right.Y());
	out[0] = Vector2D(left, top);
	out[1] = Vector2D(right, top);
	out[2] = Vector2D(right, bottom);
	out[3] = Vector2D(left, bottom);
}

void SetCorners(Document& document, Vector2D const corners[4]) {
	std::copy(corners, corners + 4, document.quad.begin());
	document.transformed = true;
}

Document FromSelection(agi::Context *c, std::vector<AssDialogue *> const& lines) {
	Document document;
	if (lines.empty()) return document;
	document.base_style = InitialStyle(c, *lines.front());
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i) document.text += '\n';
		document.text += VisibleText(*lines[i]);
	}
	document.styles.assign(document.text.length(), document.base_style);
	return document;
}

std::string Serialize(Document const& document) {
	json::Object root;
	root["v"] = 3;
	root["text"] = from_wx(document.text);
	root["x1"] = document.top_left.X();
	root["y1"] = document.top_left.Y();
	root["x2"] = document.bottom_right.X();
	root["y2"] = document.bottom_right.Y();
	root["align"] = static_cast<int>(document.alignment);
	root["line_spacing"] = document.line_spacing;
	root["padding"] = document.padding;
	root["transformed"] = document.transformed;
	Vector2D corners[4];
	Corners(document, corners);
	for (int i = 0; i < 4; ++i) {
		root["q" + std::to_string(i) + "x"] = corners[i].X();
		root["q" + std::to_string(i) + "y"] = corners[i].Y();
	}
	root["base"] = StyleObject(document.base_style);

	json::Array spans;
	for (size_t start = 0; start < document.styles.size();) {
		size_t end = start + 1;
		while (end < document.styles.size() && document.styles[end] == document.styles[start]) ++end;
		json::Object span;
		span["start"] = static_cast<int64_t>(start);
		span["end"] = static_cast<int64_t>(end);
		span["style"] = StyleObject(document.styles[start]);
		spans.emplace_back(std::move(span));
		start = end;
	}
	root["spans"] = std::move(spans);
	std::ostringstream stream;
	agi::JsonWriter::Write(root, stream);
	return stream.str();
}

std::optional<Document> Load(AssFile const& file, AssDialogue const& line) {
	auto encoded = Data(file, line);
	if (!encoded) return std::nullopt;
	try {
		std::istringstream stream(*encoded);
		json::UnknownElement value;
		json::Reader::Read(value, stream);
		auto const& root = static_cast<json::Object const&>(value);
		Document document;
		document.text = to_wx(StringField(root, "text"));
		document.top_left = Vector2D(static_cast<float>(Field(root, "x1", 0)),
			static_cast<float>(Field(root, "y1", 0)));
		document.bottom_right = Vector2D(static_cast<float>(Field(root, "x2", 0)),
			static_cast<float>(Field(root, "y2", 0)));
		int alignment = static_cast<int>(Field(root, "align", 0));
		document.alignment = static_cast<Alignment>(std::clamp(alignment, 0, 3));
		document.line_spacing = Field(root, "line_spacing", 0);
		document.padding = std::max(0.0, Field(root, "padding", 0));
		document.transformed = BoolField(root, "transformed", false);
		if (document.transformed) {
			Vector2D fallback[4];
			document.transformed = false;
			Corners(document, fallback);
			document.transformed = true;
			for (int i = 0; i < 4; ++i)
				document.quad[i] = Vector2D(
					static_cast<float>(Field(root, ("q" + std::to_string(i) + "x").c_str(), fallback[i].X())),
					static_cast<float>(Field(root, ("q" + std::to_string(i) + "y").c_str(), fallback[i].Y())));
		}
		auto base = root.find("base");
		if (base != root.end())
			document.base_style = ParseStyle(static_cast<json::Object const&>(base->second), document.base_style);
		document.styles.assign(document.text.length(), document.base_style);
		auto spans = root.find("spans");
		if (spans != root.end()) {
			for (auto const& value_span : static_cast<json::Array const&>(spans->second)) {
				auto const& span = static_cast<json::Object const&>(value_span);
				size_t start = static_cast<size_t>(std::max(0.0, Field(span, "start", 0)));
				size_t end = static_cast<size_t>(std::max(0.0, Field(span, "end", start)));
				auto style = span.find("style");
				if (style == span.end()) continue;
				TextStyle parsed = ParseStyle(static_cast<json::Object const&>(style->second), document.base_style);
				for (size_t i = start; i < std::min(end, document.styles.size()); ++i)
					document.styles[i] = parsed;
			}
		}
		return document;
	}
	catch (...) { return std::nullopt; }
}

bool IsEffect(AssDialogue const *line) {
	return line && line->Effect.get() == effect_name;
}

bool IsSource(AssFile const& file, AssDialogue const *line) {
	return IsEffect(line) && Data(file, *line).has_value();
}

std::string Label(AssFile const& file, AssDialogue const& line) {
	auto document = Load(file, line);
	if (!document) return {};
	wxString label = document->text;
	label.Replace("\n", " / ");
	if (label.length() > 80) label = label.Left(77) + "...";
	return from_wx(label);
}

std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line) {
	if (!IsSource(file, &line)) return {};
	auto document = Data(file, line);
	return document ? ClipboardMarker(*document) : std::string();
}

bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line) {
	std::string text = line.Text.get();
	auto document = TakeClipboardMarker(text);
	line.Text = std::move(text);
	if (!document || !IsEffect(&line)) return false;
	// Reject a marker which is not a valid textbox document before it becomes group metadata.
	try {
		std::istringstream stream(*document);
		json::UnknownElement value;
		json::Reader::Read(value, stream);
		(void)static_cast<json::Object const&>(value);
	}
	catch (...) { return false; }
	file.SetExtradataValue(line, data_key, *document);
	auto ids = line.ExtradataIds.get();
	std::sort(ids.begin(), ids.end());
	line.ExtradataIds = std::move(ids);
	return true;
}

std::vector<AssDialogue> Generate(agi::Context *c, AssDialogue const& prototype,
	Document const& document, std::vector<LayoutRow> *returned_layout) {
	auto rows = Layout(c, prototype, document);
	if (returned_layout) *returned_layout = rows;
	auto box = GetContentBox(document);
	double left = box.left;
	double right = box.right;
	double top = box.top;
	std::string preserved_tags = PreservedTags(prototype);
	AssStyle fallback;
	AssStyle const *ass_style = c->ass->GetStyle(prototype.Style.get());
	TextStyle style_default = FromAssStyle(ass_style ? *ass_style : fallback);
	PerspectiveTags plane;
	if (document.transformed) {
		Vector2D corners[4];
		Corners(document, corners);
		double width = std::max(1.0, static_cast<double>(
			std::abs(document.bottom_right.X() - document.top_left.X())));
		double height = std::max(1.0, static_cast<double>(
			std::abs(document.bottom_right.Y() - document.top_left.Y())));
		int script_w = 1, script_h = 1, layout_w = 1, layout_h = 1;
		c->ass->GetResolution(script_w, script_h);
		c->ass->GetEffectiveLayoutResolution(c, layout_w, layout_h);
		plane = SolvePerspective(corners, 7, Vector2D(), Vector2D(width, height),
			Vector2D(static_cast<float>(script_w) / std::max(1, layout_w),
				static_cast<float>(script_h) / std::max(1, layout_h)), Vector2D());
	}

	std::vector<AssDialogue> output;
	output.reserve(rows.size());
	for (auto const& row : rows) {
		wxString row_text = document.text.Mid(row.start, row.visible_end - row.start);
		if (row_text.Trim(true).Trim(false).empty()) continue;
		TextStyle const& first_style = StyleAt(document, row.start);
		if (document.transformed && plane.ok) {
			ProjectedStyle first = ProjectStyle(first_style, plane);
			double local_left = std::min(document.top_left.X(), document.bottom_right.X());
			double local_top = std::min(document.top_left.Y(), document.bottom_right.Y());
			double offset_x = row.carets.front() - local_left;
			double offset_y = row.y - local_top;
			Vector2D offset(
				static_cast<float>(plane.scale.X() / 100.0 * (offset_x + offset_y * plane.shear_x)),
				static_cast<float>(plane.scale.Y() / 100.0 * (offset_x * plane.shear_y + offset_y)));
			Vector2D position = plane.pos + offset;
			std::string tags = "\\an7\\q2\\pos(" + FormatNumber(position.X()) + "," +
				FormatNumber(position.Y()) + ")";
			bool rotated = std::abs(plane.angle_z) > .0005 ||
				std::abs(plane.angle_x) > .0005 || std::abs(plane.angle_y) > .0005;
			if (rotated)
				tags += "\\org(" + FormatNumber(plane.org.X()) + "," +
					FormatNumber(plane.org.Y()) + ")";
			if (std::abs(plane.angle_z) > .0005) tags += "\\frz" + FormatNumber(plane.angle_z);
			if (std::abs(plane.angle_x) > .0005) tags += "\\frx" + FormatNumber(plane.angle_x);
			if (std::abs(plane.angle_y) > .0005) tags += "\\fry" + FormatNumber(plane.angle_y);
			tags += preserved_tags;
			tags += StyleDeltaTags(style_default, first.style);
			if (row.letter_spacing > 0.0)
				tags += "\\fsp" + FormatNumber(JustifiedSpacing(
					first_style, row.letter_spacing));
			ProjectedStyle neutral{first.style};
			tags += ShearDelta(neutral, first);
			std::string text = "{" + tags + "}";
			ProjectedStyle running = first;
			for (size_t i = row.start; i < row.visible_end; ++i) {
				ProjectedStyle style = ProjectStyle(StyleAt(document, i), plane);
				if (style.style != running.style ||
					std::abs(style.shear_x - running.shear_x) > .0005 ||
					std::abs(style.shear_y - running.shear_y) > .0005) {
					text += "{" + StyleDeltaTags(running.style, style.style) +
						ShearDelta(running, style);
					if (row.letter_spacing > 0.0)
						text += "\\fsp" + FormatNumber(JustifiedSpacing(
							StyleAt(document, i), row.letter_spacing));
					text += "}";
					running = style;
				}
				wxString character = document.text.Mid(i, 1);
				if (row.word_spacing > 0.0 && IsSpace(document.text[i]))
					text += "{\\fsp" + FormatNumber(JustifiedSpacing(
						StyleAt(document, i), row.letter_spacing + row.word_spacing)) + "}" +
						EscapeCharacter(character) + "{\\fsp" + FormatNumber(JustifiedSpacing(
							StyleAt(document, i), row.letter_spacing)) + "}";
				else text += EscapeCharacter(character);
			}
			AssDialogue generated(prototype);
			generated.Comment = false;
			generated.Effect = effect_name;
			generated.ExtradataIds = std::vector<uint32_t>();
			generated.Text = std::move(text);
			output.push_back(std::move(generated));
			continue;
		}
		int an = document.alignment == Alignment::Centre ? 8 :
			document.alignment == Alignment::Right ? 9 : 7;
		double x = an == 8 ? (left + right) * .5 : an == 9 ? right : left;
		std::string tags = agi::format("\\an%d\\q2\\pos(%s,%s)", an,
			FormatNumber(x), FormatNumber(row.y));
		tags += preserved_tags;
		tags += StyleDeltaTags(style_default, first_style);
		if (row.letter_spacing > 0.0)
			tags += "\\fsp" + FormatNumber(JustifiedSpacing(first_style, row.letter_spacing));
		std::string text = "{" + tags + "}";
		TextStyle running = first_style;
		for (size_t i = row.start; i < row.visible_end; ++i) {
			TextStyle const& style = StyleAt(document, i);
			if (style != running) {
				text += "{" + StyleDeltaTags(running, style);
				if (row.letter_spacing > 0.0)
					text += "\\fsp" + FormatNumber(JustifiedSpacing(style, row.letter_spacing));
				text += "}";
				running = style;
			}
			wxString character = document.text.Mid(i, 1);
			if (row.word_spacing > 0.0 && IsSpace(document.text[i]))
				text += "{\\fsp" + FormatNumber(JustifiedSpacing(
					style, row.letter_spacing + row.word_spacing)) + "}" +
					EscapeCharacter(character) + "{\\fsp" + FormatNumber(
						JustifiedSpacing(style, row.letter_spacing)) + "}";
			else
				text += EscapeCharacter(character);
		}

		AssDialogue generated(prototype);
		generated.Comment = false;
		generated.Effect = effect_name;
		generated.ExtradataIds = std::vector<uint32_t>();
		generated.Text = std::move(text);
		output.push_back(std::move(generated));
	}
	if (output.empty()) {
		AssDialogue generated(prototype);
		generated.Comment = false;
		generated.Effect = effect_name;
		generated.ExtradataIds = std::vector<uint32_t>();
		generated.Text = "{\\an7\\q2\\pos(" + FormatNumber(left) + "," +
			FormatNumber(top) + ")}\u200B";
		output.push_back(std::move(generated));
	}
	return output;
}

AssDialogue *Apply(agi::Context *c, AssDialogue const& prototype,
	std::vector<AssDialogue *> originals, Document const& document) {
	if (originals.empty()) return nullptr;
	std::sort(originals.begin(), originals.end(), [](AssDialogue const *left, AssDialogue const *right) {
		return left->Row < right->Row;
	});
	originals.erase(std::unique(originals.begin(), originals.end()), originals.end());
	auto generated = Generate(c, prototype, document);
	if (generated.empty()) return nullptr;
	auto insert_at = c->ass->Events.iterator_to(*originals.front());
	Selection selection;
	AssDialogue *anchor = nullptr;
	for (size_t i = 0; i < generated.size(); ++i) {
		auto line = new AssDialogue(generated[i]);
		if (i == 0) {
			c->ass->SetExtradataValue(*line, data_key, Serialize(document));
			auto ids = line->ExtradataIds.get();
			std::sort(ids.begin(), ids.end());
			line->ExtradataIds = std::move(ids);
			anchor = line;
		}
		c->ass->Events.insert(insert_at, *line);
		selection.insert(line);
	}
	std::vector<std::unique_ptr<AssDialogue>> removed;
	removed.reserve(originals.size());
	for (auto line : originals) {
		c->ass->Events.erase(c->ass->Events.iterator_to(*line));
		removed.emplace_back(line);
	}
	c->selectionController->SetSelectionAndActive(std::move(selection), anchor);
	c->ass->CleanExtradata();
	c->ass->Commit(_("apply text box"), AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return anchor;
}

} // namespace typesetting::textbox
