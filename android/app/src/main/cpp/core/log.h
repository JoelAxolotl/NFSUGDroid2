#pragma once
// Logging to logcat (tag "NFSUG2") and to <user>/log.txt, which `adb pull` can fetch.
#include <cstdarg>

namespace logx {
void init(const char* path);
void write(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void flush();
}  // namespace logx

#define LOGD(...) ::logx::write(3, __VA_ARGS__)
#define LOGI(...) ::logx::write(4, __VA_ARGS__)
#define LOGW(...) ::logx::write(5, __VA_ARGS__)
#define LOGE(...) ::logx::write(6, __VA_ARGS__)

// Logs once per call site; use for unimplemented-but-survivable paths.
#define LOG_ONCE(...)                     \
    do {                                  \
        static bool _logged = false;      \
        if (!_logged) {                   \
            _logged = true;               \
            LOGW(__VA_ARGS__);            \
        }                                 \
    } while (0)
