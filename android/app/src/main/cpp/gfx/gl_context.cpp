#include "gfx/gl_context.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cstring>
#include <string>

#include "core/log.h"
#include "platform/window.h"

namespace gfx {

namespace {
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLConfig g_cfg = nullptr;
EGLContext g_ctx = EGL_NO_CONTEXT;
EGLSurface g_surf = EGL_NO_SURFACE;
ANativeWindow* g_win = nullptr;
unsigned g_gen = ~0u;
int g_w = 0, g_h = 0;
std::string g_ext;

bool init_display() {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }
    const EGLint attrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
                            EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, attrs, &g_cfg, 1, &n) || n == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext(ES 3.2) failed: %x", eglGetError());
        return false;
    }
    return true;
}

void drop_surface() {
    if (g_surf != EGL_NO_SURFACE) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx);
        eglDestroySurface(g_dpy, g_surf);
        g_surf = EGL_NO_SURFACE;
    }
    if (g_win) {
        ANativeWindow_release(g_win);
        g_win = nullptr;
    }
}
}  // namespace

bool make_current() {
    if (g_dpy == EGL_NO_DISPLAY && !init_display()) return false;
    if (g_surf != EGL_NO_SURFACE && g_gen == window::generation() && !window::paused()) return true;

    drop_surface();
    g_win = window::wait_for_surface(&g_w, &g_h);
    g_gen = window::generation();
    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, g_win, nullptr);
    if (g_surf == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: %x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        LOGE("eglMakeCurrent failed: %x", eglGetError());
        return false;
    }
    eglSwapInterval(g_dpy, 1);
    if (g_ext.empty()) {
        g_ext = (const char*)glGetString(GL_EXTENSIONS);
        LOGI("GL: %s | %s | %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
        LOGI("GL extensions: %s", g_ext.c_str());
    }
    LOGI("EGL surface bound (%dx%d)", g_w, g_h);
    return true;
}

void present(GLuint src_fbo, int src_w, int src_h) {
    if (!make_current()) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, g_w, g_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Fit the game's frame into the window, preserving its aspect ratio.
    float sa = (float)src_w / (float)src_h, wa = (float)g_w / (float)g_h;
    int dw = g_w, dh = g_h;
    if (wa > sa)
        dw = (int)((float)g_h * sa + 0.5f);
    else
        dh = (int)((float)g_w / sa + 0.5f);
    int dx = (g_w - dw) / 2, dy = (g_h - dh) / 2;
    glBlitFramebuffer(0, 0, src_w, src_h, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (!eglSwapBuffers(g_dpy, g_surf)) {
        EGLint e = eglGetError();
        LOGW("eglSwapBuffers failed: %x", e);
        if (e == EGL_BAD_SURFACE || e == EGL_BAD_NATIVE_WINDOW) g_gen = ~0u;  // rebind next frame
    }
}

int window_width() { return g_w; }
int window_height() { return g_h; }

bool has_extension(const char* name) {
    size_t n = strlen(name);
    for (size_t p = g_ext.find(name); p != std::string::npos; p = g_ext.find(name, p + 1)) {
        bool start = p == 0 || g_ext[p - 1] == ' ';
        bool end = p + n == g_ext.size() || g_ext[p + n] == ' ';
        if (start && end) return true;
    }
    return false;
}

}  // namespace gfx
