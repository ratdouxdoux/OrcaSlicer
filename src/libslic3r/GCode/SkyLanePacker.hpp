// SkyLane packer.
// Pure, dependency-free (std only) so it can be tested on its own, without a printer
// or a slice. The tower depth (Y) is split into `lanes` parallel lanes; each purge
// goes to the lowest lane and is stacked as a tall pass (<= hmax) instead of a thin
// full-tower shelf. The nozzle may drop into the tower, but no lane may stay more
// than `max_pit` below the nozzle: lagging lanes get sparse fill.
#ifndef slic3r_SkyLanePacker_hpp_
#define slic3r_SkyLanePacker_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r { namespace SkyLane {

struct Demand
{
    float z       = 0.f; // nozzle Z when the toolchange happens
    float volume  = 0.f; // purge volume, mm3
    float layer_h = 0.f; // nominal layer height here (0 = unknown → hmax): how far the nozzle rises next
    int   layer   = 0;   // print layer index (a visit ends when it changes)
    // Lanes in use at this point (0 = all), from the offline schedule (pack_scheduled).
    // 1 = the simple zone: ALL lanes are fused into one cell spanning the whole footprint
    // (like merging cells in a spreadsheet): an open grid raised in tall passes, the purge
    // spread over it — the classic tower's sparse layer. The writer gives every cell pass its
    // own perimeter, which is the tower wall there (same height as the grid: nothing to scrape).
    // 2..k-1: lanes with index >= active are RETIRED: never
    // purged into, never levelled again — the tower tapers where the rest of the print no longer
    // needs them (a lower neighbour is harmless: the nozzle never hits something below it).
    int active = 0;
    // taper: depth of the merged cell from this demand on (0 = as it is). The cell only
    // ever gets shallower, centred on the footprint, and only between rounds (when it is level).
    float            depth          = 0.f;
    size_t           old_tool       = 0;
    size_t           new_tool       = 0;
    float            ramming_volume = 0.f; // outgoing-tool portion of volume
    std::vector<int> present_blocks;       // material blocks with a compatible tool visiting at this Z
};

struct Params
{
    double width          = 20.0; // tower X width, mm
    int    lanes          = 2;
    double max_pit        = 0.4; // max depth a lane may lag below the nozzle (z_hop)
    double hmin           = 0.04;
    double hmax           = 0.30;
    double r_tip          = 0.5;  // nozzle tip flat radius
    double cone_deg       = 40.0; // nozzle cone half-angle from vertical
    double line_w         = 0.5;
    double fill_density   = 0.5;
    double merged_density = 0.1; // open grid of the merged cell (lines every 10 widths)
    // a purge never fills more than this share of its box: lines 5 % apart instead of
    // touching (a full box of lw x h lines has no room for any excess; Classic spaces 120 %).
    double purge_density = 1. / 1.05;
    // The merged cell keeps this much room below the nozzle for the next purge (spread over the
    // whole footprint a purge is thin), and is raised in tall passes when it lags one pass more.
    double merged_reserve = 0.2;
    // On a visit WITHOUT purge (a "traditional" layer: one colour, no toolchange), when a lane
    // hits the pit limit, how much of the pit is recovered in one go (0 = just back to the limit,
    // a thin pass every layer; 1 = up to the nozzle Z: tall passes, then nothing for several
    // layers). Never on purge visits: full lanes leave no room for the next purges (in simulation:
    // depth 35 → 53 mm on bigtest-var).
    double catch_up     = 0.;
    bool   solid_base   = false; // close the whole footprint solid at the end of the first visit
    double unpurged_base_height = 0.;    // configured bed layer when the entire first layer has no tool change
    bool   allow_over_z = false; // see Plan::over_z; pack_min_depth only enables it as a last resort
    // taper: the merged cell never gets shallower than this (0 = no taper at all).
    double taper_min = 0.;
    // Multi-material towers keep consecutive lanes in permanent adhesion-category
    // blocks. Empty on a single-category tower, which may merge and taper freely.
    std::vector<int>    tool_block;
    std::vector<int>    block_lane_ends; // exclusive lane index
    std::vector<double> block_last_z;
    bool                block_rounds = false; // pack neighbouring strips at a common height within each block
};

// One deposited pass. demand < 0 => sparse fill of a lagging lane.
// owner = the demand being served when the pass is laid down (the TCR that must emit it):
// fills that level lagging lanes at the end of a visit belong to that visit's last demand,
// so they never sit above the nozzle Z of the TCR that prints them.
// Zero-volume demands are allowed: they are tower visits with no purge (structural layers)
// and only trigger the levelling.
struct Pass
{
    int    demand = -1;
    int    owner  = -1;
    int    lane   = 0;
    double z_base = 0.; // lane top before the pass
    double h      = 0.; // pass height (pass top = z_base + h)
    double y0     = 0.; // usable Y band inside the tower [y0, y1]
    double y1     = 0.;
    bool   solid  = false; // 100 % fill (first-layer base), otherwise fill_density
    bool   merged = false; // pass of the merged cell (all lanes fused)
    // merged cell only: this pass also prints the cell's perimeter (the tower wall), from
    // z_base to z_base + h around the WHOLE footprint. True on the pass that opens a round (see
    // Lanes::place) and on the full-cell grid passes; [y0, y1] is then only where the lines go.
    bool wall = false;
    // Purge passes: fraction of the pass box the purge really fills (1 = solid lines). When the
    // box is bigger than the purge asked for (min pass height over a big cell), the lines are
    // spaced out so the pass extrudes exactly the requested volume, not the box.
    double density = 1.;
    // merged cell: the cell this pass belongs to, [cell_y0, cell_y1] (the tower wall there).
    double cell_y0 = 0., cell_y1 = 0.;
    size_t tool        = 0;
    bool   ramming     = false;
    bool   ends_round  = false;
    bool   block_round = false; // strip within one material block, inside the shared external fence
    int    lane_count  = 1;     // a solid foundation pass can support several lanes at once
};

struct Plan
{
    bool              ok    = false;
    double            depth = 0.; // tower Y depth
    std::vector<Pass> passes;
    double            fill_volume = 0.;
    // A demand whose nozzle Z is already below every lane top (a sublayer emitted after a
    // higher pass of the same layer) gets a min-height pass on the lowest lane, ABOVE its
    // nominal Z. Counted here; > 0 means the demand order is not monotonic in Z.
    int                over_z     = 0;
    double             over_z_max = 0.; // worst height above the nozzle Z
    int                first_over = -1; // demand index of the first one
    int                fail_index = -1; // !ok: the demand that found no room
    std::vector<int>   active;          // pack_min_depth: lanes in use per demand (1 = merged)
    std::vector<float> cell_depth;      // pack_taper: depth of the merged cell per demand (0 = full depth)
};

// Incremental lane state: one demand at a time. Used offline (pack) and at emission time.
class Lanes
{
public:
    Lanes(const Params& p, double depth);
    // Places demand `idx`: [solid first-layer base] + purge passes + levelling fills, appended
    // to `out` in print order. false = impossible (only without allow_over_z).
    bool place(const Demand& e, int idx, std::vector<Pass>& out);

    const std::vector<double>& heights() const { return m_H; }
    double                     depth() const { return m_depth; }
    double                     y0() const { return m_y0; }
    double                     z0() const { return m_z0; }
    bool                       base_now() const { return m_base_now; } // the last place() closed the base
    bool                       merged() const { return m_merged; }
    double                     fill_volume() const { return m_fill; }
    int                        over_z() const { return m_over_z; }
    double                     over_z_max() const { return m_over_z_max; }
    int                        first_over() const { return m_first_over; }

private:
    bool   place_material_blocks(const Demand& e, int idx, std::vector<Pass>& out);
    bool   place_lanes(const Demand& e, int idx, int first_lane, int end_lane, std::vector<Pass>& out);
    double band(int c, double t, double& y0, double& y1) const;
    void   close_round(std::vector<Pass>& out, int idx);
    void   push(std::vector<Pass>& out, int demand, int owner, int c, double h, double y0, double y1, bool solid);

    Params              m_p;
    double              m_depth, m_d, m_tg;
    double              m_y0 = 0.; // where the (tapered) cell starts; lanes go from here too
    std::vector<double> m_H;
    std::vector<Lanes>  m_block_cells;
    double              m_z0      = 0.;
    bool                m_have_z0 = false, m_base_done = false, m_base_now = false, m_merged = false;
    double              m_fill = 0., m_over_z_max = 0.;
    // merged cell "round" in progress: m_H is its base, purges fill strips of height
    // m_round_h; the first material grows from the far end (m_round_tail), the
    // others from the near end (m_round_y). The remaining gap closes with grid.
    double              m_round_h = 0., m_round_y = 0., m_round_tail = 0.;
    size_t              m_round_tool = 0;
    std::vector<size_t> m_round_tools;
    int                 m_over_z = 0, m_first_over = -1;
};

// Simulate with a fixed depth. ok == false when infeasible.
Plan pack(const std::vector<Demand>& demand, double depth, const Params& p);

// pack() with the lanes merged wherever the print allows it: every layer starts merged and
// only the busy spots (plus the run-up they need) become lanes layers. Merged and lanes zones
// can alternate along Z. Fills ev[i].active and Plan::active.
Plan pack_scheduled(std::vector<Demand>& demand, double depth, const Params& p);

// Smallest feasible depth (binary search, same bounds as the prototype: 1..400 mm, 35 iters),
// with the lane schedule of pack_scheduled.
Plan pack_min_depth(const std::vector<Demand>& demand, const Params& p);

// taper: with the depth and the schedule settled (demand[i].active filled in), makes the
// merged cell shallower in steps wherever the rest of the print still fits (Params::taper_min =
// the shallowest cell allowed). Fills demand[i].depth and Plan::cell_depth. !ok = no taper: the
// demand is left with depth 0 everywhere.
Plan pack_taper(std::vector<Demand>& demand, double depth, const Params& p, double margin = 1.15);

}} // namespace Slic3r::SkyLane

#endif
