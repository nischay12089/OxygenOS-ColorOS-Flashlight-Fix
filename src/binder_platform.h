#pragma once

#include <android/binder_ibinder.h>

#include <cstdint>

// LL-NDK entry points that libbinder_ndk.so exports on-device but the public NDK does not
// declare (android/binder_manager.h, android/binder_process.h). Resolved with dlsym.
namespace flashlightd::platform {

// Looks up a service from servicemanager; `wait` blocks until it is registered.
AIBinder* getService(const char* name, bool wait);

bool setThreadPoolMaxThreadCount(uint32_t numThreads);
void startThreadPool();

}  // namespace flashlightd::platform
