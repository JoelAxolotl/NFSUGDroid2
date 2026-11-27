#include "hle/com.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "core/guest_heap.h"
#include "core/log.h"

namespace com {

namespace {
// Live objects by id (id = slot + 1). Lookups happen on every COM call from the
// game (thousands per frame) and are lock-free; g_mu only guards id allocation.
constexpr uint32_t kMaxObjects = 1u << 18;
std::atomic<Object*> g_slots[kMaxObjects];
std::mutex g_mu;
uint32_t g_next_id = 1;
std::vector<uint32_t> g_free_ids;

struct DefaultRet {
    uint32_t hr;
    bool logged;
};
// Thunk address -> default HRESULT for unimplemented methods.
std::mutex g_def_mu;
std::vector<std::pair<uint32_t, DefaultRet>> g_defaults;

void unimplemented(cpu::Cpu& c) {
    uint32_t hr = 0;
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(g_def_mu);
        for (auto& [addr, d] : g_defaults) {
            if (addr == c.eip) {
                hr = d.hr;
                first = !d.logged;
                d.logged = true;
                break;
            }
        }
    }
    if (first) LOGW("COM method not implemented: %s (returning %08x)", hle::thunk_name(c.eip), hr);
    c.r[cpu::EAX] = hr;
}

const uint32_t kIUnknown[4] = {0x00000000, 0x00000000, 0x000000C0, 0x46000000};
}  // namespace

uint32_t make_vtable(const Method* methods, uint32_t count) {
    uint32_t vt = mem::alloc_system(count * 4);
    for (uint32_t i = 0; i < count; ++i) {
        const Method& m = methods[i];
        uint32_t t = hle::make_thunk(m.name, m.fn ? m.fn : unimplemented, m.arg_bytes);
        if (!m.fn) {
            std::lock_guard<std::mutex> lk(g_def_mu);
            g_defaults.push_back({t, DefaultRet{m.default_hr, false}});
        }
        mem::wr32(vt + i * 4, t);
    }
    return vt;
}

uint32_t wrap(Object* host, uint32_t vtable, uint32_t payload_bytes) {
    uint32_t g = gheap::alloc(8 + payload_bytes, true);
    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_free_ids.empty()) {
            id = g_free_ids.back();
            g_free_ids.pop_back();
        } else {
            if (g_next_id > kMaxObjects) {
                LOGE("COM: more than %u live objects", kMaxObjects);
                abort();
            }
            id = g_next_id++;
        }
    }
    host->guest = g;
    mem::wr32(g, vtable);
    mem::wr32(g + 4, id);
    g_slots[id - 1].store(host, std::memory_order_release);
    return g;
}

Object* from_guest(uint32_t p) {
    if (!p) return nullptr;
    uint32_t id = mem::rd32(p + 4);
    if (id == 0 || id > kMaxObjects) return nullptr;
    Object* o = g_slots[id - 1].load(std::memory_order_acquire);
    return o && o->guest == p ? o : nullptr;
}

uint32_t add_ref(Object* o) { return o->refs.fetch_add(1) + 1; }

uint32_t release(Object* o) {
    uint32_t n = o->refs.fetch_sub(1) - 1;
    if (n == 0) {
        uint32_t g = o->guest;
        uint32_t id = mem::rd32(g + 4);
        if (id && id <= kMaxObjects) {
            g_slots[id - 1].store(nullptr, std::memory_order_release);
            std::lock_guard<std::mutex> lk(g_mu);
            g_free_ids.push_back(id);
        }
        delete o;
        mem::wr32(g, 0);
        gheap::free(g);
    }
    return n;
}

void QueryInterface(cpu::Cpu& c) {
    Object* o = from_guest(hle::arg(c, 0));
    uint32_t riid = hle::arg(c, 1), out = hle::arg(c, 2);
    if (!o || !out) {
        c.r[cpu::EAX] = win::E_INVALIDARG;
        return;
    }
    bool ok = guid_eq(riid, kIUnknown);
    for (uint32_t i = 0; i < o->iid_count && !ok; ++i) ok = guid_eq(riid, o->iids + i * 4);
    if (!ok) {
        mem::wr32(out, 0);
        c.r[cpu::EAX] = win::E_NOINTERFACE;
        return;
    }
    add_ref(o);
    mem::wr32(out, o->guest);
    c.r[cpu::EAX] = win::S_OK;
}

void AddRef(cpu::Cpu& c) {
    Object* o = from_guest(hle::arg(c, 0));
    c.r[cpu::EAX] = o ? add_ref(o) : 0;
}

void Release(cpu::Cpu& c) {
    Object* o = from_guest(hle::arg(c, 0));
    c.r[cpu::EAX] = o ? release(o) : 0;
}

}  // namespace com
