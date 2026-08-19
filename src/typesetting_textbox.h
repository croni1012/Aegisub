// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#pragma once

#include "vector2d.h"

#include <libaegisub/color.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <wx/string.h>

class AssDialogue;
class AssFile;
namespace agi { struct Context; }

namespace typesetting::textbox {

constexpr char effect_name[] = "textbox-fx";
constexpr char data_key[] = "aegisub/textbox";

enum class Alignment {
	Left,
	Centre,
	Right,
	Justified
};

struct TextStyle {
	std::string font = "Arial";
	double size = 48.0;
	double scale_x = 100.0;
	double scale_y = 100.0;
	double spacing = 0.0;
	double border = 2.0;
	double shadow = 2.0;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	agi::Color primary{255, 255, 255};
	agi::Color outline{0, 0, 0};
	agi::Color shadow_colour{0, 0, 0};
};

bool operator==(TextStyle const& left, TextStyle const& right);
inline bool operator!=(TextStyle const& left, TextStyle const& right) {
	return !(left == right);
}

struct Document {
	wxString text;
	TextStyle base_style;
	std::vector<TextStyle> styles;
	Vector2D top_left;
	Vector2D bottom_right;
	Alignment alignment = Alignment::Left;
	double line_spacing = 0.0;
	double padding = 0.0;
	/// The visible boundary after a free transform or distort. The text is still laid out
	/// in the axis-aligned local rectangle above and projected into this quadrilateral.
	std::array<Vector2D, 4> quad{};
	bool transformed = false;
};

bool operator==(Document const& left, Document const& right);
inline bool operator!=(Document const& left, Document const& right) {
	return !(left == right);
}

struct LayoutRow {
	size_t start = 0;
	size_t end = 0;
	/// End of visible content. Soft-wrap whitespace belongs to the document but
	/// must not consume or receive justification width at the right edge.
	size_t visible_end = 0;
	double y = 0.0;
	double height = 0.0;
	double width = 0.0;
	double word_spacing = 0.0;
	double letter_spacing = 0.0;
	bool paragraph_last = true;
	std::vector<double> carets;
	/// Visual bounds for each logical character. Populated by the interactive
	/// editor when bidirectional layout makes a selection non-contiguous.
	std::vector<std::pair<double, double>> character_bounds;
};

Document FromSelection(agi::Context *c, std::vector<AssDialogue *> const& lines);
std::optional<Document> Load(AssFile const& file, AssDialogue const& line);
std::string Serialize(Document const& document);

/// The visible corners of the document, ordered top-left, top-right, bottom-right,
/// bottom-left. An untransformed document derives these from its local rectangle.
void Corners(Document const& document, Vector2D out[4]);
void SetCorners(Document& document, Vector2D const corners[4]);

bool IsSource(AssFile const& file, AssDialogue const *line);
bool IsEffect(AssDialogue const *line);
std::string Label(AssFile const& file, AssDialogue const& line);

/// Carry the editable textbox document through the plain-text ASS clipboard format.
std::string ClipboardMetadata(AssFile const& file, AssDialogue const& line);
/// Remove the clipboard marker and recreate the file-local textbox extradata.
bool RestoreClipboardMetadata(AssFile& file, AssDialogue& line);

std::vector<AssDialogue> Generate(agi::Context *c, AssDialogue const& prototype,
	Document const& document, std::vector<LayoutRow> *layout = nullptr);

/// Replace @p originals with the generated rows and return the new anchor.
AssDialogue *Apply(agi::Context *c, AssDialogue const& prototype,
	std::vector<AssDialogue *> originals, Document const& document);

} // namespace typesetting::textbox
