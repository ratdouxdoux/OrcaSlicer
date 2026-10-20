#ifndef slic3r_InfillDiagnostics_hpp_
#define slic3r_InfillDiagnostics_hpp_

#include "PreparationDiagnostics.hpp"

namespace Slic3r {

// Developer-only switches for same-input validation and fresh-process timings.
// Read once per process; never change the environment during a slice.
inline bool infill_experiment(const char* name)
{
    static const std::string modes = [] {
        const char* value = std::getenv("ORCA_INFILL_EXPERIMENTS");
        return std::string(",") + (value ? value : "gap") + ",";
    }();
    return modes == ",all," || modes.find(std::string(",") + name + ",") != std::string::npos;
}

inline bool infill_validate()
{
    static const bool enabled = preparation_diagnostic("ORCA_INFILL_VALIDATE");
    return enabled;
}

inline PreparationProfile& infill_profile()
{
    static PreparationProfile profile("infill");
    return profile;
}

} // namespace Slic3r

#endif
