#include "binder_platform.h"

#include <dlfcn.h>

#include "log.h"

namespace flashlightd::platform {
namespace {

void* lib() {
    static void* handle = [] {
        void* h = dlopen("libbinder_ndk.so", RTLD_NOW);
        if (!h) LOGE("dlopen libbinder_ndk.so: %s", dlerror());
        return h;
    }();
    return handle;
}

template <typename Fn>
Fn sym(const char* name) {
    void* h = lib();
    return h ? reinterpret_cast<Fn>(dlsym(h, name)) : nullptr;
}

}  // namespace

AIBinder* getService(const char* name, bool wait) {
    using Fn = AIBinder* (*)(const char*);
    static const auto waitFor = sym<Fn>("AServiceManager_waitForService");  // API 31
    static const auto check = sym<Fn>("AServiceManager_checkService");
    static const auto get = sym<Fn>("AServiceManager_getService");
    if (wait && waitFor) return waitFor(name);
    if (check) return check(name);
    return get ? get(name) : nullptr;
}

bool setThreadPoolMaxThreadCount(uint32_t numThreads) {
    static const auto fn = sym<bool (*)(uint32_t)>("ABinderProcess_setThreadPoolMaxThreadCount");
    return fn && fn(numThreads);
}

void startThreadPool() {
    static const auto fn = sym<void (*)()>("ABinderProcess_startThreadPool");
    if (fn) fn();
    else LOGE("ABinderProcess_startThreadPool unavailable: torch callbacks will not arrive");
}

}  // namespace flashlightd::platform
