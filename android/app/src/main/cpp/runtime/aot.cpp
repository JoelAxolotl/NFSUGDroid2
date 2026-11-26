#include "runtime/aot.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace aot {

namespace {
// Populated once at startup before any guest thread runs, then read-only.
std::unordered_map<uint32_t, Fn> g_map;
bool g_any = false;

std::mutex g_miss_mu;
std::unordered_map<uint32_t, uint32_t> g_misses;
}  // namespace

void register_table(const Entry* entries, uint32_t count) {
    g_map.reserve(g_map.size() + count);
    for (uint32_t i = 0; i < count; ++i) g_map[entries[i].guest] = entries[i].fn;
    g_any = !g_map.empty();
}

Fn lookup(uint32_t guest_eip) {
    if (!g_any) return nullptr;
    auto it = g_map.find(guest_eip);
    return it == g_map.end() ? nullptr : it->second;
}

bool enabled() { return g_any; }

void note_miss(uint32_t guest_eip) {
    std::lock_guard<std::mutex> lk(g_miss_mu);
    g_misses[guest_eip]++;
}

void dump_misses(const char* path) {
    std::lock_guard<std::mutex> lk(g_miss_mu);
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (auto& [a, n] : g_misses) fprintf(f, "%08x %u\n", a, n);
    fclose(f);
}

}  // namespace aot
