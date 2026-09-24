#pragma once

#include <android/binder_ibinder.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace flashlightd {

// android.hardware.ICameraServiceListener torch status values.
enum TorchStatus : int32_t {
    kTorchUnknown = -1,
    kTorchNotAvailable = 0,
    kTorchAvailableOff = 1,
    kTorchAvailableOn = 2,
};

// Receives the same torch events SystemUI's CameraManager.TorchCallback is built on.
struct TorchCallbacks {
    std::function<void(const std::string& cameraId, int32_t status)> onTorchStatus;
    std::function<void(const std::string& cameraId, int32_t level)> onTorchStrength;
};

// Hand-rolled client for cameraserver's "media.camera" (android.hardware.ICameraService).
// The interface is not NDK-stable, so transaction codes and parcel layouts are pinned to
// what this build's framework.jar stubs use (see re/framework).
class CameraService {
public:
    static std::shared_ptr<CameraService> connect(bool waitForService);
    ~CameraService();

    CameraService(const CameraService&) = delete;
    CameraService& operator=(const CameraService&) = delete;

    binder_status_t addListener(AIBinder* listener);
    binder_status_t removeListener(AIBinder* listener);
    binder_status_t setTorchMode(const std::string& cameraId, bool enabled, AIBinder* client);
    binder_status_t turnOnTorchWithStrengthLevel(const std::string& cameraId, int32_t level,
                                                 AIBinder* client);
    binder_status_t getTorchStrengthLevel(const std::string& cameraId, int32_t* level);

    binder_status_t linkToDeath(AIBinder_DeathRecipient* recipient, void* cookie);

private:
    explicit CameraService(AIBinder* binder) : binder_(binder) {}
    AIBinder* binder_;  // owned strong ref
};

// New local ICameraServiceListener; `callbacks` must outlive the binder. Returns a strong ref.
AIBinder* newTorchListener(TorchCallbacks* callbacks);

// New local binder used as the torch "client token": cameraserver turns the torch off
// when the process holding it dies. Returns a strong ref.
AIBinder* newClientToken();

}  // namespace flashlightd
