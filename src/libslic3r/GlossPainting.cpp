#include "GlossPainting.hpp"
#include "ClipperUtils.hpp"
#include <algorithm>
#include <cmath>

namespace Slic3r {

double GlossSpeedRange::speed_for_time(double seconds) const
{
    if (!std::isfinite(seconds) || seconds <= 0.)
        return max_speed;
    return std::clamp(GLOSS_REFERENCE_LENGTH / seconds, min_speed, max_speed);
}

GlossSpeedRange gloss_speed_range(double feature_speed,
                                  double minimum_speed,
                                  double maximum_speed,
                                  double max_x,
                                  double max_y,
                                  double max_e,
                                  double volumetric_limit,
                                  double mm3_per_mm,
                                  double e_per_mm)
{
    double     maximum = std::isfinite(feature_speed) && feature_speed > 0. ? feature_speed : 1.;
    const auto cap     = [&maximum](double value) {
        if (std::isfinite(value) && value > 0.)
            maximum = std::min(maximum, value);
    };
    cap(maximum_speed);
    cap(max_x);
    cap(max_y);
    if (e_per_mm > 0.)
        cap(max_e / e_per_mm);
    if (mm3_per_mm > 0.)
        cap(volumetric_limit / mm3_per_mm);
    const double minimum = std::isfinite(minimum_speed) && minimum_speed > 0. ? minimum_speed : 1.;
    return {std::min(minimum, maximum), maximum};
}

std::vector<GlossPaintPath> split_gloss_path(const Polyline& path, const GlossPaintRegions& regions)
{
    std::vector<GlossPaintPath> result;
    if (path.points.size() < 2)
        return result;

    // Clip once to the path's bounding box, avoiding the rest of a large model
    // in the per-segment intersections below.
    std::array<Polygons, GLOSS_PAINT_COLORS> clips;
    const BoundingBox                        bbox = get_extents(path).inflated(SCALED_EPSILON);
    for (size_t color = 0; color < regions.size(); ++color)
        clips[color] = ClipperUtils::clip_clipper_polygons_with_subject_bbox(regions[color], bbox);

    struct Span
    {
        double begin, end;
        size_t color;
    };
    for (size_t i = 1; i < path.points.size(); ++i) {
        const Point &a = path.points[i - 1], &b = path.points[i];
        const Vec2d  delta          = (b - a).cast<double>();
        const double length_squared = delta.squaredNorm();
        if (length_squared == 0.)
            continue;

        std::vector<Span>   spans;
        std::vector<double> cuts{0., 1.};
        const auto parameter = [&](const Point& p) { return std::clamp((p - a).cast<double>().dot(delta) / length_squared, 0., 1.); };
        for (size_t color = 0; color < clips.size(); ++color) {
            if (clips[color].empty())
                continue;
            for (const Polyline& inside : intersection_pl(Polyline(a, b), clips[color])) {
                double begin = parameter(inside.first_point()), end = parameter(inside.last_point());
                if (begin > end)
                    std::swap(begin, end);
                if (end > begin) {
                    spans.push_back({begin, end, color + 1});
                    cuts.push_back(begin);
                    cuts.push_back(end);
                }
            }
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        Point from = a;
        for (size_t cut = 1; cut < cuts.size(); ++cut) {
            const double midpoint = (cuts[cut - 1] + cuts[cut]) * 0.5;
            size_t       color    = 0;
            for (const Span& span : spans)
                if (midpoint >= span.begin && midpoint <= span.end) {
                    color = span.color;
                    break;
                }
            const Point to = cuts[cut] == 1. ? b :
                                               Point(coord_t(std::llround(double(a.x()) + delta.x() * cuts[cut])),
                                                     coord_t(std::llround(double(a.y()) + delta.y() * cuts[cut])));
            if (from != to) {
                if (!result.empty() && result.back().color == color && result.back().polyline.last_point() == from)
                    result.back().polyline.points.push_back(to);
                else
                    result.push_back({Polyline(from, to), color});
            }
            from = to;
        }
    }
    return result;
}

} // namespace Slic3r
