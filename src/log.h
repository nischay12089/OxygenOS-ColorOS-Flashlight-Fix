#pragma once

#include <android/log.h>
#include <cstdio>

namespace flashlightd {
// CLI sub-commands mirror every log line to stderr; the daemon only logs to logcat.
extern bool gLogToStderr;
}  // namespace flashlightd

#define FL_LOG(prio, fmt, ...)                                              \
    do {                                                                    \
        __android_log_print(prio, "flashlightd", fmt, ##__VA_ARGS__);       \
        if (::flashlightd::gLogToStderr) {                                  \
            std::fprintf(stderr, fmt "\n", ##__VA_ARGS__);                  \
        }                                                                   \
    } while (0)

#define LOGD(...) FL_LOG(ANDROID_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) FL_LOG(ANDROID_LOG_INFO, __VA_ARGS__)
#define LOGW(...) FL_LOG(ANDROID_LOG_WARN, __VA_ARGS__)
#define LOGE(...) FL_LOG(ANDROID_LOG_ERROR, __VA_ARGS__)
