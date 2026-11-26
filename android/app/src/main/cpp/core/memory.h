#pragma once
// Guest (x86-32) address space hosted in a single 4 GiB host reservation.
// A guest address `a` lives at host `mem::base + a`, so every 32-bit value is
// a valid offset and no bounds checks are needed on the fast path. Uncommitted
// pages are PROT_NONE; touching them raises SIGSEGV, which the fault handler
// reports with the guest EIP.
#include <cstdint>
#include <cstring>

namespace mem {

extern uint8_t* base;

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kAllocGranularity = 0x10000;

// Guest layout (below 2 GiB so signed pointer compares in the game stay valid).
constexpr uint32_t kNullGuardEnd = 0x00010000;
constexpr uint32_t kDynamicStart = 0x01000000;  // VirtualAlloc / heaps / stacks
constexpr uint32_t kDynamicEnd = 0x7F000000;
constexpr uint32_t kSystemStart = 0x7F000000;   // thunks, PEB/TEB, HLE objects
constexpr uint32_t kSystemEnd = 0x7FFF0000;

bool init();

inline uint8_t* ptr(uint32_t a) { return base + a; }
inline uint32_t addr_of(const void* p) {
    return (uint32_t)((const uint8_t*)p - base);
}

template <typename T>
inline T rd(uint32_t a) {
    T v;
    memcpy(&v, base + a, sizeof(T));
    return v;
}
template <typename T>
inline void wr(uint32_t a, T v) {
    memcpy(base + a, &v, sizeof(T));
}
inline uint8_t rd8(uint32_t a) { return base[a]; }
inline uint16_t rd16(uint32_t a) { return rd<uint16_t>(a); }
inline uint32_t rd32(uint32_t a) { return rd<uint32_t>(a); }
inline void wr8(uint32_t a, uint8_t v) { base[a] = v; }
inline void wr16(uint32_t a, uint16_t v) { wr<uint16_t>(a, v); }
inline void wr32(uint32_t a, uint32_t v) { wr<uint32_t>(a, v); }

// --- Virtual memory (VirtualAlloc semantics) ---
enum Prot : uint32_t { kNoAccess = 0, kRead = 1, kWrite = 2, kExec = 4, kRW = 3, kRWX = 7 };

// Reserve (and optionally commit) `size` bytes. `addr` 0 = anywhere in the
// dynamic range; otherwise the exact (64K aligned) address. Returns 0 on failure.
uint32_t reserve(uint32_t addr, uint32_t size, bool commit, uint32_t lo = kDynamicStart,
                 uint32_t hi = kDynamicEnd);
bool commit(uint32_t addr, uint32_t size);
bool decommit(uint32_t addr, uint32_t size);
bool release(uint32_t addr);

struct RegionInfo {
    uint32_t alloc_base;
    uint32_t base;        // start of the run of pages with identical state
    uint32_t size;        // length of that run
    bool reserved;        // false = free address space
    bool committed;
};
// VirtualQuery: describes the run of same-state pages containing `addr`.
RegionInfo query(uint32_t addr);
bool is_committed(uint32_t addr, uint32_t size);

// Convenience: committed RW block in the system range (never freed).
uint32_t alloc_system(uint32_t size);

// Copy helpers for strings.
inline const char* cstr(uint32_t a) { return a ? (const char*)(base + a) : nullptr; }
uint32_t strlen_g(uint32_t a);
void write_cstr(uint32_t a, const char* s, uint32_t cap);  // always NUL terminates if cap>0

}  // namespace mem
