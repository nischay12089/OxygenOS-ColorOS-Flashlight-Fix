#pragma once

#include <vector>

namespace flashlightd {

// Translates what cameraserver reports as the torch strength level into a channel current.
//
// OxygenOS SystemUI sends its slider step (1..uiSteps) straight to
// CameraManager.turnOnTorchWithStrengthLevel(); the OPlus camera HAL it was written for
// interprets that as a step, but this device's Xiaomi HAL treats it as a 1..100 level
// (0.6 mA/level, 6 mA floor), so every step lands on the same 6 mA.
class LevelMap {
public:
    // `sustainedMa`: current the kernel allows indefinitely (the curve's ceiling).
    // `maxMa`: hard channel limit; explicit config values may go up to it (burst, derated).
    // `explicitMa`: optional per-step currents from the config (index 0 = step 1).
    LevelMap(int uiSteps, int sustainedMa, int maxMa, std::vector<int> explicitMa);

    int uiSteps() const { return uiSteps_; }

    // mA for a HAL level, or 0 when the level is not a SystemUI step (leave the HAL's value).
    int milliampsFor(int halLevel) const;

private:
    int uiSteps_;
    int sustainedMa_;
    int maxMa_;
    std::vector<int> explicitMa_;
};

// Default brightness curve: slider `step` in [1, uiSteps] -> per-channel current in mA,
// with `sustainedMa` the brightest current that will not be thermally derated.
int stepToMilliamps(int step, int uiSteps, int sustainedMa);

}  // namespace flashlightd
