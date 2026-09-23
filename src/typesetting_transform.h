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

/// @file typesetting_transform.h
/// @brief Reshaping the drawings of the selected lines
///
/// Every transform is one point-to-point mapping applied to the drawings of the whole
/// selection at once, in absolute script coordinates. Working in absolute coordinates
/// is what lets lines that sit in different places be bent together: each line's own
/// origin is taken out before the mapping and put back after, so a single deformation
/// covers all of them.
///
/// The mapping is deliberately separate from the code that walks the drawings, because
/// the arch, the free distort and everything still to come - scale, rotate, skew,
/// perspective, the flips - differ only in the mapping.

#pragma once

#include "vector2d.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/string.h>

namespace agi { struct Context; }
class AssDialogue;

namespace typesetting {

/// A transform: where a point in script coordinates goes.
using PointMap = std::function<Vector2D (Vector2D)>;

/// A rectangle that need not be upright.
///
/// Everything a transform does is expressed inside this box - the arch bends along its
/// long axis, the corners of the distort are its corners - so text lying at an angle
/// bends along itself rather than along the screen.
struct OrientedBox {
	Vector2D centre;
	float angle = 0;      ///< of the box's own x axis, in degrees, clockwise on screen
	Vector2D half;        ///< half the width and height, along that axis and across it

	/// A point of the box's own frame, in script coordinates.
	Vector2D ToScript(Vector2D local) const;
	/// A point in script coordinates, in the box's frame.
	Vector2D ToLocal(Vector2D point) const;
	/// The four corners in script coordinates: top left, top right, bottom right,
	/// bottom left of the box as it lies.
	void Corners(Vector2D out[4]) const;
};

/// Whether there is a selection with something in it to transform.
bool CanTransform(const agi::Context *c);

/// The drawings of the selection, kept as they were so a mapping can be applied to
/// the originals as often as a drag needs - which is what makes a live preview
/// possible without the shape drifting.
///
/// Nothing here touches the file. Text that has to become a drawing is converted in
/// memory, the mapping is applied to that, and the result waits in here until Apply is
/// called - so a reshaping that is never accepted leaves no trace in the lines, the edit
/// box or the undo history.
class ShapeEditor {
	struct Impl;
	std::shared_ptr<Impl> impl;

public:
	enum class LayerKind {
		Primary,
		Outline,
		Shadow
	};

	/// One visible layer produced by the last Build, together with the same geometry
	/// in absolute script coordinates. This lets callers cut an already converted
	/// text outline into smaller drawings without parsing its placement again.
	struct ShapeLayer {
		AssDialogue *source = nullptr;
		LayerKind kind = LayerKind::Primary;
		std::string text;
		Vector2D centre;
		std::vector<std::vector<Vector2D>> contours;
		bool covered = false;
	};

	/// Collects the drawings of the current selection.
	explicit ShapeEditor(const agi::Context *c);
	/// Collects an explicit set of source lines without changing the grid selection.
	ShapeEditor(const agi::Context *c, std::vector<AssDialogue *> const& lines);

	/// Whether anything was found to work on.
	bool ok() const;
	/// How many lines held a drawing this cannot model, and were left alone.
	int refused() const;

	/// The box the untransformed drawings occupy together, turned to lie along them.
	///
	/// Not an upright rectangle: a rotation is baked into a converted shape's
	/// coordinates, so an upright box around rotated text is a big loose rectangle
	/// that says nothing about the text. This is the smallest box that fits, whatever
	/// angle it has to take, which for a line of text is the box along its baseline.
	OrientedBox Box() const;
	/// The same box without the minimum handle spacing added by the visual editor.
	/// Geometry consumers such as Gradient need the actual ink bounds so that their
	/// spatial range is not enlarged merely to make the transform handles usable.
	OrientedBox ContentBox() const;

	/// Why some lines had to be left alone, one message per reason.
	std::vector<std::string> const& refusals() const;

	/// Work out what the lines would say with this mapping applied, without writing it.
	///
	/// Long straight runs are split up first, because a straight line stays straight
	/// however it is mapped and would cut across a curve instead of following it. A
	/// mapping that keeps straight lines straight - a flip, say - does not need that and
	/// is better off without the extra points.
	///
	/// The clips go through the same mapping unless asked not to: they are given in script
	/// coordinates and belong to the picture, so a gradient written as a stack of clipped
	/// copies only holds together if its bands bend with the text.
	/// `decorations` asks for the border and the shadow to be stood in for by shapes of their own,
	/// which is the only way either of them can follow a bend: a pen stays upright whatever happens
	/// to the letters, and \bord cannot go negative, so no value would be right. One line then
	/// becomes up to three - the shadow, the border over it, and the fill over both - and the fill
	/// is left out when it is painted the border's own colour, since the widened shape holds it.
	void Build(PointMap const& map, bool subdivide = true, bool map_clips = true,
	           bool decorations = false, bool collect_geometry = true);

	/// The outline of the last Build, as closed polylines in script coordinates.
	std::vector<std::vector<Vector2D>> const& contours() const;
	/// Fill, border and shadow outlines made by the last Build, in paint order.
	std::vector<ShapeLayer> const& layers() const;
	/// Replace a built layer's drawing with an arbitrary subset of its absolute contours.
	std::string TextForContours(ShapeLayer const& layer,
		std::vector<std::vector<Vector2D>> const& contours) const;

	/// The lines the session is working on.
	std::vector<AssDialogue *> const& lines() const;

	/// What the video should show instead of the file while the reshaping is being worked
	/// out: the lines it came from, silenced, and the drawings that will replace them.
	///
	/// Exactly what Apply would write, so the preview and the result are the same picture -
	/// including the case where one line of text becomes several drawings.
	struct Preview {
		std::vector<std::unique_ptr<AssDialogue>> silenced;
		std::vector<std::unique_ptr<AssDialogue>> drawings;
	};
	Preview PreviewLines() const;

	/// Write the last Build into the file. The drawing goes in as a new line and the one it
	/// came from is kept as a comment, so the text is never thrown away.
	///
	/// The caller commits, and has to say that lines were added. From inside a visual tool
	/// the commit also has to go through the tool's own, which blocks the file-changed
	/// listener - committing here let it run mid-drag, and it clears the drag flags.
	void Apply();

	/// The lines Apply put in, in case the caller wants to leave them selected.
	std::vector<AssDialogue *> const& applied() const;
};

/// The mapping that sends the corners of a box to four arbitrary points. Straight lines
/// stay straight, which is what makes it a distort rather than a warp. Corners are
/// ordered the way OrientedBox::Corners gives them.
PointMap QuadMap(OrientedBox const& box, Vector2D const corners[4]);

/// The inverse of QuadMap: from the visible quadrilateral back into the box's own
/// coordinates. Used by editors whose caret and wrapping remain in local box space.
PointMap QuadInverseMap(OrientedBox const& box, Vector2D const corners[4]);

/// Carry every rectangular or vector clip in a line through the same screen map.
/// Nonlinear warps subdivide edges and restrict rectangles to the working bounds.
std::string TransformClips(std::string const& text, PointMap const& map,
	OrientedBox const& bounds, double subdivision_span = 8.0);

/// Motion preserves straight edges, contour closure and affine Bezier control points.
/// Only curves distorted by perspective may need additional control points.
std::string TransformProjectiveClips(std::string const& text, PointMap const& map);

/// How far a point is from where that same map sends the plane to infinity.
///
/// One at the top left corner of the box and falling away towards the vanishing line, which is
/// where it reaches zero. It is affine in the point, so a shape can be cut against it exactly -
/// and it has to be, because past that line a shape comes back inside out and enormous.
double QuadDepth(OrientedBox const& box, Vector2D const corners[4], Vector2D point);

/// What the user drags in a warp, which is less than the sixteen points of the patch.
///
/// Photoshop's warp is a bicubic Bézier patch over a three by three grid of cells. Its
/// handles are the four corners with two direction handles each - twelve points, all on
/// the boundary. The four inside the patch are never shown: they follow from the boundary
/// as a Coons patch, which is what makes moving a corner carry the middle of the mesh
/// along with it instead of leaving it pinned. Dragging the mesh itself is the only thing
/// that moves the middle on its own, and that is what `inner` holds.
struct WarpNet {
	Vector2D corner[4];    ///< top left, top right, bottom right, bottom left
	Vector2D tangent[8];   ///< two per edge in that corner order, first nearer its corner
	Vector2D inner[4];     ///< what dragging the mesh added to the middle, row major
};

/// Put the net back on an untouched box, where the patch is the box itself.
void WarpReset(OrientedBox const& box, WarpNet& net);

/// The sixteen Bézier control points, row major with the rows running down the box.
void WarpControls(WarpNet const& net, Vector2D out[16]);

/// A point of the patch. (0,0) is the box's top left corner, (1,1) its bottom right.
Vector2D WarpPoint(Vector2D const control[16], double u, double v);

/// The same bend, measured from a net the selection already rests on.
///
/// WarpMap measures its displacement against the box's own untouched net, so it is the identity
/// while the net is untouched - which is true only for a selection that rests in the box. Text
/// that leans or stands out of the plane rests in a quadrilateral instead and its net starts
/// there, and measuring from the box would drag the whole selection onto the box the moment the
/// tool opened. `into_box` takes a point of that quadrilateral back to the box, which is what
/// gives the patch its u and v - and being a plain map it can be projective, where the patch
/// itself cannot: a Bezier patch reproduces an affine shape exactly and a keystone only nearly.
PointMap WarpMapFrom(OrientedBox const& box, WarpNet const& rest, WarpNet const& net,
                     PointMap const& into_box);

/// Which two direction handles belong to a corner.
void WarpCornerTangents(int corner, int& first, int& second);

/// Move a corner, taking its two direction handles with it - as dragging a corner does.
void WarpMoveCorner(WarpNet& net, int corner, Vector2D delta);

/// Where on the patch a point of the video is. False if it is not on the patch, which is
/// how a click beside the mesh is told from one inside it.
bool WarpLocate(Vector2D const control[16], Vector2D point, double tolerance,
                double& u, double& v);

/// Drag the mesh itself at (u,v): the smallest change to the net that moves that point of
/// the surface by `delta`. The corners stay where they are, as they do in Photoshop; what
/// gives are the direction handles and the middle.
void WarpDragInside(WarpNet& net, double u, double v, Vector2D delta);

/// The mapping the warp describes.
PointMap WarpMap(OrientedBox const& box, WarpNet const& net);

/// Mirror about a centre, along the video's own axes rather than the box's, because that
/// is the flip the eye expects.
PointMap FlipMap(Vector2D centre, bool horizontal);

} // namespace typesetting
