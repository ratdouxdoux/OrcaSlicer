#pragma once

#include "WipeTower.hpp"
#include "libslic3r/BoundingBox.hpp"
#include <map>

namespace Slic3r {
class PrintConfig;
class PrintRegionConfig;

// Requests are in actual emission order: Local-Z transitions, then nominal
// transitions, then any structural visit. Positional identity is preserved.
struct SkyLaneRequest
{
    size_t layer    = 0;
    bool   local_z  = false;
    size_t old_tool = 0, new_tool = 0;
    double z = 0., height = .2, volume = 0.;
};

struct SkyLaneTowerResult
{
    std::vector<std::vector<WipeTower::ToolChangeResult>> nominal, local_z;
    std::vector<float>                                    used_filament;
    double                                                depth = 0., height = 0., brim = 0.;
    int                                                   toolchanges = 0;
    BoundingBoxf                                          bounds;
    std::map<float, Polylines>                            outer_wall;
};

SkyLaneTowerResult generate_skylane_tower(const PrintConfig&                 config,
                                          const PrintRegionConfig&           region,
                                          const std::vector<SkyLaneRequest>& requests,
                                          size_t                             layer_count);
} // namespace Slic3r
