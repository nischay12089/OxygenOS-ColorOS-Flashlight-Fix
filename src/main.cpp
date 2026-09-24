// flashlightd: makes the OxygenOS flashlight-intensity slider drive this device's torch LEDs.
//
// SystemUI (FlashlightControllerImpl) calls turnOnTorchWithStrengthLevel(id, step) with the
// slider step 1..4. The Xiaomi camera HAL on this port reads that as a 1..100 level and programs
// 0.6 mA/level with a 6 mA floor, so all four steps come out as the same 6 mA per channel
// (measured on led:torch_0/led:torch_3). This daemon listens to cameraserver's torch callbacks
// (the same ICameraServiceListener stream SystemUI's TorchCallback uses) and, whenever a lit
// torch sits at a SystemUI step, programs the PMIC torch channels with a real current.

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "binder_platform.h"
#include "camera_binder.h"
#include "level_map.h"
#include "log.h"
#include "service.h"
#include "torch_leds.h"

namespace flashlightd {

bool gLogToStderr = false;

namespace {

using namespace std::chrono_literals;

// Device settings, compiled in (no config file to deploy).
struct Config {
    // LED switch the rear camera torch strobes; its device-tree led-mask selects the channels.
    std::string switchName = "led:switch_0";
    // Explicit torch channel nodes; empty = discover from the switch's led-mask.
    std::vector<std::string> nodes;
    // Steps in the OxygenOS flashlight slider (SystemUI sends 1..uiSteps as the HAL level).
    int uiSteps = 4;
    // Per-step current in mA per channel; empty = stepToMilliamps() curve.
    std::vector<int> stepMa;
    // Camera id for the CLI torch commands (SystemUI uses the first back camera with a flash).
    std::string torchCamera = "0";
};

const Config kConfig;

// Tracks torch state per camera id and re-programs the LED channels on every change.
class TorchFixer {
public:
    TorchFixer(const TorchLeds& leds, const LevelMap& map) : leds_(leds), map_(map) {}

    void start() { std::thread([this] { run(); }).detach(); }

    void setService(std::shared_ptr<CameraService> svc) {
        std::lock_guard lk(mu_);
        service_ = std::move(svc);
    }

    void onTorchStatus(const std::string& id, int32_t status) {
        std::lock_guard lk(mu_);
        auto& cam = cams_[id];
        const bool on = status == kTorchAvailableOn;
        if (on && !cam.on) cam.level = 0;  // freshly lit: level unknown until reported/queried
        if (!on) cam.level = 0;
        cam.on = on;
        LOGD("torch status cam=%s status=%d", id.c_str(), status);
        bump();
    }

    void onTorchStrength(const std::string& id, int32_t level) {
        std::lock_guard lk(mu_);
        cams_[id].level = level;
        LOGD("torch strength cam=%s level=%d", id.c_str(), level);
        bump();
    }

    // cameraserver restarted: every torch is off and levels are gone.
    void reset() {
        std::lock_guard lk(mu_);
        cams_.clear();
        bump();
    }

private:
    struct CamState {
        bool on = false;
        int level = 0;  // 0 = unknown
    };

    void bump() {
        ++gen_;
        cv_.notify_all();
    }

    // Highest mapped current over lit cameras; 0 = no SystemUI step lit, leave the HAL alone.
    int targetLocked() const {
        int target = 0;
        for (const auto& [id, cam] : cams_) {
            if (cam.on) target = std::max(target, map_.milliampsFor(cam.level));
        }
        return target;
    }

    void run() {
        std::unique_lock lk(mu_);
        uint64_t seen = 0;
        for (;;) {
            cv_.wait(lk, [&] { return gen_ != seen; });
            seen = gen_;

            // Torch lit via setTorchMode() (no strength callback): ask cameraserver.
            for (auto& [id, cam] : cams_) {
                if (!cam.on || cam.level != 0 || !service_) continue;
                auto svc = service_;
                const std::string camId = id;
                lk.unlock();
                int32_t level = 0;
                const bool ok = svc->getTorchStrengthLevel(camId, &level) == STATUS_OK;
                lk.lock();
                auto it = cams_.find(camId);
                if (ok && it != cams_.end() && it->second.on && it->second.level == 0) {
                    it->second.level = level;
                }
                break;  // map may have changed while unlocked; one query per pass is enough
            }

            const int target = targetLocked();
            if (target <= 0) continue;

            // cameraserver sends status + strength back to back; write only when needed.
            lk.unlock();
            const bool already = leds_.matches(target);
            if (!already) leds_.setMilliamps(target);
            lk.lock();
            if (!already) LOGI("torch step -> %d mA", target);

            // The HAL can land its own (tiny) current a few ms after cameraserver notifies us.
            // Re-assert briefly; stop well before the kernel's 5 s thermal derate kicks in.
            for (auto delay : {30ms, 80ms, 200ms, 500ms}) {
                if (cv_.wait_for(lk, delay, [&] { return gen_ != seen; })) break;
                lk.unlock();
                const bool held = leds_.matches(target);
                if (!held) leds_.setMilliamps(target);
                lk.lock();
                if (!held) LOGI("re-applied %d mA after HAL overwrite", target);
            }
        }
    }

    const TorchLeds& leds_;
    const LevelMap& map_;
    std::mutex mu_;
    std::condition_variable cv_;
    uint64_t gen_ = 0;
    std::map<std::string, CamState> cams_;
    std::shared_ptr<CameraService> service_;
};

struct DeathFlag {
    std::mutex mu;
    std::condition_variable cv;
    bool died = false;
};

void onServiceDied(void* cookie) {
    auto* flag = static_cast<DeathFlag*>(cookie);
    std::lock_guard lk(flag->mu);
    flag->died = true;
    flag->cv.notify_all();
}

bool setupLeds(const Config& cfg, TorchLeds* leds) {
    if (leds->discover(cfg.switchName, cfg.nodes)) return true;
    LOGE("no torch LED channels found (switch=%s)", cfg.switchName.c_str());
    return false;
}

int runDaemon(const Config& cfg) {
    TorchLeds leds;
    if (!setupLeds(cfg, &leds)) return 1;
    LevelMap map(cfg.uiSteps, leds.sustainedMilliamps(), leds.maxMilliamps(), cfg.stepMa);
    for (int s = 1; s <= map.uiSteps(); ++s) LOGI("step %d -> %d mA", s, map.milliampsFor(s));

    platform::setThreadPoolMaxThreadCount(2);
    platform::startThreadPool();

    TorchFixer fixer(leds, map);
    fixer.start();

    TorchCallbacks callbacks{
            [&](const std::string& id, int32_t st) { fixer.onTorchStatus(id, st); },
            [&](const std::string& id, int32_t lvl) { fixer.onTorchStrength(id, lvl); },
    };
    AIBinder* listener = newTorchListener(&callbacks);
    DeathFlag death;
    AIBinder_DeathRecipient* recipient = AIBinder_DeathRecipient_new(onServiceDied);
    // `death` outlives every link, so there is nothing to release when one is dropped.
    AIBinder_DeathRecipient_setOnUnlinked(recipient, [](void*) {});

    for (;;) {
        auto svc = CameraService::connect(/*waitForService=*/true);
        if (!svc) {
            std::this_thread::sleep_for(2s);
            continue;
        }
        {
            std::lock_guard lk(death.mu);
            death.died = false;
        }
        if (svc->linkToDeath(recipient, &death) != STATUS_OK ||
            svc->addListener(listener) != STATUS_OK) {
            LOGE("could not register with cameraserver, retrying");
            std::this_thread::sleep_for(2s);
            continue;
        }
        fixer.setService(svc);
        LOGI("listening for torch events");

        std::unique_lock lk(death.mu);
        death.cv.wait(lk, [&] { return death.died; });
        lk.unlock();

        LOGW("cameraserver died, re-registering");
        fixer.setService(nullptr);
        fixer.reset();
    }
}

int cmdStatus(const Config& cfg) {
    service::printState();
    TorchLeds leds;
    if (!setupLeds(cfg, &leds)) return 1;
    for (const auto& ch : leds.channels()) {
        std::printf("%-14s %4d mA  (max %d, sustained %d)\n", ch.name.c_str(),
                    leds.readMilliamps(ch), ch.maxMa, ch.sustainedMa);
    }
    LevelMap map(cfg.uiSteps, leds.sustainedMilliamps(), leds.maxMilliamps(), cfg.stepMa);
    for (int s = 1; s <= map.uiSteps(); ++s) {
        std::printf("step %d -> %d mA\n", s, map.milliampsFor(s));
    }
    if (auto svc = CameraService::connect(false)) {
        int32_t level = -1;
        if (svc->getTorchStrengthLevel(cfg.torchCamera, &level) == STATUS_OK) {
            std::printf("camera %s torch strength level: %d\n", cfg.torchCamera.c_str(), level);
        }
    }
    return 0;
}

int cmdSet(const Config& cfg, int ma) {
    TorchLeds leds;
    if (!setupLeds(cfg, &leds)) return 1;
    return leds.setMilliamps(ma) ? 0 : 1;
}

// Drives the torch through cameraserver exactly like SystemUI does, for testing.
int cmdTorch(const Config& cfg, int argc, char** argv) {
    auto svc = CameraService::connect(false);
    if (!svc) return 1;
    AIBinder* token = newClientToken();
    const std::string what = argc > 0 ? argv[0] : "";
    int rc = 1;
    if (what == "off") {
        rc = svc->setTorchMode(cfg.torchCamera, false, token) == STATUS_OK ? 0 : 1;
    } else if (what == "level") {
        int32_t level = -1;
        rc = svc->getTorchStrengthLevel(cfg.torchCamera, &level) == STATUS_OK ? 0 : 1;
        if (rc == 0) std::printf("%d\n", level);
    } else if (what == "on") {
        // Keep the process (and its client token) alive: cameraserver turns the torch off
        // when the token dies.
        const int level = argc > 1 ? atoi(argv[1]) : 0;
        const int holdMs = argc > 2 ? atoi(argv[2]) : 3000;
        binder_status_t st = level > 0
                ? svc->turnOnTorchWithStrengthLevel(cfg.torchCamera, level, token)
                : svc->setTorchMode(cfg.torchCamera, true, token);
        if (st == STATUS_OK) {
            std::printf("torch on (level %d) for %d ms\n", level, holdMs);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
            svc->setTorchMode(cfg.torchCamera, false, token);
            rc = 0;
        }
    } else {
        std::fprintf(stderr, "torch on [level] [hold_ms] | off | level\n");
    }
    AIBinder_decStrong(token);
    return rc;
}

void usage() {
    std::fprintf(stderr,
                 "usage: flashlightd <command>\n"
                 "  start | stop | restart      manage the background service (auto-restarts)\n"
                 "  daemon                      run the torch-intensity fixer in the foreground\n"
                 "  status                      service state, torch channels, step table\n"
                 "  set <mA>                    write a current to the torch channels\n"
                 "  torch on [level] [hold_ms]  light the torch via cameraserver (test)\n"
                 "  torch off | torch level\n");
}

}  // namespace
}  // namespace flashlightd

int main(int argc, char** argv) {
    using namespace flashlightd;
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    int i = 2;
    gLogToStderr = cmd != "daemon" || isatty(STDERR_FILENO);
    const Config& cfg = kConfig;

    if (cmd == "daemon") return runDaemon(cfg);
    // Service commands fork, so they must run before this process touches binder.
    if (cmd == "start") return service::start();
    if (cmd == "stop") return service::stop();
    if (cmd == "restart") return service::restart();
    // One binder thread for the CLI: the torch client token must stay reachable.
    platform::setThreadPoolMaxThreadCount(1);
    platform::startThreadPool();
    if (cmd == "status") return cmdStatus(cfg);
    if (cmd == "set" && i < argc) return cmdSet(cfg, atoi(argv[i]));
    if (cmd == "torch") return cmdTorch(cfg, argc - i, argv + i);
    usage();
    return 2;
}
