#include "core/log.h"

#include <android/log.h>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace logx {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
}  // namespace

void init(const char* path) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fclose(g_file);
    g_file = fopen(path, "w");
}

void write(int level, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_write(level, "NFSUG2", buf);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        static const char kLv[] = "??VDIWEF";
        fprintf(g_file, "%5ld.%03ld %c %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000,
                kLv[level & 7], buf);
        if (level >= 5) fflush(g_file);
    }
}

void flush() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_file) fflush(g_file);
}
}  // namespace logx
