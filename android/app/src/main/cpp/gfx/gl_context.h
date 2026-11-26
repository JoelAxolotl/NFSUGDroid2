#pragma once
// EGL / GLES 3.2 context owned by the guest's rendering thread (the thread
// that created the D3D9 device). Handles Android surface loss and recreation.
#include <GLES3/gl32.h>

namespace gfx {

// Ensures a current context with a valid window surface on this thread.
// Blocks while the app is paused or has no surface. Returns false on failure.
bool make_current();

// Scales `src_fbo` (src_w x src_h) onto the window, preserving aspect ratio,
// and swaps buffers.
void present(GLuint src_fbo, int src_w, int src_h);

int window_width();
int window_height();
bool has_extension(const char* name);

}  // namespace gfx
