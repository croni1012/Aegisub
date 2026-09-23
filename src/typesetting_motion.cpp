// Copyright (c) 2026, Muteki Aegisub
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted.

#include "typesetting_motion.h"
#include "typesetting_motion_tags.h"
#include "typesetting_motion_transform.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "auto4_base.h"
#include "imagemask_codec.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "spline.h"
#include "subtitle_line_combiner.h"
#include "typesetting_perspective.h"
#include "typesetting_glitch.h"
#include "typesetting_transform.h"
#include "video_controller.h"

#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/split.h>

#include <boost/algorithm/string/replace.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace typesetting::motion {
namespace {

constexpr char motion_id_key[] = "aegisub/motion-id";
constexpr char motion_source_key[] = "aegisub/motion-source";
constexpr char source_separator = '\x1e';

using detail::Number;
using detail::RemoveFirstTag;
using detail::SetFirstTag;
using detail::SetFirstTagUnlessDefault;

bool SolveLinear(double matrix[8][9], double out[8]) {
	for (int column = 0; column < 8; ++column) {
		int pivot = column;
		for (int row = column + 1; row < 8; ++row)
			if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
		if (std::abs(matrix[pivot][column]) < 1e-12) return false;
		if (pivot != column)
			for (int item = column; item <= 8; ++item)
				std::swap(matrix[pivot][item], matrix[column][item]);
		double divisor = matrix[column][column];
		for (int item = column; item <= 8; ++item) matrix[column][item] /= divisor;
		for (int row = 0; row < 8; ++row) {
			if (row == column) continue;
			double factor = matrix[row][column];
			for (int item = column; item <= 8; ++item)
				matrix[row][item] -= factor * matrix[column][item];
		}
	}
	for (int row = 0; row < 8; ++row) out[row] = matrix[row][8];
	return true;
}

Homography FromQuads(std::array<Vector2D, 4> const& from,
	std::array<Vector2D, 4> const& to) {
	double equations[8][9]{};
	for (int point = 0; point < 4; ++point) {
		double x = from[point].X(), y = from[point].Y();
		double u = to[point].X(), v = to[point].Y();
		double *first = equations[point * 2];
		first[0] = x; first[1] = y; first[2] = 1;
		first[6] = -u * x; first[7] = -u * y; first[8] = u;
		double *second = equations[point * 2 + 1];
		second[3] = x; second[4] = y; second[5] = 1;
		second[6] = -v * x; second[7] = -v * y; second[8] = v;
	}
	double solved[8];
	if (!SolveLinear(equations, solved)) return {};
	Homography out;
	for (int i = 0; i < 8; ++i) out.value[i] = solved[i];
	out.value[8] = 1;
	return out;
}

Sample LinearSample(Track const& track, size_t index) {
	if (track.samples.empty()) return {};
	index = std::min(index, track.samples.size() - 1);
	if (track.samples.size() == 1) return track.samples.front();
	double progress = static_cast<double>(index) / (track.samples.size() - 1);
	auto const& first = track.samples.front();
	auto const& last = track.samples.back();
	auto blend = [progress](Vector2D left, Vector2D right) {
		return left * static_cast<float>(1.0 - progress) + right * static_cast<float>(progress);
	};
	Sample out;
	out.source_frame = static_cast<int>(std::lround(
		first.source_frame * (1.0 - progress) + last.source_frame * progress));
	out.position = blend(first.position, last.position);
	out.scale = blend(first.scale, last.scale);
	double turn = std::remainder(last.rotation - first.rotation, 360.0);
	out.rotation = first.rotation + turn * progress;
	for (size_t corner = 0; corner < out.corners.size(); ++corner)
		out.corners[corner] = blend(first.corners[corner], last.corners[corner]);
	return out;
}

Homography FilteredMap(Track const& track, size_t sample, size_t reference,
	ApplyOptions::Components const& components, bool linear) {
	if (track.samples.empty()) return {};
	sample = std::min(sample, track.samples.size() - 1);
	reference = std::min(reference, track.samples.size() - 1);
	Sample current = linear ? LinearSample(track, sample) : track.samples[sample];
	Sample origin = linear ? LinearSample(track, reference) : track.samples[reference];
	if (track.kind == TrackKind::Transform) {
		if (!components.track_x) current.position = Vector2D(origin.position.X(), current.position.Y());
		if (!components.track_y) current.position = Vector2D(current.position.X(), origin.position.Y());
		if (!components.scale) current.scale = origin.scale;
		if (!components.rotate) current.rotation = origin.rotation;
		return detail::TransformMap(current, origin);
	}

	auto centre = [](std::array<Vector2D, 4> const& corners) {
		Vector2D value;
		for (auto corner : corners) value = value + corner;
		return value / 4.f;
	};
	Vector2D origin_centre = centre(origin.corners);
	Vector2D current_centre = centre(current.corners);
	double numerator_a = 0, numerator_b = 0, denominator = 0;
	for (size_t index = 0; index < origin.corners.size(); ++index) {
		Vector2D from = origin.corners[index] - origin_centre;
		Vector2D to = current.corners[index] - current_centre;
		numerator_a += from.X() * to.X() + from.Y() * to.Y();
		numerator_b += from.X() * to.Y() - from.Y() * to.X();
		denominator += from.SquareLen();
	}
	double a = denominator > 1e-9 ? numerator_a / denominator : 1.0;
	double b = denominator > 1e-9 ? numerator_b / denominator : 0.0;
	double scale = std::hypot(a, b);
	double angle = std::atan2(b, a);
	if (!std::isfinite(scale) || scale < 1e-9) scale = 1.0;
	if (!std::isfinite(angle)) angle = 0.0;
	auto rotate = [](Vector2D point, double radians) {
		double cosine = std::cos(radians), sine = std::sin(radians);
		return Vector2D(static_cast<float>(point.X() * cosine - point.Y() * sine),
			static_cast<float>(point.X() * sine + point.Y() * cosine));
	};
	Vector2D target_centre(
		components.track_x ? current_centre.X() : origin_centre.X(),
		components.track_y ? current_centre.Y() : origin_centre.Y());
	double output_scale = components.scale ? scale : 1.0;
	double output_angle = components.rotate ? angle : 0.0;
	std::array<Vector2D, 4> target;
	for (size_t index = 0; index < target.size(); ++index) {
		Vector2D local = components.perspective ?
			rotate(current.corners[index] - current_centre, -angle) /
				static_cast<float>(scale) : origin.corners[index] - origin_centre;
		target[index] = target_centre + rotate(
			local * static_cast<float>(output_scale), output_angle);
	}
	return FromQuads(origin.corners, target);
}

Homography Compose(Homography const& after, Homography const& before) {
	Homography out;
	for (size_t row = 0; row < 3; ++row) {
		for (size_t column = 0; column < 3; ++column) {
			out.value[row * 3 + column] = 0;
			for (size_t at = 0; at < 3; ++at)
				out.value[row * 3 + column] +=
					after.value[row * 3 + at] * before.value[at * 3 + column];
		}
	}
	return out;
}

Homography AbsoluteTransformMap(Sample const& sample, Vector2D anchor) {
	double radians = sample.rotation * 3.14159265358979 / 180.0;
	double cosine = std::cos(radians), sine = std::sin(radians);
	double scale_x = sample.scale.X() / 100.0;
	double scale_y = sample.scale.Y() / 100.0;
	double a = cosine * scale_x, b = -sine * scale_y;
	double c = sine * scale_x, d = cosine * scale_y;
	Homography out;
	out.value = {a, b,
		sample.position.X() - a * anchor.X() - b * anchor.Y(),
		c, d,
		sample.position.Y() - c * anchor.X() - d * anchor.Y(),
		0, 0, 1};
	return out;
}

Homography AppliedMap(Track const& transform_track,
	std::optional<Track> const& perspective_track, size_t sample, size_t reference,
	ApplyOptions::Components const& components, bool linear) {
	// A tracker adapter may provide one complete Corner Pin track, and then its own perspective is
	// the one to use: nothing else has been given, and taking the transform out of it only to put
	// it back would be the long way round to the same map.
	//
	// Unless a perspective track has been handed over as well. That is a deliberate act - somebody
	// pasted an export into the Corner Pin box - and then theirs is the one that counts: the
	// adapter's track keeps its position, scale and turn, and the perspective comes from the
	// pasted data instead.
	if (!components.perspective ||
		(transform_track.kind == TrackKind::CornerPin && !perspective_track))
		return FilteredMap(transform_track, sample, reference, components, linear);

	auto transform_components = components;
	transform_components.perspective = false;
	Homography transform = FilteredMap(transform_track, sample, reference,
		transform_components, linear);
	if (!perspective_track) return transform;

	Sample current = linear ? LinearSample(transform_track, sample) :
		transform_track.samples[std::min(sample, transform_track.samples.size() - 1)];
	Sample origin = linear ? LinearSample(transform_track, reference) :
		transform_track.samples[std::min(reference, transform_track.samples.size() - 1)];
	if (!components.track_x)
		current.position = Vector2D(origin.position.X(), current.position.Y());
	if (!components.track_y)
		current.position = Vector2D(current.position.X(), origin.position.Y());
	if (!components.scale) current.scale = origin.scale;
	if (!components.rotate) current.rotation = origin.rotation;

	// AE applies Corner Pin in layer coordinates and Position/Scale/Rotation
	// afterwards. A subtitle is already in the reference frame's screen
	// coordinates, so first undo that frame's layer transform, apply the Corner
	// Pin delta, and finally apply the current layer transform. Multiplying the
	// two relative screen-space maps directly mixes their coordinate systems and
	// causes large position and angle drift away from the tracked surface.
	double coordinate_width = transform_track.coordinate_width > 0 ?
		transform_track.coordinate_width : perspective_track->coordinate_width;
	double coordinate_height = transform_track.coordinate_height > 0 ?
		transform_track.coordinate_height : perspective_track->coordinate_height;
	Vector2D anchor(static_cast<float>(coordinate_width / 2.0),
		static_cast<float>(coordinate_height / 2.0));
	Homography reference_layer = AbsoluteTransformMap(origin, anchor);
	auto inverse_reference_layer = reference_layer.Inverse();
	if (!inverse_reference_layer) return transform;
	Homography current_layer = AbsoluteTransformMap(current, anchor);

	Sample perspective_current = linear ? LinearSample(*perspective_track, sample) :
		perspective_track->samples[std::min(sample, perspective_track->samples.size() - 1)];
	Sample perspective_origin = linear ? LinearSample(*perspective_track, reference) :
		perspective_track->samples[std::min(reference, perspective_track->samples.size() - 1)];
	Homography corner_delta = FromQuads(perspective_origin.corners,
		perspective_current.corners);
	return Compose(current_layer, Compose(corner_delta, *inverse_reference_layer));
}

using param_vec = const std::vector<AssOverrideParameter> *;

param_vec FindTag(std::vector<std::unique_ptr<AssDialogueBlock>>& blocks,
	std::string const& name) {
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
		for (auto const& tag : block->Tags)
			if (tag.Name == name) return &tag.Params;
	return nullptr;
}

double FirstNumber(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks,
	char const *name, double fallback) {
	for (auto const& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE) continue;
		for (auto const& tag : static_cast<AssDialogueBlockOverride *>(block.get())->Tags)
			if (tag.Name == name && !tag.Params.empty())
				return tag.Params[0].Get<double>(fallback);
		return fallback;
	}
	return fallback;
}

Vector2D TagVector(param_vec params) {
	if (!params || params->size() < 2 || (*params)[0].omitted || (*params)[1].omitted)
		return {};
	return Vector2D((*params)[0].Get<float>(), (*params)[1].Get<float>());
}

int Alignment(agi::Context *context, AssDialogue& line,
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	int align = 2;
	if (auto style = context->ass->GetStyle(line.Style)) align = style->alignment;
	if (auto tag = FindTag(blocks, "\\an")) align = tag->front().Get(align);
	return std::clamp(align, 1, 9);
}

Vector2D Position(agi::Context *context, AssDialogue& line,
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks, int align, int at_ms) {
	if (auto position = TagVector(FindTag(blocks, "\\pos"))) return position;
	if (auto move = FindTag(blocks, "\\move"); move && move->size() >= 4) {
		Vector2D first((*move)[0].Get<float>(), (*move)[1].Get<float>());
		Vector2D second((*move)[2].Get<float>(), (*move)[3].Get<float>());
		int relative = at_ms - static_cast<int>(line.Start);
		int start = 0;
		int end = static_cast<int>(line.End - line.Start);
		if (move->size() >= 6 && !(*move)[4].omitted && !(*move)[5].omitted) {
			start = (*move)[4].Get<int>();
			end = (*move)[5].Get<int>();
		}
		double progress = end == start ? (relative >= end ? 1.0 : 0.0) :
			std::clamp(static_cast<double>(relative - start) / (end - start), 0.0, 1.0);
		return first * static_cast<float>(1.0 - progress) + second * static_cast<float>(progress);
	}
	int script_width = 0, script_height = 0;
	context->ass->GetResolution(script_width, script_height);
	auto margin = line.Margin;
	if (auto style = context->ass->GetStyle(line.Style))
		for (int i = 0; i < 3; ++i) if (!margin[i]) margin[i] = style->Margin[i];
	int column = (align - 1) % 3;
	int row = (align - 1) / 3;
	float x = column == 0 ? margin[0] : column == 1 ?
		(script_width + margin[0] - margin[1]) / 2.f : script_width - margin[1];
	float y = row == 0 ? script_height - margin[2] : row == 1 ?
		script_height / 2.f : margin[2];
	return Vector2D(x, y);
}

std::pair<Vector2D, Vector2D> BaseExtents(agi::Context *context, AssDialogue& line,
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	auto drawing_tag = FindTag(blocks, "\\p");
	if (drawing_tag && drawing_tag->front().Get(0)) {
		Spline spline;
		spline.SetScale(drawing_tag->front().Get(1));
		std::string drawing;
		for (auto block : blocks | agi::of_type<AssDialogueBlockDrawing>()) drawing += block->GetText();
		spline.DecodeFromAss(drawing);
		if (spline.empty()) return {Vector2D(), Vector2D(1, 1)};
		Vector2D low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
		Vector2D high(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
		for (auto curve : spline)
			for (auto point : curve.AnchorPoints()) { low = low.Min(point); high = high.Max(point); }
		return {low, high};
	}

	AssStyle style;
	if (auto base = context->ass->GetStyle(line.Style)) style = AssStyle(base->GetEntryData());
	if (auto tag = FindTag(blocks, "\\fs")) style.fontsize = tag->front().Get(style.fontsize);
	if (auto tag = FindTag(blocks, "\\fn")) style.font = tag->front().Get(style.font);
	std::string text = line.GetStrippedText();
	boost::replace_all(text, "\\N", "\n");
	std::vector<std::string> rows;
	agi::Split(rows, text, '\n');
	double width = 0, height = 0;
	for (auto const& row : rows) {
		double row_width = 0, row_height = 0, descent = 0, lead = 0;
		if (!Automation4::CalculateTextExtents(&style, row, row_width, row_height, descent, lead)) {
			row_width = style.fontsize * row.size();
			row_height = style.fontsize;
		}
		width = std::max(width, row_width);
		height += row_height;
	}
	return {Vector2D(0, 0), Vector2D(std::max(1.0, width), std::max(1.0, height))};
}

using AnimationState = std::unordered_map<std::string, std::vector<double>>;

std::string AnimationKey(std::string name) {
	if (name == "\\c") return "\\1c";
	if (name == "\\fr") return "\\frz";
	return name;
}

bool ReadAnimationValue(AssOverrideTag const& tag, std::vector<double>& value) {
	std::string name = AnimationKey(tag.Name);
	if ((name == "\\clip" || name == "\\iclip") && tag.Params.size() == 4 &&
		std::none_of(tag.Params.begin(), tag.Params.end(),
			[](AssOverrideParameter const& parameter) { return parameter.omitted; })) {
		value.clear();
		for (auto const& parameter : tag.Params) value.push_back(parameter.Get<double>());
		return true;
	}
	if (tag.Params.empty() || tag.Params.front().omitted) return false;
	if (name == "\\alpha" || name == "\\1a" || name == "\\2a" ||
		name == "\\3a" || name == "\\4a") {
		value = {static_cast<double>(tag.Params.front().Get<int>())};
		return true;
	}
	if (name == "\\1c" || name == "\\2c" || name == "\\3c" || name == "\\4c") {
		auto colour = tag.Params.front().Get<agi::Color>();
		value = {static_cast<double>(colour.r), static_cast<double>(colour.g),
			static_cast<double>(colour.b)};
		return true;
	}
	if (name == "\\fs" || name == "\\fsp" || name == "\\fscx" ||
		name == "\\fscy" || name == "\\frz" || name == "\\frx" ||
		name == "\\fry" || name == "\\bord" || name == "\\xbord" ||
		name == "\\ybord" || name == "\\shad" || name == "\\xshad" ||
		name == "\\yshad" || name == "\\be" || name == "\\blur" ||
		name == "\\fax" || name == "\\fay") {
		value = {tag.Params.front().Get<double>()};
		return true;
	}
	return false;
}

void StoreAnimationValue(AnimationState& state, std::string const& raw_name,
	std::vector<double> const& value) {
	std::string name = AnimationKey(raw_name);
	state[name] = value;
	if (name == "\\alpha")
		for (auto channel : {"\\1a", "\\2a", "\\3a", "\\4a"}) state[channel] = value;
	else if (name == "\\bord") {
		state["\\xbord"] = value;
		state["\\ybord"] = value;
	}
	else if (name == "\\shad") {
		state["\\xshad"] = value;
		state["\\yshad"] = value;
	}
}

AnimationState InitialAnimationState(agi::Context *context, AssDialogue const& line) {
	AssStyle fallback;
	auto style = context->ass->GetStyle(line.Style);
	if (!style) style = &fallback;
	int width = 0, height = 0;
	context->ass->GetResolution(width, height);
	AnimationState state;
	auto store = [&](char const *name, std::initializer_list<double> values) {
		state[name] = std::vector<double>(values);
	};
	store("\\fs", {style->fontsize});
	store("\\fsp", {style->spacing});
	store("\\fscx", {style->scalex});
	store("\\fscy", {style->scaley});
	store("\\frz", {style->angle});
	for (auto name : {"\\frx", "\\fry", "\\be", "\\blur", "\\fax", "\\fay"})
		store(name, {0});
	store("\\bord", {style->outline_w});
	store("\\xbord", {style->outline_w});
	store("\\ybord", {style->outline_w});
	store("\\shad", {style->shadow_w});
	store("\\xshad", {style->shadow_w});
	store("\\yshad", {style->shadow_w});
	store("\\alpha", {0});
	store("\\1a", {static_cast<double>(style->primary.a)});
	store("\\2a", {static_cast<double>(style->secondary.a)});
	store("\\3a", {static_cast<double>(style->outline.a)});
	store("\\4a", {static_cast<double>(style->shadow.a)});
	store("\\1c", {static_cast<double>(style->primary.r),
		static_cast<double>(style->primary.g), static_cast<double>(style->primary.b)});
	store("\\2c", {static_cast<double>(style->secondary.r),
		static_cast<double>(style->secondary.g), static_cast<double>(style->secondary.b)});
	store("\\3c", {static_cast<double>(style->outline.r),
		static_cast<double>(style->outline.g), static_cast<double>(style->outline.b)});
	store("\\4c", {static_cast<double>(style->shadow.r),
		static_cast<double>(style->shadow.g), static_cast<double>(style->shadow.b)});
	store("\\clip", {0, 0, static_cast<double>(width), static_cast<double>(height)});
	store("\\iclip", {0, 0, static_cast<double>(width), static_cast<double>(height)});
	return state;
}

std::string FormatAnimationTag(std::string name, std::vector<double> const& value) {
	name = AnimationKey(std::move(name));
	auto byte = [](double item) {
		return static_cast<int>(std::clamp(std::lround(item), 0l, 255l));
	};
	if (name == "\\alpha" || name == "\\1a" || name == "\\2a" ||
		name == "\\3a" || name == "\\4a")
		return agi::format("%s&H%02X&", name, byte(value.front()));
	if (name == "\\1c" || name == "\\2c" || name == "\\3c" || name == "\\4c")
		return agi::format("%s&H%02X%02X%02X&", name, byte(value[2]),
			byte(value[1]), byte(value[0]));
	if (name == "\\clip" || name == "\\iclip")
		return name + "(" + Number(value[0]) + "," + Number(value[1]) + "," +
			Number(value[2]) + "," + Number(value[3]) + ")";
	if (name == "\\fs" || name == "\\be")
		return name + std::to_string(static_cast<int>(std::lround(value.front())));
	return name + Number(value.front());
}

void InterpolateTransforms(agi::Context *context, AssDialogue& line, int relative_ms) {
	auto blocks = line.ParseTags();
	auto state = InitialAnimationState(context, line);
	bool changed = false;
	int duration = static_cast<int>(line.End - line.Start);
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		std::vector<AssOverrideTag> rewritten;
		for (auto& tag : block->Tags) {
			if (tag.Name != "\\t" || tag.Params.size() < 4 || tag.Params.back().omitted ||
				tag.Params.back().GetType() != VariableDataType::BLOCK) {
				std::vector<double> value;
				if (ReadAnimationValue(tag, value)) StoreAnimationValue(state, tag.Name, value);
				if (tag.Name == "\\r") state = InitialAnimationState(context, line);
				rewritten.push_back(std::move(tag));
				continue;
			}
			changed = true;
			int start = tag.Params[0].Get(0);
			int end = tag.Params[1].Get(duration);
			if (!end) end = duration;
			double accel = tag.Params[2].Get(1.0);
			double linear = end == start ? (relative_ms >= end ? 1.0 : 0.0) :
				static_cast<double>(relative_ms - start) / (end - start);
			double progress = linear <= 0 ? 0 : linear >= 1 ? 1 :
				std::pow(linear, accel);
			auto effect = tag.Params.back().Get<AssDialogueBlockOverride *>();
			if (!effect) continue;
			for (auto const& inner : effect->Tags) {
				std::vector<double> target;
				if (!ReadAnimationValue(inner, target)) continue;
				std::string key = AnimationKey(inner.Name);
				auto found = state.find(key);
				std::vector<double> current = found == state.end() ?
					std::vector<double>(target.size(), 0.0) : found->second;
				if (current.size() != target.size()) current.assign(target.size(), 0.0);
				for (size_t index = 0; index < target.size(); ++index)
					current[index] += (target[index] - current[index]) * progress;
				rewritten.emplace_back(FormatAnimationTag(key, current));
				StoreAnimationValue(state, key, current);
			}
		}
		block->Tags = std::move(rewritten);
	}
	if (changed) line.UpdateText(blocks);
}

std::optional<double> FadeAlphaAt(AssOverrideTag const& tag, int relative_ms,
	int duration) {
	double a1 = 0, a2 = 0, a3 = 0;
	int t1 = 0, t2 = 0, t3 = 0, t4 = 0;
	if (tag.Name == "\\fad" && tag.Params.size() >= 2 &&
		!tag.Params[0].omitted && !tag.Params[1].omitted) {
		a1 = 255; a2 = 0; a3 = 255;
		t2 = tag.Params[0].Get<int>();
		t3 = duration - tag.Params[1].Get<int>();
		t4 = duration;
	}
	else if (tag.Name == "\\fade" && tag.Params.size() >= 7 &&
		std::none_of(tag.Params.begin(), tag.Params.end(),
			[](AssOverrideParameter const& parameter) { return parameter.omitted; })) {
		a1 = tag.Params[0].Get<double>();
		a2 = tag.Params[1].Get<double>();
		a3 = tag.Params[2].Get<double>();
		t1 = tag.Params[3].Get<int>();
		t2 = tag.Params[4].Get<int>();
		t3 = tag.Params[5].Get<int>();
		t4 = tag.Params[6].Get<int>();
	}
	else return std::nullopt;
	auto blend = [](double before, double after, int at, int start, int end) {
		if (end <= start) return after;
		double progress = std::clamp(static_cast<double>(at - start) / (end - start),
			0.0, 1.0);
		return before + (after - before) * progress;
	};
	if (relative_ms < t1) return a1;
	if (relative_ms < t2) return blend(a1, a2, relative_ms, t1, t2);
	if (relative_ms < t3) return a2;
	if (relative_ms < t4) return blend(a2, a3, relative_ms, t3, t4);
	return a3;
}

int FadeAdjustedAlpha(int alpha, double fade_alpha) {
	double opacity = (255.0 - std::clamp(fade_alpha, 0.0, 255.0)) / 255.0;
	return static_cast<int>(std::clamp(std::lround(255.0 - opacity * (255 - alpha)),
		0l, 255l));
}

void InterpolateFade(agi::Context *context, AssDialogue& line, int relative_ms) {
	auto blocks = line.ParseTags();
	std::optional<double> fade_alpha;
	int duration = static_cast<int>(line.End - line.Start);
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
		for (auto const& tag : block->Tags)
			if (!fade_alpha) fade_alpha = FadeAlphaAt(tag, relative_ms, duration);
	if (!fade_alpha) return;

	AssStyle fallback;
	auto style = context->ass->GetStyle(line.Style);
	if (!style) style = &fallback;
	std::array<int, 4> effective_alpha = {
		style->primary.a, style->secondary.a, style->outline.a, style->shadow.a
	};
	double border = FirstNumber(blocks, "\\bord", style->outline_w);
	bool border_used = std::abs(FirstNumber(blocks, "\\xbord", border)) > 1e-9 ||
		std::abs(FirstNumber(blocks, "\\ybord", border)) > 1e-9;
	double shadow = FirstNumber(blocks, "\\shad", style->shadow_w);
	bool shadow_used = std::abs(FirstNumber(blocks, "\\xshad", shadow)) > 1e-9 ||
		std::abs(FirstNumber(blocks, "\\yshad", shadow)) > 1e-9;
	bool karaoke_used = false;
	AssDialogueBlockOverride *first_override = nullptr;
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		if (!first_override) first_override = block;
		for (auto const& tag : block->Tags) {
			std::string key = AnimationKey(tag.Name);
			if ((key == "\\bord" || key == "\\xbord" || key == "\\ybord") &&
				!tag.Params.empty() && !tag.Params.front().omitted &&
				std::abs(tag.Params.front().Get<double>()) > 1e-9)
				border_used = true;
			if ((key == "\\shad" || key == "\\xshad" || key == "\\yshad") &&
				!tag.Params.empty() && !tag.Params.front().omitted &&
				std::abs(tag.Params.front().Get<double>()) > 1e-9)
				shadow_used = true;
			if (key == "\\k" || key == "\\K" || key == "\\kf" || key == "\\ko")
				karaoke_used = true;
		}
	}
	if (first_override) {
		for (auto const& tag : first_override->Tags) {
			std::string key = AnimationKey(tag.Name);
			if (key == "\\r") {
				effective_alpha = {
					style->primary.a, style->secondary.a, style->outline.a, style->shadow.a
				};
			}
			else if (key == "\\alpha" && !tag.Params.empty() && !tag.Params.front().omitted) {
				effective_alpha.fill(tag.Params.front().Get<int>());
			}
			else if (key.size() == 3 && key[0] == '\\' && key[2] == 'a' &&
				key[1] >= '1' && key[1] <= '4' && !tag.Params.empty() &&
				!tag.Params.front().omitted) {
				effective_alpha[static_cast<size_t>(key[1] - '1')] =
					tag.Params.front().Get<int>();
			}
		}
	}
	std::array<int, 4> adjusted_alpha;
	for (size_t channel = 0; channel < adjusted_alpha.size(); ++channel)
		adjusted_alpha[channel] = FadeAdjustedAlpha(effective_alpha[channel], *fade_alpha);
	bool shared_alpha = adjusted_alpha[0] == adjusted_alpha[2] &&
		adjusted_alpha[0] == adjusted_alpha[3];
	if (!border_used) shared_alpha = adjusted_alpha[0] == adjusted_alpha[3];
	if (!shadow_used) shared_alpha = border_used ?
		adjusted_alpha[0] == adjusted_alpha[2] : true;

	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		std::vector<AssOverrideTag> rewritten;
		for (auto& tag : block->Tags) {
			if (FadeAlphaAt(tag, relative_ms, duration)) continue;
			std::string key = AnimationKey(tag.Name);
			if ((key == "\\alpha" || key == "\\1a" || key == "\\2a" ||
				key == "\\3a" || key == "\\4a") && !tag.Params.empty() &&
				!tag.Params.front().omitted) {
				if (block != first_override) {
					int adjusted = FadeAdjustedAlpha(tag.Params.front().Get<int>(), *fade_alpha);
					rewritten.emplace_back(agi::format("%s&H%02X&", key, adjusted));
				}
			}
			else rewritten.push_back(std::move(tag));
		}
		block->Tags = std::move(rewritten);
	}
	std::vector<std::string> alpha_tags;
	if (shared_alpha) {
		alpha_tags.push_back(agi::format("\\alpha&H%02X&", adjusted_alpha[0]));
		if (karaoke_used && adjusted_alpha[1] != adjusted_alpha[0])
			alpha_tags.push_back(agi::format("\\2a&H%02X&", adjusted_alpha[1]));
	}
	else {
		alpha_tags.push_back(agi::format("\\1a&H%02X&", adjusted_alpha[0]));
		if (karaoke_used) alpha_tags.push_back(agi::format("\\2a&H%02X&", adjusted_alpha[1]));
		if (border_used) alpha_tags.push_back(agi::format("\\3a&H%02X&", adjusted_alpha[2]));
		if (shadow_used) alpha_tags.push_back(agi::format("\\4a&H%02X&", adjusted_alpha[3]));
	}
	if (first_override) {
		for (auto const& tag : alpha_tags) first_override->Tags.emplace_back(tag);
		line.UpdateText(blocks);
	}
	else {
		std::string alpha_prefix = "{";
		for (auto const& tag : alpha_tags) alpha_prefix += tag;
		alpha_prefix += "}";
		line.Text = alpha_prefix + line.Text.get();
	}
}

void InterpolateMove(agi::Context *context, AssDialogue& line, int at_ms) {
	auto blocks = line.ParseTags();
	auto move = FindTag(blocks, "\\move");
	if (!move) return;
	int align = Alignment(context, line, blocks);
	Vector2D position = Position(context, line, blocks, align, at_ms);
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
		block->Tags.erase(std::remove_if(block->Tags.begin(), block->Tags.end(),
			[](AssOverrideTag const& tag) { return tag.Name == "\\move"; }), block->Tags.end());
	line.UpdateText(blocks);
	std::string text = line.Text.get();
	SetFirstTag(text, "\\pos", "(" + Number(position.X()) + "," + Number(position.Y()) + ")");
	line.Text = text;
}

void InterpolateAnimations(agi::Context *context, AssDialogue& line,
	int midpoint_ms, int frame_start_ms) {
	// The legacy script samples transforms at the frame midpoint and \move at the
	// frame start. Fades intentionally use the midpoint too, fixing the script's
	// fully invisible first generated frame for \fad.
	int relative_ms = midpoint_ms - static_cast<int>(line.Start);
	InterpolateTransforms(context, line, relative_ms);
	InterpolateFade(context, line, relative_ms);
	InterpolateMove(context, line, frame_start_ms);
}

std::string MapLine(agi::Context *context, AssDialogue& line, Homography const& matrix,
	ApplyOptions const& options, bool map_clips, int at_ms) {
	auto blocks = line.ParseTags();
	int align = Alignment(context, line, blocks);
	Vector2D position = Position(context, line, blocks, align, at_ms);
	Vector2D origin = TagVector(FindTag(blocks, "\\org"));
	if (!origin) origin = position;
	AssStyle fallback;
	auto style = context->ass->GetStyle(line.Style);
	if (!style) style = &fallback;
	Vector2D scale(
		static_cast<float>(FirstNumber(blocks, "\\fscx", style->scalex)),
		static_cast<float>(FirstNumber(blocks, "\\fscy", style->scaley)));
	typesetting::PerspectiveTags tags;
	tags.pos = position;
	tags.org = origin;
	tags.scale = scale;
	tags.shear_x = FirstNumber(blocks, "\\fax", 0);
	tags.shear_y = FirstNumber(blocks, "\\fay", 0);
	tags.angle_z = FirstNumber(blocks, "\\frz",
		FirstNumber(blocks, "\\fr", style->angle));
	tags.angle_x = FirstNumber(blocks, "\\frx", 0);
	tags.angle_y = FirstNumber(blocks, "\\fry", 0);
	auto extents = BaseExtents(context, line, blocks);
	int script_width = 0, script_height = 0, layout_width = 0, layout_height = 0;
	context->ass->GetResolution(script_width, script_height);
	context->ass->GetEffectiveLayoutResolution(context, layout_width, layout_height);
	Vector2D screen_scale(
		layout_width ? static_cast<float>(script_width) / layout_width : 1.f,
		layout_height ? static_cast<float>(script_height) / layout_height : 1.f);
	Vector2D corners[4];
	typesetting::PerspectiveQuad(tags, align, extents.first, extents.second,
		screen_scale, corners);
	for (auto& corner : corners) corner = matrix.Map(corner);
	auto solved = typesetting::SolvePerspective(corners, align, extents.first,
		extents.second, screen_scale, matrix.Map(origin));
	if (!solved.ok)
		solved = typesetting::SolvePerspective(corners, align, extents.first,
			extents.second, screen_scale, Vector2D());
	if (!solved.ok) return line.Text.get();

	std::string text = line.Text.get();
	SetFirstTag(text, "\\pos", "(" + Number(solved.pos.X()) + "," +
		Number(solved.pos.Y()) + ")", {"\\move"});
	if ((solved.org - solved.pos).Len() < .0005f)
		RemoveFirstTag(text, "\\org");
	else
		SetFirstTag(text, "\\org", "(" + Number(solved.org.X()) + "," +
			Number(solved.org.Y()) + ")");
	SetFirstTagUnlessDefault(text, "\\fscx", solved.scale.X(), style->scalex);
	SetFirstTagUnlessDefault(text, "\\fscy", solved.scale.Y(), style->scaley);
	SetFirstTagUnlessDefault(text, "\\fax", solved.shear_x, 0);
	SetFirstTagUnlessDefault(text, "\\fay", solved.shear_y, 0);
	SetFirstTagUnlessDefault(text, "\\frz", solved.angle_z, style->angle, {"\\fr"});
	SetFirstTagUnlessDefault(text, "\\frx", solved.angle_x, 0);
	SetFirstTagUnlessDefault(text, "\\fry", solved.angle_y, 0);

	Vector2D mapped = matrix.Map(position);
	double growth_x = (matrix.Map(position + Vector2D(1, 0)) - mapped).Len();
	double growth_y = (matrix.Map(position + Vector2D(0, 1)) - mapped).Len();
	double growth = std::sqrt(std::max(0.0, growth_x * growth_y));
	if (options.scale_border) {
		double border = FirstNumber(blocks, "\\bord", style->outline_w);
		if (FindTag(blocks, "\\xbord") || FindTag(blocks, "\\ybord")) {
			double xborder = FirstNumber(blocks, "\\xbord", border);
			double yborder = FirstNumber(blocks, "\\ybord", border);
			SetFirstTagUnlessDefault(text, "\\xbord", xborder * growth_x,
				style->outline_w, {"\\bord"});
			SetFirstTagUnlessDefault(text, "\\ybord", yborder * growth_y,
				style->outline_w, {"\\bord"});
		}
		else
			SetFirstTagUnlessDefault(text, "\\bord", border * growth, style->outline_w);
	}
	if (options.scale_shadow) {
		double shadow = FirstNumber(blocks, "\\shad", style->shadow_w);
		if (FindTag(blocks, "\\xshad") || FindTag(blocks, "\\yshad")) {
			double xshadow = FirstNumber(blocks, "\\xshad", shadow);
			double yshadow = FirstNumber(blocks, "\\yshad", shadow);
			SetFirstTagUnlessDefault(text, "\\xshad", xshadow * growth_x,
				style->shadow_w, {"\\shad"});
			SetFirstTagUnlessDefault(text, "\\yshad", yshadow * growth_y,
				style->shadow_w, {"\\shad"});
		}
		else
			SetFirstTagUnlessDefault(text, "\\shad", shadow * growth, style->shadow_w);
	}
	if (options.scale_blur) {
		double blur = FirstNumber(blocks, "\\blur", 0);
		SetFirstTagUnlessDefault(text, "\\blur", blur * growth, 0);
	}
	if (map_clips) {
		text = typesetting::TransformProjectiveClips(text,
			[&matrix](Vector2D point) { return matrix.Map(point); });
	}
	return text;
}

bool HasClip(AssDialogue& line) {
	auto blocks = line.ParseTags();
	return FindTag(blocks, "\\clip") || FindTag(blocks, "\\iclip");
}

std::optional<std::string> FirstTagText(
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks, char const *name) {
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
		for (auto const& tag : block->Tags)
			if (tag.Name == name) return static_cast<std::string>(tag);
	return std::nullopt;
}

void AppendFirstOverride(std::string& text, std::string const& tags) {
	if (tags.empty()) return;
	if (text.empty() || text[0] != '{') text = "{}" + text;
	size_t close = text.find('}');
	if (close == std::string::npos) { text = "{}" + text; close = 1; }
	text.insert(close, tags);
}

std::vector<std::string> FirstOverrideTransforms(AssDialogue const& source) {
	AssDialogue line(source);
	auto blocks = line.ParseTags();
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		std::vector<std::string> transforms;
		for (auto const& tag : block->Tags)
			if (tag.Name == "\\t") transforms.push_back(static_cast<std::string>(tag));
		return transforms;
	}
	return {};
}

void RestoreFirstOverrideTransforms(AssDialogue& line,
	std::vector<std::string> const& transforms) {
	if (transforms.empty()) return;
	auto blocks = line.ParseTags();
	for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		block->Tags.erase(std::remove_if(block->Tags.begin(), block->Tags.end(),
			[](AssOverrideTag const& tag) { return tag.Name == "\\t"; }), block->Tags.end());
		for (auto const& transform : transforms) block->Tags.emplace_back(transform);
		line.UpdateText(blocks);
		return;
	}
}

AssDialogue BuildLinearLine(agi::Context *context, AssDialogue const& source,
	Homography const& first_map, Homography const& last_map,
	ApplyOptions const& options, int first_midpoint, int last_midpoint) {
	AssDialogue first(source), last(source);
	first.Comment = false;
	last.Comment = false;
	first.ExtradataIds = std::vector<uint32_t>();
	last.ExtradataIds = std::vector<uint32_t>();
	if (options.clip_only) return first;
	auto original_transforms = FirstOverrideTransforms(source);
	first.Text = MapLine(context, first, first_map, options, false, first_midpoint);
	last.Text = MapLine(context, last, last_map, options, false, last_midpoint);
	RestoreFirstOverrideTransforms(first, original_transforms);
	RestoreFirstOverrideTransforms(last, original_transforms);
	auto first_blocks = first.ParseTags();
	auto last_blocks = last.ParseTags();
	std::string text = first.Text.get();
	auto first_position_tag = FindTag(first_blocks, "\\pos");
	auto last_position_tag = FindTag(last_blocks, "\\pos");
	int duration = static_cast<int>(source.End - source.Start);
	int begin_time = std::clamp(first_midpoint - static_cast<int>(source.Start), 0, duration);
	int end_time = std::clamp(last_midpoint - static_cast<int>(source.Start), begin_time, duration);
	if (first_position_tag && first_position_tag->size() >= 2 && last_position_tag &&
		last_position_tag->size() >= 2) {
		Vector2D first_position((*first_position_tag)[0].Get<float>(),
			(*first_position_tag)[1].Get<float>());
		Vector2D last_position((*last_position_tag)[0].Get<float>(),
			(*last_position_tag)[1].Get<float>());
		if ((last_position - first_position).Len() < .0005f)
			SetFirstTag(text, "\\pos", "(" + Number(first_position.X()) + "," +
				Number(first_position.Y()) + ")", {"\\move"});
		else
			SetFirstTag(text, "\\move", "(" + Number(first_position.X()) + "," +
				Number(first_position.Y()) + "," + Number(last_position.X()) + "," +
				Number(last_position.Y()) + "," + std::to_string(begin_time) + "," +
				std::to_string(end_time) + ")", {"\\pos"});
	}
	std::string effect;
	for (auto name : {"\\fscx", "\\fscy", "\\fax", "\\fay", "\\frz", "\\frx",
		"\\fry", "\\bord", "\\xbord", "\\ybord", "\\shad", "\\xshad",
		"\\yshad", "\\blur"}) {
		auto before = FirstTagText(first_blocks, name);
		auto after = FirstTagText(last_blocks, name);
		if (after && before != after) effect += *after;
	}
	if (!effect.empty()) {
		if (end_time > begin_time)
			AppendFirstOverride(text, agi::format("\\t(%d,%d,%s)", begin_time,
				end_time, effect));
		else
			AppendFirstOverride(text, effect);
	}
	first.Text = text;
	first.Start = source.Start;
	first.End = source.End;
	return first;
}

imagemask::Raster Warp(imagemask::Raster const& source, Homography const& map,
	int script_width, int script_height) {
	if (!source.IsOk()) return {};
	std::array<Vector2D, 4> corners = {
		Vector2D(source.x, source.y), Vector2D(source.x + source.width, source.y),
		Vector2D(source.x + source.width, source.y + source.height),
		Vector2D(source.x, source.y + source.height)
	};
	Vector2D low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
	Vector2D high(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
	for (auto corner : corners) { corner = map.Map(corner); low = low.Min(corner); high = high.Max(corner); }
	int left = std::clamp(static_cast<int>(std::floor(low.X())), 0, script_width);
	int top = std::clamp(static_cast<int>(std::floor(low.Y())), 0, script_height);
	int right = std::clamp(static_cast<int>(std::ceil(high.X())), 0, script_width);
	int bottom = std::clamp(static_cast<int>(std::ceil(high.Y())), 0, script_height);
	if (left >= right || top >= bottom) return {};
	auto inverse = map.Inverse();
	if (!inverse) return {};
	imagemask::Raster out;
	out.x = left; out.y = top; out.width = right - left; out.height = bottom - top;
	out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
	auto sample = [&](int x, int y, int channel) -> double {
		// The area outside the raster is transparent. Clamping to the closest
		// source pixel duplicates the edge during a fractional transform and
		// creates a visibly thicker, opaque rim around a moving ImageMask.
		if (x < 0 || y < 0 || x >= source.width || y >= source.height) return 0;
		return source.rgba[(static_cast<size_t>(y) * source.width + x) * 4 + channel];
	};
	for (int y = 0; y < out.height; ++y) {
		for (int x = 0; x < out.width; ++x) {
			Vector2D at = inverse->Map(Vector2D(left + x + .5f, top + y + .5f));
			double fx = at.X() - source.x - .5, fy = at.Y() - source.y - .5;
			if (fx < -.5 || fy < -.5 || fx > source.width - .5 || fy > source.height - .5) continue;
			int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
			double tx = fx - x0, ty = fy - y0;
			auto filtered = [&](int channel) -> double {
				double first = sample(x0, y0, channel) * (1 - tx) + sample(x0 + 1, y0, channel) * tx;
				double second = sample(x0, y0 + 1, channel) * (1 - tx) + sample(x0 + 1, y0 + 1, channel) * tx;
				return first * (1 - ty) + second * ty;
			};
			auto filtered_premultiplied = [&](int channel) -> double {
				auto premultiplied = [&](int sx, int sy) {
					return sample(sx, sy, channel) * sample(sx, sy, 3) / 255.0;
				};
				double first = premultiplied(x0, y0) * (1 - tx) +
					premultiplied(x0 + 1, y0) * tx;
				double second = premultiplied(x0, y0 + 1) * (1 - tx) +
					premultiplied(x0 + 1, y0 + 1) * tx;
				return first * (1 - ty) + second * ty;
			};
			size_t offset = (static_cast<size_t>(y) * out.width + x) * 4;
			double alpha = filtered(3);
			out.rgba[offset + 3] = static_cast<unsigned char>(
				std::clamp(std::lround(alpha), 0l, 255l));
			if (alpha > .001) {
				for (int channel = 0; channel < 3; ++channel)
					out.rgba[offset + channel] = static_cast<unsigned char>(
						std::clamp(std::lround(filtered_premultiplied(channel) * 255.0 / alpha),
							0l, 255l));
			}
		}
	}
	return out;
}

std::string NewId() {
	static std::atomic<uint64_t> sequence{0};
	auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	return agi::format("%lld-%llu", static_cast<long long>(now),
		static_cast<unsigned long long>(++sequence));
}

std::string Sources(std::vector<AssDialogue *> const& lines) {
	std::string value;
	for (auto line : lines) {
		if (!value.empty()) value.push_back(source_separator);
		value += line->GetEntryData();
	}
	return value;
}

std::string Extra(AssFile const& file, AssDialogue const& line, char const *key) {
	for (auto const& extra : file.GetExtradata(line.ExtradataIds))
		if (extra.key == key) return extra.value;
	return {};
}

std::vector<std::string> SplitSources(std::string const& source) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at <= source.size()) {
		size_t end = source.find(source_separator, at);
		out.push_back(source.substr(at, end == std::string::npos ? std::string::npos : end - at));
		if (end == std::string::npos) break;
		at = end + 1;
	}
	return out;
}

} // namespace

void SnapshotAnimations(agi::Context *context, AssDialogue& line,
		int midpoint_ms, int frame_start_ms) {
	InterpolateAnimations(context, line, midpoint_ms, frame_start_ms);
}

Vector2D Homography::Map(Vector2D point) const {
	double denominator = value[6] * point.X() + value[7] * point.Y() + value[8];
	if (std::abs(denominator) < 1e-12) return point;
	return Vector2D(
		static_cast<float>((value[0] * point.X() + value[1] * point.Y() + value[2]) / denominator),
		static_cast<float>((value[3] * point.X() + value[4] * point.Y() + value[5]) / denominator));
}

std::optional<Homography> Homography::Inverse() const {
	double a = value[0], b = value[1], c = value[2];
	double d = value[3], e = value[4], f = value[5];
	double g = value[6], h = value[7], i = value[8];
	double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::abs(determinant) < 1e-12) return std::nullopt;
	Homography out;
	out.value = {
		(e * i - f * h) / determinant, (c * h - b * i) / determinant,
		(b * f - c * e) / determinant,
		(f * g - d * i) / determinant, (a * i - c * g) / determinant,
		(c * d - a * f) / determinant,
		(d * h - e * g) / determinant, (b * g - a * h) / determinant,
		(a * e - b * d) / determinant
	};
	return out;
}

Homography Track::MapAt(size_t sample, size_t reference) const {
	if (samples.empty()) return {};
	sample = std::min(sample, samples.size() - 1);
	reference = std::min(reference, samples.size() - 1);
	if (kind == TrackKind::CornerPin)
		return FromQuads(samples[reference].corners, samples[sample].corners);
	return detail::TransformMap(samples[sample], samples[reference]);
}

bool Apply(agi::Context *context, Track const& main_track,
	std::optional<Track> const& main_perspective_track,
	std::optional<Track> const& clip_track,
	std::optional<Track> const& clip_perspective_track,
	ApplyOptions const& options, std::string& error,
	ApplyProgressCallback progress) {
	error.clear();
	if (!context || !context->videoController || !main_track.IsOk()) {
		error = "Nincs alkalmazható motion adat vagy videó.";
		return false;
	}
	if (!options.main.Any()) {
		error = "Válassz legalább egy alkalmazandó motion komponenst.";
		return false;
	}
	if (clip_track && options.map_clips && !options.clip.Any()) {
		error = "Válassz legalább egy alkalmazandó clip motion komponenst.";
		return false;
	}
	if (main_perspective_track && main_perspective_track->kind != TrackKind::CornerPin) {
		error = "A fő Perspective mezőbe Corner Pin adat szükséges.";
		return false;
	}
	if (clip_perspective_track && clip_perspective_track->kind != TrackKind::CornerPin) {
		error = "A clip Perspective mezőbe Corner Pin adat szükséges.";
		return false;
	}
	if (options.main.perspective && main_track.kind == TrackKind::Transform &&
		!main_perspective_track) {
		error = "A Perspective be van kapcsolva; illeszd be a fő Corner Pin adatot.";
		return false;
	}
	if (clip_track && options.map_clips && options.clip.perspective &&
		clip_track->kind == TrackKind::Transform && !clip_perspective_track) {
		error = "A clip Perspective be van kapcsolva; illeszd be a clip Corner Pin adatot.";
		return false;
	}
	auto selected = context->selectionController->GetSelectedSet();
	if (selected.empty()) { error = "Nincs kijelölt feliratsor."; return false; }
	if (context->imageMask) context->imageMask->ExpandTypesettingSelection(selected);
	int selection_start = std::numeric_limits<int>::max();
	int selection_end = 0;
	for (auto line : selected)
	{
		selection_start = std::min(selection_start,
			context->videoController->FrameAtTime(line->Start, agi::vfr::START));
		selection_end = std::max(selection_end,
			context->videoController->FrameAtTime(line->End, agi::vfr::END));
	}
	size_t required_frames = static_cast<size_t>(selection_end - selection_start + 1);
	auto require_length = [&](Track const& track, char const *name) {
		if (track.samples.size() == required_frames) return true;
		error = agi::format("A %s frame-száma (%zu) nem egyezik a kijelölt sor "
			"frame-számával (%zu).", name, track.samples.size(), required_frames);
		return false;
	};
	if (!require_length(main_track, "Transformation Data") ||
		(main_perspective_track && !require_length(*main_perspective_track,
			"Perspective - Corner Pin")) ||
		(clip_track && !require_length(*clip_track, "clip Transformation Data")) ||
		(clip_perspective_track && !require_length(*clip_perspective_track,
			"clip Perspective - Corner Pin")))
		return false;
	if (options.reference_sample >= required_frames ||
		(options.clip_reference_sample && *options.clip_reference_sample >= required_frames)) {
		error = "A referenciaképkocka kívül esik a motion adaton.";
		return false;
	}
	auto sample_for_frame = [&](int frame) {
		return static_cast<size_t>(frame - selection_start);
	};
	auto report = [&](ApplyProgressStage stage, size_t complete, size_t total) {
		if (progress) progress(stage, complete, std::max<size_t>(1, total));
	};
	auto originals_for = [&](AssDialogue *line) -> std::vector<AssDialogue *> {
		// A gradient, a textbox and an image mask are each one object made of several rows. All
		// of them have to go through together: tracked a row at a time the object comes apart,
		// and taking its rows away while putting only the first one back loses the rest.
		//
		// Image masks were already handled here. The other two were not, and every group kind the
		// grid knows about is asked for the same way.
		if (context->imageMask && context->imageMask->IsInGroup(line))
			return context->imageMask->GetGroupLines(line);
		return std::vector<AssDialogue *>{line};
	};
	auto object_times = [](std::vector<AssDialogue *> const& originals) {
		int start = std::numeric_limits<int>::max();
		int end = 0;
		for (auto original : originals) {
			start = std::min(start, static_cast<int>(original->Start));
			end = std::max(end, static_cast<int>(original->End));
		}
		return std::pair{start, end};
	};

	report(ApplyProgressStage::Preparing, 0, 1);
	size_t apply_total = 0;
	std::set<AssDialogue *> counted;
	for (auto line : selected) {
		if (counted.count(line)) continue;
		auto originals = originals_for(line);
		for (auto original : originals) counted.insert(original);
		auto [object_start, object_end] = object_times(originals);
		int start_frame = context->videoController->FrameAtTime(object_start,
			agi::vfr::START);
		int end_frame = context->videoController->FrameAtTime(object_end,
			agi::vfr::END);
		apply_total += static_cast<size_t>(std::max(1, end_frame - start_frame + 1));
	}
	report(ApplyProgressStage::Preparing, 1, 1);
	report(ApplyProgressStage::Applying, 0, apply_total);

	struct Work {
		std::vector<AssDialogue *> originals;
		std::vector<AssDialogue> output;
	};
	std::vector<Work> work;
	std::set<AssDialogue *> consumed;
	size_t apply_complete = 0;
	for (auto line : selected) {
		if (consumed.count(line)) continue;
		Work item;
		item.originals = originals_for(line);
		for (auto original : item.originals) consumed.insert(original);
		std::sort(item.originals.begin(), item.originals.end(),
			[](auto left, auto right) { return left->Row < right->Row; });

		auto [object_start, object_end] = object_times(item.originals);
		int start_frame = context->videoController->FrameAtTime(object_start,
			agi::vfr::START);
		int end_frame = context->videoController->FrameAtTime(object_end,
			agi::vfr::END);
		bool mask = IsImageMaskLine(item.originals.front());
		bool glitch_group = context->imageMask &&
			context->imageMask->IsGlitchGroup(item.originals.front());
		bool glitch_anchor_written = false;
		auto raster = mask ? imagemask::Decode(item.originals) : std::nullopt;
		if (mask && !raster) { error = "Az ImageMask képe nem olvasható vissza."; return false; }
		int script_width = 0, script_height = 0;
		context->ass->GetResolution(script_width, script_height);
		// One line in, one line out - so it is only for a line that stands alone. An object made
		// of several rows has to be written frame by frame like a mask, or the rest of its rows
		// would be taken away with nothing put back.
		bool linear_line = options.linear && !mask && item.originals.size() == 1 &&
			(!options.map_clips || !HasClip(*item.originals.front()));
		if (linear_line) {
			size_t first_sample = sample_for_frame(start_frame);
			size_t last_sample = sample_for_frame(end_frame);
			Homography first_map = AppliedMap(main_track, main_perspective_track,
				first_sample, options.reference_sample, options.main, true);
			Homography last_map = AppliedMap(main_track, main_perspective_track,
				last_sample, options.reference_sample, options.main, true);
			int first_start = std::max(static_cast<int>(item.originals.front()->Start),
				context->videoController->TimeAtFrame(start_frame, agi::vfr::START));
			int first_end = std::min(static_cast<int>(item.originals.front()->End),
				context->videoController->TimeAtFrame(start_frame, agi::vfr::END));
			int last_start = std::max(static_cast<int>(item.originals.front()->Start),
				context->videoController->TimeAtFrame(end_frame, agi::vfr::START));
			int last_end = std::min(static_cast<int>(item.originals.front()->End),
				context->videoController->TimeAtFrame(end_frame, agi::vfr::END));
			item.output.push_back(BuildLinearLine(context, *item.originals.front(),
				first_map, last_map, options, (first_start + first_end) / 2,
				(last_start + last_end) / 2));
			apply_complete += static_cast<size_t>(std::max(1, end_frame - start_frame + 1));
			report(ApplyProgressStage::Applying, apply_complete, apply_total);
			work.push_back(std::move(item));
			continue;
		}
		std::string previous_signature;
		size_t previous_begin = 0;
		for (int frame = start_frame; frame <= end_frame; ++frame) {
			size_t sample = sample_for_frame(frame);
			Homography main_map = AppliedMap(main_track, main_perspective_track, sample,
				options.reference_sample, options.main, options.linear);
			int start = std::max(object_start,
				context->videoController->TimeAtFrame(frame, agi::vfr::START));
			int end = std::min(object_end,
				context->videoController->TimeAtFrame(frame, agi::vfr::END));
			if (end <= start) {
				++apply_complete;
				report(ApplyProgressStage::Applying, apply_complete, apply_total);
				continue;
			}
			std::vector<AssDialogue> generated;
			if (mask) {
				auto warped = Warp(*raster, main_map, script_width, script_height);
				auto style = context->ass->GetStyle(item.originals.front()->Style.get());
				generated = imagemask::Encode(warped, *item.originals.front(), start, end,
					style);
			}
			else {
				int sample_time = (start + end) / 2;

				// Worked out once for the frame rather than once per row of the object.
				std::optional<Homography> clip_map;
				if (options.map_clips && clip_track) {
					size_t clip_sample = sample_for_frame(frame);
					size_t clip_reference = options.clip_reference_sample.value_or(
						options.reference_sample);
					clip_map = AppliedMap(*clip_track, clip_perspective_track, clip_sample,
						std::min(clip_reference, clip_track->samples.size() - 1),
						options.clip, options.linear);
				}

				bool alone = item.originals.size() == 1;
				for (auto original : item.originals) {
					if (!alone && (static_cast<int>(original->End) <= start ||
						static_cast<int>(original->Start) >= end)) continue;
					AssDialogue generated_line(*original);
					if (glitch_group &&
						typesetting::glitch::IsSource(*context->ass, &generated_line)) {
						if (glitch_anchor_written)
							typesetting::glitch::ClearGroupMetadata(*context->ass, generated_line);
						else
							glitch_anchor_written = true;
					}
					// A single line is written out plainly, as it always was. The rows of an
					// object keep what says they belong together - the extradata a gradient and a
					// textbox are known by - and keep being comments where they were meant to be
					// one, since a gradient holds its source that way. Cleared, the object would
					// not be recognised as an object on the other side.
					if (alone) {
						generated_line.Comment = false;
						generated_line.ExtradataIds = std::vector<uint32_t>();
					}
					if (options.interpolate_animations)
						InterpolateAnimations(context, generated_line, sample_time, start);
					if (!options.clip_only)
						generated_line.Text = MapLine(context, generated_line, main_map, options,
							options.map_clips && !clip_track, sample_time);
					else if (options.map_clips && !clip_track)
						generated_line.Text = typesetting::TransformProjectiveClips(generated_line.Text.get(),
							[&main_map](Vector2D point) { return main_map.Map(point); });
					generated_line.Start = start;
					generated_line.End = end;
					if (clip_map) {
						Homography const& map = *clip_map;
						generated_line.Text = typesetting::TransformProjectiveClips(generated_line.Text.get(),
							[&map](Vector2D point) { return map.Map(point); });
					}
					generated.push_back(std::move(generated_line));
				}
			}
			std::string signature = imagemask::Signature(generated);
			if (!generated.empty() && signature == previous_signature) {
				for (size_t index = previous_begin; index < item.output.size(); ++index)
					item.output[index].End = end;
			}
			else {
				previous_begin = item.output.size();
				previous_signature = std::move(signature);
				std::move(generated.begin(), generated.end(), std::back_inserter(item.output));
			}
			++apply_complete;
			report(ApplyProgressStage::Applying, apply_complete, apply_total);
		}
		if (item.output.empty()) { error = "A motion nem hozott létre látható eredményt."; return false; }
		work.push_back(std::move(item));
	}

	Selection new_selection;
	AssDialogue *new_active = nullptr;
	std::vector<std::unique_ptr<AssDialogue>> removed;
	size_t writing_total = 0;
	for (auto const& item : work)
		writing_total += item.output.size() + item.originals.size();
	size_t writing_complete = 0;
	report(ApplyProgressStage::Writing, 0, writing_total);
	for (auto& item : work) {
		auto insert_at = context->ass->Events.iterator_to(*item.originals.front());
		std::string id = NewId();
		std::string source = Sources(item.originals);
		for (auto& output : item.output) {
			auto generated = new AssDialogue(std::move(output));
			context->ass->SetExtradataValue(*generated, motion_id_key, id);
			context->ass->SetExtradataValue(*generated, motion_source_key, source);
			auto ids = generated->ExtradataIds.get();
			std::sort(ids.begin(), ids.end());
			generated->ExtradataIds = std::move(ids);
			context->ass->Events.insert(insert_at, *generated);
			new_selection.insert(generated);
			if (!new_active) new_active = generated;
			report(ApplyProgressStage::Writing, ++writing_complete, writing_total);
		}
		for (auto original : item.originals) {
			context->ass->Events.erase(context->ass->Events.iterator_to(*original));
			removed.emplace_back(original);
			report(ApplyProgressStage::Writing, ++writing_complete, writing_total);
		}
	}
	context->selectionController->SetSelectionAndActive(std::move(new_selection), new_active);
	context->ass->CleanExtradata();
	context->ass->Commit("apply motion", AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

bool Revert(agi::Context *context, std::string& error) {
	error.clear();
	auto selected = context->selectionController->GetSelectedSet();
	std::set<std::string> ids;
	for (auto line : selected) {
		auto id = Extra(*context->ass, *line, motion_id_key);
		if (!id.empty()) ids.insert(std::move(id));
	}
	if (ids.empty()) { error = "A kijelölésen nincs visszaállítható motion."; return false; }
	Selection selection;
	AssDialogue *active = nullptr;
	std::vector<std::unique_ptr<AssDialogue>> removed;
	for (auto const& id : ids) {
		std::vector<AssDialogue *> generated;
		std::string source;
		for (auto& line : context->ass->Events) {
			if (Extra(*context->ass, line, motion_id_key) != id) continue;
			if (source.empty()) source = Extra(*context->ass, line, motion_source_key);
			generated.push_back(&line);
		}
		if (generated.empty() || source.empty()) continue;
		auto insert_at = context->ass->Events.iterator_to(*generated.front());
		for (auto const& entry : SplitSources(source)) {
			try {
				auto original = new AssDialogue(entry);
				context->ass->Events.insert(insert_at, *original);
				selection.insert(original);
				if (!active) active = original;
			}
			catch (...) { }
		}
		for (auto line : generated) {
			context->ass->Events.erase(context->ass->Events.iterator_to(*line));
			removed.emplace_back(line);
		}
	}
	if (selection.empty()) { error = "A motion eredeti sorai nem olvashatók."; return false; }
	context->selectionController->SetSelectionAndActive(std::move(selection), active);
	context->ass->CleanExtradata();
	context->ass->Commit("revert motion", AssFile::COMMIT_DIAG_ADDREM |
		AssFile::COMMIT_DIAG_FULL | AssFile::COMMIT_EXTRADATA);
	return true;
}

} // namespace typesetting::motion
