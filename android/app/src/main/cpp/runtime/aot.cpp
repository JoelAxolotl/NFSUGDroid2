#include "runtime/aot.h"

#include <sys/mman.h>

#include <cstdio>
#include <mutex>
#include <set>
#include <vector>

#include "core/log.h"

namespace aot {

namespace detail {
Fn* g_table = nullptr;
uint8_t* g_seen = nullptr;
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

void init(uint32_t code_start, uint32_t code_end) {
    using namespace detail;
    g_start = code_start;
    g_size = code_end - code_start;
    // One pointer per code byte; pages are only touched where functions start.
    g_table = (Fn*)mmap(nullptr, (size_t)g_size * sizeof(Fn), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_seen = (uint8_t*)mmap(nullptr, g_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                            -1, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [e, n] : pending()) apply(e, n);
    pending().clear();
    LOGI("AOT: %u translated functions registered", g_count);
}

void register_table(const Entry* entries, uint32_t count) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (detail::g_table)
        apply(entries, count);
    else
        pending().push_back({entries, count});
}

uint32_t translated_count() { return g_count; }

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
