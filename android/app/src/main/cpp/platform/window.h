#pragma once
// The Android surface the game renders into. The render path (D3D9 -> GLES)
// blocks in wait_for_surface() while the app is in the background.
#include <android/native_window.h>

namespace window {

void set_surface(ANativeWindow* w, int width, int height);  // takes a reference
void clear_surface();
void set_paused(bool paused);
bool paused();

// Returns the current surface (with an acquired reference the caller must
// release via ANativeWindow_release), blocking while none is available.
ANativeWindow* wait_for_surface(int* width, int* height);
// Monotonic counter bumped whenever the surface changes (EGL must rebind).
unsigned generation();

}  // namespace window
