#include "hle/thunks.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "hle/hle.h"
#include "runtime/process.h"

namespace hle {

namespace {
struct Thunk {
    const char* name;
    HleFn fn;
    uint16_t arg_bytes;
    std::atomic<uint32_t> calls;
};

constexpr uint32_t kMaxThunks = kThunkSpan / 4;
Thunk g_thunks[kMaxThunks];
std::atomic<uint32_t> g_thunk_count{1};  // slot 0 = return-to-host
std::mutex g_mu;
bool g_trace = false;

struct Export {
    HleFn fn;
    uint16_t arg_bytes;
    uint32_t thunk;  // lazily created
};

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    if (s.find('.') == std::string::npos) s += ".dll";
    return s;
}

std::map<std::string, Export>& exports() {
    static std::map<std::string, Export> m;
    return m;
}
std::map<std::string, std::string>& aliases() {
    static std::map<std::string, std::string> m;
    return m;
}
std::map<std::string, uint32_t>& missing() {
    static std::map<std::string, uint32_t> m;
    return m;
}
std::vector<std::string>& missing_names() {
    static std::vector<std::string> v;
    return v;
}

void missing_import_trap(cpu::Cpu& c) {
    uint32_t idx = (c.eip - kThunkBase) / 4;
    LOGE("call to unimplemented import %s (ret=%08x args %08x %08x %08x %08x)", g_thunks[idx].name,
         mem::rd32(c.r[cpu::ESP]), arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    proc::set_status(std::string("Unimplemented: ") + g_thunks[idx].name);
    c.stop = cpu::Stop::Fault;
}

const std::vector<std::string>& known_dlls() {
    static const std::vector<std::string> v = {
        "kernel32.dll", "user32.dll",  "gdi32.dll",   "advapi32.dll", "winmm.dll",
        "ws2_32.dll",   "wsock32.dll", "shell32.dll", "shfolder.dll", "tapi32.dll",
        "netapi32.dll", "ddraw.dll",   "dinput8.dll", "dsound.dll",   "d3d9.dll",
        "ole32.dll",    "version.dll", "ntdll.dll",   "msvcrt.dll",   "d3dx9_25.dll",
    };
    return v;
}
}  // namespace

Registrar::Registrar(const char* dll, const char* name, HleFn fn, uint16_t arg_bytes) {
    exports()[lower(dll) + "!" + name] = Export{fn, arg_bytes, 0};
}

OrdinalAlias::OrdinalAlias(const char* dll, uint32_t ordinal, const char* name) {
    aliases()[lower(dll) + "!#" + std::to_string(ordinal)] = lower(dll) + "!" + name;
}

uint32_t make_thunk(const char* name, HleFn fn, uint16_t arg_bytes) {
    uint32_t idx = g_thunk_count.fetch_add(1);
    if (idx >= kMaxThunks) {
        LOGE("thunk table full");
        return 0;
    }
    g_thunks[idx].name = name;
    g_thunks[idx].arg_bytes = arg_bytes;
    g_thunks[idx].fn = fn;
    return kThunkBase + idx * 4;
}

const char* thunk_name(uint32_t addr) {
    uint32_t idx = (addr - kThunkBase) / 4;
    return idx < g_thunk_count.load() && g_thunks[idx].name ? g_thunks[idx].name : "?";
}

uint32_t find_export(const std::string& dll, const std::string& name) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string key = lower(dll) + "!" + name;
    auto al = aliases().find(key);
    if (al != aliases().end()) key = al->second;
    auto it = exports().find(key);
    if (it == exports().end()) return 0;
    if (!it->second.thunk) {
        // Names must outlive the thunk table: keep them in the map key.
        it->second.thunk = make_thunk(it->first.c_str(), it->second.fn, it->second.arg_bytes);
    }
    return it->second.thunk;
}

uint32_t resolve_import(const std::string& dll, const std::string& name) {
    if (uint32_t t = find_export(dll, name)) return t;
    std::lock_guard<std::mutex> lk(g_mu);
    std::string key = lower(dll) + "!" + name;
    auto it = missing().find(key);
    if (it != missing().end()) return it->second;
    missing_names().push_back(key);
    // std::vector reallocation would move strings; store names in a stable list.
    static std::vector<std::unique_ptr<std::string>> names;
    names.push_back(std::make_unique<std::string>(key));
    uint32_t t = make_thunk(names.back()->c_str(), missing_import_trap, 0);
    missing()[key] = t;
    LOGW("import not implemented (trap installed): %s", key.c_str());
    return t;
}

bool is_known_dll(const std::string& dll) {
    std::string l = lower(dll);
    auto& v = known_dlls();
    return std::find(v.begin(), v.end(), l) != v.end();
}

uint32_t module_handle(const std::string& dll) {
    std::string l = lower(dll);
    auto& v = known_dlls();
    auto it = std::find(v.begin(), v.end(), l);
    if (it == v.end()) return 0;
    return 0x7E000000u + (uint32_t)(it - v.begin()) * 0x10000u;
}

std::string module_name(uint32_t handle) {
    auto& v = known_dlls();
    if (handle < 0x7E000000u) return "";
    uint32_t i = (handle - 0x7E000000u) / 0x10000u;
    return i < v.size() ? v[i] : "";
}

void init() {
    uint32_t a = mem::reserve(kThunkBase, kThunkSpan, true, kThunkBase, kThunkBase + kThunkSpan);
    if (a != kThunkBase) LOGE("failed to map thunk area");
    // Fill with INT3 so a stray decode of the area is obvious.
    memset(mem::ptr(kThunkBase), 0xCC, kThunkSpan);
    g_thunks[0].name = "<return-to-host>";
}

void set_trace(bool on) { g_trace = on; }
bool trace_enabled() { return g_trace; }

void dispatch(cpu::Cpu& c) {
    uint32_t idx = (c.eip - kThunkBase) >> 2;
    if (idx == 0) {
        c.stop = cpu::Stop::ReturnToHost;
        return;
    }
    Thunk& t = g_thunks[idx];
    if (idx >= g_thunk_count.load(std::memory_order_acquire) || !t.fn) {
        LOGE("jump into unassigned thunk %08x", c.eip);
        c.stop = cpu::Stop::Fault;
        return;
    }
    const uint32_t ret_addr = mem::rd32(c.r[cpu::ESP]);
    if (g_trace) {
        uint32_t n = t.calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 32)
            LOGD("[t%u] %s(%08x, %08x, %08x, %08x) from %08x", c.thread_id, t.name, arg(c, 0), arg(c, 1),
                 arg(c, 2), arg(c, 3), ret_addr);
    }
    // Only this thread writes in_host, so plain stores suffice (no atomic RMW): seq_cst
    // store (stlr) pairs with the suspender's seq_cst store/load of suspend_req.
    const uint32_t depth = __atomic_load_n(&c.in_host, __ATOMIC_RELAXED);
    __atomic_store_n(&c.in_host, depth + 1, __ATOMIC_RELEASE);  // publishes prior guest writes
    t.fn(c);
    __atomic_store_n(&c.in_host, depth, __ATOMIC_SEQ_CST);
    if (c.stop != cpu::Stop::None) return;  // thread/process exit or fault inside the HLE call
    cpu::check_suspend(c);
    c.r[cpu::ESP] += 4 + t.arg_bytes;
    c.eip = ret_addr;
}

uint32_t call_guest(cpu::Cpu& c, uint32_t fn, std::initializer_list<uint32_t> args) {
    const uint32_t saved_esp = c.r[cpu::ESP];
    const uint32_t saved_eip = c.eip;
    for (auto it = std::rbegin(args); it != std::rend(args); ++it) cpu::push32(c, *it);
    cpu::push32(c, kReturnToHostThunk);
    c.eip = fn;
    c.host_call_depth++;
    // The callback is guest code: while it runs, this thread is not "in host code".
    const uint32_t saved_in_host = __atomic_exchange_n(&c.in_host, 0, __ATOMIC_SEQ_CST);
    cpu::Stop s = cpu::run(c);
    __atomic_store_n(&c.in_host, saved_in_host, __ATOMIC_SEQ_CST);
    c.host_call_depth--;
    uint32_t result = c.r[cpu::EAX];
    if (s == cpu::Stop::ReturnToHost) {
        c.r[cpu::ESP] = saved_esp;
        c.eip = saved_eip;
    }
    return result;
}

}  // namespace hle
