#ifndef slic3r_GlossPainting_hpp_
#define slic3r_GlossPainting_hpp_

#include "ExPolygon.hpp"
#include "Polyline.hpp"
#include <array>

namespace Slic3r {

// These are surface finishes, independent of filament/extruder IDs. State zero
// is unpainted. Times are seconds per 100 mm, not a promise of total layer time.
constexpr size_t                                 GLOSS_PAINT_COLORS     = 4;
constexpr double                                 GLOSS_REFERENCE_LENGTH = 100.;
constexpr std::array<double, GLOSS_PAINT_COLORS> GLOSS_DEFAULT_TIMES{1., 2., 4., 8.};
using GlossPaintRegions = std::array<ExPolygons, GLOSS_PAINT_COLORS>;

struct GlossPaintPath
{
    Polyline polyline;
    size_t   color = 0;
};

// Preserve traversal order, including closed paths, holes and repeated points.
// Only paint boundaries introduce new vertices; adjacent spans of one color
// are joined. The first color wins on an exactly overlapping region.
std::vector<GlossPaintPath> split_gloss_path(const Polyline& path, const GlossPaintRegions& regions);

struct GlossSpeedRange
{
    double min_speed;
    double max_speed;

    double min_time() const { return GLOSS_REFERENCE_LENGTH / max_speed; }
    double max_time() const { return GLOSS_REFERENCE_LENGTH / min_speed; }
    double speed_for_time(double seconds) const;
};

// Zero limits mean unspecified. A lower maximum always wins over a conflicting
// minimum, so a cooling minimum cannot force the hotend or axes past their cap.
GlossSpeedRange gloss_speed_range(double feature_speed,
                                  double minimum_speed,
                                  double maximum_speed,
                                  double max_x,
                                  double max_y,
                                  double max_e,
                                  double volumetric_limit,
                                  double mm3_per_mm,
                                  double e_per_mm);

} // namespace Slic3r
#endif
