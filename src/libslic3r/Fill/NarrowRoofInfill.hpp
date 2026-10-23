#ifndef slic3r_NarrowRoofInfill_hpp_
#define slic3r_NarrowRoofInfill_hpp_

#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include <algorithm>
#include <unordered_map>

namespace Slic3r {

// A top surface at layer i disappears from the outline at i + 1. Inspect that
// next outline only while i itself falls within the configured roof shell.
inline ExPolygons roof_covered_outline(const Layer& layer, int roof_layers, double roof_depth)
{
    ExPolygons   covered = layer.lslices;
    const Layer* surface = &layer;
    for (int steps = 0; surface && (steps < roof_layers || surface->print_z - layer.print_z < roof_depth - EPSILON); ++steps) {
        if (surface->upper_layer == nullptr) {
            covered.clear();
            break;
        }
        covered = intersection_ex(covered, surface->upper_layer->lslices);
        surface = surface->upper_layer;
    }
    return covered;
}

// A medial-axis junction can become isolated after its other branches are
// pruned. Do not let a sub-width stub carve a break across otherwise printable
// roof infill. Reclaim its complete footprint only inside a fillable region;
// preserve real junction links, closed paths and gaps outside that region.
template<class FillableForRetained>
inline size_t reclaim_roof_gap_stubs(ThickPolylines& paths, const FillableForRetained& fillable_for_retained, double scaled_width_padding)
{
    if (paths.empty())
        return 0;
    std::unordered_map<Point, size_t, PointHash> endpoints;
    for (const ThickPolyline& path : paths)
        if (path.points.size() >= 2) {
            ++endpoints[path.first_point()];
            ++endpoints[path.last_point()];
        }
    std::vector<Polygons> candidates(paths.size());
    size_t                count = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        const ThickPolyline& path = paths[i];
        if (path.points.size() < 2 || path.width.empty() || path.first_point() == path.last_point() || endpoints[path.first_point()] > 1 ||
            endpoints[path.last_point()] > 1)
            continue;
        const double width = *std::max_element(path.width.begin(), path.width.end());
        if (path.length() < width) {
            candidates[i] = offset(Polyline(path.points), float((width + scaled_width_padding) * .5 + 10.));
            count += !candidates[i].empty();
        }
    }
    // Start by reclaiming all candidates. Restoring an unfillable candidate
    // can block a neighbour too, so repeat until the actual retained gap-fill
    // footprint leaves every reclaimed stub inside the final infill boundary.
    while (count > 0) {
        ThickPolylines retained;
        for (size_t i = 0; i < paths.size(); ++i)
            if (candidates[i].empty())
                retained.push_back(paths[i]);
        const ExPolygons fillable = fillable_for_retained(retained);
        bool             restored = false;
        for (Polygons& candidate : candidates)
            if (!candidate.empty() && !diff_ex(candidate, fillable).empty()) {
                candidate.clear();
                --count;
                restored = true;
            }
        if (!restored) {
            paths = std::move(retained);
            break;
        }
    }
    return count;
}

// Internal colour boundaries can make a broad roof appear narrow to a single
// material's filler. Keep its roof support straight, but retain concentric
// filling where the OBJECT itself is narrow (ribs, tips and small islands).
inline ExPolygons broad_roof_support(const ExPolygons& object_outline, const ExPolygons& above_roof, double spacing)
{
    const ExPolygons broad = opening_ex(object_outline, float(scale_(2.0 * spacing)));
    return above_roof.empty() ? broad : diff_ex(broad, above_roof);
}

inline void keep_partitioned_roof_infill_straight(ExPolygons&       normal,
                                                  ExPolygons&       narrow,
                                                  const ExPolygons& broad_roof,
                                                  double            boundary_tolerance = 0.0)
{
    if (narrow.empty() || broad_roof.empty())
        return;
    const ExPolygons eligible = boundary_tolerance > 0 ? offset_ex(broad_roof, float(scale_(boundary_tolerance))) : broad_roof;
    bool             restored = false;
    for (auto it = narrow.begin(); it != narrow.end();) {
        // Do not split an island into more tiny fragments. If any part belongs
        // to a genuinely narrow feature, keep its existing concentric filling.
        if (diff_ex(ExPolygons{*it}, eligible).empty()) {
            normal.emplace_back(std::move(*it));
            it       = narrow.erase(it);
            restored = true;
        } else
            ++it;
    }
    if (restored)
        normal = union_ex(normal);
}

} // namespace Slic3r
#endif
