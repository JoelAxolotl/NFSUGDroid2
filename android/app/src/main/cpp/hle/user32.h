#pragma once
// Window/message state shared with the D3D9 and DirectInput layers.
#include <cstdint>

namespace user32 {

// Virtual desktop the guest sees (GetSystemMetrics, default display mode).
constexpr uint32_t kDesktopWidth = 1280;
constexpr uint32_t kDesktopHeight = 720;

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
uint32_t main_window();

// Called when the Android activity pauses/resumes: delivers WM_ACTIVATEAPP.
void set_app_active(bool active);

}  // namespace user32
