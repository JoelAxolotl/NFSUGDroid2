#pragma once
// Guest-visible COM objects backed by host C++ objects.
//
// Guest layout of every object: [+0] vtable pointer, [+4] host object id,
// [+8...] optional guest-visible payload. The vtable is an array of thunk
// addresses in guest memory, one per interface method in declaration order.
#include <atomic>
#include <cstdint>
#include <cstring>

#include "hle/hle.h"

namespace com {

struct Method {
    const char* name;      // "IDirect3DDevice9::Clear"
    uint16_t arg_bytes;    // stdcall bytes including `this`
    hle::HleFn fn;         // null: log once, return `default_hr`
    uint32_t default_hr;
};

struct Object {
    virtual ~Object() = default;
    uint32_t guest = 0;                 // guest address of the COM object
    std::atomic<uint32_t> refs{1};
    const uint32_t* iids = nullptr;     // GUIDs (4 dwords each) answered by QueryInterface
    uint32_t iid_count = 0;
};

// Builds a guest vtable for `count` methods. Call once per interface.
uint32_t make_vtable(const Method* methods, uint32_t count);

// Allocates the guest side of `host` with the given vtable and payload size;
// returns the guest pointer (also stored in host->guest).
uint32_t wrap(Object* host, uint32_t vtable, uint32_t payload_bytes = 0);

// Host object behind a guest interface pointer (null if not ours).
Object* from_guest(uint32_t guest_ptr);
template <typename T>
T* self(cpu::Cpu& c) {
    return static_cast<T*>(from_guest(hle::arg(c, 0)));
}

uint32_t add_ref(Object* o);
uint32_t release(Object* o);  // destroys the host object and frees guest memory at 0

// Generic IUnknown implementations usable directly in method tables.
void QueryInterface(cpu::Cpu& c);
void AddRef(cpu::Cpu& c);
void Release(cpu::Cpu& c);

inline bool guid_eq(uint32_t guest_guid, const uint32_t g[4]) {
    return memcmp(mem::ptr(guest_guid), g, 16) == 0;
}

}  // namespace com
