// user32: windows, message queue, cursor, wsprintf.
#include "hle/user32.h"

#include <strings.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

#include "hle/hle.h"
#include "hle/win_util.h"

using namespace hle;
using winu::gstr;

namespace {
constexpr uint32_t WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
                   WM_KILLFOCUS = 0x08, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_SHOWWINDOW = 0x18,
                   WM_ACTIVATEAPP = 0x1C, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_ERASEBKGND = 0x14;

struct Msg {
    uint32_t hwnd, msg, wparam, lparam, time;
};

struct Window {
    uint32_t wndproc = 0;
    uint32_t style = 0, exstyle = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;
    uint32_t userdata = 0;
    std::string cls;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Msg> g_queue;
std::map<std::string, uint32_t> g_classes;  // lowercase class name -> wndproc
std::map<uint32_t, Window> g_windows;
uint32_t g_next_hwnd = 0x00010020;
uint32_t g_main_hwnd = 0;
int32_t g_cursor_count = 0;
constexpr uint32_t kDesktopHwnd = 0x00010010;

uint32_t now_ms() { return (uint32_t)(winu::mono_ns() / 1000000ull); }

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

uint32_t wndproc_of(uint32_t hwnd) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(hwnd);
    return it == g_windows.end() ? 0 : it->second.wndproc;
}

uint32_t send(cpu::Cpu& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t proc = wndproc_of(hwnd);
    if (!proc) return 0;
    return call_guest(c, proc, {hwnd, msg, wp, lp});
}

uint32_t def_window_proc(cpu::Cpu& c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

void destroy_window(cpu::Cpu& c, uint32_t hwnd) {
    send(c, hwnd, WM_DESTROY, 0, 0);
    send(c, hwnd, WM_NCDESTROY, 0, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    g_windows.erase(hwnd);
    if (g_main_hwnd == hwnd) g_main_hwnd = 0;
}

uint32_t def_window_proc(cpu::Cpu& c, uint32_t hwnd, uint32_t msg, uint32_t, uint32_t) {
    switch (msg) {
        case WM_NCCREATE: return 1;
        case WM_CLOSE: destroy_window(c, hwnd); return 0;
        case WM_ERASEBKGND: return 1;
        default: return 0;
    }
}

// --- wsprintf --------------------------------------------------------------
// Supports the wsprintf subset: %[-][#][0][width][.prec][h|l]{c,C,d,i,u,x,X,s,S,%}.
std::string format_guest(const char* fmt, uint32_t ap) {
    std::string out;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        ++p;
        if (*p == '%') {
            out += '%';
            continue;
        }
        std::string spec = "%";
        while (*p == '-' || *p == '#' || *p == '0') spec += *p++;
        while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        bool wide = false;
        if (*p == 'h' || *p == 'l' || *p == 'L') {
            wide = *p == 'l';
            ++p;
        }
        char conv = *p;
        if (!conv) break;
        char buf[512];
        switch (conv) {
            case 'd':
            case 'i':
                snprintf(buf, sizeof buf, (spec + "d").c_str(), (int32_t)mem::rd32(ap));
                ap += 4;
                break;
            case 'u':
            case 'x':
            case 'X':
                snprintf(buf, sizeof buf, (spec + conv).c_str(), mem::rd32(ap));
                ap += 4;
                break;
            case 'c':
            case 'C':
                snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)(mem::rd32(ap) & 0xFF));
                ap += 4;
                break;
            case 's':
            case 'S': {
                uint32_t sp = mem::rd32(ap);
                ap += 4;
                std::string s = wide || conv == 'S' ? winu::gwstr(sp) : gstr(sp);
                if (!sp) s = "(null)";
                snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                break;
            }
            default:
                buf[0] = conv;
                buf[1] = 0;
                break;
        }
        out += buf;
    }
    return out;
}
}  // namespace

namespace user32 {
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back({hwnd, msg, wparam, lparam, now_ms()});
    }
    g_cv.notify_all();
}
uint32_t main_window() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_main_hwnd;
}
void set_app_active(bool active) {
    uint32_t h = main_window();
    if (!h) return;
    post_message(h, WM_ACTIVATEAPP, active ? 1 : 0, 0);
    post_message(h, WM_ACTIVATE, active ? 1 : 0, 0);
}
}  // namespace user32

HLE_EXPORT(user32, RegisterClassExA, 4) {
    uint32_t wc = arg(c, 0);
    std::string name = gstr(mem::rd32(wc + 40));
    std::lock_guard<std::mutex> lk(g_mu);
    g_classes[lower(name)] = mem::rd32(wc + 8);
    LOGI("RegisterClassExA(%s, wndproc=%08x)", name.c_str(), mem::rd32(wc + 8));
    ret(c, 0xC000 + (uint32_t)g_classes.size());
}
HLE_EXPORT(user32, UnregisterClassA, 8) { ret(c, 1); }

HLE_EXPORT(user32, CreateWindowExA, 48) {
    uint32_t exstyle = arg(c, 0), cls = arg(c, 1), title = arg(c, 2), style = arg(c, 3);
    int32_t x = (int32_t)arg(c, 4), y = (int32_t)arg(c, 5), w = (int32_t)arg(c, 6), h = (int32_t)arg(c, 7);
    uint32_t param = arg(c, 11);
    std::string cname = cls < 0x10000 ? "#atom" : gstr(cls);
    Window win;
    uint32_t hwnd;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_classes.find(lower(cname));
        win.wndproc = it == g_classes.end() ? 0 : it->second;
        if (!win.wndproc && !g_classes.empty() && cls < 0x10000) win.wndproc = g_classes.begin()->second;
        win.style = style;
        win.exstyle = exstyle;
        win.x = x == (int32_t)0x80000000 ? 0 : x;
        win.y = y == (int32_t)0x80000000 ? 0 : y;
        win.w = w == (int32_t)0x80000000 ? (int32_t)user32::kDesktopWidth : w;
        win.h = h == (int32_t)0x80000000 ? (int32_t)user32::kDesktopHeight : h;
        win.cls = cname;
        hwnd = g_next_hwnd += 0x10;
        g_windows[hwnd] = win;
        if (!g_main_hwnd) g_main_hwnd = hwnd;
    }
    LOGI("CreateWindowExA(class=%s, title=%s, %dx%d) -> %08x", cname.c_str(), gstr(title).c_str(), win.w, win.h,
         hwnd);
    // CREATESTRUCTA for WM_NCCREATE / WM_CREATE.
    uint32_t cs = mem::alloc_system(48);
    mem::wr32(cs + 0, param);
    mem::wr32(cs + 16, (uint32_t)win.h);
    mem::wr32(cs + 20, (uint32_t)win.w);
    mem::wr32(cs + 24, (uint32_t)win.y);
    mem::wr32(cs + 28, (uint32_t)win.x);
    mem::wr32(cs + 32, style);
    mem::wr32(cs + 36, title);
    mem::wr32(cs + 40, cls);
    mem::wr32(cs + 44, exstyle);
    send(c, hwnd, WM_NCCREATE, 0, cs);
    if (c.stop != cpu::Stop::None) return;
    send(c, hwnd, WM_CREATE, 0, cs);
    if (c.stop != cpu::Stop::None) return;
    ret(c, hwnd);
}

HLE_EXPORT(user32, ShowWindow, 8) {
    uint32_t hwnd = arg(c, 0);
    uint32_t w, h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end()) return ret(c, 0);
        w = (uint32_t)it->second.w;
        h = (uint32_t)it->second.h;
    }
    send(c, hwnd, WM_SHOWWINDOW, 1, 0);
    if (c.stop != cpu::Stop::None) return;
    send(c, hwnd, WM_ACTIVATEAPP, 1, 0);
    if (c.stop != cpu::Stop::None) return;
    send(c, hwnd, WM_ACTIVATE, 1, 0);
    if (c.stop != cpu::Stop::None) return;
    send(c, hwnd, WM_SETFOCUS, 0, 0);
    if (c.stop != cpu::Stop::None) return;
    send(c, hwnd, WM_SIZE, 0, (h << 16) | (w & 0xFFFF));
    if (c.stop != cpu::Stop::None) return;
    ret(c, 0);
}
HLE_EXPORT(user32, UpdateWindow, 4) { ret(c, 1); }
HLE_EXPORT(user32, DestroyWindow, 4) {
    destroy_window(c, arg(c, 0));
    if (c.stop != cpu::Stop::None) return;
    ret(c, 1);
}
HLE_EXPORT(user32, DefWindowProcA, 16) {
    uint32_t r = def_window_proc(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    if (c.stop != cpu::Stop::None) return;
    ret(c, r);
}

HLE_EXPORT(user32, PeekMessageA, 20) {
    uint32_t out = arg(c, 0), remove = arg(c, 4);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue.empty()) return ret(c, 0);
    const Msg& m = g_queue.front();
    mem::wr32(out + 0, m.hwnd);
    mem::wr32(out + 4, m.msg);
    mem::wr32(out + 8, m.wparam);
    mem::wr32(out + 12, m.lparam);
    mem::wr32(out + 16, m.time);
    mem::wr32(out + 20, user32::kDesktopWidth / 2);
    mem::wr32(out + 24, user32::kDesktopHeight / 2);
    if (remove & 1) g_queue.pop_front();
    ret(c, 1);
}
HLE_EXPORT(user32, GetMessageA, 16) {
    uint32_t out = arg(c, 0);
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait(lk, [] { return !g_queue.empty(); });
    Msg m = g_queue.front();
    g_queue.pop_front();
    mem::wr32(out + 0, m.hwnd);
    mem::wr32(out + 4, m.msg);
    mem::wr32(out + 8, m.wparam);
    mem::wr32(out + 12, m.lparam);
    mem::wr32(out + 16, m.time);
    ret(c, m.msg == WM_QUIT ? 0 : 1);
}
HLE_EXPORT(user32, TranslateMessage, 4) { ret(c, 1); }
HLE_EXPORT(user32, DispatchMessageA, 4) {
    uint32_t m = arg(c, 0);
    uint32_t r = send(c, mem::rd32(m), mem::rd32(m + 4), mem::rd32(m + 8), mem::rd32(m + 12));
    if (c.stop != cpu::Stop::None) return;
    ret(c, r);
}
HLE_EXPORT(user32, PostMessageA, 16) {
    user32::post_message(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    ret(c, 1);
}
HLE_EXPORT(user32, PostThreadMessageA, 16) {
    user32::post_message(0, arg(c, 1), arg(c, 2), arg(c, 3));
    ret(c, 1);
}
HLE_EXPORT(user32, PostQuitMessage, 4) { user32::post_message(0, WM_QUIT, arg(c, 0), 0); }

HLE_EXPORT(user32, SetWindowLongA, 12) {
    uint32_t hwnd = arg(c, 0), v = arg(c, 2);
    int32_t idx = (int32_t)arg(c, 1);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) return ret(c, 0);
    Window& w = it->second;
    uint32_t old = 0;
    switch (idx) {
        case -4: old = w.wndproc; w.wndproc = v; break;
        case -16: old = w.style; w.style = v; break;
        case -20: old = w.exstyle; w.exstyle = v; break;
        case -21: old = w.userdata; w.userdata = v; break;
        default: break;
    }
    ret(c, old);
}

HLE_EXPORT(user32, GetClientRect, 8) {
    uint32_t r = arg(c, 1);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(arg(c, 0));
    int32_t w = it == g_windows.end() ? (int32_t)user32::kDesktopWidth : it->second.w;
    int32_t h = it == g_windows.end() ? (int32_t)user32::kDesktopHeight : it->second.h;
    mem::wr32(r + 0, 0);
    mem::wr32(r + 4, 0);
    mem::wr32(r + 8, (uint32_t)w);
    mem::wr32(r + 12, (uint32_t)h);
    ret(c, 1);
}
HLE_EXPORT(user32, GetWindowRect, 8) {
    uint32_t r = arg(c, 1);
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_windows.find(arg(c, 0));
    Window w;
    if (it != g_windows.end()) {
        w = it->second;
    } else {
        w.w = (int32_t)user32::kDesktopWidth;
        w.h = (int32_t)user32::kDesktopHeight;
    }
    mem::wr32(r + 0, (uint32_t)w.x);
    mem::wr32(r + 4, (uint32_t)w.y);
    mem::wr32(r + 8, (uint32_t)(w.x + w.w));
    mem::wr32(r + 12, (uint32_t)(w.y + w.h));
    ret(c, 1);
}
HLE_EXPORT(user32, AdjustWindowRect, 12) { ret(c, 1); }
HLE_EXPORT(user32, SetRect, 20) {
    uint32_t r = arg(c, 0);
    for (int i = 0; i < 4; ++i) mem::wr32(r + i * 4, arg(c, i + 1));
    ret(c, 1);
}
HLE_EXPORT(user32, GetSystemMetrics, 4) {
    switch (arg(c, 0)) {
        case 0: return ret(c, user32::kDesktopWidth);   // SM_CXSCREEN
        case 1: return ret(c, user32::kDesktopHeight);  // SM_CYSCREEN
        case 16: return ret(c, user32::kDesktopWidth);  // SM_CXFULLSCREEN
        case 17: return ret(c, user32::kDesktopHeight);
        default: return ret(c, 0);
    }
}
HLE_EXPORT(user32, GetDesktopWindow, 0) { ret(c, kDesktopHwnd); }
HLE_EXPORT(user32, GetForegroundWindow, 0) { ret(c, user32::main_window()); }
HLE_EXPORT(user32, SetForegroundWindow, 4) { ret(c, 1); }
HLE_EXPORT(user32, SetActiveWindow, 4) { ret(c, user32::main_window()); }
HLE_EXPORT(user32, SetFocus, 4) { ret(c, user32::main_window()); }
HLE_EXPORT(user32, IsIconic, 4) { ret(c, 0); }

HLE_EXPORT(user32, ShowCursor, 4) {
    g_cursor_count += arg(c, 0) ? 1 : -1;
    ret(c, (uint32_t)g_cursor_count);
}
HLE_EXPORT(user32, SetCursor, 4) { ret(c, 0); }
HLE_EXPORT(user32, LoadIconA, 8) { ret(c, 0x00020001); }
HLE_EXPORT(user32, LoadCursorA, 8) { ret(c, 0x00020002); }
HLE_EXPORT(user32, GetCursorPos, 4) {
    mem::wr32(arg(c, 0), user32::kDesktopWidth / 2);
    mem::wr32(arg(c, 0) + 4, user32::kDesktopHeight / 2);
    ret(c, 1);
}
HLE_EXPORT(user32, SetCapture, 4) { ret(c, 0); }
HLE_EXPORT(user32, ReleaseCapture, 0) { ret(c, 1); }
HLE_EXPORT(user32, BeginPaint, 8) {
    memset(mem::ptr(arg(c, 1)), 0, 64);
    ret(c, 0x00030001);
}
HLE_EXPORT(user32, EndPaint, 8) { ret(c, 1); }

HLE_EXPORT(user32, MessageBoxA, 16) {
    LOGW("MessageBox [%s]: %s", gstr(arg(c, 2)).c_str(), gstr(arg(c, 1)).c_str());
    ret(c, 1);  // IDOK
}

HLE_EXPORT(user32, wvsprintfA, 12) {
    std::string s = format_guest(mem::cstr(arg(c, 1)), arg(c, 2));
    if (s.size() > 1023) s.resize(1023);
    mem::write_cstr(arg(c, 0), s.c_str(), 1024);
    ret(c, (uint32_t)s.size());
}
HLE_EXPORT(user32, wsprintfA, 0) {  // cdecl varargs
    uint32_t ap = c.r[cpu::ESP] + 4 + 8;
    std::string s = format_guest(mem::cstr(arg(c, 1)), ap);
    if (s.size() > 1023) s.resize(1023);
    mem::write_cstr(arg(c, 0), s.c_str(), 1024);
    ret(c, (uint32_t)s.size());
}
