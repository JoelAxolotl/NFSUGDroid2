#include "core/guest_heap.h"

#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "core/memory.h"

namespace gheap {

namespace {
constexpr uint32_t kAlign = 16;
constexpr uint32_t kArena = 16u << 20;

std::mutex g_mu;
std::map<uint32_t, uint32_t> g_free_by_addr;            // addr -> size
std::multimap<uint32_t, uint32_t> g_free_by_size;       // size -> addr
std::unordered_map<uint32_t, uint32_t> g_live;          // addr -> requested size (rounded)

void remove_free(std::map<uint32_t, uint32_t>::iterator it) {
    auto range = g_free_by_size.equal_range(it->second);
    for (auto s = range.first; s != range.second; ++s) {
        if (s->second == it->first) {
            g_free_by_size.erase(s);
            break;
        }
    }
    g_free_by_addr.erase(it);
}

void insert_free(uint32_t addr, uint32_t size) {
    // Coalesce with neighbours.
    auto next = g_free_by_addr.lower_bound(addr);
    if (next != g_free_by_addr.end() && next->first == addr + size) {
        size += next->second;
        remove_free(next);
    }
    auto prev = g_free_by_addr.lower_bound(addr);
    if (prev != g_free_by_addr.begin()) {
        --prev;
        if (prev->first + prev->second == addr) {
            addr = prev->first;
            size += prev->second;
            remove_free(prev);
        }
    }
    g_free_by_addr[addr] = size;
    g_free_by_size.emplace(size, addr);
}

bool grow(uint32_t need) {
    uint32_t sz = need > kArena ? ((need + 0xFFFF) & ~0xFFFFu) : kArena;
    uint32_t a = mem::reserve(0, sz, true);
    if (!a) return false;
    insert_free(a, sz);
    return true;
}

uint32_t alloc_locked(uint32_t size) {
    size = size ? (size + kAlign - 1) & ~(kAlign - 1) : kAlign;
    auto it = g_free_by_size.lower_bound(size);
    if (it == g_free_by_size.end()) {
        if (!grow(size)) return 0;
        it = g_free_by_size.lower_bound(size);
        if (it == g_free_by_size.end()) return 0;
    }
    uint32_t addr = it->second, bsize = it->first;
    g_free_by_size.erase(it);
    g_free_by_addr.erase(addr);
    if (bsize > size) {
        g_free_by_addr[addr + size] = bsize - size;
        g_free_by_size.emplace(bsize - size, addr + size);
    }
    g_live[addr] = size;
    return addr;
}
}  // namespace

uint32_t alloc(uint32_t size, bool zero) {
    uint32_t a;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        a = alloc_locked(size);
    }
    if (!a) {
        LOGE("guest heap: out of memory for %u bytes", size);
        return 0;
    }
    if (zero) memset(mem::ptr(a), 0, size);
    return a;
}

bool free(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_live.find(addr);
    if (it == g_live.end()) return false;
    uint32_t sz = it->second;
    g_live.erase(it);
    insert_free(addr, sz);
    return true;
}

uint32_t size(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_live.find(addr);
    return it == g_live.end() ? 0xFFFFFFFFu : it->second;
}

uint32_t realloc(uint32_t addr, uint32_t new_size, bool zero_tail, bool in_place_only) {
    if (!addr) return in_place_only ? 0 : alloc(new_size, zero_tail);
    uint32_t old;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_live.find(addr);
        if (it == g_live.end()) return 0;
        old = it->second;
        uint32_t want = new_size ? (new_size + kAlign - 1) & ~(kAlign - 1) : kAlign;
        if (want <= old) {
            if (want < old) {
                it->second = want;
                insert_free(addr + want, old - want);
            }
            return addr;
        }
        // Try to extend into the following free block.
        auto nx = g_free_by_addr.find(addr + old);
        if (nx != g_free_by_addr.end() && old + nx->second >= want) {
            uint32_t total = old + nx->second;
            remove_free(nx);
            it->second = want;
            if (total > want) insert_free(addr + want, total - want);
            if (zero_tail) memset(mem::ptr(addr + old), 0, want - old);
            return addr;
        }
        if (in_place_only) return 0;
    }
    uint32_t n = alloc(new_size, false);
    if (!n) return 0;
    memcpy(mem::ptr(n), mem::ptr(addr), old);
    if (zero_tail && new_size > old) memset(mem::ptr(n + old), 0, new_size - old);
    free(addr);
    return n;
}

}  // namespace gheap
