#ifndef slic3r_NarrowRoofInfill_hpp_
#define slic3r_NarrowRoofInfill_hpp_

#include "../ClipperUtils.hpp"
#include "../Layer.hpp"

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
