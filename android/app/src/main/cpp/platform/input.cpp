#include "platform/input.h"

#include <cmath>
#include <cstring>
#include <mutex>

namespace input {

namespace {
std::mutex g_mu;
Pad g_pads[2];
uint8_t g_keys[256];

float pick(float a, float b) { return std::fabs(a) >= std::fabs(b) ? a : b; }
}  // namespace

void set_pad(int source, const Pad& p) {
    if (source < 0 || source > 1) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pads[source] = p;
}

Pad merged() {
    std::lock_guard<std::mutex> lk(g_mu);
    const Pad& a = g_pads[0];
    const Pad& b = g_pads[1];
    Pad m;
    m.buttons = a.buttons | b.buttons;
    m.lx = pick(a.lx, b.lx);
    m.ly = pick(a.ly, b.ly);
    m.rx = pick(a.rx, b.rx);
    m.ry = pick(a.ry, b.ry);
    m.lt = std::fmax(a.lt, b.lt);
    m.rt = std::fmax(a.rt, b.rt);
    return m;
}

void set_key(int dik, bool down) {
    if (dik <= 0 || dik > 255) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_keys[dik] = down ? 0x80 : 0;
}

void keyboard(uint8_t out[256]) {
    std::lock_guard<std::mutex> lk(g_mu);
    memcpy(out, g_keys, 256);
}

}  // namespace input
