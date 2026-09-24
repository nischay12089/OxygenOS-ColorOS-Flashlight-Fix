#include "torch_leds.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "log.h"

namespace flashlightd {
namespace {

constexpr char kLedClassDir[] = "/sys/class/leds/";

bool readFile(const std::string& path, std::string* out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return false;
    out->assign(buf, static_cast<size_t>(n));
    // sysfs values end in '\n', device-tree strings in '\0'.
    while (!out->empty() && (out->back() == '\n' || out->back() == '\0')) out->pop_back();
    return true;
}

bool writeFile(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    ssize_t n = write(fd, value.data(), value.size());
    int err = errno;
    close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        LOGE("write %s=%s: %s", path.c_str(), value.c_str(), strerror(err));
        return false;
    }
    return true;
}

// Device-tree u32 cells are big-endian.
std::vector<uint32_t> readBe32Array(const std::string& path) {
    std::vector<uint32_t> cells;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return cells;
    uint8_t b[64];
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    for (ssize_t i = 0; i + 3 < n; i += 4) {
        cells.push_back((uint32_t{b[i]} << 24) | (uint32_t{b[i + 1]} << 16) |
                        (uint32_t{b[i + 2]} << 8) | b[i + 3]);
    }
    return cells;
}

std::optional<uint32_t> readBe32(const std::string& path) {
    auto cells = readBe32Array(path);
    if (cells.empty()) return std::nullopt;
    return cells[0];
}

// Xiaomi derate table on a torch node: currents-with-time <I0 I1 ..> pairs with
// time-for-currents <T0 T1 ..> ms, where T == 0xffffffff means "no time limit".
// Returns the largest such unlimited current, or 0 when the node has no table.
int sustainedFromDerateTable(const std::string& dtNode) {
    const auto currents = readBe32Array(dtNode + "/xiaomi,currents-with-time");
    const auto times = readBe32Array(dtNode + "/xiaomi,time-for-currents");
    int sustained = 0;
    for (size_t i = 0; i < currents.size() && i < times.size(); ++i) {
        if (times[i] == 0xffffffffu) sustained = std::max(sustained, static_cast<int>(currents[i]));
    }
    return sustained;
}

std::string flashNodeDir(const std::string& ledName) {
    char resolved[PATH_MAX];
    std::string link = std::string(kLedClassDir) + ledName + "/device/of_node";
    return realpath(link.c_str(), resolved) ? std::string(resolved) : std::string();
}

std::vector<std::string> subdirs(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.') out.push_back(dir + "/" + e->d_name);
    }
    closedir(d);
    return out;
}

// qti-flash layout: the flash_led DT node has children for every flash/torch/switch.
// A switch's qcom,led-mask selects channels by their qcom,id; we return the torch
// children whose id is in that mask (for camera-flash0 here: torch_0 + torch_3).
std::vector<std::string> torchNamesFromSwitchMask(const std::string& switchName) {
    std::vector<std::string> names;
    const std::string flashNode = flashNodeDir(switchName);
    if (flashNode.empty()) {
        LOGW("cannot resolve device-tree node of %s", switchName.c_str());
        return names;
    }
    const auto children = subdirs(flashNode);

    std::optional<uint32_t> mask;
    for (const auto& child : children) {
        std::string ledName;
        if (readFile(child + "/qcom,led-name", &ledName) && ledName == switchName) {
            mask = readBe32(child + "/qcom,led-mask");
            break;
        }
    }
    if (!mask) {
        LOGW("no qcom,led-mask for %s", switchName.c_str());
        return names;
    }
    for (const auto& child : children) {
        std::string label, ledName;
        auto id = readBe32(child + "/qcom,id");
        if (!id || *id >= 32 || !readFile(child + "/label", &label) || label != "torch") continue;
        if ((*mask & (1u << *id)) && readFile(child + "/qcom,led-name", &ledName)) {
            names.push_back(ledName);
        }
    }
    std::sort(names.begin(), names.end());
    LOGI("%s led-mask=0x%x -> %zu torch channel(s)", switchName.c_str(), *mask, names.size());
    return names;
}

}  // namespace

bool TorchLeds::discover(const std::string& switchName,
                         const std::vector<std::string>& explicitNodes) {
    channels_.clear();
    std::vector<std::string> names =
            explicitNodes.empty() ? torchNamesFromSwitchMask(switchName) : explicitNodes;
    for (const auto& name : names) {
        LedChannel ch;
        ch.name = name;
        ch.dir = std::string(kLedClassDir) + name;
        std::string max;
        if (!readFile(ch.dir + "/max_brightness", &max) || (ch.maxMa = atoi(max.c_str())) <= 0) {
            LOGW("skipping %s: unreadable max_brightness", name.c_str());
            continue;
        }
        ch.sustainedMa = ch.maxMa;
        for (const auto& child : subdirs(flashNodeDir(name))) {
            std::string ledName;
            if (readFile(child + "/qcom,led-name", &ledName) && ledName == name) {
                if (int s = sustainedFromDerateTable(child); s > 0) {
                    ch.sustainedMa = std::min(s, ch.maxMa);
                }
                break;
            }
        }
        channels_.push_back(ch);
        LOGI("torch channel %s max=%dmA sustained=%dmA", ch.name.c_str(), ch.maxMa,
             ch.sustainedMa);
    }
    return !channels_.empty();
}

int TorchLeds::maxMilliamps() const {
    int m = 0;
    for (const auto& ch : channels_) m = (m == 0) ? ch.maxMa : std::min(m, ch.maxMa);
    return m;
}

int TorchLeds::sustainedMilliamps() const {
    int m = 0;
    for (const auto& ch : channels_) m = (m == 0) ? ch.sustainedMa : std::min(m, ch.sustainedMa);
    return m;
}

bool TorchLeds::setMilliamps(int ma) const {
    bool ok = true;
    for (const auto& ch : channels_) {
        const int v = std::clamp(ma, 1, ch.maxMa);
        ok &= writeFile(ch.dir + "/brightness", std::to_string(v));
    }
    return ok;
}

int TorchLeds::readMilliamps(const LedChannel& ch) const {
    std::string v;
    return readFile(ch.dir + "/brightness", &v) ? atoi(v.c_str()) : -1;
}

bool TorchLeds::matches(int ma) const {
    for (const auto& ch : channels_) {
        if (readMilliamps(ch) != std::clamp(ma, 1, ch.maxMa)) return false;
    }
    return true;
}

}  // namespace flashlightd
