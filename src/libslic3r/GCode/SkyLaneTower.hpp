// SkyLane prime tower, adapted from SKYLANE_PACK_RADU_2 (AGPL).
//
// It sits on top of the stock WipeTower2 and never changes how WipeTower2 plans or writes a tool
// change. WipeTower2 keeps doing everything it does (plan, generate, the tool change G-code);
// SkyLane sizes the tower from that plan and, at emission, moves each purge into one of a few
// lanes split along the tower depth. Each purge goes into the lowest lane as a tall pass, the
// nozzle dipping into the tower, so the tower is much smaller on prints with many tool changes.
//
// Plugs (all behind PrintConfig::enable_skylane):
//   Print.cpp   after WipeTower2::generate(): plan(), and depth() for the tower footprint
//   GCode.cpp   WipeTowerIntegration::tool_change(): place() on every tool change it emits;
//               append_skylane_tcr(): safe travel, dry arrival and profile macros
#pragma once

#include "libslic3r/GCode/WipeTower.hpp"

#include <memory>
#include <string>
#include <vector>
#include <map>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r {

class GCodeWriter;
class PrintConfig;

class SkyLaneTower
{
public:
    // SkyLane is the tower of this print (prime tower on + "SkyLane").
    static bool enabled(const PrintConfig& config);

    // Sizes the tower from the tool changes WipeTower2 planned (WipeTower2::generate()). Touches
    // no TCR. false = no plan could be made; the caller must reject the slice.
    bool plan(const PrintConfig&                                           config,
              const std::vector<std::vector<WipeTower::ToolChangeResult>>& tool_changes,
              const std::vector<std::vector<WipeTower::ToolChangeResult>>& local_z_tool_changes,
              float                                                        brim_width);
    bool ready() const { return m_rt != nullptr; }
    // Tower depth SkyLane builds (the footprint on the bed). 0 before plan().
    float depth() const { return m_depth; }

    // Start of a G-code export: lanes back to the bed (an export may run more than once).
    void begin_emission() const;
    // For every tool change emitted on the tower: the TCR to emit instead, with skylane_z set
    // (the Z the nozzle goes down to), skylane_park and skylane_ram.
    WipeTower::ToolChangeResult place(const WipeTower::ToolChangeResult& tcr) const;
    // End of the export: plan against emission, in the log.
    void report() const;

    // Whether the outgoing tool can arrive retracted before a tool change.
    static bool dry_arrival(
        const PrintConfig& config, GCodeWriter& writer, const WipeTower::ToolChangeResult& tcr, int new_extruder_id, bool needs_toolchange);

    // Replay the planned sequence for Prepare view and collision bounds; export resets it.
    void                              preview(std::vector<std::vector<WipeTower::ToolChangeResult>>& nominal,
                                              std::vector<std::vector<WipeTower::ToolChangeResult>>& local);
    const std::map<float, Polylines>& outlines() const { return m_outlines; }
    const BoundingBoxf&               bounds() const { return m_bounds; }
    const std::vector<float>&         used_filament() const { return m_used_filament; }
    struct Runtime;

private:
    // Emission state lives here: place() is called from const code (WipeTowerIntegration).
    std::shared_ptr<Runtime>   m_rt;
    float                      m_depth = 0.f;
    std::map<float, Polylines> m_outlines;
    BoundingBoxf               m_bounds;
    std::vector<float>         m_used_filament;
};

} // namespace Slic3r
