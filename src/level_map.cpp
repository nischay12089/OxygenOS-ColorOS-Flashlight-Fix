#include "level_map.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace flashlightd {

LevelMap::LevelMap(int uiSteps, int sustainedMa, int maxMa, std::vector<int> explicitMa)
    : uiSteps_(uiSteps),
      sustainedMa_(sustainedMa),
      maxMa_(maxMa),
      explicitMa_(std::move(explicitMa)) {}

int LevelMap::milliampsFor(int halLevel) const {
    if (halLevel < 1 || halLevel > uiSteps_) return 0;
    const size_t idx = static_cast<size_t>(halLevel - 1);
    const int ma = idx < explicitMa_.size() ? explicitMa_[idx]
                                            : stepToMilliamps(halLevel, uiSteps_, sustainedMa_);
    return std::clamp(ma, 1, maxMa_);
}

int stepToMilliamps(int step, int uiSteps, int sustainedMa) {
    // Perceived brightness is roughly logarithmic, so equal-looking steps need equal current
    // ratios: the top step runs at the sustained limit and each step below halves it
    // (19/38/75/150 mA per channel here). Nothing exceeds sustainedMa, so no thermal derate.
    return static_cast<int>(std::lround(std::ldexp(sustainedMa, step - uiSteps)));
}

}  // namespace flashlightd
