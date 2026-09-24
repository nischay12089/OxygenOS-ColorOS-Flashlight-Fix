#pragma once

#include <string>
#include <vector>

namespace flashlightd {

// One qti-flash torch channel exposed through the LED class, e.g. /sys/class/leds/led:torch_0.
// For this driver `brightness` is the channel current in mA and `max_brightness` its ceiling.
struct LedChannel {
    std::string name;
    std::string dir;
    int maxMa = 0;
    // Highest current the kernel lets run indefinitely. Xiaomi's qti-flash derates anything
    // above it after a timeout (DT xiaomi,currents-with-time / xiaomi,time-for-currents).
    int sustainedMa = 0;
};

// Direct control of the PMIC torch channels that the camera HAL drives through led triggers.
class TorchLeds {
public:
    // Resolves the torch channels behind `switchName` (its qcom,led-mask in the device tree).
    // `explicitNodes` bypasses discovery when non-empty.
    bool discover(const std::string& switchName, const std::vector<std::string>& explicitNodes);

    const std::vector<LedChannel>& channels() const { return channels_; }

    // Smallest per-channel ceiling: the highest current every channel can be given.
    int maxMilliamps() const;

    // Smallest per-channel sustained limit (== maxMilliamps() without a kernel derate table).
    int sustainedMilliamps() const;

    // Programs every channel with `ma` (clamped to [1, channel max]).
    // Never writes 0: that detaches the camera driver's led trigger from the node.
    bool setMilliamps(int ma) const;

    // Current programmed value of one channel, -1 on read failure.
    int readMilliamps(const LedChannel& ch) const;

    // True when every channel reads back `ma` (after the same clamping setMilliamps applies).
    bool matches(int ma) const;

private:
    std::vector<LedChannel> channels_;
};

}  // namespace flashlightd
