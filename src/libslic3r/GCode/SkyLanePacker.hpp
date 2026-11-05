// SkyLane lane packing, adapted from Neotko's supplied TourHTML/skylane.html
// (the JavaScript port of SkyLanePacker.cpp, October 2026).
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <vector>

namespace Slic3r::SkyLane {

struct Parameters
{
    double width             = 20.;
    int    lanes             = 2;
    double max_dip           = .4;
    double min_height        = .04;
    double max_height        = .3;
    double tip_radius        = .5;
    double nozzle_cone_angle = 40.;
    double line_width        = .5;
    double fill_density      = .2;
    double merged_density    = .1;
    double merged_reserve    = .2;
    double purge_density     = .8;
    bool   solid_base        = true;
};

struct Demand
{
    double z            = 0.;
    double layer_height = .2;
    double volume       = 0.;
    size_t layer        = 0;
};

struct Pass
{
    size_t owner   = 0;
    int    demand  = -1; // -1 is structural fill; otherwise an index into the demand sequence.
    int    lane    = 0;
    double z_base  = 0.;
    double height  = 0.;
    double y0      = 0.;
    double y1      = 0.;
    double density = 1.;
    bool   solid   = false;
    bool   merged  = false;
    bool   wall    = false; // One shared perimeter when a merged round opens.
    double cell_y0 = 0., cell_y1 = 0.;

    double z() const { return z_base + height; }
};

struct Plan
{
    bool              ok            = false;
    int               lanes         = 0;
    double            depth         = 0.;
    size_t            failed_demand = 0;
    std::vector<Pass> passes;
    double            width = 0.;
};

// Deterministic chronological packing. No input or configuration is mutated.
Plan pack_scheduled(const std::vector<Demand>& demands, double depth, const Parameters& params);
// Try 2..6 lanes, binary-search depth, then replan with 15% reserve.
Plan plan(const std::vector<Demand>& demands, const Parameters& params);
// Size both dimensions together, with 15% area reserve. params.width is the
// minimum side length; the returned square is replanned, never stretched.
Plan plan_square(const std::vector<Demand>& demands, const Parameters& params);
// Reduce only merged cells, centred on the original footprint. Each step is
// accepted only if the remaining demand sequence can still be packed.
Plan taper(const std::vector<Demand>& demands, const Parameters& params, const Plan& full);

} // namespace Slic3r::SkyLane
