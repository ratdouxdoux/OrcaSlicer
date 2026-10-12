#pragma once

#include "MixedFilament.hpp"

namespace Slic3r {

// All application blend entry points share the manager's calibration check:
// KM/K-S requires every contributing material to be recognized. Otherwise the
// whole recipe uses FilamentMixer, including static previews and badges.
inline std::string blend_mixed_color_inputs_auto(const std::vector<MixedFilamentColorInput>& inputs, MixedFilamentColorEngine engine)
{
    return MixedFilamentManager::blend_color_multi(inputs, engine);
}

} // namespace Slic3r
