#pragma once
// Host input state, written by the Java side (touch overlay, controllers,
// keyboards) and read by the DirectInput8 implementation.
#include <cstdint>

namespace input {

enum Button : uint32_t {
    kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
    kStart = 0x0010, kBack = 0x0020, kLThumb = 0x0040, kRThumb = 0x0080,
    kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};

struct Pad {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // [-1, 1], +y down
    float lt = 0, rt = 0;                  // [0, 1]
};

constexpr int kSourceTouch = 0;
constexpr int kSourceController = 1;

void set_pad(int source, const Pad& p);
// Touch and controller merged: buttons OR-ed, axes by larger magnitude.
Pad merged();

void set_key(int dik, bool down);
void keyboard(uint8_t out[256]);  // DirectInput layout: 0x80 = pressed

}  // namespace input
