#ifndef slic3r_PaintedWallInset_hpp_
#define slic3r_PaintedWallInset_hpp_

#include "ClipperUtils.hpp"

namespace Slic3r {

// Short, sharp teeth in a painted boundary survive a miter inset and can move
// inward by several times the requested distance. Each subsequent wall then
// amplifies the error. Use a distance-based first inset at material interfaces,
// constrained by the ordinary object inset to preserve physical corners/holes.
inline ExPolygons inset_painted_wall(const ExPolygons& region, const ExPolygons& object_inset, float distance)
{
    ExPolygons       miter     = offset_ex(region, -distance);
    const ExPolygons round     = offset_ex(region, -distance, jtRound, scale_(.005));
    const ExPolygons recovered = diff_ex(round, miter);
    if (recovered.empty())
        return miter;
    ExPolygons result = union_ex(miter, intersection_ex(recovered, object_inset));
    // Boolean joins can leave a one-unit-wide needle that becomes a retraced
    // extrusion after G-code rounding. Clean only integer-grid noise (2 nm).
    for (ExPolygon& polygon : result) {
        ClipperLib::CleanPolygon(polygon.contour.points, 2.);
        for (Polygon& hole : polygon.holes)
            ClipperLib::CleanPolygon(hole.points, 2.);
    }
    return union_ex(result);
}

} // namespace Slic3r
#endif
