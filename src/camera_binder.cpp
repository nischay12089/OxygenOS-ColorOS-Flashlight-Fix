#include "camera_binder.h"

#include <android/binder_parcel.h>
#include <android/binder_status.h>

#include "binder_platform.h"
#include "log.h"

namespace flashlightd {
namespace {

constexpr char kServiceName[] = "media.camera";
constexpr char kServiceDescriptor[] = "android.hardware.ICameraService";
constexpr char kListenerDescriptor[] = "android.hardware.ICameraServiceListener";
constexpr char kTokenDescriptor[] = "flashlightd.TorchClient";

// ICameraService$Stub.TRANSACTION_* on this build.
constexpr transaction_code_t kTxAddListener = 5;
constexpr transaction_code_t kTxRemoveListener = 9;
constexpr transaction_code_t kTxSetTorchMode = 16;
constexpr transaction_code_t kTxTurnOnTorchWithStrengthLevel = 17;
constexpr transaction_code_t kTxGetTorchStrengthLevel = 18;

// ICameraServiceListener$Stub.TRANSACTION_* (all oneway).
constexpr transaction_code_t kTxOnTorchStatusChanged = 3;        // (int status, String id, int deviceId)
constexpr transaction_code_t kTxOnTorchStrengthLevelChanged = 4; // (String id, int level, int deviceId)
constexpr transaction_code_t kTxListenerLast = 8;

constexpr int32_t kUseCallingId = -1;       // AttributionSourceState pid/uid: resolve from binder
constexpr int32_t kDeviceIdDefault = 0;     // Context.DEVICE_ID_DEFAULT
constexpr int32_t kDevicePolicyDefault = 0; // VirtualDeviceParams.DEVICE_POLICY_DEFAULT

// Binder header codes Status::writeToParcel may put in front of a reply.
constexpr int32_t kExHasNotedAppOpsReplyHeader = -127;
constexpr int32_t kExHasReplyHeader = -128;

// --- AIBinder_Class plumbing ------------------------------------------------------------

void* passThroughCreate(void* args) { return args; }
void noopDestroy(void*) {}
binder_status_t rejectTransact(AIBinder*, transaction_code_t, const AParcel*, AParcel*) {
    return STATUS_UNKNOWN_TRANSACTION;
}

bool allocString(void* data, int32_t length, char** buffer) {
    auto* s = static_cast<std::string*>(data);
    if (length < 0) {  // null String
        s->clear();
        *buffer = nullptr;
        return true;
    }
    s->resize(static_cast<size_t>(length));  // length includes the NUL terminator
    *buffer = s->data();
    return true;
}

binder_status_t readString(const AParcel* p, std::string* out) {
    binder_status_t st = AParcel_readString(p, out, allocString);
    if (st == STATUS_OK && !out->empty() && out->back() == '\0') out->pop_back();
    return st;
}

binder_status_t onListenerTransact(AIBinder* binder, transaction_code_t code, const AParcel* in,
                                   AParcel* /*out*/) {
    auto* cb = static_cast<TorchCallbacks*>(AIBinder_getUserData(binder));
    std::string id;
    int32_t a = 0, deviceId = 0;
    switch (code) {
        case kTxOnTorchStatusChanged:
            if (AParcel_readInt32(in, &a) != STATUS_OK || readString(in, &id) != STATUS_OK ||
                AParcel_readInt32(in, &deviceId) != STATUS_OK) {
                return STATUS_BAD_VALUE;
            }
            if (cb && cb->onTorchStatus) cb->onTorchStatus(id, a);
            return STATUS_OK;
        case kTxOnTorchStrengthLevelChanged:
            if (readString(in, &id) != STATUS_OK || AParcel_readInt32(in, &a) != STATUS_OK ||
                AParcel_readInt32(in, &deviceId) != STATUS_OK) {
                return STATUS_BAD_VALUE;
            }
            if (cb && cb->onTorchStrength) cb->onTorchStrength(id, a);
            return STATUS_OK;
        default:
            // Camera open/close/availability callbacks: accepted and ignored.
            return (code >= FIRST_CALL_TRANSACTION && code <= kTxListenerLast)
                           ? STATUS_OK
                           : STATUS_UNKNOWN_TRANSACTION;
    }
}

const AIBinder_Class* serviceClass() {
    static const AIBinder_Class* c = AIBinder_Class_define(kServiceDescriptor, passThroughCreate,
                                                           noopDestroy, rejectTransact);
    return c;
}
const AIBinder_Class* listenerClass() {
    static const AIBinder_Class* c = AIBinder_Class_define(kListenerDescriptor, passThroughCreate,
                                                           noopDestroy, onListenerTransact);
    return c;
}
const AIBinder_Class* tokenClass() {
    static const AIBinder_Class* c = AIBinder_Class_define(kTokenDescriptor, passThroughCreate,
                                                           noopDestroy, rejectTransact);
    return c;
}

const AIBinder_Class* tokenClass();

// Process-wide AttributionSourceState.token (the C++ stub rejects a null one).
AIBinder* attributionToken() {
    static AIBinder* token = AIBinder_new(tokenClass(), nullptr);
    return token;
}

// `in AttributionSourceState` as the AIDL Java/C++ backends marshal it:
// non-null marker, then a size-prefixed structured parcelable. Mirrors what
// CameraManager sends: calling pid/uid, no package, empty `next` chain.
binder_status_t writeAttributionSource(AParcel* p) {
    binder_status_t st = AParcel_writeInt32(p, 1);
    if (st != STATUS_OK) return st;
    const int32_t start = AParcel_getDataPosition(p);
    if ((st = AParcel_writeInt32(p, 0)) != STATUS_OK) return st;             // size placeholder
    if ((st = AParcel_writeInt32(p, kUseCallingId)) != STATUS_OK) return st; // pid
    if ((st = AParcel_writeInt32(p, kUseCallingId)) != STATUS_OK) return st; // uid
    if ((st = AParcel_writeInt32(p, kDeviceIdDefault)) != STATUS_OK) return st;
    if ((st = AParcel_writeString(p, nullptr, -1)) != STATUS_OK) return st;  // packageName
    if ((st = AParcel_writeString(p, nullptr, -1)) != STATUS_OK) return st;  // attributionTag
    if ((st = AParcel_writeStrongBinder(p, attributionToken())) != STATUS_OK) return st;
    if ((st = AParcel_writeInt32(p, -1)) != STATUS_OK) return st;  // renouncedPermissions: null
    if ((st = AParcel_writeInt32(p, 0)) != STATUS_OK) return st;   // next: empty, not null
    const int32_t end = AParcel_getDataPosition(p);
    if ((st = AParcel_setDataPosition(p, start)) != STATUS_OK) return st;
    if ((st = AParcel_writeInt32(p, end - start)) != STATUS_OK) return st;
    return AParcel_setDataPosition(p, end);
}

// Reads the AIDL Status header of a reply. Returns STATUS_OK only for EX_NONE.
binder_status_t readReplyStatus(const AParcel* reply, const char* what) {
    int32_t ex = 0;
    binder_status_t st = AParcel_readInt32(reply, &ex);
    if (st != STATUS_OK) return st;
    if (ex == kExHasReplyHeader || ex == kExHasNotedAppOpsReplyHeader) {
        int32_t headerSize = 0;
        const int32_t pos = AParcel_getDataPosition(reply);
        if ((st = AParcel_readInt32(reply, &headerSize)) != STATUS_OK) return st;
        AParcel_setDataPosition(reply, pos + headerSize);
        ex = 0;
    }
    if (ex != 0) {
        std::string msg;
        readString(reply, &msg);
        int32_t serviceSpecific = 0;
        AParcel_readInt32(reply, &serviceSpecific);
        LOGE("%s: remote exception %d (%d) \"%s\"", what, ex, serviceSpecific, msg.c_str());
        return STATUS_FAILED_TRANSACTION;
    }
    return STATUS_OK;
}

template <typename WriteFn, typename ReadFn>
binder_status_t call(AIBinder* binder, transaction_code_t code, const char* what, WriteFn&& write,
                     ReadFn&& read) {
    AParcel* in = nullptr;
    binder_status_t st = AIBinder_prepareTransaction(binder, &in);  // writes the interface token
    if (st != STATUS_OK) {
        LOGE("%s: prepareTransaction %d", what, st);
        return st;
    }
    if ((st = write(in)) != STATUS_OK) {
        AParcel_delete(in);
        LOGE("%s: marshal %d", what, st);
        return st;
    }
    AParcel* out = nullptr;
    st = AIBinder_transact(binder, code, &in, &out, 0);  // consumes `in`
    if (st != STATUS_OK) {
        LOGE("%s: transact %d", what, st);
        return st;
    }
    st = readReplyStatus(out, what);
    if (st == STATUS_OK) st = read(out);
    AParcel_delete(out);
    return st;
}

constexpr auto kNoReply = [](const AParcel*) { return STATUS_OK; };

}  // namespace

std::shared_ptr<CameraService> CameraService::connect(bool waitForService) {
    AIBinder* b = platform::getService(kServiceName, waitForService);
    if (!b) {
        LOGE("service %s not found", kServiceName);
        return nullptr;
    }
    if (!AIBinder_associateClass(b, serviceClass())) {
        LOGE("%s is not %s", kServiceName, kServiceDescriptor);
        AIBinder_decStrong(b);
        return nullptr;
    }
    return std::shared_ptr<CameraService>(new CameraService(b));
}

CameraService::~CameraService() { AIBinder_decStrong(binder_); }

binder_status_t CameraService::addListener(AIBinder* listener) {
    // Reply carries CameraStatus[]; we only need the torch callbacks that follow.
    return call(
            binder_, kTxAddListener, "addListener",
            [&](AParcel* p) { return AParcel_writeStrongBinder(p, listener); }, kNoReply);
}

binder_status_t CameraService::removeListener(AIBinder* listener) {
    return call(
            binder_, kTxRemoveListener, "removeListener",
            [&](AParcel* p) { return AParcel_writeStrongBinder(p, listener); }, kNoReply);
}

binder_status_t CameraService::setTorchMode(const std::string& cameraId, bool enabled,
                                            AIBinder* client) {
    return call(
            binder_, kTxSetTorchMode, "setTorchMode",
            [&](AParcel* p) {
                binder_status_t st;
                if ((st = AParcel_writeString(p, cameraId.c_str(), cameraId.size())) != STATUS_OK ||
                    (st = AParcel_writeBool(p, enabled)) != STATUS_OK ||
                    (st = AParcel_writeStrongBinder(p, client)) != STATUS_OK ||
                    (st = writeAttributionSource(p)) != STATUS_OK) {
                    return st;
                }
                return AParcel_writeInt32(p, kDevicePolicyDefault);
            },
            kNoReply);
}

binder_status_t CameraService::turnOnTorchWithStrengthLevel(const std::string& cameraId,
                                                            int32_t level, AIBinder* client) {
    return call(
            binder_, kTxTurnOnTorchWithStrengthLevel, "turnOnTorchWithStrengthLevel",
            [&](AParcel* p) {
                binder_status_t st;
                if ((st = AParcel_writeString(p, cameraId.c_str(), cameraId.size())) != STATUS_OK ||
                    (st = AParcel_writeInt32(p, level)) != STATUS_OK ||
                    (st = AParcel_writeStrongBinder(p, client)) != STATUS_OK ||
                    (st = writeAttributionSource(p)) != STATUS_OK) {
                    return st;
                }
                return AParcel_writeInt32(p, kDevicePolicyDefault);
            },
            kNoReply);
}

binder_status_t CameraService::getTorchStrengthLevel(const std::string& cameraId,
                                                     int32_t* level) {
    return call(
            binder_, kTxGetTorchStrengthLevel, "getTorchStrengthLevel",
            [&](AParcel* p) {
                binder_status_t st;
                if ((st = AParcel_writeString(p, cameraId.c_str(), cameraId.size())) != STATUS_OK ||
                    (st = writeAttributionSource(p)) != STATUS_OK) {
                    return st;
                }
                return AParcel_writeInt32(p, kDevicePolicyDefault);
            },
            [&](const AParcel* r) { return AParcel_readInt32(r, level); });
}

binder_status_t CameraService::linkToDeath(AIBinder_DeathRecipient* recipient, void* cookie) {
    return AIBinder_linkToDeath(binder_, recipient, cookie);
}

AIBinder* newTorchListener(TorchCallbacks* callbacks) {
    return AIBinder_new(listenerClass(), callbacks);
}

AIBinder* newClientToken() { return AIBinder_new(tokenClass(), nullptr); }

}  // namespace flashlightd
