#pragma once
// Small helpers shared by HLE modules.
#include <ctime>
#include <string>

#include "core/memory.h"

namespace winu {

inline std::string gstr(uint32_t a) { return a ? std::string(mem::cstr(a)) : std::string(); }

// UTF-16LE guest string -> narrow (Latin-1, '?' for anything wider).
inline std::string gwstr(uint32_t a) {
    std::string s;
    if (!a) return s;
    for (uint16_t ch; (ch = mem::rd16(a)) != 0; a += 2) s += ch < 0x100 ? (char)ch : '?';
    return s;
}

// Writes `s` to a guest buffer of `cap` chars; returns chars written without
// NUL, or the required size including NUL when it does not fit (Win32 style).
inline uint32_t put_str(uint32_t buf, uint32_t cap, const std::string& s) {
    if (cap <= s.size()) {
        if (cap) mem::wr8(buf, 0);
        return (uint32_t)s.size() + 1;
    }
    memcpy(mem::ptr(buf), s.data(), s.size());
    mem::wr8(buf + (uint32_t)s.size(), 0);
    return (uint32_t)s.size();
}

inline uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// 100-ns intervals since 1601-01-01 (FILETIME).
inline uint64_t filetime_now() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}
inline uint64_t unix_to_filetime(int64_t sec, int64_t nsec) {
    return (uint64_t)(sec + 11644473600ll) * 10000000ull + (uint64_t)nsec / 100;
}

}  // namespace winu
