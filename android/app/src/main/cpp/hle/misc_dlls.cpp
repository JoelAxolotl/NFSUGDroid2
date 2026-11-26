// gdi32, winmm, shell32, shfolder, ws2_32, tapi32, netapi32.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

#include "hle/hle.h"
#include "hle/kobjects.h"
#include "hle/win_util.h"
#include "runtime/process.h"
#include "runtime/vfs.h"

using namespace hle;
using winu::gstr;

// ---------------------------------------------------------------- gdi32
// The game only uses GDI for fallback text/bitmap work; rendering goes through D3D9.
namespace {
std::atomic<uint32_t> g_gdi_next{0x00040000};
uint32_t gdi_handle() { return g_gdi_next.fetch_add(4); }
}  // namespace
HLE_EXPORT(gdi32, CreateCompatibleDC, 4) { ret(c, gdi_handle()); }
HLE_EXPORT(gdi32, CreateFontA, 56) { ret(c, gdi_handle()); }
HLE_EXPORT(gdi32, CreateBitmap, 20) { ret(c, gdi_handle()); }
HLE_EXPORT(gdi32, SelectObject, 8) { ret(c, gdi_handle()); }
HLE_EXPORT(gdi32, DeleteObject, 4) { ret(c, 1); }
HLE_EXPORT(gdi32, DeleteDC, 4) { ret(c, 1); }
HLE_EXPORT(gdi32, SetTextColor, 8) { ret(c, 0); }
HLE_EXPORT(gdi32, SetBkColor, 8) { ret(c, 0); }
HLE_EXPORT(gdi32, SetBkMode, 8) { ret(c, 1); }
HLE_EXPORT(gdi32, ExtTextOutA, 32) {
    LOG_ONCE("gdi32 text output is not rendered");
    ret(c, 1);
}
HLE_EXPORT(gdi32, GetPixel, 12) { ret(c, 0); }
HLE_EXPORT(gdi32, BitBlt, 36) { ret(c, 1); }

// ---------------------------------------------------------------- winmm
// All multimedia timers are serviced by one helper thread (as winmm does), so
// callbacks never run concurrently with each other and re-arming a one-shot
// timer from inside its callback is cheap.
namespace {
struct Timer {
    uint32_t delay_ms, callback, user, flags;
    uint64_t due_ns;
};
std::mutex g_timer_mu;
std::condition_variable g_timer_cv;
std::map<uint32_t, Timer> g_timers;
uint32_t g_timer_next = 1;
bool g_timer_thread = false;

void fire(cpu::Cpu& tc, uint32_t id, const Timer& t) {
    if (t.flags & 0x10) {  // TIME_CALLBACK_EVENT_SET: callback is an event handle
        if (auto* e = kobj::get_as<kobj::Event>(t.callback, kobj::Type::Event)) {
            {
                std::lock_guard<std::mutex> lk(kobj::wait_mutex());
                e->state = true;
            }
            kobj::notify();
        }
    } else {
        call_guest(tc, t.callback, {id, 0, t.user, 0, 0});
    }
}

void timer_thread(cpu::Cpu& tc) {
    std::unique_lock<std::mutex> lk(g_timer_mu);
    for (;;) {
        if (g_timers.empty()) {
            g_timer_cv.wait(lk);
            continue;
        }
        auto next = g_timers.begin();
        for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
            if (it->second.due_ns < next->second.due_ns) next = it;
        uint64_t now = winu::mono_ns();
        if (next->second.due_ns > now) {
            g_timer_cv.wait_for(lk, std::chrono::nanoseconds(next->second.due_ns - now));
            continue;
        }
        uint32_t id = next->first;
        Timer t = next->second;
        if (t.flags & 1) {
            next->second.due_ns += (uint64_t)t.delay_ms * 1000000ull;
            if (next->second.due_ns < now) next->second.due_ns = now;  // don't spiral after a stall
        } else {
            g_timers.erase(next);
        }
        lk.unlock();
        fire(tc, id, t);
        lk.lock();
        if (tc.stop != cpu::Stop::None) {
            LOGE("winmm timer callback stopped the timer thread");
            return;
        }
    }
}
}  // namespace

HLE_EXPORT(winmm, timeGetTime, 0) { ret(c, (uint32_t)(winu::mono_ns() / 1000000ull)); }
HLE_EXPORT(winmm, timeBeginPeriod, 4) { ret(c, 0); }
HLE_EXPORT(winmm, timeEndPeriod, 4) { ret(c, 0); }
HLE_EXPORT(winmm, timeGetDevCaps, 8) {
    mem::wr32(arg(c, 0), 1);
    mem::wr32(arg(c, 0) + 4, 1000000);
    ret(c, 0);
}
HLE_EXPORT(winmm, timeSetEvent, 20) {
    uint32_t delay = arg(c, 0), cb = arg(c, 2), user = arg(c, 3), flags = arg(c, 4);
    if (!delay) delay = 1;
    uint32_t id;
    bool start;
    {
        std::lock_guard<std::mutex> lk(g_timer_mu);
        id = g_timer_next++;
        g_timers[id] = Timer{delay, cb, user, flags, winu::mono_ns() + (uint64_t)delay * 1000000ull};
        start = !g_timer_thread;
        g_timer_thread = true;
    }
    g_timer_cv.notify_all();
    if (start) proc::spawn_helper("mmtimer", timer_thread);
    ret(c, id);
}
HLE_EXPORT(winmm, timeKillEvent, 4) {
    std::lock_guard<std::mutex> lk(g_timer_mu);
    if (!g_timers.erase(arg(c, 0))) return ret(c, 97);  // MMSYSERR_INVALPARAM
    ret(c, 0);
}

// ---------------------------------------------------------------- shell32 / shfolder
HLE_EXPORT(shell32, ShellExecuteA, 24) {
    LOGI("ShellExecuteA(%s, %s) ignored", gstr(arg(c, 1)).c_str(), gstr(arg(c, 2)).c_str());
    ret(c, 33);
}
HLE_EXPORT(shfolder, SHGetFolderPathA, 20) {
    uint32_t csidl = arg(c, 1) & 0xFF, out = arg(c, 4);
    std::string p;
    std::string prof = vfs::kUserProfileWin;
    switch (csidl) {
        case 0x05: p = prof + "\\Documents"; break;              // CSIDL_PERSONAL
        case 0x1A: p = prof + "\\AppData\\Roaming"; break;       // CSIDL_APPDATA
        case 0x1C: p = prof + "\\AppData\\Local"; break;         // CSIDL_LOCAL_APPDATA
        case 0x23: p = "C:\\ProgramData"; break;                 // CSIDL_COMMON_APPDATA
        case 0x2E: p = "C:\\Users\\Public\\Documents"; break;    // CSIDL_COMMON_DOCUMENTS
        default: p = prof; break;
    }
    // Create the folder chain on the host so later CreateFile/CreateDirectory work.
    std::string host = vfs::to_host(p);
    for (size_t i = 1; i <= host.size(); ++i)
        if (i == host.size() || host[i] == '/') mkdir(host.substr(0, i).c_str(), 0755);
    mem::write_cstr(out, p.c_str(), 260);
    LOGI("SHGetFolderPathA(csidl=%x) -> %s", arg(c, 1), p.c_str());
    ret(c, 0);
}

// ---------------------------------------------------------------- ws2_32
// Networking (online / LAN) is not supported yet: Winsock reports no network.
namespace {
constexpr uint32_t WSAENETDOWN = 10050;
thread_local uint32_t t_wsa_error = 0;
}  // namespace
HLE_EXPORT(ws2_32, WSAStartup, 8) {
    uint32_t d = arg(c, 1);
    memset(mem::ptr(d), 0, 400);
    mem::wr16(d + 0, 0x0202);
    mem::wr16(d + 2, 0x0202);
    mem::write_cstr(d + 4, "WinSock 2.0", 257);
    mem::write_cstr(d + 261, "Running", 129);
    mem::wr16(d + 390, 64);
    mem::wr16(d + 392, 65507);
    ret(c, 0);
}
HLE_EXPORT(ws2_32, WSACleanup, 0) { ret(c, 0); }
HLE_EXPORT(ws2_32, WSAGetLastError, 0) { ret(c, t_wsa_error); }
HLE_EXPORT(ws2_32, socket, 12) {
    t_wsa_error = WSAENETDOWN;
    ret(c, 0xFFFFFFFFu);
}
#define WS_FAIL(name, bytes)              \
    HLE_EXPORT(ws2_32, name, bytes) {     \
        t_wsa_error = WSAENETDOWN;        \
        ret(c, 0xFFFFFFFFu);              \
    }
WS_FAIL(setsockopt, 20)
WS_FAIL(ioctlsocket, 12)
WS_FAIL(closesocket, 4)
WS_FAIL(shutdown, 8)
WS_FAIL(bind, 12)
WS_FAIL(connect, 12)
WS_FAIL(listen, 8)
WS_FAIL(accept, 12)
WS_FAIL(sendto, 24)
WS_FAIL(send, 16)
WS_FAIL(recvfrom, 24)
WS_FAIL(recv, 16)
WS_FAIL(select, 20)
WS_FAIL(getpeername, 12)
WS_FAIL(getsockname, 12)
WS_FAIL(WSAIoctl, 36)
HLE_EXPORT(ws2_32, gethostbyname, 4) {
    t_wsa_error = 11001;  // WSAHOST_NOT_FOUND
    ret(c, 0);
}
HLE_EXPORT(ws2_32, gethostname, 8) {
    mem::write_cstr(arg(c, 0), "android", arg(c, 1));
    ret(c, 0);
}

// SPEED2.EXE imports Winsock by ordinal.
namespace {
const hle::OrdinalAlias kWsOrdinals[] = {
    {"ws2_32", 1, "accept"},       {"ws2_32", 2, "bind"},          {"ws2_32", 3, "closesocket"},
    {"ws2_32", 4, "connect"},      {"ws2_32", 5, "getpeername"},   {"ws2_32", 6, "getsockname"},
    {"ws2_32", 10, "ioctlsocket"}, {"ws2_32", 13, "listen"},       {"ws2_32", 16, "recv"},
    {"ws2_32", 17, "recvfrom"},    {"ws2_32", 18, "select"},       {"ws2_32", 19, "send"},
    {"ws2_32", 20, "sendto"},      {"ws2_32", 21, "setsockopt"},   {"ws2_32", 22, "shutdown"},
    {"ws2_32", 23, "socket"},      {"ws2_32", 52, "gethostbyname"}, {"ws2_32", 57, "gethostname"},
    {"ws2_32", 111, "WSAGetLastError"}, {"ws2_32", 115, "WSAStartup"}, {"ws2_32", 116, "WSACleanup"},
};
}  // namespace

// ---------------------------------------------------------------- tapi32 (modem play) / netapi32
#define TAPI_FAIL(name, bytes)             \
    HLE_EXPORT(tapi32, name, bytes) { ret(c, 0x80000048u); }  // LINEERR_OPERATIONUNAVAIL
TAPI_FAIL(lineInitialize, 20)
TAPI_FAIL(lineShutdown, 4)
TAPI_FAIL(lineNegotiateAPIVersion, 24)
TAPI_FAIL(lineGetDevCaps, 20)
TAPI_FAIL(lineOpen, 36)
TAPI_FAIL(lineClose, 4)
TAPI_FAIL(lineMakeCall, 20)
TAPI_FAIL(lineAnswer, 12)
TAPI_FAIL(lineGetID, 24)

HLE_EXPORT(netapi32, Netbios, 4) {
    mem::wr8(arg(c, 0) + 1, 0x40);  // ncb_retcode = NRC_SYSTEM
    ret(c, 0x40);
}
