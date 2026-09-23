// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file typesetting_transform.cpp
/// @brief Reshaping the drawings of the selected lines

#include "typesetting_transform.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "subtitle_line_combiner.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "text_to_shape.h"

#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

namespace typesetting {
namespace {

/// The parameter list of an override tag, as the visual tools spell it.
typedef const std::vector<AssOverrideParameter> * param_vec;

/// One command of a drawing, with its points already in absolute script
/// coordinates. Only the commands a text outline is made of are handled; a drawing
/// using the b-spline commands is refused rather than mangled.
struct Segment {
	char command;                  ///< 'm', 'n', 'l' or 'b'
	std::vector<Vector2D> points;  ///< one for m/n/l, three for b
};

/// A drawing block of one line, ready to be transformed and written back.
struct Drawing {
	AssDialogue *line = nullptr;
	size_t block_index = 0;        ///< which block of the parsed line it came from
	int scale = 1;                 ///< the \p<n> the block was written with
	Vector2D origin;               ///< the line's position, added into the points
	std::vector<Segment> segments;
	Vector2D start;                ///< pen position while parsing
};

double ScaleDivisor(int scale) {
	return static_cast<double>(1 << std::max(0, scale - 1));
}

/// The turn a line asks for, in degrees: what it says itself, and what the style behind it says
/// when it says nothing.
double LineAngle(const agi::Context *c, AssDialogue *diag) {
	double angle = 0;
	if (AssStyle *style = c->ass->GetStyle(diag->Style)) angle = style->angle;
	auto blocks = diag->ParseTags();
	for (auto ovr : blocks | agi::of_type<AssDialogueBlockOverride>())
		for (auto const& tag : ovr->Tags)
			if ((tag.Name == "\frz" || tag.Name == "\fr") && !tag.Params.empty())
				angle = tag.Params[0].Get<double>(angle);
	return angle;
}

/// Where a line sits, so its drawing can be read in absolute coordinates.
///
/// The same rules the visual tools use: an explicit \pos or \move if there is one,
/// otherwise the position the alignment and the margins put it at.
Vector2D LinePosition(const agi::Context *c, AssDialogue *diag) {
	auto blocks = diag->ParseTags();

	auto vec_or_bad = [](param_vec tag) -> Vector2D {
		if (!tag || tag->size() < 2 || (*tag)[0].omitted || (*tag)[1].omitted)
			return Vector2D();
		return Vector2D((*tag)[0].Get<float>(), (*tag)[1].Get<float>());
	};

	auto find = [&](const char *name) -> param_vec {
		for (auto ovr : blocks | agi::of_type<AssDialogueBlockOverride>())
			for (auto const& tag : ovr->Tags)
				if (tag.Name == name) return &tag.Params;
		return nullptr;
	};

	if (Vector2D at = vec_or_bad(find("\\pos"))) return at;
	// A line on a run is where the run has got to on the frame on screen, rather than where it
	// began. What is bent is what can be seen, so it is that point the drawing is read from - and
	// the shape it becomes then stands there, since numbers that hold the turn cannot be carried
	// along a run without the run turning them too.
	{
		auto placed = text_to_shape::WherePlaced(c, diag);
		if (placed.moving) return placed.at;
	}

	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);

	auto margin = diag->Margin;
	int align = 2;
	if (AssStyle *style = c->ass->GetStyle(diag->Style)) {
		align = style->alignment;
		for (int i = 0; i < 3; ++i)
			if (margin[i] == 0) margin[i] = style->Margin[i];
	}

	if (auto tag = find("\\an")) {
		int value = (*tag)[0].Get<int>(align);
		if (value > 0 && value <= 9) align = value;
	}
	else if (auto tag = find("\\a")) {
		int value = AssStyle::SsaToAss((*tag)[0].Get<int>(2));
		if (value > 0 && value <= 9) align = value;
	}

	int horizontal = (align - 1) % 3;
	int vertical = (align - 1) / 3;

	float x = horizontal == 0 ? margin[0] :
		horizontal == 1 ? (script_w + margin[0] - margin[1]) / 2.f :
		script_w - margin[1];
	float y = vertical == 0 ? script_h - margin[2] :
		vertical == 1 ? script_h / 2.f : (float)margin[2];

	return Vector2D(x, y);
}

/// Read one drawing block into segments in absolute script coordinates.
/// Returns false for a drawing this cannot represent faithfully.
bool ParseDrawing(std::string const& text, int scale, Vector2D origin,
                  std::vector<Segment>& out) {
	double divisor = ScaleDivisor(scale);
	std::vector<double> numbers;
	char command = 0;

	auto flush = [&]() -> bool {
		if (!command) return true;
		size_t per = command == 'b' ? 6 : 2;
		if (numbers.size() < per || numbers.size() % per) return false;
		for (size_t at = 0; at < numbers.size(); at += per) {
			Segment segment;
			segment.command = command;
			for (size_t i = 0; i < per; i += 2)
				segment.points.push_back(origin +
					Vector2D((float)(numbers[at + i] / divisor),
					         (float)(numbers[at + i + 1] / divisor)));
			out.push_back(std::move(segment));
			// Only the first of a repeated m/n starts a contour; the rest are the
			// same command again, which is how a drawing repeats points.
		}
		numbers.clear();
		return true;
	};

	size_t at = 0;
	while (at < text.size()) {
		char ch = text[at];
		if (std::isspace(static_cast<unsigned char>(ch))) { ++at; continue; }

		if (std::isalpha(static_cast<unsigned char>(ch))) {
			if (!flush()) return false;
			// s, p and c describe b-splines, which this does not model. Refusing is
			// better than silently turning them into something else.
			if (ch != 'm' && ch != 'n' && ch != 'l' && ch != 'b') return false;
			command = ch;
			++at;
			continue;
		}

		size_t end = at;
		while (end < text.size() &&
			(std::isdigit(static_cast<unsigned char>(text[end])) ||
			 text[end] == '-' || text[end] == '+' || text[end] == '.'))
			++end;
		if (end == at) return false;
		try { numbers.push_back(std::stod(text.substr(at, end - at))); }
		catch (...) { return false; }
		at = end;
	}

	return flush() && !out.empty();
}

std::string FormatNumber(double value) {
	std::string text = agi::format("%.3f", value);
	// Trailing zeroes are noise in a drawing, and drawings get long.
	auto dot = text.find('.');
	if (dot != std::string::npos) {
		while (!text.empty() && text.back() == '0') text.pop_back();
		if (!text.empty() && text.back() == '.') text.pop_back();
	}
	return text.empty() ? "0" : text;
}

std::string EmitDrawing(std::vector<Segment> const& segments, int scale, Vector2D origin) {
	double divisor = ScaleDivisor(scale);
	std::string out;
	char previous = 0;
	for (auto const& segment : segments) {
		if (segment.command != previous) {
			if (!out.empty()) out += ' ';
			out += segment.command;
			previous = segment.command;
		}
		for (auto point : segment.points) {
			out += ' ';
			out += FormatNumber((point.X() - origin.X()) * divisor);
			out += ' ';
			out += FormatNumber((point.Y() - origin.Y()) * divisor);
		}
	}
	return out;
}

/// What a style would apply to a drawing a second time, and so has to be said away in the line.
///
/// Only these. A drawing written in absolute coordinates already holds its scale, its turn and
/// the widening of its border, so a style that says none of those needs nothing said back to it -
/// and writing \fscx100\fscy100\bord0\shad0 regardless only makes every line longer and less
/// readable for a reader who then has to work out that they mean nothing.
struct StyleForces {
	bool scale_x = false;
	bool scale_y = false;
	bool angle = false;
	bool border = false;
	bool shadow = false;
};

StyleForces WhatStyleForces(const agi::Context *c, AssDialogue const *line) {
	StyleForces out;
	if (!c || !line) return out;
	AssStyle const *style = c->ass->GetStyle(line->Style);
	if (!style) return out;
	out.scale_x = std::abs(style->scalex - 100.0) > 1e-6;
	out.scale_y = std::abs(style->scaley - 100.0) > 1e-6;
	out.angle = std::abs(style->angle) > 1e-6;
	out.border = style->outline_w > 1e-6;
	out.shadow = style->shadow_w > 1e-6;
	return out;
}

/// Move a line's \pos to a point and align it from the middle, so that a drawing written
/// around its own centre lands exactly where it was.
///
/// Done on the text rather than through the tag objects because that is what has to come
/// out at the end anyway, and because a line that never had a \pos needs one inserting.
std::string HangFromCentre(std::string const& text, Vector2D centre, bool baked = false,
                           StyleForces forced = {}) {
	// Anchored from the top left, because for a drawing that means its own origin lands on
	// the position - so coordinates written around the centre put the centre exactly there.
	// Centring the box instead (an5) shifts it by half its size, since the renderer has
	// already counted the box once.
	std::string anchor = agi::format("\\an7\\pos(%s,%s)",
		FormatNumber(centre.X()), FormatNumber(centre.Y()));
	// Once the scale and the turn are in the coordinates, saying them again would apply them
	// twice. Taking the tags out is not enough for the three a style can also say - but only for
	// those it actually does say.
	if (baked) {
		std::string undone;
		if (forced.scale_x) undone += "\\fscx100";
		if (forced.scale_y) undone += "\\fscy100";
		if (forced.angle) undone += "\\frz0";
		anchor = undone + anchor;
	}

	// Wherever in the line they were written, since a drawing only ever has the one block
	// worth reading and a line that animates any of this was left alone before it got here.
	std::string body = text;
	if (baked) {
		// The three rotations before \fr on its own, or taking that one out first would leave
		// the letter of the others behind.
		body = std::regex_replace(body, std::regex(R"(\\fr[xyz]?-?[\d.]*)"), "");
		body = std::regex_replace(body, std::regex(R"(\\fsc[xy]-?[\d.]*)"), "");
		body = std::regex_replace(body, std::regex(R"(\\fa[xy]-?[\d.]*)"), "");
		body = std::regex_replace(body, std::regex(R"(\\org\([^)]*\))"), "");
	}

	// Only the first tag block is touched by the rest: that is where the position of a line
	// lives, and a \pos later in the line is ignored by the renderers anyway.
	if (body.empty() || body.front() != '{')
		return "{" + anchor + "}" + body;

	size_t close = body.find('}');
	if (close == std::string::npos) return "{" + anchor + "}" + body;

	std::string tags = body.substr(1, close - 1);
	tags = std::regex_replace(tags, std::regex(R"(\\pos\([^)]*\))"), "");
	// And the run gives way to the place: the shape was bent where the run had got to, and its
	// numbers hold the turn, so being carried on would turn it as well. It stands where it was bent.
	tags = std::regex_replace(tags, std::regex(R"(\\move\([^)]*\))"), "");
	tags = std::regex_replace(tags, std::regex(R"(\\an\d*)"), "");
	tags = std::regex_replace(tags, std::regex(R"(\\a\d+)"), "");

	return "{" + tags + anchor + "}" + body.substr(close + 1);
}

bool IsDecorationTag(std::string const& name) {
	return name == "\\bord" || name == "\\xbord" || name == "\\ybord" ||
		name == "\\shad" || name == "\\xshad" || name == "\\yshad" ||
		name == "\\3c" || name == "\\4c" || name == "\\3a" || name == "\\4a";
}

void StripDecorationTags(AssDialogueBlockOverride& block) {
	for (auto& tag : block.Tags)
		for (auto& parameter : tag.Params)
			if (parameter.GetType() == VariableDataType::BLOCK)
				StripDecorationTags(*parameter.Get<AssDialogueBlockOverride*>());

	block.Tags.erase(std::remove_if(block.Tags.begin(), block.Tags.end(),
		[](AssOverrideTag const& tag) { return IsDecorationTag(tag.Name); }),
		block.Tags.end());
}

/// A decoration layer is already the fill, widened and/or shifted into the pixels which the
/// renderer would have painted. Native border and shadow tags would paint those pixels again,
/// and their colours and alpha no longer affect anything, so remove all of them and neutralise
/// the style before choosing the layer's colour.
std::string PaintDecorationShape(std::string text, std::string const& paint,
                                 StyleForces forced = {}) {
	AssDialogue line;
	line.Text = std::move(text);
	auto blocks = line.ParseTags();
	for (auto& block : blocks)
		if (block->GetType() == AssBlockType::OVERRIDE)
			StripDecorationTags(*static_cast<AssDialogueBlockOverride*>(block.get()));
	line.UpdateText(blocks);
	text = line.Text.get();

	size_t close = text.find('}');
	// The line's own border and shadow have just been taken out above; these turn off what the
	// style would put back, and are worth saying only where the style says something.
	std::string tags;
	if (forced.border) tags += "\\bord0";
	if (forced.shadow) tags += "\\shad0";
	tags += paint;
	if (tags.empty()) return text;
	if (close == std::string::npos) return "{" + tags + "}" + text;
	text.insert(close, tags);
	return text;
}

// ---------------------------------------------------------------------- the clips

/// The same mapping applied to a clip.
///
/// A clip is given in script coordinates like everything else, so it belongs to the picture
/// and has to bend with it: a gradient written as a stack of clipped copies falls apart
/// otherwise, every band still cutting where the straight text used to be.
///
/// A bent rectangle is no longer a rectangle, so the two-corner form has to become a drawing -
/// and a straight edge has to be cut up before it can follow a curve, since mapping its two
/// ends would only move the ends.
std::string MapClipBody(std::string const& body, PointMap const& map,
                        OrientedBox const& box, double span, bool projective);
/// Close every contour, then cut every straight run and every curve into pieces short enough
/// to follow one.
std::vector<Segment> Subdivide(std::vector<Segment> const& given, double span);
std::vector<Segment> SubdivideProjectiveCurves(std::vector<Segment> const& segments,
                                              PointMap const& map);

std::string MapClips(std::string const& text, PointMap const& map, OrientedBox const& box,
                     double span, bool projective = false) {
	std::string out;
	std::regex pattern(R"(\\(i?clip)\(([^)]*)\))");
	auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
	auto end = std::sregex_iterator();
	size_t last = 0;
	for (auto at = begin; at != end; ++at) {
		auto const& found = *at;
		std::string mapped = MapClipBody(found[2].str(), map, box, span, projective);
		if (mapped.empty()) continue;
		out += text.substr(last, found.position(0) - last);
		out += "\\" + found[1].str() + "(" + mapped + ")";
		last = found.position(0) + found.length(0);
	}
	if (last == 0) return text;
	out += text.substr(last);
	return out;
}

/// A convex shape cut down to a box, in the box's own frame: Sutherland and Hodgman's method,
/// one side at a time. Fewer than three points come back when nothing is left.
std::vector<Vector2D> CutToBox(std::vector<Vector2D> shape, OrientedBox const& box,
                               Vector2D margin) {
	Vector2D limit = box.half + margin;
	for (auto& point : shape) point = box.ToLocal(point);

	for (int side = 0; side < 4 && shape.size() >= 3; ++side) {
		bool vertical = side < 2;
		float wanted = side == 0 ? -limit.X() : side == 1 ? limit.X() :
			side == 2 ? -limit.Y() : limit.Y();
		bool keep_above = side == 0 || side == 2;
		auto inside = [&](Vector2D point) {
			float value = vertical ? point.X() : point.Y();
			return keep_above ? value >= wanted : value <= wanted;
		};
		auto crossing = [&](Vector2D from, Vector2D to) {
			float start = vertical ? from.X() : from.Y();
			float end = vertical ? to.X() : to.Y();
			float along = std::abs(end - start) < 1e-9f ? 0.f : (wanted - start) / (end - start);
			return from + (to - from) * along;
		};

		std::vector<Vector2D> kept;
		kept.reserve(shape.size() + 4);
		for (size_t at = 0; at < shape.size(); ++at) {
			Vector2D current = shape[at];
			Vector2D next = shape[(at + 1) % shape.size()];
			bool in_now = inside(current), in_next = inside(next);
			if (in_now) kept.push_back(current);
			if (in_now != in_next) kept.push_back(crossing(current, next));
		}
		shape = std::move(kept);
	}

	for (auto& point : shape) point = box.ToScript(point);
	return shape;
}

std::string MapClipBody(std::string const& body, PointMap const& map,
                        OrientedBox const& box, double span, bool projective) {
	std::vector<std::string> parts;
	size_t at = 0;
	while (true) {
		size_t comma = body.find(',', at);
		parts.push_back(body.substr(at, comma == std::string::npos ?
			std::string::npos : comma - at));
		if (comma == std::string::npos) break;
		at = comma + 1;
	}

	auto number = [](std::string const& text, double& into) {
		try {
			size_t used = 0;
			double value = std::stod(text, &used);
			while (used < text.size() && std::isspace((unsigned char)text[used])) ++used;
			if (used != text.size()) return false;
			into = value;
			return true;
		}
		catch (...) { return false; }
	};

	int scale = 1;
	std::vector<Segment> segments;

	double corner[4] = {0, 0, 0, 0};
	if (parts.size() == 4 && number(parts[0], corner[0]) && number(parts[1], corner[1]) &&
		number(parts[2], corner[2]) && number(parts[3], corner[3])) {
		Vector2D low((float)std::min(corner[0], corner[2]), (float)std::min(corner[1], corner[3]));
		Vector2D high((float)std::max(corner[0], corner[2]), (float)std::max(corner[1], corner[3]));
		if (!std::isfinite(low.X()) || !std::isfinite(low.Y()) ||
			!std::isfinite(high.X()) || !std::isfinite(high.Y())) return {};

		if (!projective) {
			// A rectangular clip snaps to whole pixels; a drawing one has soft edges. Two bands of
			// a gradient that shared an edge would each cover it half way and leave a seam down the
			// middle, so the band is grown by half a unit on screen - which here means half a unit
			// divided by however much the mapping magnifies this part of the picture, and never
			// more than half the band itself, or one band would swallow the next.
			Vector2D here = map(low);
			float grow_x = (map(low + Vector2D(1.f, 0.f)) - here).Len();
			float grow_y = (map(low + Vector2D(0.f, 1.f)) - here).Len();
			Vector2D pad(
				std::min((high.X() - low.X()) * .5f, .5f / std::max(grow_x, 1e-6f)),
				std::min((high.Y() - low.Y()) * .5f, .5f / std::max(grow_y, 1e-6f)));
			low = low - pad;
			high = high + pad;
		}

		// The part of the band that is nowhere near the shapes cuts nothing, and a bend is only
		// a bend inside the box it was worked out on - a few hundred units out, which is where
		// the ends of a gradient band sit, the same curve carries on into nonsense. Cutting it
		// away first leaves the picture as it was and every coordinate where it belongs. The
		// margin leaves room for what spreads beyond the shape itself - a border, a glow.
		std::vector<Vector2D> shape = {low, Vector2D(high.X(), low.Y()), high,
		                               Vector2D(low.X(), high.Y())};
		if (!projective)
			shape = CutToBox(std::move(shape), box, box.half * .5f + Vector2D(48.f, 48.f));
		if (shape.size() < 3) return {};

		for (size_t i = 0; i < shape.size(); ++i)
			segments.push_back({i == 0 ? 'm' : 'l', {shape[i]}});
		// And back to where it started, so that the closing edge is cut into pieces along with
		// the rest of them rather than left as one straight line across the bend.
		if (!projective) segments.push_back({'l', {shape[0]}});
	}
	else if (parts.size() == 1) {
		if (!ParseDrawing(parts[0], 1, Vector2D(0.f, 0.f), segments)) return {};
	}
	else if (parts.size() == 2) {
		double given = 1;
		if (!number(parts[0], given)) return {};
		scale = std::max(1, (int)std::lround(given));
		if (!ParseDrawing(parts[1], scale, Vector2D(0.f, 0.f), segments)) return {};
	}
	else return {};

	segments = projective ? SubdivideProjectiveCurves(segments, map) : Subdivide(segments, span);
	for (auto& segment : segments)
		for (auto& point : segment.points) {
			point = map(point);
			if (!std::isfinite(point.X()) || !std::isfinite(point.Y()) ||
				std::abs(point.X()) > 1e5f || std::abs(point.Y()) > 1e5f) return {};
		}

	// Keep axis-aligned rectangles rectangular, including at the reference frame.
	if (projective && parts.size() == 4 && segments.size() == 4) {
		auto a = segments[0].points[0], b = segments[1].points[0];
		auto c = segments[2].points[0], d = segments[3].points[0];
		if ((a.Y() == b.Y() && b.X() == c.X() && c.Y() == d.Y() && d.X() == a.X()) ||
			(a.X() == b.X() && b.Y() == c.Y() && c.X() == d.X() && d.Y() == a.Y()))
			return FormatNumber(std::min(a.X(), c.X())) + "," +
				FormatNumber(std::min(a.Y(), c.Y())) + "," +
				FormatNumber(std::max(a.X(), c.X())) + "," +
				FormatNumber(std::max(a.Y(), c.Y()));
	}
	std::string drawing = EmitDrawing(segments, scale, Vector2D(0.f, 0.f));
	if (drawing.empty()) return {};
	return scale == 1 ? drawing : agi::format("%d,%s", scale, drawing);
}

// ------------------------------------------------------------------ subdivision

/// Split a cubic at t, keeping the piece before it and leaving the rest in place.
void SplitCubic(Vector2D p0, Vector2D& p1, Vector2D& p2, Vector2D& p3, double t,
                Vector2D& q1, Vector2D& q2, Vector2D& q3) {
	auto lerp = [t](Vector2D a, Vector2D b) {
		return a + (b - a) * static_cast<float>(t);
	};
	Vector2D a = lerp(p0, p1), b = lerp(p1, p2), c = lerp(p2, p3);
	Vector2D d = lerp(a, b), e = lerp(b, c);
	Vector2D f = lerp(d, e);
	q1 = a; q2 = d; q3 = f;
	p1 = e; p2 = c;
	// p3 stays where it was; the tail runs from f.
}

std::vector<Segment> SubdivideProjectiveCurves(std::vector<Segment> const& segments,
                                              PointMap const& map) {
	std::vector<Segment> out;
	auto cubic = [](Vector2D a, Vector2D b, Vector2D c, Vector2D d, float t) {
		float s = 1 - t;
		return a * (s*s*s) + b * (3*s*s*t) + c * (3*s*t*t) + d * (t*t*t);
	};
	auto append = [&](auto&& self, Vector2D a, Vector2D b, Vector2D c, Vector2D d, int depth) -> void {
		// Affine maps preserve a cubic exactly. Perspective produces a rational cubic;
		// subdivide only where moving its controls exceeds 0.1 script pixels of error.
		bool split = false;
		for (float t : {.25f, .5f, .75f}) {
			Vector2D error = map(cubic(a, b, c, d, t)) - cubic(map(a), map(b), map(c), map(d), t);
			if (error.SquareLen() > .01f) split = true;
		}
		if (!split || depth == 8) {
			out.push_back({'b', {b, c, d}});
			return;
		}
		Vector2D q1, q2, q3;
		SplitCubic(a, b, c, d, .5, q1, q2, q3);
		self(self, a, q1, q2, q3, depth + 1);
		self(self, q3, b, c, d, depth + 1);
	};
	Vector2D pen;
	for (auto const& segment : segments) {
		if (segment.command == 'b' && segment.points.size() == 3)
			append(append, pen, segment.points[0], segment.points[1], segment.points[2], 0);
		else
			out.push_back(segment);
		if (!segment.points.empty()) pen = segment.points.back();
	}
	return out;
}

/// Cut every curve into short enough pieces that mapping its control points is a
/// good approximation of mapping the curve.
///
/// A nonlinear map does not send a cubic to a cubic, so moving the four control
/// points is only ever an approximation - but the shorter the piece, the smaller the
/// error, and subdividing keeps the drawing compact where flattening to straight
/// lines would blow it up.
/// Write out the edge every contour closes itself with.
///
/// A drawing joins the last point of a contour back to its first one on its own, and that edge
/// is nowhere in the numbers. Mapping the numbers therefore moves both of its ends and leaves
/// the edge itself straight - a chord across the bend, while every edge that was written out
/// follows it. Putting it in makes it an edge like the others; the shape it describes is the
/// same one the renderer was already drawing.
std::vector<Segment> CloseContours(std::vector<Segment> const& segments) {
	std::vector<Segment> out;
	out.reserve(segments.size() + 4);

	Vector2D start, pen;
	size_t drawn = 0;
	bool closes = false;

	auto finish = [&]() {
		if (closes && drawn && (pen - start).Len() > 1e-4) out.push_back({'l', {start}});
	};

	for (auto const& segment : segments) {
		if (segment.points.empty()) continue;
		if (segment.command == 'm' || segment.command == 'n') {
			finish();
			start = segment.points.back();
			drawn = 0;
			// An open contour is left open: the renderer joins it up for the fill, but where it
			// would draw that edge is its own business.
			closes = segment.command == 'm';
		}
		else ++drawn;
		out.push_back(segment);
		pen = segment.points.back();
	}
	finish();

	return out;
}

std::vector<Segment> Subdivide(std::vector<Segment> const& given, double span) {
	std::vector<Segment> segments = CloseContours(given);
	std::vector<Segment> out;
	Vector2D pen;
	for (auto const& segment : segments) {
		// A straight run stays straight however it is mapped, so a long one has to be
		// broken into a polyline first or it will cut across the curve instead of
		// following it. Short ones - which is most of a glyph outline - are left alone.
		if (segment.command == 'l' && segment.points.size() == 1 && pen) {
			Vector2D target = segment.points[0];
			double reach = std::max(std::abs(target.X() - pen.X()),
			                       std::abs(target.Y() - pen.Y()));
			int pieces = span > 0 ?
				std::clamp((int)std::ceil(reach / span * 16.0), 1, 16) : 1;
			for (int piece = 1; piece <= pieces; ++piece)
				out.push_back({'l', {pen + (target - pen) * ((float)piece / pieces)}});
			pen = target;
			continue;
		}

		if (segment.command != 'b' || segment.points.size() != 3) {
			out.push_back(segment);
			if (!segment.points.empty()) pen = segment.points.back();
			continue;
		}

		Vector2D p0 = pen, p1 = segment.points[0], p2 = segment.points[1], p3 = segment.points[2];
		double extent = std::max({std::abs(p1.X() - p0.X()), std::abs(p2.X() - p0.X()),
			std::abs(p3.X() - p0.X())});
		int pieces = span > 0 ?
			std::clamp((int)std::ceil(extent / span * 16.0), 1, 16) : 1;

		for (int piece = 0; piece < pieces; ++piece) {
			if (piece + 1 == pieces) {
				out.push_back({'b', {p1, p2, p3}});
				break;
			}
			// Split off one piece of the remaining curve.
			double t = 1.0 / (pieces - piece);
			Vector2D q1, q2, q3;
			SplitCubic(p0, p1, p2, p3, t, q1, q2, q3);
			out.push_back({'b', {q1, q2, q3}});
			p0 = q3;
		}
		pen = p3;
	}
	return out;
}


// ----------------------------------------------------------------- folding over

/// Whether the mapping turns this part of the plane inside out, and how strongly.
///
/// Two short steps out of the point, and which way round their turn comes out the other side:
/// where the surface has folded over itself, the turn changes hand. Divided by the two steps, so
/// what comes back is about one where nothing is stretched, and the sign survives a step that had
/// to be taken backwards.
///
/// The steps are taken along the box's own axes and kept inside it. Outside the box the warp holds
/// still - it clamps what it is asked for - so a step across that edge answers with a turn of
/// nothing, and the edge is exactly where this gets asked: the box is the shape's own bounding box.
double FoldTurn(PointMap const& map, OrientedBox const& box, Vector2D at, double step) {
	Vector2D local = box.ToLocal(at);
	double dx = local.X() + step > box.half.X() ? -step : step;
	double dy = local.Y() + step > box.half.Y() ? -step : step;

	Vector2D here = map(at);
	Vector2D along = map(box.ToScript(local + Vector2D((float)dx, 0.f))) - here;
	Vector2D down = map(box.ToScript(local + Vector2D(0.f, (float)dy))) - here;
	double turn = (double)along.X() * down.Y() - (double)along.Y() * down.X();
	return turn / (dx * dy);
}

/// Whether a point is inside a contour, by the crossings of a ray.
bool InsideWalk(std::vector<Vector2D> const& walk, Vector2D point) {
	bool in = false;
	for (size_t at = 0; at < walk.size(); ++at) {
		Vector2D one = walk[at], other = walk[(at + 1) % walk.size()];
		if ((one.Y() <= point.Y()) == (other.Y() <= point.Y())) continue;
		float along = (point.Y() - one.Y()) / (other.Y() - one.Y());
		if (one.X() + (other.X() - one.X()) * along > point.X()) in = !in;
	}
	return in;
}

/// Where two segments cross: how far along each of them, and the point. False when they do not.
bool Meet(Vector2D from, Vector2D to, Vector2D other_from, Vector2D other_to,
          double& along, double& across, Vector2D& point) {
	Vector2D run = to - from, other = other_to - other_from;
	double bottom = (double)run.X() * other.Y() - (double)run.Y() * other.X();
	if (std::abs(bottom) < 1e-12) return false;

	Vector2D gap = other_from - from;
	double one = ((double)gap.X() * other.Y() - (double)gap.Y() * other.X()) / bottom;
	double two = ((double)gap.X() * run.Y() - (double)gap.Y() * run.X()) / bottom;
	if (one < 0 || one > 1 || two < 0 || two > 1) return false;

	along = one;
	across = two;
	point = from + run * (float)one;
	return true;
}

/// The fold: where the mapping turns the plane inside out, as the zero line of that turn.
///
/// Taken off a grid over the box by marching squares rather than followed a step at a time. There
/// is nothing here to converge, nothing to set off in the wrong direction and nothing to walk out
/// of the shape: the turn is read at every node of the grid, the line is picked up where it crosses
/// the grid's own edges, and the pieces are chained by which edge they share - which is exact,
/// since an edge is named rather than measured.
///
/// What comes back is one polyline per branch of the fold, in script coordinates, closed on itself
/// where a branch is a loop. Nothing at all when the mapping does not fold: that is the gate.
/// How finely the fold is looked for across the box. The grid resolves the fold to about one
/// cell, which is what makes it too coarse to cut a shape only a few cells wide.
const int fold_cells = 48;

/// Whether the mapping turns the plane over anywhere over the box.
///
/// Only that. Where it does, a sheet has a back as well as a front and has to be written out as a
/// mesh; where it does not, nothing can be hollowed out and every contour goes through exactly as
/// it stands, curves and all. Where the turn changes over does not matter here - only that it
/// changes somewhere - so the answer comes off a coarse grid, and off the first two readings that
/// disagree.
bool TurnsOver(PointMap const& map, OrientedBox const& box, double step, int cells) {
	bool one_way = false, the_other = false;
	for (int j = 0; j <= cells; ++j)
		for (int i = 0; i <= cells; ++i) {
			Vector2D at = box.ToScript(Vector2D(
				(float)(-box.half.X() + 2.0 * box.half.X() * i / cells),
				(float)(-box.half.Y() + 2.0 * box.half.Y() * j / cells)));
			if (FoldTurn(map, box, at, step) > 0) one_way = true;
			else the_other = true;
			if (one_way && the_other) return true;
		}
	return false;
}

std::vector<std::vector<Vector2D>> FoldLines(PointMap const& map, OrientedBox const& box,
                                             double step, int cells) {
	int nodes = cells + 1;
	auto place = [&](int i, int j) {
		return box.ToScript(Vector2D(
			(float)(-box.half.X() + 2.0 * box.half.X() * i / cells),
			(float)(-box.half.Y() + 2.0 * box.half.Y() * j / cells)));
	};

	std::vector<double> value((size_t)nodes * nodes);
	for (int j = 0; j < nodes; ++j)
		for (int i = 0; i < nodes; ++i)
			value[(size_t)j * nodes + i] = FoldTurn(map, box, place(i, j), step);

	// Every grid edge the line crosses, named by the edge: the two cells that share it then agree
	// exactly rather than to within rounding.
	size_t ends = (size_t)2 * nodes * nodes;
	std::vector<Vector2D> spot(ends);
	std::vector<char> crossed(ends, 0);
	auto flat = [&](int i, int j, bool upright) { return (size_t)2 * (j * nodes + i) + upright; };

	auto cut = [&](int i, int j, int other_i, int other_j, bool upright) {
		double one = value[(size_t)j * nodes + i];
		double two = value[(size_t)other_j * nodes + other_i];
		if ((one > 0) == (two > 0)) return;
		double along = one / (one - two);
		Vector2D from = place(i, j), to = place(other_i, other_j);
		size_t id = flat(i, j, upright);
		spot[id] = from + (to - from) * (float)along;
		crossed[id] = 1;
	};

	for (int j = 0; j < nodes; ++j)
		for (int i = 0; i < cells; ++i) cut(i, j, i + 1, j, false);
	for (int j = 0; j < cells; ++j)
		for (int i = 0; i < nodes; ++i) cut(i, j, i, j + 1, true);

	// One chord per cell the line passes through, two where it passes through twice.
	std::vector<size_t> first(ends, ends), second(ends, ends);
	auto join = [&](size_t one, size_t other) {
		(first[one] == ends ? first[one] : second[one]) = other;
		(first[other] == ends ? first[other] : second[other]) = one;
	};

	for (int j = 0; j < cells; ++j)
		for (int i = 0; i < cells; ++i) {
			size_t bottom = flat(i, j, false), top = flat(i, j + 1, false);
			size_t left = flat(i, j, true), right = flat(i + 1, j, true);
			size_t here[4];
			int count = 0;
			for (size_t id : {bottom, right, top, left})
				if (crossed[id]) here[count++] = id;

			if (count == 2) join(here[0], here[1]);
			else if (count == 4) {
				// A saddle: the middle of the cell says which way the two chords run.
				double middle = (value[(size_t)j * nodes + i] +
					value[(size_t)j * nodes + i + 1] +
					value[(size_t)(j + 1) * nodes + i] +
					value[(size_t)(j + 1) * nodes + i + 1]) / 4;
				if ((middle > 0) == (value[(size_t)j * nodes + i] > 0)) {
					join(bottom, right);
					join(top, left);
				}
				else {
					join(bottom, left);
					join(top, right);
				}
			}
		}

	// Chained: from an open end where there is one, round a loop where there is not.
	std::vector<std::vector<Vector2D>> out;
	std::vector<char> taken(ends, 0);
	auto chain = [&](size_t from, bool loop) {
		std::vector<Vector2D> walk;
		size_t at = from;
		while (at != ends && !taken[at]) {
			taken[at] = 1;
			walk.push_back(spot[at]);
			size_t step_to = first[at] != ends && !taken[first[at]] ? first[at] :
				(second[at] != ends && !taken[second[at]] ? second[at] : ends);
			at = step_to;
		}
		if (loop && walk.size() > 2) walk.push_back(walk.front());
		if (walk.size() > 1) out.push_back(std::move(walk));
	};

	for (size_t id = 0; id < ends; ++id)
		if (crossed[id] && !taken[id] && first[id] != ends && second[id] == ends)
			chain(id, false);
	for (size_t id = 0; id < ends; ++id)
		if (crossed[id] && !taken[id] && first[id] != ends) chain(id, true);

	return out;
}

/// The same contour walked the other way round. A curve keeps its shape: its two control
/// points swap, because the first of them belongs to whichever end the curve leaves.
std::vector<Segment> ReverseContour(std::vector<Segment> const& contour) {
	if (contour.size() < 2) return contour;

	std::vector<Vector2D> stops;
	stops.reserve(contour.size());
	for (auto const& segment : contour) {
		if (segment.points.empty()) return contour;
		stops.push_back(segment.points.back());
	}

	std::vector<Segment> out;
	out.reserve(contour.size());
	out.push_back({contour.front().command, {stops.back()}});
	for (size_t at = contour.size(); at-- > 1;) {
		Segment const& segment = contour[at];
		if (segment.command == 'b' && segment.points.size() == 3)
			out.push_back({'b', {segment.points[1], segment.points[0], stops[at - 1]}});
		else
			out.push_back({segment.command, {stops[at - 1]}});
	}
	return out;
}

/// One shape put through the mapping, cut along the fold so that nothing cancels itself.
///
/// Where the mapping folds, the shape comes back lying over itself, and the part behind the fold
/// has its edges running the other way. The renderer counts how many times its edges wind round a
/// pixel and fills what does not come to nothing - so two parts wound opposite ways cancel exactly
/// where they overlap, and one of them shows as a hole.
///
/// So the contour is cut where the fold crosses it, and what comes out - the faces of a little map
/// whose edges are the arcs of the contour and the pieces of the fold between them - is handed over
/// as a contour apiece, turned round where its own side of the fold says so. Two faces share a
/// piece of the fold exactly, so between them they still cover the shape that was there; and since
/// they are all wound the same way, overlapping counts twice rather than not at all.
///
/// A contour the fold misses needs none of that: it is only turned round if it lies on the far
/// side. And if the crossings cannot be paired up, the contour goes through whole - the plain hole
/// again, which is better than a wrong cut.
/// Where a closed ring of points crosses itself, and the simple loops it falls into there.
///
/// A mapping that folds the plane over sends a shape's outline across itself. Under the fill rule
/// the renderer uses - non-zero winding, which is what libass accumulates and clamps - the part
/// that came back the other way round counts as minus one where the rest counts as plus one, and
/// the two cancel: that is the hole. Cut at the crossings the ring falls into loops that each keep
/// to one side of themselves, and wound the same way they add up instead of cancelling.
///
/// Deliberately read off the ring itself and nowhere else. The fold can also be found in the plane
/// the shape came from and the shape cut along it, which is what used to happen here - but that
/// line is only ever known as a polyline sampled over a grid, so points that belong to the grid
/// rather than to the shape ended up in the outline, and moved about in it as the grid shifted.
std::vector<std::vector<Vector2D>> SimpleLoops(std::vector<Vector2D> const& ring) {
	size_t count = ring.size();
	if (count < 4) return {ring};

	// The crossings, found on the pairs of edges that could possibly meet: bucketed by where they
	// lie, so a long outline does not cost every edge against every other.
	struct Split {
		double along = 0;   ///< how far along the edge it sits
		Vector2D point;
		int name = 0;       ///< the crossing it is, shared with the other edge
	};
	std::vector<std::vector<Split>> splits(count);

	Vector2D low = ring.front(), high = ring.front();
	for (auto point : ring) { low = low.Min(point); high = high.Max(point); }
	Vector2D reach = high - low;
	double side = std::max({(double)reach.X(), (double)reach.Y(), 1.0}) /
		std::max<size_t>(1, (size_t)std::sqrt((double)count / 4.0) + 1);
	auto cell_of = [&](Vector2D point) {
		return std::pair<int, int>((int)std::floor((point.X() - low.X()) / side),
		                           (int)std::floor((point.Y() - low.Y()) / side));
	};

	std::map<std::pair<int, int>, std::vector<size_t>> buckets;
	for (size_t k = 0; k < count; ++k) {
		auto one = cell_of(ring[k]), other = cell_of(ring[(k + 1) % count]);
		for (int j = std::min(one.second, other.second); j <= std::max(one.second, other.second); ++j)
			for (int i = std::min(one.first, other.first); i <= std::max(one.first, other.first); ++i)
				buckets[{i, j}].push_back(k);
	}

	int named = 0;
	auto cross = [&](size_t a, size_t b) {
		Vector2D p = ring[a], q = ring[(a + 1) % count];
		Vector2D r = ring[b], t = ring[(b + 1) % count];
		double denominator = (double)(q.X() - p.X()) * (t.Y() - r.Y()) -
			(double)(q.Y() - p.Y()) * (t.X() - r.X());
		if (std::abs(denominator) < 1e-12) return;
		double one = ((double)(r.X() - p.X()) * (t.Y() - r.Y()) -
			(double)(r.Y() - p.Y()) * (t.X() - r.X())) / denominator;
		double other = ((double)(r.X() - p.X()) * (q.Y() - p.Y()) -
			(double)(r.Y() - p.Y()) * (q.X() - p.X())) / denominator;
		// Strictly inside both, so that two edges merely meeting end to end are not a crossing.
		if (!(one > 1e-9 && one < 1 - 1e-9 && other > 1e-9 && other < 1 - 1e-9)) return;
		Vector2D point = p + (q - p) * (float)one;
		int name = named++;
		splits[a].push_back({one, point, name});
		splits[b].push_back({other, point, name});
	};

	for (auto const& bucket : buckets) {
		auto const& here = bucket.second;
		for (size_t x = 0; x < here.size(); ++x)
			for (size_t y = x + 1; y < here.size(); ++y) {
				size_t a = here[x], b = here[y];
				if (a > b) std::swap(a, b);
				// Neighbours share an end, and the ring's two ends share one as well.
				if (b == a + 1 || (a == 0 && b == count - 1)) continue;
				cross(a, b);
			}
	}
	if (!named) return {ring};

	// A crossing found from both of its edges is one place, so the two namings are made one.
	std::vector<int> same(named);
	for (int k = 0; k < named; ++k) same[k] = k;
	{
		std::vector<std::pair<int, Vector2D>> seen;
		for (auto& edge : splits)
			for (auto& split : edge) {
				bool matched = false;
				for (auto const& before : seen)
					if ((before.second - split.point).Len() < 1e-6) {
						same[split.name] = before.first;
						matched = true;
						break;
					}
				if (!matched) seen.push_back({same[split.name], split.point});
			}
	}

	// The ring again, with every crossing standing in it as a point of its own, in order.
	std::vector<Vector2D> path;
	std::vector<int> name_at;
	for (size_t k = 0; k < count; ++k) {
		path.push_back(ring[k]);
		name_at.push_back(-1);
		auto& edge = splits[k];
		std::sort(edge.begin(), edge.end(),
			[](Split const& one, Split const& other) { return one.along < other.along; });
		for (auto const& split : edge) {
			path.push_back(split.point);
			name_at.push_back(same[split.name]);
		}
	}

	// Walked once through, holding what has been seen. Coming back to a crossing closes off
	// everything since it was last passed, and that piece is a loop that does not cross itself.
	std::vector<std::vector<Vector2D>> loops;
	std::vector<Vector2D> held;
	std::vector<int> held_name;
	std::vector<int> last_at(named, -1);
	for (size_t k = 0; k < path.size(); ++k) {
		int name = name_at[k];
		if (name >= 0 && last_at[name] >= 0) {
			size_t from = (size_t)last_at[name];
			std::vector<Vector2D> loop(held.begin() + from, held.end());
			if (loop.size() >= 3) loops.push_back(std::move(loop));
			for (size_t i = from; i < held.size(); ++i)
				if (held_name[i] >= 0) last_at[held_name[i]] = -1;
			held.resize(from);
			held_name.resize(from);
			last_at[name] = (int)held.size();
			held.push_back(path[k]);
			held_name.push_back(name);
			continue;
		}
		if (name >= 0) last_at[name] = (int)held.size();
		held.push_back(path[k]);
		held_name.push_back(name);
	}
	if (held.size() >= 3) loops.push_back(std::move(held));
	if (loops.empty()) return {ring};
	return loops;
}

std::vector<Segment> MapShape(std::vector<Segment> const& original, PointMap const& map,
                              OrientedBox const& box, bool turns_over,
                              bool subdivide, double span, double step) {
	(void)step;
	std::vector<Segment> pieces = subdivide ? Subdivide(original, span) : original;

	std::vector<Segment> out;

	// Twice the area a ring of points encloses, signed - which way round it is wound.
	auto twice_area = [](std::vector<Vector2D> const& ring) {
		double sum = 0;
		for (size_t k = 0; k < ring.size(); ++k) {
			Vector2D const& one = ring[k];
			Vector2D const& other = ring[(k + 1) % ring.size()];
			sum += (double)one.X() * other.Y() - (double)other.X() * one.Y();
		}
		return sum;
	};

	// A sheet laid out as a mesh: every small piece of it written as a closed shape of its own,
	// wound the way the sheet was wound.
	//
	// This is what gives a bent sheet a back as well as a front. Where the mapping turns the plane
	// over, the far half of the sheet comes home wound the other way round, and the fill rule the
	// renderer uses - non-zero winding, which libass accumulates and clamps - has it take away
	// what the near half puts down. The sheet goes hollow exactly there. Written as a mesh there
	// is no far half to speak of: each piece is small enough to lie flat, each is turned to face
	// the same way, and where two of them come to rest on top of one another that counts twice
	// over rather than not at all. Front and back both paint.
	//
	// Text never showed this because a line of text is hundreds of separate letter outlines, each
	// far too small to fold within itself. A backdrop is one four-cornered sheet, and half of it
	// turns over at once.
	auto as_mesh = [&](std::vector<Vector2D> const& walk, double wanted, char opening) {
		Vector2D middle(0.f, 0.f);
		for (auto point : walk) middle = middle + point;
		middle = middle / (float)walk.size();

		Vector2D low = walk.front(), high = walk.front();
		for (auto point : walk) { low = low.Min(point); high = high.Max(point); }
		Vector2D reach = high - low;
		// Small enough that the mapping does not turn over within one piece of it.
		double least = std::max(std::max((double)reach.X(), (double)reach.Y()) / 24.0, 1.0);

		std::vector<std::array<Vector2D, 3>> mesh;
		mesh.reserve(walk.size() * 4);
		for (size_t k = 0; k < walk.size(); ++k)
			mesh.push_back({middle, walk[k], walk[(k + 1) % walk.size()]});

		// Split four ways at a time, which keeps the pieces meeting edge to edge - splitting only
		// the big ones would leave a corner of one against the middle of another, and the seam
		// between them would open as the mapping pulled them apart.
		for (int pass = 0; pass < 5 && mesh.size() < 3000; ++pass) {
			bool too_big = false;
			for (auto const& piece : mesh) {
				double side = std::max(std::max((piece[1] - piece[0]).Len(),
					(piece[2] - piece[1]).Len()), (piece[0] - piece[2]).Len());
				if (side > least) { too_big = true; break; }
			}
			if (!too_big) break;

			std::vector<std::array<Vector2D, 3>> finer;
			finer.reserve(mesh.size() * 4);
			for (auto const& piece : mesh) {
				Vector2D one = (piece[0] + piece[1]) / 2;
				Vector2D other = (piece[1] + piece[2]) / 2;
				Vector2D third = (piece[2] + piece[0]) / 2;
				finer.push_back({piece[0], one, third});
				finer.push_back({one, piece[1], other});
				finer.push_back({third, other, piece[2]});
				finer.push_back({one, other, third});
			}
			mesh = std::move(finer);
		}

		for (auto const& piece : mesh) {
			Vector2D moved[3] = {map(piece[0]), map(piece[1]), map(piece[2])};
			double sum = 0;
			for (int k = 0; k < 3; ++k) {
				Vector2D const& one = moved[k];
				Vector2D const& other = moved[(k + 1) % 3];
				sum += (double)one.X() * other.Y() - (double)other.X() * one.Y();
			}
			// Nothing to paint, and nothing to get the facing of either.
			if (!(std::abs(sum) > 1e-9)) continue;
			bool backwards = sum * wanted < 0;
			for (int k = 0; k < 3; ++k)
				out.push_back({k ? 'l' : opening, {moved[backwards ? 2 - k : k]}});
		}
	};

	size_t at = 0;
	while (at < pieces.size()) {
		size_t past = at + 1;
		while (past < pieces.size() && pieces[past].command != 'm' &&
			pieces[past].command != 'n') ++past;
		char opening = pieces[at].command;

		auto straight_through = [&]() {
			for (size_t i = at; i < past; ++i) {
				Segment segment = pieces[i];
				for (auto& point : segment.points) point = map(point);
				out.push_back(std::move(segment));
			}
		};

		// Nowhere over the box does the mapping turn over, so nothing can be hollowed out and the
		// contour goes through as it stands - curves and all.
		if (!turns_over) { straight_through(); at = past; continue; }

		std::vector<Vector2D> walk;
		bool readable = opening == 'm' && !pieces[at].points.empty();
		if (readable) {
			walk.push_back(pieces[at].points.back());
			for (size_t i = at + 1; i < past; ++i) {
				if (pieces[i].points.empty()) { readable = false; break; }
				walk.push_back(pieces[i].points.back());
			}
		}
		// A closure written out is a closure implied from here on.
		if (readable && walk.size() > 1 && (walk.back() - walk.front()).Len() < 1e-9) walk.pop_back();

		// Only something big enough to fold within itself is worth laying out as a mesh - which
		// is measured against the box rather than counted in points, because how many points a
		// contour has says nothing at all once it has been subdivided.
		//
		// A backdrop covers most of what is being bent, so the fold runs right through it and the
		// far half turns over. A letter is a fraction of it, and lands wholly on one side.
		bool worth_it = false;
		double wanted = 0;
		if (readable && walk.size() >= 3 && walk.size() <= 4096) {
			Vector2D low = walk.front(), high = walk.front();
			for (auto point : walk) { low = low.Min(point); high = high.Max(point); }
			double across = (high - low).Len();
			double box_across = Vector2D(box.half.X() * 2, box.half.Y() * 2).Len();
			wanted = twice_area(walk);
			worth_it = across > box_across * .25 && std::abs(wanted) > 1e-6;
		}
		if (!worth_it) { straight_through(); at = past; continue; }

		as_mesh(walk, wanted, opening);
		at = past;
	}
	return out;
}

// -------------------------------------------------------------- the oriented box

/// The convex hull of a set of points, anticlockwise, by monotone chain.
std::vector<Vector2D> ConvexHull(std::vector<Vector2D> points) {
	if (points.size() < 3) return points;

	std::sort(points.begin(), points.end(), [](Vector2D a, Vector2D b) {
		return a.X() < b.X() || (a.X() == b.X() && a.Y() < b.Y());
	});
	points.erase(std::unique(points.begin(), points.end(),
		[](Vector2D a, Vector2D b) { return a.X() == b.X() && a.Y() == b.Y(); }),
		points.end());
	if (points.size() < 3) return points;

	auto cross = [](Vector2D o, Vector2D a, Vector2D b) {
		return (double)(a.X() - o.X()) * (b.Y() - o.Y()) -
		       (double)(a.Y() - o.Y()) * (b.X() - o.X());
	};

	std::vector<Vector2D> hull(points.size() * 2);
	size_t count = 0;
	for (auto point : points) {
		while (count >= 2 && cross(hull[count - 2], hull[count - 1], point) <= 0) --count;
		hull[count++] = point;
	}
	size_t lower = count + 1;
	for (size_t i = points.size() - 1; i-- > 0;) {
		while (count >= lower && cross(hull[count - 2], hull[count - 1], points[i]) <= 0)
			--count;
		hull[count++] = points[i];
	}
	hull.resize(count ? count - 1 : 0);
	return hull;
}

/// The smallest box that contains the points, at whatever angle it needs.
///
/// Rotating calipers: the smallest such box always has a side lying along an edge of
/// the convex hull, so trying every hull edge as the box's axis finds it.
OrientedBox SmallestBox(std::vector<Vector2D> const& points) {
	OrientedBox box;
	if (points.empty()) return box;

	auto hull = ConvexHull(points);
	if (hull.size() < 3) {
		// A point or a line: no angle worth finding, so leave it upright.
		Vector2D low = points.front(), high = points.front();
		for (auto point : points) { low = low.Min(point); high = high.Max(point); }
		box.centre = (low + high) / 2;
		box.half = (high - low) / 2;
		return box;
	}

	double best_area = -1;
	for (size_t i = 0; i < hull.size(); ++i) {
		Vector2D edge = hull[(i + 1) % hull.size()] - hull[i];
		double length = edge.Len();
		if (length < 1e-9) continue;
		double ux = edge.X() / length, uy = edge.Y() / length;

		double min_u = 0, max_u = 0, min_v = 0, max_v = 0;
		bool first = true;
		for (auto point : hull) {
			double u = point.X() * ux + point.Y() * uy;
			double v = -point.X() * uy + point.Y() * ux;
			if (first) { min_u = max_u = u; min_v = max_v = v; first = false; }
			else {
				min_u = std::min(min_u, u); max_u = std::max(max_u, u);
				min_v = std::min(min_v, v); max_v = std::max(max_v, v);
			}
		}

		double area = (max_u - min_u) * (max_v - min_v);
		if (best_area >= 0 && area >= best_area) continue;

		best_area = area;
		double cu = (min_u + max_u) / 2, cv = (min_v + max_v) / 2;
		box.centre = Vector2D((float)(cu * ux - cv * uy), (float)(cu * uy + cv * ux));
		box.half = Vector2D((float)((max_u - min_u) / 2), (float)((max_v - min_v) / 2));
		box.angle = (float)(std::atan2(uy, ux) * 180.0 / 3.14159265358979);
	}

	// A line of text is wider than it is tall, and the axis found above may have come
	// out along the short side. Turning it a quarter keeps "along the text" meaning the
	// box's own x, which is what the arch bends along.
	if (box.half.X() < box.half.Y()) {
		box.half = Vector2D(box.half.Y(), box.half.X());
		box.angle += 90;
	}
	// And keep the angle in a range where "upright" reads as zero rather than 180.
	while (box.angle > 90) box.angle -= 180;
	while (box.angle < -90) box.angle += 180;

	return box;
}

// ------------------------------------------------------------- collecting shapes

/// One drawing of the selection, with the original kept so a mapping can be applied
/// to it again and again.
struct Collected {
	Drawing drawing;
	std::vector<Segment> original;
	std::vector<Segment> subdivided;
	std::vector<Segment> widened;
	std::vector<Segment> widened_subdivided;
	bool widened_ready = false;
};

/// Everything needed to rebuild one line of output: where it starts from, and which of its
/// blocks are drawings this can reshape.
///
/// One line of the file can turn into several of these, because text that changes font
/// partway through becomes a drawing per piece.
struct LineWork {
	AssDialogue *line = nullptr;   ///< the line of the file this came from
	std::string source;      ///< the line's own text, or what a piece of it would say
	bool converted = false;  ///< whether `source` came from converting text
	/// Whether the line's own scale and turn were taken out of the drawing's numbers, and so
	/// have to be neutralised on the line the result goes on.
	bool baked = false;
	std::vector<Collected> shapes;
	std::string result;      ///< `source` with the mapped drawings, hung from their centre
	/// The border and the shadow of the same line, as shapes, when they were asked for. Written in
	/// front of the result so that they are drawn under it.
	std::string border_result;
	std::string shadow_result;
	/// Whether the fill is painted the border's own colour, in which case the widened shape holds it
	/// already and the result itself is left out.
	bool covered = false;
	text_to_shape::DecorPaint paint;
	bool decorations_ready = false;
	bool want_border = false;
	bool want_shadow = false;
};

/// Flatten a cubic into a few straight pieces, for drawing the preview.
void FlattenCubic(std::vector<Vector2D>& into, Vector2D p0, Vector2D p1, Vector2D p2,
                  Vector2D p3) {
	// As many steps as the curve's own size asks for, rather than a fixed count for all of them. The
	// control polygon is longer than the curve and bows further from the chord than it does, so what
	// it asks for is on the safe side - and a short curve stops costing what a long one does.
	double bow = (p1 - p0).Len() + (p2 - p1).Len() + (p3 - p2).Len();
	const int steps = std::min(std::max((int)std::ceil(std::sqrt(bow)), 2), 8);
	for (int step = 1; step <= steps; ++step) {
		double t = (double)step / steps, s = 1 - t;
		into.push_back(p0 * (float)(s * s * s) + p1 * (float)(3 * t * s * s) +
			p2 * (float)(3 * t * t * s) + p3 * (float)(t * t * t));
	}
}

/// A drawing's segments as closed rings of points, which is what widening works on.
std::vector<std::vector<Vector2D>> RingsOf(std::vector<Segment> const& segments) {
	std::vector<std::vector<Vector2D>> out;
	std::vector<Vector2D> ring;
	for (auto const& segment : segments) {
		if (segment.points.empty()) continue;
		if (segment.command == 'm' || segment.command == 'n') {
			if (ring.size() > 2) out.push_back(ring);
			ring.clear();
			ring.push_back(segment.points.back());
		}
		else if (segment.command == 'b' && segment.points.size() == 3 && !ring.empty())
			FlattenCubic(ring, ring.back(), segment.points[0], segment.points[1],
				segment.points[2]);
		else
			ring.push_back(segment.points.back());
	}
	if (ring.size() > 2) out.push_back(ring);

	// A ring that closes on itself says the same point twice, and a widening reads every point as a
	// corner - so the repeat would put a second pen mark on top of the first.
	for (auto& closed : out)
		while (closed.size() > 3 && (closed.back() - closed.front()).Len() < 1e-4)
			closed.pop_back();

	// And points finer than a border can show go as well. Each of them costs a band and a wedge, and
	// costs them again for every mapping a drag applies; dropping one moves the outline by less than
	// it was drawn to, and only where the line through it is nearly straight anyway.
	for (auto& closed : out) {
		for (size_t i = closed.size(); i-- > 1;) {
			if (closed.size() <= 4) break;
			Vector2D step = closed[i] - closed[i - 1];
			double back = step.Len();
			if (back > .2 || i + 1 >= closed.size()) continue;
			Vector2D onward = closed[i + 1] - closed[i];
			double along = onward.Len();
			if (back < 1e-6) { closed.erase(closed.begin() + i); continue; }
			if (along < 1e-6) continue;
			double turn = std::abs((double)step.X() * onward.Y() - (double)step.Y() * onward.X()) /
				(back * along);
			if (turn >= .05) continue;
			closed.erase(closed.begin() + i);
			// The neighbour stays, whatever it looks like: a run of these would move the outline by
			// the sum of the steps rather than by one of them.
			if (i > 1) --i;
		}
	}
	return out;
}

/// And back again, as straight runs: a widened shape has no curves left in it anyway.
std::vector<Segment> SegmentsOf(std::vector<std::vector<Vector2D>> const& rings) {
	std::vector<Segment> out;
	for (auto const& ring : rings) {
		if (ring.size() < 3) continue;
		out.push_back({'m', {ring.front()}});
		for (size_t i = 1; i < ring.size(); ++i) out.push_back({'l', {ring[i]}});
	}
	return out;
}

} // namespace

std::string TransformClips(std::string const& text, PointMap const& map,
	OrientedBox const& bounds, double subdivision_span) {
	return MapClips(text, map, bounds, subdivision_span);
}

std::string TransformProjectiveClips(std::string const& text, PointMap const& map) {
	return MapClips(text, map, {}, 0, true);
}

Vector2D OrientedBox::ToScript(Vector2D local) const {
	double radians = angle * 3.14159265358979 / 180.0;
	double sine = std::sin(radians), cosine = std::cos(radians);
	return centre + Vector2D((float)(local.X() * cosine - local.Y() * sine),
	                         (float)(local.X() * sine + local.Y() * cosine));
}

Vector2D OrientedBox::ToLocal(Vector2D point) const {
	double radians = -angle * 3.14159265358979 / 180.0;
	double sine = std::sin(radians), cosine = std::cos(radians);
	Vector2D offset = point - centre;
	return Vector2D((float)(offset.X() * cosine - offset.Y() * sine),
	                (float)(offset.X() * sine + offset.Y() * cosine));
}

void OrientedBox::Corners(Vector2D out[4]) const {
	out[0] = ToScript(Vector2D(-half.X(), -half.Y()));
	out[1] = ToScript(Vector2D(half.X(), -half.Y()));
	out[2] = ToScript(Vector2D(half.X(), half.Y()));
	out[3] = ToScript(Vector2D(-half.X(), half.Y()));
}

struct ShapeEditor::Impl {
	agi::Context *c = nullptr;
	std::vector<LineWork> lines;
	std::vector<AssDialogue *> line_list;
	std::vector<AssDialogue *> applied;
	std::vector<std::string> refusals;
	std::vector<std::vector<Vector2D>> contours;
	std::vector<ShapeLayer> layers;
	OrientedBox box;
	OrientedBox content_box;
	int refused = 0;
	size_t shape_count = 0;
};

ShapeEditor::ShapeEditor(const agi::Context *c)
: ShapeEditor(c, c->selectionController->GetSortedSelection()) {
}

ShapeEditor::ShapeEditor(const agi::Context *c, std::vector<AssDialogue *> const& lines)
: impl(std::make_shared<Impl>()) {
	impl->c = const_cast<agi::Context *>(c);
	std::vector<Vector2D> all_points;

	// Text becomes a drawing here and nowhere else: in memory, so the line keeps saying
	// what it said until the reshaping is accepted.
	auto converted = text_to_shape::ConvertLines(c, lines, impl->refusals);

	for (auto line : lines) {
		// The rectangle an image is pinned to is not a shape to be reshaped.
		if (IsImageMaskLine(line)) continue;

		// A line that was converted comes back as one entry per piece; one that was already
		// a drawing is a single piece that is its own text.
		std::vector<std::string> sources;
		for (auto const& item : converted)
			if (item.line == line) sources.push_back(item.text);
		bool from_text = !sources.empty();
		if (!from_text) sources.push_back(line->Text.get());

		for (auto const& source : sources) {
		LineWork work;
		work.line = line;
		work.source = source;
		work.converted = from_text;

		// Parsed on a copy, because the starting text may be the conversion rather than
		// what the line actually says.
		AssDialogue reading(*line);
		reading.Text = work.source;
		Vector2D origin = LinePosition(c, &reading);
		auto blocks = reading.ParseTags();

		// Where a drawing sits is not what its numbers say: the alignment hangs it by the box
		// those numbers make, and the line's own scale, lean and turn are applied afterwards.
		// So the numbers are read on their own first, measured, and only then put where the
		// picture really is - otherwise the bend would be worked out somewhere the shape is
		// not. Only for a line that is nothing but one drawing: with text beside it the
		// renderer walks a pen along the row, and where the drawing lands then is a different
		// question.
		size_t drawing_blocks = 0;
		bool alone = true;
		for (auto& block : blocks) {
			if (block->GetType() == AssBlockType::DRAWING) ++drawing_blocks;
			else if (block->GetType() == AssBlockType::PLAIN &&
				block->GetText().find_first_not_of(" \t") != std::string::npos)
				alone = false;
		}

		text_to_shape::DrawingPlace place;
		if (drawing_blocks == 1 && alone) {
			for (auto& block : blocks) {
				if (block->GetType() != AssBlockType::DRAWING) continue;
				auto reading_block = static_cast<AssDialogueBlockDrawing*>(block.get());
				std::vector<Segment> plain;
				if (!ParseDrawing(reading_block->GetText(), reading_block->Scale,
					Vector2D(0.f, 0.f), plain)) break;

				Vector2D low, high;
				bool first_point = true;
				for (auto const& segment : plain)
					for (auto point : segment.points) {
						if (first_point) { low = high = point; first_point = false; }
						else { low = low.Min(point); high = high.Max(point); }
					}
				if (first_point) break;
				place = text_to_shape::PlaceDrawing(c, &reading, low, high);
				break;
			}
		}
		work.baked = place.ok && place.baked;

		for (size_t index = 0; index < blocks.size(); ++index) {
			if (blocks[index]->GetType() != AssBlockType::DRAWING) continue;
			auto block = static_cast<AssDialogueBlockDrawing*>(blocks[index].get());

			Drawing drawing;
			drawing.line = line;
			drawing.block_index = index;
			drawing.scale = block->Scale;
			drawing.origin = origin;
			if (!ParseDrawing(block->GetText(), drawing.scale,
				place.ok ? Vector2D(0.f, 0.f) : origin, drawing.segments)) {
				++impl->refused;
				continue;
			}
			if (place.ok)
				for (auto& segment : drawing.segments)
					for (auto& point : segment.points) point = place.map(point);

			for (auto const& segment : drawing.segments)
				for (auto point : segment.points)
					all_points.push_back(point);

			Collected collected;
			collected.original = drawing.segments;
			collected.drawing = std::move(drawing);
			work.shapes.push_back(std::move(collected));
		}

		impl->shape_count += work.shapes.size();
		if (!work.shapes.empty()) {
			if (std::find(impl->line_list.begin(), impl->line_list.end(), work.line) ==
				impl->line_list.end())
				impl->line_list.push_back(work.line);
			impl->lines.push_back(std::move(work));
		}
		}
	}

	impl->box = SmallestBox(all_points);
	impl->content_box = impl->box;

	// A floor on the box, because on a small shape the handles would otherwise sit almost
	// on top of one another and the gentlest drag would tear it apart. The mapping is the
	// identity whatever size the box is, so padding it costs nothing.
	int script_w = 0, script_h = 0;
	c->ass->GetResolution(script_w, script_h);
	float floor_size = std::max(script_h * 0.06f, 28.f);
	impl->box.half = impl->box.half.Max(Vector2D(floor_size, floor_size));
}

bool ShapeEditor::ok() const { return impl->shape_count > 0; }
int ShapeEditor::refused() const { return impl->refused; }
OrientedBox ShapeEditor::Box() const { return impl->box; }
OrientedBox ShapeEditor::ContentBox() const { return impl->content_box; }
std::vector<std::string> const& ShapeEditor::refusals() const { return impl->refusals; }
std::vector<std::vector<Vector2D>> const& ShapeEditor::contours() const {
	return impl->contours;
}
std::vector<ShapeEditor::ShapeLayer> const& ShapeEditor::layers() const {
	return impl->layers;
}

std::string ShapeEditor::TextForContours(ShapeLayer const& layer,
		std::vector<std::vector<Vector2D>> const& contours) const {
	if (!layer.source || contours.empty() || layer.text.empty()) return {};
	AssDialogue writing(*layer.source);
	writing.Text = layer.text;
	auto blocks = writing.ParseTags();
	bool written = false;
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::DRAWING) continue;
		auto drawing = static_cast<AssDialogueBlockDrawing*>(block.get());
		if (!written) {
			drawing->text = EmitDrawing(SegmentsOf(contours), drawing->Scale, layer.centre);
			written = !drawing->text.empty();
		}
		else
			drawing->text.clear();
	}
	if (!written) return {};
	writing.UpdateText(blocks);
	return writing.Text.get();
}
std::vector<AssDialogue *> const& ShapeEditor::lines() const { return impl->line_list; }

ShapeEditor::Preview ShapeEditor::PreviewLines() const {
	Preview out;
	for (auto origin : impl->line_list) {
		bool any = false;
		for (auto const& work : impl->lines) {
			if (work.line != origin || work.result.empty()) continue;
			// The shadow first, the border over it, and the fill over both - unless the border is
			// already holding the fill, in which case there is nothing left for it to add.
			for (auto const& text : {work.shadow_result, work.border_result}) {
				if (text.empty()) continue;
				auto extra = std::make_unique<AssDialogue>(*origin);
				extra->Text = text;
				extra->Comment = false;
				out.drawings.push_back(std::move(extra));
				any = true;
			}
			if (work.covered) { any = true; continue; }
			auto drawing = std::make_unique<AssDialogue>(*origin);
			drawing->Text = work.result;
			drawing->Comment = false;
			out.drawings.push_back(std::move(drawing));
			any = true;
		}
		if (!any) continue;

		// The line the drawings came from has to stop being drawn, and a comment is how a
		// line stops being drawn without being taken away.
		auto silenced = std::make_unique<AssDialogue>(*origin);
		silenced->Comment = true;
		out.silenced.push_back(std::move(silenced));
	}
	return out;
}

void ShapeEditor::Build(PointMap const& map, bool subdivide, bool map_clips, bool decorations,
		bool collect_geometry) {
	double span = impl->box.half.X() * 2;
	impl->contours.clear();
	impl->layers.clear();

	// Whether the mapping folds the shape over itself anywhere, which is worth knowing once
	// rather than per point: only then is there a back of the shape to put right.
	// Where the mapping folds the plane over itself, if it does at all: read off a grid over the
	// box once, and shared by every shape below - it depends on the mapping, not on them.
	double step = std::max(span * 1e-3, 1e-2);
	// How finely the fold has to be looked for: fine enough to resolve the thinnest thing being
	// bent. Its place is only known to about one cell, so a ribbon no thicker than a cell or two
	// gains and loses its crossings whole as the grid shifts under it - and its fill flickers,
	// which no amount of filtering afterwards can undo. Thickness is taken as twice the area over
	// the perimeter, which for anything long and thin is its width.
	// Only ever asked one thing: whether the mapping turns the plane over anywhere over the box.
	// Where it does, MapShape looks for the crossings on each contour itself; the line traced out
	// here is never used for that, because a traced line is sampled and a sampled line flickers.
	// The one thing that has to be known before anything is mapped: whether the mapping turns the
	// plane over at all. It nearly never does, and then there is nothing to put right.
	bool turns_over = subdivide && TurnsOver(map, impl->box, step, 24);

	// One line's mapped drawings, waiting for the centre they will be written around.
	struct Mapped {
		size_t block_index;
		int scale;
		std::vector<Segment> segments;
	};
	std::vector<Mapped> mapped;
	// The same again for the widened shape, when the border and the shadow are being stood in for.
	std::vector<Mapped> widened;

	for (auto& work : impl->lines) {
		work.border_result.clear();
		work.shadow_result.clear();
		work.covered = false;
		std::vector<std::vector<Vector2D>> fill_contours;
		std::vector<std::vector<Vector2D>> wide_contours;

		AssDialogue writing(*work.line);
		writing.Text = work.source;
		auto blocks = writing.ParseTags();

		// What this line paints its border and its shadow with, and how wide the pen is. Read off
		// the text the shapes were read from, which for converted text already says what the
		// transform did to the pair - so the numbers are the ones that were on screen.
		text_to_shape::DecorPaint paint;
		bool want_border = false, want_shadow = false;
		if (decorations) {
			if (!work.decorations_ready) {
				AssDialogue reading(*work.line);
				reading.Text = work.source;
				work.paint = text_to_shape::ReadDecorations(impl->c, &reading);
				work.want_border = work.paint.pen.Len() > .01f;
				work.want_shadow = work.paint.offset.Len() > .01f;
				work.decorations_ready = true;
			}
			paint = work.paint;
			want_border = work.want_border;
			want_shadow = work.want_shadow;
			work.covered = want_border && paint.covers;
		}

		// Where the whole line's drawings end up, so that the line can be hung from the
		// middle of them rather than from wherever it used to be anchored.
		Vector2D low, high;
		bool first_point = true;

		for (auto& shape : work.shapes) {
			// Always from the original, never from the last result: a drag applies dozens
			// of mappings, and chaining them would let the shape creep.
			if (subdivide && shape.subdivided.empty())
				shape.subdivided = Subdivide(shape.original, span);
			auto const& source = subdivide ? shape.subdivided : shape.original;
			auto pieces = MapShape(source, map, impl->box, turns_over, false, span, step);

			// And the same shape widened by its own pen, through the same mapping - which is what
			// makes the border bend with the letters instead of being stroked around them.
			//
			// The widening happens in script coordinates because that is the frame the pen is
			// measured in: the renderer divides the border by the scale before stroking, and the
			// scale then puts it back, so a border comes out the width that was asked for however
			// the letters were scaled.
			std::vector<Segment> wide_pieces;
			if (want_border || want_shadow) {
				if (!shape.widened_ready) {
					auto rings = RingsOf(shape.original);
					auto wide = rings;
					if (want_border) {
						auto added = text_to_shape::WidenRings(rings, paint.pen.X(), paint.pen.Y());
						// The shape is part of its own border: what the pen adds is a band around it,
						// and the middle has to stay solid or the seam would show along every edge.
						wide.insert(wide.end(), added.begin(), added.end());
					}
					shape.widened = SegmentsOf(wide);
					shape.widened_ready = true;
				}
				if (subdivide && shape.widened_subdivided.empty())
					shape.widened_subdivided = Subdivide(shape.widened, span);
				auto const& wide_source = subdivide ? shape.widened_subdivided : shape.widened;
				wide_pieces = MapShape(wide_source, map, impl->box, turns_over, false, span,
					step);
			}
			if (collect_geometry) {
				auto rings = RingsOf(pieces);
				fill_contours.insert(fill_contours.end(),
					std::make_move_iterator(rings.begin()), std::make_move_iterator(rings.end()));
			}
			if (collect_geometry && !wide_pieces.empty()) {
				auto rings = RingsOf(wide_pieces);
				wide_contours.insert(wide_contours.end(),
					std::make_move_iterator(rings.begin()), std::make_move_iterator(rings.end()));
			}
			for (auto const& segment : pieces)
				for (auto point : segment.points) {
					if (first_point) { low = high = point; first_point = false; }
					else { low = low.Min(point); high = high.Max(point); }
				}

			// The same points again as plain polylines, which is what gets drawn over the
			// video while the reshaping is only a preview.
			if (collect_geometry) {
				std::vector<Vector2D> contour;
				for (auto const& segment : pieces) {
					if (segment.points.empty()) continue;
					if (segment.command == 'm' || segment.command == 'n') {
						if (contour.size() > 1) impl->contours.push_back(contour);
						contour.clear();
						contour.push_back(segment.points.back());
					}
					else if (segment.command == 'b' && segment.points.size() == 3 &&
						!contour.empty()) {
						FlattenCubic(contour, contour.back(), segment.points[0],
							segment.points[1], segment.points[2]);
					}
					else {
						contour.push_back(segment.points.back());
					}
				}
				if (contour.size() > 1) impl->contours.push_back(contour);
			}

			if (shape.drawing.block_index >= blocks.size()) continue;
			if (blocks[shape.drawing.block_index]->GetType() != AssBlockType::DRAWING) continue;
			mapped.push_back({shape.drawing.block_index, shape.drawing.scale, std::move(pieces)});
			if (!wide_pieces.empty())
				widened.push_back({shape.drawing.block_index, shape.drawing.scale,
					std::move(wide_pieces)});
		}

		// The centre is only known once every drawing of the line has been mapped, and it
		// is what all of their coordinates are then written against.
		Vector2D centre = first_point ? Vector2D(0.f, 0.f) : (low + high) / 2;

		for (auto const& piece : mapped)
			static_cast<AssDialogueBlockDrawing*>(blocks[piece.block_index].get())->text =
				EmitDrawing(piece.segments, piece.scale, centre);
		mapped.clear();

		StyleForces forced = WhatStyleForces(impl->c, work.line);

		writing.UpdateText(blocks);
		work.result = HangFromCentre(writing.Text.get(), centre, work.baked, forced);
		if (decorations)
			work.result = PaintDecorationShape(std::move(work.result), {}, forced);

		// The pair of them, from the widened shape and the same centre. Both are written from the
		// same body: a shadow is what stands behind the bordered shape, so it is that shape again,
		// hung from a point the offset has moved - and since the coordinates are written around the
		// centre, moving where it hangs from moves the whole of it.
		Vector2D shadow_centre = centre;
		std::vector<std::vector<Vector2D>> shadow_contours;
		if (!widened.empty()) {
			for (auto const& piece : widened)
				static_cast<AssDialogueBlockDrawing*>(blocks[piece.block_index].get())->text =
					EmitDrawing(piece.segments, piece.scale, centre);
			widened.clear();
			writing.UpdateText(blocks);
			std::string body = writing.Text.get();

			if (want_border)
				work.border_result = PaintDecorationShape(
					HangFromCentre(body, centre, work.baked, forced),
					paint.border, forced);
			if (want_shadow) {
				// Where the renderer would have put it. The offset is turned by the line's own
				// \frz and by nothing else - it rides in the translation column of the transform -
				// so it is turned here exactly when the turn has been taken out of the numbers.
				Vector2D drift = paint.offset;
				if (work.baked) {
					AssDialogue reading(*work.line);
					reading.Text = work.source;
					double radians = LineAngle(impl->c, &reading) * 3.14159265358979 / 180.0;
					double sine = -std::sin(radians), cosine = std::cos(radians);
					drift = Vector2D(
						(float)(paint.offset.X() * cosine - paint.offset.Y() * sine),
						(float)(paint.offset.X() * sine + paint.offset.Y() * cosine));
				}
				work.shadow_result = PaintDecorationShape(
					HangFromCentre(body, centre + drift, work.baked, forced),
					paint.shadow, forced);
				shadow_centre = centre + drift;
				shadow_contours = wide_contours;
				for (auto& contour : shadow_contours)
					for (auto& point : contour) point = point + drift;
			}
		}
		// The clip belongs to the picture too, and a gradient is a stack of clipped copies -
		// leave the clips where they were and every band goes on cutting where the straight
		// text used to be.
		if (map_clips) {
			work.result = MapClips(work.result, map, impl->box, span);
			if (!work.border_result.empty())
				work.border_result = MapClips(work.border_result, map, impl->box, span);
			if (!work.shadow_result.empty())
				work.shadow_result = MapClips(work.shadow_result, map, impl->box, span);
		}
		if (collect_geometry && !work.shadow_result.empty())
			impl->layers.push_back({work.line, LayerKind::Shadow, work.shadow_result,
				shadow_centre, std::move(shadow_contours), false});
		if (collect_geometry && !work.border_result.empty())
			impl->layers.push_back({work.line, LayerKind::Outline, work.border_result,
				centre, wide_contours, false});
		if (collect_geometry && !work.result.empty())
			impl->layers.push_back({work.line, LayerKind::Primary, work.result,
				centre, std::move(fill_contours), work.covered});
	}
}

void ShapeEditor::Apply() {
	auto& events = impl->c->ass->Events;
	if (impl->line_list.empty()) return;

	// Everything new goes in as one block after the last of the lines it came from, rather than
	// each drawing tucked under its own line: the originals stay where they are as comments, so
	// the group reads as what it was followed by what it became, and the new lines sit together
	// where they can be selected, retimed or moved as one. It is also the order the video was
	// already showing, since the preview draws the new lines after all of the old ones.
	auto at = events.iterator_to(*impl->line_list.back());
	++at;

	for (auto origin : impl->line_list) {
		// The line a drawing came from is kept as a comment rather than taken away: the text is
		// not thrown away, so it can still be read, fixed and converted again.
		bool any = false;
		for (auto& work : impl->lines) {
			if (work.line != origin || work.result.empty()) continue;
			if (!work.converted && origin->Text.get() == work.result &&
				work.border_result.empty() && work.shadow_result.empty()) continue;

			auto put = [&](std::string const& text) {
				auto shape = new AssDialogue(*origin);
				shape->Text = text;
				shape->Comment = false;
				events.insert(at, *shape);
				impl->applied.push_back(shape);
			};
			// The shadow first, the border over it, and the fill over both - which is the order
			// they have to be drawn in, and so the order they go into the file in.
			if (!work.shadow_result.empty()) put(work.shadow_result);
			if (!work.border_result.empty()) put(work.border_result);
			if (!work.covered) put(work.result);
			any = true;
		}
		if (any) origin->Comment = true;
	}
}

std::vector<AssDialogue *> const& ShapeEditor::applied() const { return impl->applied; }

// ------------------------------------------------------------------- the mappings

namespace {

/// The unit square to an arbitrary quadrilateral, in the usual eight coefficients, and the two
/// that say where the map sends the plane to infinity.
struct QuadCoefficients {
	double a = 1, b = 0, c = 0;
	double d = 0, e = 1, f = 0;
	double g = 0, h = 0;
	bool ok = false;
};

QuadCoefficients QuadFrom(Vector2D const corners[4]) {
	QuadCoefficients out;
	// The corners run top left, top right, bottom right, bottom left, so the square's
	// (0,0), (1,0), (1,1), (0,1) line up with them in that order.
	double x0 = corners[0].X(), y0 = corners[0].Y();
	double x1 = corners[1].X(), y1 = corners[1].Y();
	double x2 = corners[2].X(), y2 = corners[2].Y();
	double x3 = corners[3].X(), y3 = corners[3].Y();

	double sx = x0 - x1 + x2 - x3;
	double sy = y0 - y1 + y2 - y3;

	if (std::abs(sx) < 1e-9 && std::abs(sy) < 1e-9) {
		// A parallelogram, so the mapping is affine and the two perspective terms drop out.
		out.a = x1 - x0; out.b = x2 - x1; out.c = x0;
		out.d = y1 - y0; out.e = y2 - y1; out.f = y0;
		out.g = out.h = 0;
		out.ok = true;
		return out;
	}

	double dx1 = x1 - x2, dx2 = x3 - x2;
	double dy1 = y1 - y2, dy2 = y3 - y2;
	double denominator = dx1 * dy2 - dx2 * dy1;
	if (std::abs(denominator) < 1e-9) return out;

	out.g = (sx * dy2 - dx2 * sy) / denominator;
	out.h = (dx1 * sy - sx * dy1) / denominator;
	out.a = x1 - x0 + out.g * x1;
	out.b = x3 - x0 + out.h * x3;
	out.c = x0;
	out.d = y1 - y0 + out.g * y1;
	out.e = y3 - y0 + out.h * y3;
	out.f = y0;
	out.ok = true;
	return out;
}

} // namespace

PointMap QuadMap(OrientedBox const& box, Vector2D const corners[4]) {
	double span_x = box.half.X() * 2;
	double span_y = box.half.Y() * 2;
	if (span_x <= 0 || span_y <= 0)
		return [](Vector2D point) { return point; };

	auto found = QuadFrom(corners);
	if (!found.ok) return [](Vector2D point) { return point; };
	double a = found.a, b = found.b, cc = found.c;
	double d = found.d, e = found.e, f = found.f;
	double g = found.g, h = found.h;

	return [=](Vector2D point) {
		// Into the box's frame first, so the unit square really is the box.
		Vector2D local = box.ToLocal(point);
		double u = (local.X() + box.half.X()) / span_x;
		double v = (local.Y() + box.half.Y()) / span_y;
		double w = g * u + h * v + 1;
		if (std::abs(w) < 1e-9) return point;
		return Vector2D((float)((a * u + b * v + cc) / w),
		                (float)((d * u + e * v + f) / w));
	};
}

PointMap QuadInverseMap(OrientedBox const& box, Vector2D const corners[4]) {
	double span_x = box.half.X() * 2;
	double span_y = box.half.Y() * 2;
	if (span_x <= 0 || span_y <= 0)
		return [](Vector2D point) { return point; };

	auto found = QuadFrom(corners);
	if (!found.ok) return [](Vector2D point) { return point; };

	// Invert the homogeneous 3x3 matrix of QuadFrom. Keeping this as a projective
	// inverse (rather than iterating towards an answer) makes caret hit-testing stable
	// even when the textbox has a strong perspective.
	double i00 = found.e - found.f * found.h;
	double i01 = found.c * found.h - found.b;
	double i02 = found.b * found.f - found.c * found.e;
	double i10 = found.f * found.g - found.d;
	double i11 = found.a - found.c * found.g;
	double i12 = found.c * found.d - found.a * found.f;
	double i20 = found.d * found.h - found.e * found.g;
	double i21 = found.b * found.g - found.a * found.h;
	double i22 = found.a * found.e - found.b * found.d;

	return [=](Vector2D point) {
		double x = point.X(), y = point.Y();
		double w = i20 * x + i21 * y + i22;
		if (std::abs(w) < 1e-9) return point;
		double u = (i00 * x + i01 * y + i02) / w;
		double v = (i10 * x + i11 * y + i12) / w;
		return box.ToScript(Vector2D(
			static_cast<float>(u * span_x - box.half.X()),
			static_cast<float>(v * span_y - box.half.Y())));
	};
}

double QuadDepth(OrientedBox const& box, Vector2D const corners[4], Vector2D point) {
	double span_x = box.half.X() * 2;
	double span_y = box.half.Y() * 2;
	if (span_x <= 0 || span_y <= 0) return 1.0;

	auto found = QuadFrom(corners);
	if (!found.ok) return 1.0;

	Vector2D local = box.ToLocal(point);
	double u = (local.X() + box.half.X()) / span_x;
	double v = (local.Y() + box.half.Y()) / span_y;
	return found.g * u + found.h * v + 1;
}

// ---------------------------------------------------------------------- the warp

namespace {

/// The Coons patch that fills in the four points inside the net.
///
/// A bicubic patch has sixteen control points, but a warp only shows the twelve on its
/// boundary. The classic Coons formula gives the other four from those, and it is what
/// makes the middle of the mesh follow when a corner or a direction handle moves. Each
/// row is one interior point: the index it fills, then the boundary points it is made of.
struct CoonsTerm { int index; double weight; };
const int coons_target[4] = {5, 6, 9, 10};
const CoonsTerm coons_terms[4][8] = {
	{{0, -4}, {1, 6}, {4, 6}, {3, -2}, {12, -2}, {7, 3}, {13, 3}, {15, -1}},
	{{3, -4}, {2, 6}, {7, 6}, {0, -2}, {15, -2}, {4, 3}, {14, 3}, {12, -1}},
	{{12, -4}, {13, 6}, {8, 6}, {15, -2}, {0, -2}, {11, 3}, {1, 3}, {3, -1}},
	{{15, -4}, {14, 6}, {11, 6}, {12, -2}, {3, -2}, {8, 3}, {2, 3}, {0, -1}}
};

/// Which net index each of the twelve boundary points has, walking the boundary from the
/// top left corner: corner, its two edge points, corner, and so on.
const int tangent_index[8] = {1, 2, 7, 11, 14, 13, 8, 4};
const int corner_index[4] = {0, 3, 15, 12};

/// How much of a boundary point ends up in one interior point.
double CoonsWeight(int interior, int net_index) {
	double total = 0;
	for (auto const& term : coons_terms[interior])
		if (term.index == net_index) total += term.weight / 9.0;
	return total;
}

void Bernstein(double t, double out[4]) {
	double s = 1 - t;
	out[0] = s * s * s;
	out[1] = 3 * t * s * s;
	out[2] = 3 * t * t * s;
	out[3] = t * t * t;
}

void BernsteinSlope(double t, double out[4]) {
	double s = 1 - t;
	out[0] = -3 * s * s;
	out[1] = 3 * s * (1 - 3 * t);
	out[2] = 3 * t * (2 - 3 * t);
	out[3] = 3 * t * t;
}

/// How much the surface at (u,v) owes to each of the sixteen control points.
void NetWeights(double u, double v, double out[16]) {
	double bu[4], bv[4];
	Bernstein(u, bu);
	Bernstein(v, bv);
	for (int row = 0; row < 4; ++row)
		for (int column = 0; column < 4; ++column)
			out[row * 4 + column] = bv[row] * bu[column];
}

} // namespace

void WarpReset(OrientedBox const& box, WarpNet& net) {
	// The net starts as the box divided in thirds, which is exactly the patch that maps
	// every point to itself.
	auto at = [&](int row, int column) {
		return box.ToScript(Vector2D(box.half.X() * (column * 2.f / 3.f - 1.f),
		                             box.half.Y() * (row * 2.f / 3.f - 1.f)));
	};
	net.corner[0] = at(0, 0);
	net.corner[1] = at(0, 3);
	net.corner[2] = at(3, 3);
	net.corner[3] = at(3, 0);
	net.tangent[0] = at(0, 1);
	net.tangent[1] = at(0, 2);
	net.tangent[2] = at(1, 3);
	net.tangent[3] = at(2, 3);
	net.tangent[4] = at(3, 2);
	net.tangent[5] = at(3, 1);
	net.tangent[6] = at(2, 0);
	net.tangent[7] = at(1, 0);
	for (int i = 0; i < 4; ++i) net.inner[i] = Vector2D(0.f, 0.f);
}

void WarpControls(WarpNet const& net, Vector2D out[16]) {
	for (int i = 0; i < 16; ++i) out[i] = Vector2D(0.f, 0.f);
	for (int i = 0; i < 4; ++i) out[corner_index[i]] = net.corner[i];
	for (int i = 0; i < 8; ++i) out[tangent_index[i]] = net.tangent[i];

	for (int interior = 0; interior < 4; ++interior) {
		Vector2D sum(0.f, 0.f);
		for (auto const& term : coons_terms[interior])
			sum = sum + out[term.index] * (float)(term.weight / 9.0);
		out[coons_target[interior]] = sum + net.inner[interior];
	}
}

Vector2D WarpPoint(Vector2D const control[16], double u, double v) {
	double weights[16];
	NetWeights(u, v, weights);
	Vector2D out(0.f, 0.f);
	for (int i = 0; i < 16; ++i) out = out + control[i] * (float)weights[i];
	return out;
}

void WarpCornerTangents(int corner, int& first, int& second) {
	// The edges run top, right, bottom, left, and each corner ends one edge and starts
	// the next, so it owns the last handle of one and the first of the other.
	first = corner * 2;
	second = (corner * 2 + 7) % 8;
}

void WarpMoveCorner(WarpNet& net, int corner, Vector2D delta) {
	if (corner < 0 || corner > 3) return;
	net.corner[corner] = net.corner[corner] + delta;
	int first, second;
	WarpCornerTangents(corner, first, second);
	net.tangent[first] = net.tangent[first] + delta;
	net.tangent[second] = net.tangent[second] + delta;
}

bool WarpLocate(Vector2D const control[16], Vector2D point, double tolerance,
                double& u, double& v) {
	// A coarse sweep for the nearest sample, because Newton on its own can walk off to
	// the wrong part of a bent patch, and then a few steps to sharpen it.
	const int steps = 12;
	double best = -1;
	double best_u = 0, best_v = 0;
	for (int i = 0; i <= steps; ++i) {
		for (int j = 0; j <= steps; ++j) {
			double su = (double)j / steps, sv = (double)i / steps;
			double distance = (WarpPoint(control, su, sv) - point).SquareLen();
			if (best < 0 || distance < best) { best = distance; best_u = su; best_v = sv; }
		}
	}

	u = best_u;
	v = best_v;
	for (int step = 0; step < 8; ++step) {
		double bu[4], bv[4], du[4], dv[4];
		Bernstein(u, bu);
		Bernstein(v, bv);
		BernsteinSlope(u, du);
		BernsteinSlope(v, dv);

		Vector2D surface(0.f, 0.f), along_u(0.f, 0.f), along_v(0.f, 0.f);
		for (int row = 0; row < 4; ++row)
			for (int column = 0; column < 4; ++column) {
				Vector2D at = control[row * 4 + column];
				surface = surface + at * (float)(bv[row] * bu[column]);
				along_u = along_u + at * (float)(bv[row] * du[column]);
				along_v = along_v + at * (float)(dv[row] * bu[column]);
			}

		Vector2D residual = point - surface;
		double determinant = (double)along_u.X() * along_v.Y() -
			(double)along_v.X() * along_u.Y();
		if (std::abs(determinant) < 1e-9) break;
		double step_u = ((double)residual.X() * along_v.Y() -
			(double)along_v.X() * residual.Y()) / determinant;
		double step_v = ((double)along_u.X() * residual.Y() -
			(double)residual.X() * along_u.Y()) / determinant;
		u = std::clamp(u + step_u, 0.0, 1.0);
		v = std::clamp(v + step_v, 0.0, 1.0);
		if (std::abs(step_u) < 1e-6 && std::abs(step_v) < 1e-6) break;
	}

	return (WarpPoint(control, u, v) - point).Len() <= tolerance;
}

void WarpDragInside(WarpNet& net, double u, double v, Vector2D delta) {
	double weights[16];
	NetWeights(u, v, weights);

	// How much the grabbed point of the surface moves when each part of the net that is
	// allowed to give moves by one. A direction handle counts twice over: once where it
	// sits, and once through the Coons patch, which carries part of it into the middle.
	double sensitivity[12];
	for (int i = 0; i < 8; ++i) {
		double total = weights[tangent_index[i]];
		for (int interior = 0; interior < 4; ++interior)
			total += CoonsWeight(interior, tangent_index[i]) * weights[coons_target[interior]];
		sensitivity[i] = total;
	}
	for (int interior = 0; interior < 4; ++interior)
		sensitivity[8 + interior] = weights[coons_target[interior]];

	double norm = 0;
	for (double value : sensitivity) norm += value * value;
	// Right at a corner nothing that is allowed to give has any say, and dividing by that
	// would fling the mesh across the screen. The floor turns it into a drag that simply
	// falls short, which is what a corner should feel like - it has its own handle.
	norm = std::max(norm, 0.05);

	for (int i = 0; i < 8; ++i)
		net.tangent[i] = net.tangent[i] + delta * (float)(sensitivity[i] / norm);
	for (int interior = 0; interior < 4; ++interior)
		net.inner[interior] = net.inner[interior] +
			delta * (float)(sensitivity[8 + interior] / norm);
}

PointMap WarpMap(OrientedBox const& box, WarpNet const& net) {
	double span_x = box.half.X() * 2;
	double span_y = box.half.Y() * 2;
	if (span_x <= 0 || span_y <= 0)
		return [](Vector2D point) { return point; };

	Vector2D control[16];
	WarpControls(net, control);
	WarpNet source;
	WarpReset(box, source);
	Vector2D original[16];
	WarpControls(source, original);
	std::vector<Vector2D> delta(16);
	for (int i = 0; i < 16; ++i) delta[i] = control[i] - original[i];

	return [=](Vector2D point) {
		Vector2D local = box.ToLocal(point);
		double u = std::clamp((local.X() + box.half.X()) / span_x, 0.0, 1.0);
		double v = std::clamp((local.Y() + box.half.Y()) / span_y, 0.0, 1.0);
		// Apply the patch's displacement to the original point. The nearest boundary
		// displacement continues outside the box, so glyph overhang, borders and shadows
		// do not collapse onto its edge.
		return point + WarpPoint(delta.data(), u, v);
	};
}

PointMap WarpMapFrom(OrientedBox const& box, WarpNet const& rest, WarpNet const& net,
                     PointMap const& into_box) {
	double span_x = box.half.X() * 2;
	double span_y = box.half.Y() * 2;
	if (span_x <= 0 || span_y <= 0)
		return [](Vector2D point) { return point; };

	Vector2D control[16], original[16];
	WarpControls(net, control);
	WarpControls(rest, original);
	std::vector<Vector2D> delta(16);
	for (int i = 0; i < 16; ++i) delta[i] = control[i] - original[i];

	return [=](Vector2D point) {
		// Where the point is along the shape the selection rests in, which is what the patch is
		// laid out over. Reading it straight off the box would be reading it off a rectangle the
		// text does not fill, and the bend would run across the words rather than along them.
		Vector2D at = point;
		if (into_box) {
			Vector2D taken = into_box(point);
			// A projective map has a line it sends to infinity, and a clip or a border can
			// reach across it. Past there the point says nothing, so the box is read straight
			// rather than letting a NaN through into the geometry.
			if (std::isfinite(taken.X()) && std::isfinite(taken.Y())) at = taken;
		}
		Vector2D local = box.ToLocal(at);
		double u = std::clamp((local.X() + box.half.X()) / span_x, 0.0, 1.0);
		double v = std::clamp((local.Y() + box.half.Y()) / span_y, 0.0, 1.0);
		return point + WarpPoint(delta.data(), u, v);
	};
}

PointMap FlipMap(Vector2D centre, bool horizontal) {
	return [=](Vector2D point) {
		return horizontal ? Vector2D(2 * centre.X() - point.X(), point.Y())
		                  : Vector2D(point.X(), 2 * centre.Y() - point.Y());
	};
}

// --------------------------------------------------------------- the entry points

bool CanTransform(const agi::Context *c) {
	return c && c->selectionController &&
		!c->selectionController->GetSelectedSet().empty();
}

} // namespace typesetting
