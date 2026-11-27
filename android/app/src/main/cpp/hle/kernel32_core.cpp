// kernel32: process, threads, memory, synchronization, time, locale, misc.
#include <sched.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/guest_heap.h"
#include "hle/hle.h"
#include "hle/kobjects.h"
#include "hle/win_util.h"
#include "loader/pe_loader.h"
#include "runtime/process.h"

using namespace hle;
using winu::gstr;

namespace hle {
void set_last_error(uint32_t err) {
    if (cpu::Cpu* c = cpu::current()) mem::wr32(c->fs_base + 0x34, err);
}
uint32_t get_last_error() {
    cpu::Cpu* c = cpu::current();
    return c ? mem::rd32(c->fs_base + 0x34) : 0;
}
}  // namespace hle

namespace {
constexpr uint32_t kPseudoProcess = 0xFFFFFFFFu;
constexpr uint32_t kPseudoThread = 0xFFFFFFFEu;
constexpr uint32_t kStdIn = 0x10, kStdOut = 0x14, kStdErr = 0x18;

std::mutex g_tls_mu;
uint64_t g_tls_used = 0;

std::mutex g_cs_mu;
std::unordered_map<uint32_t, std::recursive_mutex*> g_cs;

std::recursive_mutex* cs_for(uint32_t addr) {
    std::lock_guard<std::mutex> lk(g_cs_mu);
    auto& m = g_cs[addr];
    if (!m) m = new std::recursive_mutex();
    return m;
}

proc::Thread* thread_from_handle(uint32_t h) {
    if (h == kPseudoThread) return proc::current_thread();
    return kobj::get_as<proc::Thread>(h, kobj::Type::Thread);
}

const char kEnvA[] = "PATH=C:\\Windows\\system32;C:\\Windows\0WINDIR=C:\\Windows\0\0";
uint32_t g_env_a = 0, g_env_w = 0;
uint32_t g_unhandled_filter = 0;
}  // namespace

HLE_EXPORT(kernel32, GetLastError, 0) { ret(c, mem::rd32(c.fs_base + 0x34)); }
HLE_EXPORT(kernel32, SetLastError, 4) { mem::wr32(c.fs_base + 0x34, arg(c, 0)); }

// ---------------------------------------------------------------- process
HLE_EXPORT(kernel32, GetCurrentProcessId, 0) { ret(c, (uint32_t)getpid()); }
HLE_EXPORT(kernel32, GetCurrentProcess, 0) { ret(c, kPseudoProcess); }
HLE_EXPORT(kernel32, GetCurrentThread, 0) { ret(c, kPseudoThread); }
HLE_EXPORT(kernel32, GetCurrentThreadId, 0) { ret(c, proc::current_tid()); }
HLE_EXPORT(kernel32, GetCommandLineA, 0) { ret(c, proc::command_line_a()); }

HLE_EXPORT(kernel32, GetStartupInfoA, 4) {
    uint32_t si = arg(c, 0);
    memset(mem::ptr(si), 0, 68);
    mem::wr32(si, 68);
}

HLE_EXPORT(kernel32, GetModuleHandleA, 4) {
    std::string name = gstr(arg(c, 0));
    if (name.empty()) return ret(c, loader::main_image().base);
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (strcasecmp(name.c_str(), proc::kExeName) == 0) return ret(c, loader::main_image().base);
    uint32_t h = module_handle(name);
    if (!h) set_last_error(126);
    ret(c, h);
}

HLE_EXPORT(kernel32, GetModuleFileNameA, 12) {
    uint32_t h = arg(c, 0), buf = arg(c, 1), size = arg(c, 2);
    std::string path;
    if (!h || h == loader::main_image().base)
        path = std::string(proc::kGameRootWin) + "\\" + proc::kExeName;
    else
        path = "C:\\Windows\\system32\\" + module_name(h);
    uint32_t n = std::min<uint32_t>((uint32_t)path.size(), size ? size - 1 : 0);
    memcpy(mem::ptr(buf), path.data(), n);
    if (size) mem::wr8(buf + n, 0);
    ret(c, n);
}

HLE_EXPORT(kernel32, LoadLibraryA, 4) {
    std::string name = gstr(arg(c, 0));
    size_t slash = name.find_last_of("\\/");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    uint32_t h = module_handle(base);
    if (!h) {
        LOGW("LoadLibraryA(%s) failed: not an emulated DLL", name.c_str());
        set_last_error(126);
    } else if (trace_enabled()) {
        LOGD("LoadLibraryA(%s) -> %08x", name.c_str(), h);
    }
    ret(c, h);
}
HLE_EXPORT(kernel32, FreeLibrary, 4) { ret(c, 1); }

HLE_EXPORT(kernel32, GetProcAddress, 8) {
    uint32_t h = arg(c, 0), np = arg(c, 1);
    std::string mod = h == loader::main_image().base ? std::string() : module_name(h);
    std::string sym = np < 0x10000 ? "#" + std::to_string(np) : gstr(np);
    uint32_t a = mod.empty() ? 0 : find_export(mod, sym);
    if (!a) {
        static std::mutex mu;
        static std::map<std::string, bool> seen;
        std::lock_guard<std::mutex> lk(mu);
        if (!seen[mod + "!" + sym]) {
            seen[mod + "!" + sym] = true;
            LOGW("GetProcAddress(%s, %s): not implemented", mod.c_str(), sym.c_str());
        }
        set_last_error(127);
    }
    ret(c, a);
}

HLE_EXPORT(kernel32, ExitProcess, 4) { proc::exit_process(arg(c, 0)); }
HLE_EXPORT(kernel32, TerminateProcess, 8) {
    if (arg(c, 0) == kPseudoProcess) proc::exit_process(arg(c, 1));
    ret(c, 0);
}
HLE_EXPORT(kernel32, ExitThread, 4) { proc::exit_current_thread(c, arg(c, 0)); }

HLE_EXPORT(kernel32, CreateThread, 24) {
    uint32_t stack = arg(c, 1), start = arg(c, 2), param = arg(c, 3), flags = arg(c, 4), ptid = arg(c, 5);
    uint32_t tid = 0;
    uint32_t h = proc::create_thread(start, param, stack, (flags & 4) != 0, &tid);
    if (ptid) mem::wr32(ptid, tid);
    LOGI("CreateThread(start=%08x, param=%08x, flags=%x) -> %08x tid %x", start, param, flags, h, tid);
    ret(c, h);
}
HLE_EXPORT(kernel32, ResumeThread, 4) {
    proc::Thread* t = thread_from_handle(arg(c, 0));
    ret(c, t ? proc::resume_thread(t) : 0xFFFFFFFFu);
}
HLE_EXPORT(kernel32, SuspendThread, 4) {
    proc::Thread* t = thread_from_handle(arg(c, 0));
    ret(c, t ? proc::suspend_thread(t) : 0xFFFFFFFFu);
}
HLE_EXPORT(kernel32, TerminateThread, 8) {
    LOG_ONCE("TerminateThread is not supported; ignored");
    ret(c, 1);
}
HLE_EXPORT(kernel32, GetExitCodeThread, 8) {
    proc::Thread* t = thread_from_handle(arg(c, 0));
    if (!t) return ret(c, 0);
    mem::wr32(arg(c, 1), t->exited ? t->exit_code : 0x103);
    ret(c, 1);
}
HLE_EXPORT(kernel32, SetThreadPriority, 8) {
    if (proc::Thread* t = thread_from_handle(arg(c, 0))) t->priority = (int32_t)arg(c, 1);
    ret(c, 1);
}
HLE_EXPORT(kernel32, GetThreadPriority, 4) {
    proc::Thread* t = thread_from_handle(arg(c, 0));
    ret(c, t ? (uint32_t)t->priority : 0);
}
HLE_EXPORT(kernel32, SetPriorityClass, 8) { ret(c, 1); }
HLE_EXPORT(kernel32, GetPriorityClass, 4) { ret(c, 0x20); }  // NORMAL_PRIORITY_CLASS
HLE_EXPORT(kernel32, SetProcessAffinityMask, 8) { ret(c, 1); }
HLE_EXPORT(kernel32, GetProcessAffinityMask, 12) {
    mem::wr32(arg(c, 1), 1);
    mem::wr32(arg(c, 2), 1);
    ret(c, 1);
}

HLE_EXPORT(kernel32, DuplicateHandle, 28) {
    uint32_t src = arg(c, 1), out = arg(c, 3);
    uint32_t h;
    if (src == kPseudoThread) {
        proc::Thread* t = proc::current_thread();
        t->refs.fetch_add(1);
        h = kobj::add(t);
    } else {
        h = kobj::duplicate(src);
    }
    if (out) mem::wr32(out, h);
    ret(c, h ? 1 : 0);
}

HLE_EXPORT(kernel32, GetSystemInfo, 4) {
    uint32_t p = arg(c, 0);
    memset(mem::ptr(p), 0, 36);
    mem::wr16(p + 0, 0);           // PROCESSOR_ARCHITECTURE_INTEL
    mem::wr32(p + 4, 0x1000);      // dwPageSize
    mem::wr32(p + 8, 0x10000);
    mem::wr32(p + 12, 0x7FFEFFFF);
    mem::wr32(p + 16, 1);          // active processor mask
    mem::wr32(p + 20, 1);          // number of processors
    mem::wr32(p + 24, 586);
    mem::wr32(p + 28, 0x10000);    // allocation granularity
    mem::wr16(p + 32, 6);
    mem::wr16(p + 34, 0x0806);
}
HLE_EXPORT(kernel32, IsProcessorFeaturePresent, 4) {
    uint32_t f = arg(c, 0);
    // PF_RDTSC_INSTRUCTION_AVAILABLE (8), PF_COMPARE_EXCHANGE_DOUBLE (2); no MMX/SSE/3DNow!.
    ret(c, (f == 8 || f == 2) ? 1 : 0);
}
HLE_EXPORT(kernel32, GetVersionExA, 4) {
    uint32_t p = arg(c, 0);
    uint32_t sz = mem::rd32(p);
    memset(mem::ptr(p + 4), 0, sz > 4 ? sz - 4 : 0);
    mem::wr32(p + 4, 5);
    mem::wr32(p + 8, 1);
    mem::wr32(p + 12, 2600);
    mem::wr32(p + 16, 2);  // VER_PLATFORM_WIN32_NT
    mem::write_cstr(p + 20, "Service Pack 2", 128);
    ret(c, 1);
}

HLE_EXPORT(kernel32, IsDebuggerPresent, 0) { ret(c, 0); }
HLE_EXPORT(kernel32, DebugBreak, 0) { LOGW("DebugBreak() from %08x", mem::rd32(c.r[cpu::ESP])); }
HLE_EXPORT(kernel32, OutputDebugStringA, 4) { LOGD("ODS: %s", gstr(arg(c, 0)).c_str()); }
HLE_EXPORT(kernel32, SetErrorMode, 4) { ret(c, 0); }
HLE_EXPORT(kernel32, SetUnhandledExceptionFilter, 4) {
    uint32_t prev = g_unhandled_filter;
    g_unhandled_filter = arg(c, 0);
    ret(c, prev);
}
HLE_EXPORT(kernel32, UnhandledExceptionFilter, 4) {
    LOGE("UnhandledExceptionFilter called");
    ret(c, 1);
}
HLE_EXPORT(kernel32, RaiseException, 16) {
    uint32_t code = arg(c, 0), flags = arg(c, 1), nargs = arg(c, 2), args = arg(c, 3);
    LOGE("RaiseException(code=%08x flags=%x nargs=%u args=%08x) from %08x — SEH dispatch not yet implemented",
         code, flags, nargs, args, mem::rd32(c.r[cpu::ESP]));
    proc::set_status("RaiseException " + std::to_string(code));
    c.stop = cpu::Stop::Fault;
}
HLE_EXPORT(kernel32, RtlUnwind, 16) {
    // Pop SEH frames up to TargetFrame without calling unwind handlers.
    uint32_t target = arg(c, 0);
    LOG_ONCE("RtlUnwind: unwind handlers are not invoked");
    if (target) mem::wr32(c.fs_base, target);
    ret(c, arg(c, 3));
}

HLE_EXPORT(kernel32, CreateToolhelp32Snapshot, 8) { ret(c, kobj::add(new kobj::Object(kobj::Type::Other))); }
HLE_EXPORT(kernel32, Process32First, 8) {
    set_last_error(win::ERROR_NO_MORE_FILES);
    ret(c, 0);
}
HLE_EXPORT(kernel32, Process32Next, 8) {
    set_last_error(win::ERROR_NO_MORE_FILES);
    ret(c, 0);
}

HLE_EXPORT(kernel32, Sleep, 4) {
    uint32_t ms = arg(c, 0);
    if (ms == 0)
        sched_yield();
    else
        usleep(ms * 1000);
}
HLE_EXPORT(kernel32, SleepEx, 8) {
    uint32_t ms = arg(c, 0);
    if (ms == 0)
        sched_yield();
    else if (ms != win::INFINITE)
        usleep(ms * 1000);
    ret(c, 0);
}

// ---------------------------------------------------------------- environment / CRT support
HLE_EXPORT(kernel32, GetEnvironmentStrings, 0) {
    if (!g_env_a) {
        g_env_a = mem::alloc_system(sizeof kEnvA);
        memcpy(mem::ptr(g_env_a), kEnvA, sizeof kEnvA);
    }
    ret(c, g_env_a);
}
HLE_EXPORT(kernel32, GetEnvironmentStringsW, 0) {
    if (!g_env_w) {
        g_env_w = mem::alloc_system(sizeof kEnvA * 2);
        for (size_t i = 0; i < sizeof kEnvA; ++i) mem::wr16(g_env_w + (uint32_t)i * 2, (uint8_t)kEnvA[i]);
    }
    ret(c, g_env_w);
}
HLE_EXPORT(kernel32, FreeEnvironmentStringsA, 4) { ret(c, 1); }
HLE_EXPORT(kernel32, FreeEnvironmentStringsW, 4) { ret(c, 1); }
HLE_EXPORT(kernel32, SetEnvironmentVariableA, 8) { ret(c, 1); }
HLE_EXPORT(kernel32, SetHandleCount, 4) { ret(c, arg(c, 0)); }
HLE_EXPORT(kernel32, GetStdHandle, 4) {
    uint32_t which = arg(c, 0);
    ret(c, which == (uint32_t)-10 ? kStdIn : which == (uint32_t)-11 ? kStdOut : kStdErr);
}
HLE_EXPORT(kernel32, SetStdHandle, 8) { ret(c, 1); }

// ---------------------------------------------------------------- locale
HLE_EXPORT(kernel32, GetACP, 0) { ret(c, 1252); }
HLE_EXPORT(kernel32, GetOEMCP, 0) { ret(c, 437); }
HLE_EXPORT(kernel32, IsValidCodePage, 4) { ret(c, (arg(c, 0) == 1252 || arg(c, 0) == 437) ? 1 : 0); }
HLE_EXPORT(kernel32, IsValidLocale, 8) { ret(c, 1); }
HLE_EXPORT(kernel32, EnumSystemLocalesA, 8) { ret(c, 1); }
HLE_EXPORT(kernel32, GetUserDefaultLCID, 0) { ret(c, 0x0409); }
HLE_EXPORT(kernel32, GetCPInfo, 8) {
    uint32_t p = arg(c, 1);
    memset(mem::ptr(p), 0, 20);
    mem::wr32(p, 1);       // MaxCharSize
    mem::wr8(p + 4, '?');  // DefaultChar
    ret(c, 1);
}

namespace {
std::string locale_info(uint32_t type) {
    switch (type & 0xFFFF) {
        case 0x0001: return "0409";
        case 0x0002: return "English (United States)";
        case 0x0003: return "ENU";
        case 0x0007: return "USA";
        case 0x000B: return "437";
        case 0x1001: return "English";
        case 0x1002: return "United States";
        case 0x1004: return "1252";
        case 0x000E: return ".";
        case 0x000F: return ",";
        case 0x0059: return "en";
        case 0x005A: return "US";
        default: return "";
    }
}
}  // namespace

HLE_EXPORT(kernel32, GetLocaleInfoA, 16) {
    std::string s = locale_info(arg(c, 1));
    uint32_t buf = arg(c, 2), cap = arg(c, 3);
    if (s.empty()) return ret(c, 0);
    if (cap == 0) return ret(c, (uint32_t)s.size() + 1);
    winu::put_str(buf, cap, s);
    ret(c, std::min<uint32_t>((uint32_t)s.size() + 1, cap));
}
HLE_EXPORT(kernel32, GetLocaleInfoW, 16) {
    std::string s = locale_info(arg(c, 1));
    uint32_t buf = arg(c, 2), cap = arg(c, 3);
    if (s.empty()) return ret(c, 0);
    if (cap == 0) return ret(c, (uint32_t)s.size() + 1);
    uint32_t n = std::min<uint32_t>((uint32_t)s.size(), cap - 1);
    for (uint32_t i = 0; i < n; ++i) mem::wr16(buf + i * 2, (uint8_t)s[i]);
    mem::wr16(buf + n * 2, 0);
    ret(c, n + 1);
}

namespace {
uint16_t ctype1(uint32_t ch) {
    uint16_t t = 0;
    if (ch < 0x80) {
        if (isupper((int)ch)) t |= 0x1;
        if (islower((int)ch)) t |= 0x2;
        if (isdigit((int)ch)) t |= 0x4;
        if (isspace((int)ch)) t |= 0x8;
        if (ispunct((int)ch)) t |= 0x10;
        if (iscntrl((int)ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit((int)ch)) t |= 0x80;
        if (isalpha((int)ch)) t |= 0x100;
    } else if (ch >= 0xC0 && ch <= 0xFF && ch != 0xD7 && ch != 0xF7) {
        t |= 0x100 | (ch < 0xDF ? 0x1 : 0x2);
    }
    return t;
}
}  // namespace

HLE_EXPORT(kernel32, GetStringTypeA, 20) {
    // (LCID, dwInfoType, lpSrcStr, cchSrc, lpCharType)
    uint32_t src = arg(c, 2), out = arg(c, 4);
    int32_t n = (int32_t)arg(c, 3);
    if (n < 0) n = (int32_t)mem::strlen_g(src) + 1;
    for (int32_t i = 0; i < n; ++i) mem::wr16(out + i * 2, ctype1(mem::rd8(src + i)));
    ret(c, 1);
}
HLE_EXPORT(kernel32, GetStringTypeW, 16) {
    // (dwInfoType, lpSrcStr, cchSrc, lpCharType)
    uint32_t src = arg(c, 1), out = arg(c, 3);
    int32_t n = (int32_t)arg(c, 2);
    if (n < 0) {
        n = 0;
        while (mem::rd16(src + n * 2)) ++n;
        ++n;
    }
    for (int32_t i = 0; i < n; ++i) mem::wr16(out + i * 2, ctype1(mem::rd16(src + i * 2)));
    ret(c, 1);
}

namespace {
uint32_t map_char(uint32_t ch, uint32_t flags) {
    if ((flags & 0x100) && ch < 0x100) return (uint32_t)tolower((int)ch);   // LCMAP_LOWERCASE
    if ((flags & 0x200) && ch < 0x100) return (uint32_t)toupper((int)ch);   // LCMAP_UPPERCASE
    return ch;
}
}  // namespace

HLE_EXPORT(kernel32, LCMapStringA, 24) {
    uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    if (n < 0) n = (int32_t)mem::strlen_g(src) + 1;
    if (cap == 0) return ret(c, (uint32_t)n);
    if ((uint32_t)n > cap) return ret(c, 0);
    for (int32_t i = 0; i < n; ++i) mem::wr8(dst + i, (uint8_t)map_char(mem::rd8(src + i), flags));
    ret(c, (uint32_t)n);
}
HLE_EXPORT(kernel32, LCMapStringW, 24) {
    uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    if (n < 0) {
        n = 0;
        while (mem::rd16(src + n * 2)) ++n;
        ++n;
    }
    if (cap == 0) return ret(c, (uint32_t)n);
    if ((uint32_t)n > cap) return ret(c, 0);
    for (int32_t i = 0; i < n; ++i) mem::wr16(dst + i * 2, (uint16_t)map_char(mem::rd16(src + i * 2), flags));
    ret(c, (uint32_t)n);
}

namespace {
uint32_t compare_strings(uint32_t flags, const std::u16string& a, const std::u16string& b) {
    bool icase = flags & 1;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        uint32_t x = a[i], y = b[i];
        if (icase && x < 0x100) x = (uint32_t)tolower((int)x);
        if (icase && y < 0x100) y = (uint32_t)tolower((int)y);
        if (x != y) return x < y ? 1 : 3;
    }
    return a.size() == b.size() ? 2 : (a.size() < b.size() ? 1 : 3);
}
std::u16string read_a(uint32_t p, int32_t n) {
    std::u16string s;
    if (n < 0) n = (int32_t)mem::strlen_g(p);
    for (int32_t i = 0; i < n; ++i) s += (char16_t)mem::rd8(p + i);
    return s;
}
std::u16string read_w(uint32_t p, int32_t n) {
    std::u16string s;
    if (n < 0) {
        for (uint16_t ch; (ch = mem::rd16(p)) != 0; p += 2) s += (char16_t)ch;
        return s;
    }
    for (int32_t i = 0; i < n; ++i) s += (char16_t)mem::rd16(p + i * 2);
    return s;
}
}  // namespace

HLE_EXPORT(kernel32, CompareStringA, 24) {
    ret(c, compare_strings(arg(c, 1), read_a(arg(c, 2), (int32_t)arg(c, 3)), read_a(arg(c, 4), (int32_t)arg(c, 5))));
}
HLE_EXPORT(kernel32, CompareStringW, 24) {
    ret(c, compare_strings(arg(c, 1), read_w(arg(c, 2), (int32_t)arg(c, 3)), read_w(arg(c, 4), (int32_t)arg(c, 5))));
}
HLE_EXPORT(kernel32, lstrcmpiA, 8) {
    int r = strcasecmp(mem::cstr(arg(c, 0)), mem::cstr(arg(c, 1)));
    ret(c, (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0));
}

HLE_EXPORT(kernel32, MultiByteToWideChar, 24) {
    // (CodePage, dwFlags, lpMultiByteStr, cbMultiByte, lpWideCharStr, cchWideChar)
    uint32_t src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = (int32_t)arg(c, 3);
    if (n < 0) n = (int32_t)mem::strlen_g(src) + 1;
    if (cap == 0) return ret(c, (uint32_t)n);
    if ((uint32_t)n > cap) {
        set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
        return ret(c, 0);
    }
    for (int32_t i = 0; i < n; ++i) mem::wr16(dst + i * 2, mem::rd8(src + i));
    ret(c, (uint32_t)n);
}
HLE_EXPORT(kernel32, WideCharToMultiByte, 32) {
    // (CodePage, dwFlags, lpWideCharStr, cchWideChar, lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar)
    uint32_t src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5), used = arg(c, 7);
    int32_t n = (int32_t)arg(c, 3);
    if (n < 0) {
        n = 0;
        while (mem::rd16(src + n * 2)) ++n;
        ++n;
    }
    bool def = false;
    if (cap == 0) return ret(c, (uint32_t)n);
    if ((uint32_t)n > cap) {
        set_last_error(122);
        return ret(c, 0);
    }
    for (int32_t i = 0; i < n; ++i) {
        uint16_t ch = mem::rd16(src + i * 2);
        if (ch > 0xFF) {
            ch = '?';
            def = true;
        }
        mem::wr8(dst + i, (uint8_t)ch);
    }
    if (used) mem::wr32(used, def ? 1 : 0);
    ret(c, (uint32_t)n);
}

// ---------------------------------------------------------------- memory
HLE_EXPORT(kernel32, VirtualAlloc, 16) {
    uint32_t addr = arg(c, 0), size = arg(c, 1), type = arg(c, 2);
    uint32_t r = 0;
    if (type & 0x2000) {  // MEM_RESERVE (optionally with MEM_COMMIT)
        r = mem::reserve(addr, size, (type & 0x1000) != 0);
    } else if (type & 0x1000) {  // MEM_COMMIT inside an existing reservation
        if (!addr) {
            r = mem::reserve(0, size, true);
        } else if (mem::commit(addr, size)) {
            r = addr & ~(mem::kPage - 1);
        }
    }
    if (!r) set_last_error(win::ERROR_NOT_ENOUGH_MEMORY);
    if (trace_enabled()) LOGD("VirtualAlloc(%08x, %x, %x) -> %08x", addr, size, type, r);
    ret(c, r);
}
HLE_EXPORT(kernel32, VirtualFree, 12) {
    uint32_t addr = arg(c, 0), size = arg(c, 1), type = arg(c, 2);
    bool ok = (type & 0x8000) ? mem::release(addr) : mem::decommit(addr, size);
    ret(c, ok ? 1 : 0);
}
HLE_EXPORT(kernel32, VirtualQuery, 12) {
    uint32_t addr = arg(c, 0), out = arg(c, 1);
    mem::RegionInfo ri = mem::query(addr);
    const loader::Image& img = loader::main_image();
    bool image = ri.alloc_base == img.base && img.base;
    mem::wr32(out + 0, ri.base);
    mem::wr32(out + 4, ri.alloc_base);
    mem::wr32(out + 8, ri.reserved ? (image ? 0x80 : 0x04) : 0);
    mem::wr32(out + 12, ri.size);
    mem::wr32(out + 16, !ri.reserved ? 0x10000 : ri.committed ? 0x1000 : 0x2000);
    mem::wr32(out + 20, ri.committed ? (image ? 0x40 : 0x04) : (ri.reserved ? 0 : 0x01));
    mem::wr32(out + 24, !ri.reserved ? 0 : image ? 0x1000000 : 0x20000);
    ret(c, 28);
}
HLE_EXPORT(kernel32, VirtualProtect, 16) {
    uint32_t addr = arg(c, 0), size = arg(c, 1), prot = arg(c, 2), old = arg(c, 3);
    const loader::Image& img = loader::main_image();
    if (addr >= img.text_start && addr < img.text_end)
        LOGW("VirtualProtect on code %08x+%x prot %x (self-modifying code?)", addr, size, prot);
    if (old) mem::wr32(old, 0x40);
    ret(c, 1);
}

HLE_EXPORT(kernel32, HeapCreate, 12) {
    static std::atomic<uint32_t> next{0x00160000};
    ret(c, next.fetch_add(0x10000));
}
HLE_EXPORT(kernel32, HeapDestroy, 4) { ret(c, 1); }
HLE_EXPORT(kernel32, GetProcessHeap, 0) { ret(c, proc::process_heap_handle()); }
HLE_EXPORT(kernel32, HeapAlloc, 12) { ret(c, gheap::alloc(arg(c, 2), (arg(c, 1) & 0x8) != 0)); }
HLE_EXPORT(kernel32, HeapFree, 12) { ret(c, gheap::free(arg(c, 2)) ? 1 : 0); }
HLE_EXPORT(kernel32, HeapSize, 12) { ret(c, gheap::size(arg(c, 2))); }
HLE_EXPORT(kernel32, HeapReAlloc, 16) {
    uint32_t flags = arg(c, 1);
    ret(c, gheap::realloc(arg(c, 2), arg(c, 3), (flags & 0x8) != 0, (flags & 0x10) != 0));
}
HLE_EXPORT(kernel32, GlobalAlloc, 8) { ret(c, gheap::alloc(arg(c, 1), (arg(c, 0) & 0x40) != 0)); }
HLE_EXPORT(kernel32, GlobalFree, 4) { ret(c, gheap::free(arg(c, 0)) ? 0 : arg(c, 0)); }

HLE_EXPORT(kernel32, IsBadReadPtr, 8) { ret(c, mem::is_committed(arg(c, 0), std::max<uint32_t>(arg(c, 1), 1)) ? 0 : 1); }
HLE_EXPORT(kernel32, IsBadWritePtr, 8) { ret(c, mem::is_committed(arg(c, 0), std::max<uint32_t>(arg(c, 1), 1)) ? 0 : 1); }
HLE_EXPORT(kernel32, IsBadCodePtr, 4) { ret(c, mem::is_committed(arg(c, 0), 1) ? 0 : 1); }

// ---------------------------------------------------------------- TLS
HLE_EXPORT(kernel32, TlsAlloc, 0) {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    for (uint32_t i = 0; i < 64; ++i) {
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            return ret(c, i);
        }
    }
    ret(c, 0xFFFFFFFFu);
}
HLE_EXPORT(kernel32, TlsFree, 4) {
    std::lock_guard<std::mutex> lk(g_tls_mu);
    if (arg(c, 0) < 64) g_tls_used &= ~(1ull << arg(c, 0));
    ret(c, 1);
}
HLE_EXPORT(kernel32, TlsGetValue, 4) {
    uint32_t i = arg(c, 0);
    set_last_error(0);
    ret(c, i < 64 ? mem::rd32(c.fs_base + 0xE10 + i * 4) : 0);
}
HLE_EXPORT(kernel32, TlsSetValue, 8) {
    uint32_t i = arg(c, 0);
    if (i >= 64) return ret(c, 0);
    mem::wr32(c.fs_base + 0xE10 + i * 4, arg(c, 1));
    ret(c, 1);
}

// ---------------------------------------------------------------- synchronization
HLE_EXPORT(kernel32, InitializeCriticalSection, 4) {
    uint32_t cs = arg(c, 0);
    memset(mem::ptr(cs), 0, 24);
    mem::wr32(cs + 4, 0xFFFFFFFFu);  // LockCount
    cs_for(cs);
}
HLE_EXPORT(kernel32, DeleteCriticalSection, 4) {
    std::lock_guard<std::mutex> lk(g_cs_mu);
    auto it = g_cs.find(arg(c, 0));
    if (it != g_cs.end()) {
        delete it->second;
        g_cs.erase(it);
    }
}
HLE_EXPORT(kernel32, EnterCriticalSection, 4) {
    uint32_t cs = arg(c, 0);
    cs_for(cs)->lock();
    mem::wr32(cs + 8, mem::rd32(cs + 8) + 1);         // RecursionCount
    mem::wr32(cs + 12, proc::current_tid());          // OwningThread
}
HLE_EXPORT(kernel32, LeaveCriticalSection, 4) {
    uint32_t cs = arg(c, 0);
    uint32_t rc = mem::rd32(cs + 8);
    if (rc) mem::wr32(cs + 8, rc - 1);
    if (rc <= 1) mem::wr32(cs + 12, 0);
    cs_for(cs)->unlock();
}

HLE_EXPORT(kernel32, InterlockedExchange, 8) {
    cpu::bus_lock();
    uint32_t old = mem::rd32(arg(c, 0));
    mem::wr32(arg(c, 0), arg(c, 1));
    cpu::bus_unlock();
    ret(c, old);
}
HLE_EXPORT(kernel32, InterlockedCompareExchange, 12) {
    cpu::bus_lock();
    uint32_t old = mem::rd32(arg(c, 0));
    if (old == arg(c, 2)) mem::wr32(arg(c, 0), arg(c, 1));
    cpu::bus_unlock();
    ret(c, old);
}

HLE_EXPORT(kernel32, CreateEventA, 16) {
    uint32_t manual = arg(c, 1), initial = arg(c, 2);
    std::string name = gstr(arg(c, 3));
    if (!name.empty()) {
        if (kobj::Object* o = kobj::find_named(name)) {
            set_last_error(win::ERROR_ALREADY_EXISTS);
            return ret(c, kobj::add(o));
        }
    }
    auto* e = new kobj::Event(manual != 0, initial != 0);
    if (!name.empty()) kobj::set_name(e, name);
    set_last_error(0);
    ret(c, kobj::add(e));
}
HLE_EXPORT(kernel32, SetEvent, 4) {
    auto* e = kobj::get_as<kobj::Event>(arg(c, 0), kobj::Type::Event);
    if (!e) return ret(c, 0);
    {
        std::lock_guard<std::mutex> lk(kobj::wait_mutex());
        e->state = true;
    }
    kobj::notify();
    ret(c, 1);
}
HLE_EXPORT(kernel32, ResetEvent, 4) {
    auto* e = kobj::get_as<kobj::Event>(arg(c, 0), kobj::Type::Event);
    if (!e) return ret(c, 0);
    std::lock_guard<std::mutex> lk(kobj::wait_mutex());
    e->state = false;
    ret(c, 1);
}
HLE_EXPORT(kernel32, CreateMutexA, 12) {
    uint32_t owned = arg(c, 1);
    std::string name = gstr(arg(c, 2));
    if (!name.empty()) {
        if (kobj::Object* o = kobj::find_named(name)) {
            set_last_error(win::ERROR_ALREADY_EXISTS);
            return ret(c, kobj::add(o));
        }
    }
    auto* m = new kobj::Mutex(owned ? proc::current_tid() : 0);
    if (!name.empty()) kobj::set_name(m, name);
    set_last_error(0);
    ret(c, kobj::add(m));
}
HLE_EXPORT(kernel32, ReleaseMutex, 4) {
    auto* m = kobj::get_as<kobj::Mutex>(arg(c, 0), kobj::Type::Mutex);
    if (!m) return ret(c, 0);
    {
        std::lock_guard<std::mutex> lk(kobj::wait_mutex());
        if (m->owner != proc::current_tid() || m->count == 0) return ret(c, 0);
        if (--m->count == 0) m->owner = 0;
    }
    kobj::notify();
    ret(c, 1);
}
HLE_EXPORT(kernel32, WaitForSingleObject, 8) {
    uint32_t h = arg(c, 0);
    if (h == kPseudoThread || h == kPseudoProcess) return ret(c, win::WAIT_TIMEOUT);
    ret(c, kobj::wait(&h, 1, false, arg(c, 1), proc::current_tid()));
}
HLE_EXPORT(kernel32, WaitForMultipleObjects, 16) {
    uint32_t n = arg(c, 0);
    uint32_t hs[64];
    for (uint32_t i = 0; i < n && i < 64; ++i) hs[i] = mem::rd32(arg(c, 1) + i * 4);
    ret(c, kobj::wait(hs, n, arg(c, 2) != 0, arg(c, 3), proc::current_tid()));
}
HLE_EXPORT(kernel32, CloseHandle, 4) {
    uint32_t h = arg(c, 0);
    if (h == kPseudoProcess || h == kPseudoThread || h == kStdIn || h == kStdOut || h == kStdErr)
        return ret(c, 1);
    bool ok = kobj::close(h);
    if (!ok) set_last_error(win::ERROR_INVALID_HANDLE);
    ret(c, ok ? 1 : 0);
}

// ---------------------------------------------------------------- time
HLE_EXPORT(kernel32, GetTickCount, 0) { ret(c, (uint32_t)(winu::mono_ns() / 1000000ull)); }
HLE_EXPORT(kernel32, QueryPerformanceFrequency, 4) {
    mem::wr<uint64_t>(arg(c, 0), 10000000ull);
    ret(c, 1);
}
HLE_EXPORT(kernel32, QueryPerformanceCounter, 4) {
    mem::wr<uint64_t>(arg(c, 0), winu::mono_ns() / 100);
    ret(c, 1);
}
HLE_EXPORT(kernel32, GetSystemTimeAsFileTime, 4) { mem::wr<uint64_t>(arg(c, 0), winu::filetime_now()); }
HLE_EXPORT(kernel32, GetTimeZoneInformation, 4) {
    memset(mem::ptr(arg(c, 0)), 0, 172);
    ret(c, 0);  // TIME_ZONE_ID_UNKNOWN: report UTC
}
HLE_EXPORT(kernel32, FileTimeToLocalFileTime, 8) {
    mem::wr<uint64_t>(arg(c, 1), mem::rd<uint64_t>(arg(c, 0)));
    ret(c, 1);
}
HLE_EXPORT(kernel32, FileTimeToSystemTime, 8) {
    uint64_t ft = mem::rd<uint64_t>(arg(c, 0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint32_t st = arg(c, 1);
    mem::wr16(st + 0, (uint16_t)(tmv.tm_year + 1900));
    mem::wr16(st + 2, (uint16_t)(tmv.tm_mon + 1));
    mem::wr16(st + 4, (uint16_t)tmv.tm_wday);
    mem::wr16(st + 6, (uint16_t)tmv.tm_mday);
    mem::wr16(st + 8, (uint16_t)tmv.tm_hour);
    mem::wr16(st + 10, (uint16_t)tmv.tm_min);
    mem::wr16(st + 12, (uint16_t)tmv.tm_sec);
    mem::wr16(st + 14, (uint16_t)((ft / 10000ull) % 1000));
    ret(c, 1);
}

// ---------------------------------------------------------------- serial ports (modem play: absent)
#define NO_COMM(name, bytes)                     \
    HLE_EXPORT(kernel32, name, bytes) {          \
        set_last_error(win::ERROR_INVALID_HANDLE); \
        ret(c, 0);                               \
    }
NO_COMM(WaitCommEvent, 12)
NO_COMM(SetCommMask, 8)
NO_COMM(PurgeComm, 8)
NO_COMM(SetCommState, 8)
NO_COMM(GetCommState, 8)
NO_COMM(SetCommConfig, 12)
NO_COMM(GetCommConfig, 12)
NO_COMM(SetCommTimeouts, 8)
NO_COMM(SetupComm, 12)
