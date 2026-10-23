#ifndef slic3r_LocalZMaskTimeline_hpp_
#define slic3r_LocalZMaskTimeline_hpp_

#include "ClipperUtils.hpp"

namespace Slic3r {

// Expand the outer footprint before partitioning its history, never each
// temporal fragment independently (which would overlap adjacent fragments).
constexpr double LOCAL_Z_PERIMETER_MASK_EXPAND_MM = 0.10;

struct LocalZMaskSlab
{
    size_t     layer_id;
    double     z_lo, z_hi;
    ExPolygons masks;
};

// Partition ONE cadence pass into continuous ownership spans. State is bounded
// by the nominal layers touched by that pass, not the total object height.
// Departing areas use their last owning layer's paths; entering areas cannot
// inherit thickness deposited before they belonged to this material.
inline std::vector<LocalZMaskSlab> partition_local_z_mask_span(const std::vector<LocalZMaskSlab>& slabs)
{
    std::vector<LocalZMaskSlab> pending, result;
    for (const LocalZMaskSlab& slab : slabs) {
        ExPolygons                  previous;
        std::vector<LocalZMaskSlab> next;
        for (LocalZMaskSlab& span : pending) {
            if (slab.z_lo > span.z_hi + EPSILON) {
                result.emplace_back(std::move(span));
                continue;
            }
            append(previous, span.masks);
            if (span.masks == slab.masks) {
                span.layer_id = slab.layer_id;
                span.z_hi     = slab.z_hi;
                next.emplace_back(std::move(span));
                continue;
            }
            ExPolygons continuing = intersection_ex(span.masks, slab.masks);
            ExPolygons departing  = diff_ex(span.masks, slab.masks);
            if (!departing.empty())
                result.push_back({span.layer_id, span.z_lo, span.z_hi, std::move(departing)});
            if (!continuing.empty())
                next.push_back({slab.layer_id, span.z_lo, slab.z_hi, std::move(continuing)});
        }
        ExPolygons entering = previous.empty() ? slab.masks : diff_ex(slab.masks, previous);
        if (!entering.empty())
            next.push_back({slab.layer_id, slab.z_lo, slab.z_hi, std::move(entering)});
        pending = std::move(next);
    }
    for (LocalZMaskSlab& span : pending)
        result.emplace_back(std::move(span));
    return result;
}

} // namespace Slic3r
#endif
