#include "core/memory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

#include "core/log.h"

namespace mem {

uint8_t* base = nullptr;

namespace {
struct Region {
    uint32_t size;
    std::vector<uint8_t> committed;  // one flag per guest 4K page
};

std::mutex g_mu;
std::map<uint32_t, Region> g_regions;  // keyed by allocation base
uint32_t g_host_page = 4096;

inline uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
inline uint32_t align_down(uint32_t v, uint32_t a) { return v & ~(a - 1); }

bool host_protect(uint32_t addr, uint32_t size, bool rw) {
    uint64_t s = align_down(addr, g_host_page);
    uint64_t e = (uint64_t)addr + size;
    e = (e + g_host_page - 1) & ~(uint64_t)(g_host_page - 1);
    if (mprotect(base + s, (size_t)(e - s), rw ? (PROT_READ | PROT_WRITE) : PROT_NONE) != 0) {
        LOGE("mprotect(%08x,%x) failed", (uint32_t)s, (uint32_t)(e - s));
        return false;
    }
    return true;
}

// Region containing addr, or end().
std::map<uint32_t, Region>::iterator find_region(uint32_t addr) {
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return g_regions.end();
    --it;
    if ((uint64_t)addr < (uint64_t)it->first + it->second.size) return it;
    return g_regions.end();
}

bool range_free(uint32_t addr, uint32_t size) {
    auto it = g_regions.lower_bound(addr);
    if (it != g_regions.end() && it->first < (uint64_t)addr + size) return false;
    if (it != g_regions.begin()) {
        --it;
        if ((uint64_t)it->first + it->second.size > addr) return false;
    }
    return true;
}

uint32_t find_gap(uint32_t size, uint32_t lo, uint32_t hi) {
    uint32_t cand = align_up(lo, kAllocGranularity);
    for (auto it = g_regions.lower_bound(0); it != g_regions.end(); ++it) {
        uint64_t rend = (uint64_t)it->first + it->second.size;
        if (rend <= cand) continue;
        if (it->first >= cand + (uint64_t)size) break;
        cand = align_up((uint32_t)rend, kAllocGranularity);
    }
    if ((uint64_t)cand + size > hi) return 0;
    return cand;
}
}  // namespace

bool init() {
    g_host_page = (uint32_t)sysconf(_SC_PAGESIZE);
    void* p = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOGE("failed to reserve 4 GiB guest address space");
        return false;
    }
    base = (uint8_t*)p;
    LOGI("guest memory at host %p (host page %u)", p, g_host_page);
    return true;
}

uint32_t reserve(uint32_t addr, uint32_t size, bool do_commit, uint32_t lo, uint32_t hi) {
    if (size == 0) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    size = align_up(size, kPage);
    if (addr) {
        addr = align_down(addr, kAllocGranularity);
        if (!range_free(addr, size)) return 0;
    } else {
        addr = find_gap(size, lo, hi);
        if (!addr) {
            LOGE("guest address space exhausted (size %x)", size);
            return 0;
        }
    }
    Region r;
    r.size = size;
    r.committed.assign(size / kPage, do_commit ? 1 : 0);
    if (do_commit && !host_protect(addr, size, true)) return 0;
    g_regions.emplace(addr, std::move(r));
    return addr;
}

bool commit(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = find_region(addr);
    if (it == g_regions.end()) return false;
    uint32_t s = align_down(addr, kPage);
    uint64_t e = std::min<uint64_t>(align_up(addr + size, kPage), (uint64_t)it->first + it->second.size);
    for (uint64_t p = s; p < e; p += kPage) it->second.committed[(p - it->first) / kPage] = 1;
    return host_protect(s, (uint32_t)(e - s), true);
}

bool decommit(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = find_region(addr);
    if (it == g_regions.end()) return false;
    uint32_t s = align_down(addr, kPage);
    uint64_t e = std::min<uint64_t>(align_up(addr + size, kPage), (uint64_t)it->first + it->second.size);
    for (uint64_t p = s; p < e; p += kPage) it->second.committed[(p - it->first) / kPage] = 0;
    // Zero the contents (Windows returns zeroed pages on recommit) but keep the
    // host mapping accessible: host pages may be larger than guest pages.
    memset(base + s, 0, (size_t)(e - s));
    return true;
}

bool release(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_regions.find(addr);
    if (it == g_regions.end()) return false;
    // Host pages may straddle neighbouring regions; only zero, then drop access
    // for the fully-owned interior.
    memset(base + addr, 0, it->second.size);
    uint32_t s = align_up(addr, g_host_page);
    uint32_t e = align_down(addr + it->second.size, g_host_page);
    if (e > s) madvise(base + s, e - s, MADV_DONTNEED), mprotect(base + s, e - s, PROT_NONE);
    g_regions.erase(it);
    return true;
}

RegionInfo query(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_mu);
    RegionInfo ri{};
    auto it = find_region(addr);
    if (it == g_regions.end()) {
        // Free run: up to the next region.
        auto nx = g_regions.upper_bound(addr);
        ri.base = align_down(addr, kPage);
        uint64_t end = nx == g_regions.end() ? 0x80000000ull : nx->first;
        ri.size = (uint32_t)(end > ri.base ? end - ri.base : kPage);
        return ri;
    }
    const Region& r = it->second;
    uint32_t idx = (addr - it->first) / kPage;
    uint8_t st = r.committed[idx];
    uint32_t a = idx, b = idx;
    while (a > 0 && r.committed[a - 1] == st) --a;
    while (b + 1 < r.committed.size() && r.committed[b + 1] == st) ++b;
    ri.alloc_base = it->first;
    ri.base = it->first + a * kPage;
    ri.size = (b - a + 1) * kPage;
    ri.reserved = true;
    ri.committed = st != 0;
    return ri;
}

bool is_committed(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = find_region(addr);
    if (it == g_regions.end()) return false;
    uint64_t e = (uint64_t)addr + size;
    if (e > (uint64_t)it->first + it->second.size) return false;
    for (uint64_t p = align_down(addr, kPage); p < e; p += kPage)
        if (!it->second.committed[(p - it->first) / kPage]) return false;
    return true;
}

uint32_t alloc_system(uint32_t size) {
    // Small, never-freed allocations (PEB/TEB, strings, HLE objects) are
    // carved from 1 MiB chunks; big ones get their own reservation.
    static std::mutex s_mu;
    static uint32_t s_cur = 0, s_end = 0;
    constexpr uint32_t kChunk = 1u << 20;
    size = align_up(size ? size : 1, 16);
    if (size > kChunk / 4) return reserve(0, size, true, kSystemStart, kSystemEnd);
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_cur + size > s_end) {
        uint32_t a = reserve(0, kChunk, true, kSystemStart, kSystemEnd);
        if (!a) {
            LOGE("system arena exhausted");
            return 0;
        }
        s_cur = a;
        s_end = a + kChunk;
    }
    uint32_t r = s_cur;
    s_cur += size;
    return r;
}

uint32_t strlen_g(uint32_t a) { return a ? (uint32_t)strlen((const char*)(base + a)) : 0; }

void write_cstr(uint32_t a, const char* s, uint32_t cap) {
    if (!cap) return;
    uint32_t n = (uint32_t)strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(base + a, s, n);
    base[a + n] = 0;
}

}  // namespace mem
