#include "platform/window.h"

#include <condition_variable>
#include <mutex>

namespace window {

namespace {
std::mutex g_mu;
std::condition_variable g_cv;
ANativeWindow* g_win = nullptr;
int g_w = 0, g_h = 0;
bool g_paused = false;
unsigned g_gen = 0;
}  // namespace

void set_surface(ANativeWindow* w, int width, int height) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_win) ANativeWindow_release(g_win);
        g_win = w;
        g_w = width;
        g_h = height;
        ++g_gen;
    }
    g_cv.notify_all();
}

void clear_surface() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_win) ANativeWindow_release(g_win);
    g_win = nullptr;
    ++g_gen;
}

void set_paused(bool p) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_paused = p;
    }
    g_cv.notify_all();
}

bool paused() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_paused;
}

ANativeWindow* wait_for_surface(int* width, int* height) {
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.wait(lk, [] { return g_win != nullptr && !g_paused; });
    ANativeWindow_acquire(g_win);
    if (width) *width = g_w;
    if (height) *height = g_h;
    return g_win;
}

unsigned generation() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_gen;
}

}  // namespace window
