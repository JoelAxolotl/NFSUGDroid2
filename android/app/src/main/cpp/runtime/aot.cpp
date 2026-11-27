#include "runtime/aot.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "core/log.h"

namespace aot {

namespace detail {
Fn* g_table = nullptr;
uint8_t* g_seen = nullptr;
uint32_t* g_hot = nullptr;
uint32_t* g_entry = nullptr;
uint32_t g_start = 0, g_size = 0;
}  // namespace detail

namespace {
// Tables registered before init() (static constructors of generated files).
std::vector<std::pair<const Entry*, uint32_t>>& pending() {
    static std::vector<std::pair<const Entry*, uint32_t>> v;
    return v;
}
uint32_t g_count = 0;
std::mutex g_mu;
bool g_has_identity = false;
uint32_t g_src_size = 0;
uint64_t g_src_hash = 0;
bool g_enabled = false;

// FNV-1a 64 over the whole file; returns false if unreadable.
bool hash_file(const char* path, uint32_t* size, uint64_t* hash) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t n = 0;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        for (size_t i = 0; i < got; ++i) h = (h ^ buf[i]) * 0x100000001B3ull;
        n += (uint32_t)got;
    }
    fclose(f);
    *size = n;
    *hash = h;
    return true;
}

void apply(const Entry* e, uint32_t n) {
    using namespace detail;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t off = e[i].guest - g_start;
        if (off < g_size) {
            g_table[off] = e[i].fn;
            ++g_count;
        }
    }
}
}  // namespace

void set_source_identity(uint32_t size, uint64_t fnv1a64) {
    g_has_identity = true;
    g_src_size = size;
    g_src_hash = fnv1a64;
}

bool native_enabled() { return g_enabled; }

void init(uint32_t code_start, uint32_t code_end, const char* exe_path) {
    using namespace detail;
    g_start = code_start;
    g_size = code_end - code_start;
    // One pointer per code byte; pages are only touched where functions start.
    g_table = (Fn*)mmap(nullptr, (size_t)g_size * sizeof(Fn), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_seen = (uint8_t*)mmap(nullptr, g_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                            -1, 0);
    g_hot = (uint32_t*)mmap(nullptr, ((size_t)g_size >> 4) * sizeof(uint32_t) + 4, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_entry = (uint32_t*)mmap(nullptr, ((size_t)g_size >> 4) * sizeof(uint32_t) + 4, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    if (pending().empty()) {
        LOGI("AOT: this build contains no recompiled code; running on the interpreter");
        return;
    }
    uint32_t size = 0;
    uint64_t hash = 0;
    bool readable = hash_file(exe_path, &size, &hash);
    if (!g_has_identity || !readable || size != g_src_size || hash != g_src_hash) {
        LOGE("AOT: %s does not match the executable this build was recompiled from "
             "(%u bytes / %016llx, expected %u / %016llx); native code disabled",
             exe_path, size, (unsigned long long)hash, g_src_size, (unsigned long long)g_src_hash);
        pending().clear();
        return;
    }
    for (auto& [e, n] : pending()) apply(e, n);
    pending().clear();
    g_enabled = true;
    LOGI("AOT: %s verified; %u natively recompiled functions enabled", exe_path, g_count);
}

void register_table(const Entry* entries, uint32_t count) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (detail::g_table)
        apply(entries, count);
    else
        pending().push_back({entries, count});
}

uint32_t translated_count() { return g_count; }

namespace {
void log_top(uint32_t* hist, const char* label, int top) {
    using namespace detail;
    uint32_t n = g_size >> 4;
    std::vector<std::pair<uint32_t, uint32_t>> v;
    for (uint32_t i = 0; i < n; ++i)
        if (hist[i]) v.push_back({hist[i], i});
    std::partial_sort(v.begin(), v.begin() + std::min<size_t>(v.size(), (size_t)top), v.end(),
                      [](auto& a, auto& b) { return a.first > b.first; });
    for (int i = 0; i < top && i < (int)v.size(); ++i)
        LOGI("  %s %08x: %u", label, g_start + (v[i].second << 4), v[i].first);
    memset(hist, 0, (size_t)n * sizeof(uint32_t));
}
}  // namespace

void log_hot_spots(int top) {
    using namespace detail;
    if (!g_hot) return;
    log_top(g_hot, "hot", top);
    log_top(g_entry, "entry", top);
}

void dump_profile(const char* path) {
    using namespace detail;
    if (!g_seen) return;
    std::set<uint32_t> all;
    if (FILE* f = fopen(path, "r")) {
        unsigned a;
        while (fscanf(f, "%x", &a) == 1) all.insert(a);
        fclose(f);
    }
    size_t before = all.size();
    for (uint32_t i = 0; i < g_size; ++i)
        if (g_seen[i]) all.insert(g_start + i);
    if (all.size() == before) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (uint32_t a : all) fprintf(f, "%08x\n", a);
    fclose(f);
}

}  // namespace aot
