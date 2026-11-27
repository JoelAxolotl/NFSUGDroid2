#include "platform/input.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "core/log.h"

namespace input {

namespace {
std::mutex g_mu;
Pad g_pads[2];
uint8_t g_keys[256];        // physical keyboard (DIK layout)
uint8_t g_game_state[256];  // last game-visible keyboard, for transition events
std::deque<KeyEvent> g_events;  // bounded log of transitions
uint64_t g_next_seq = 1;

float pick(float a, float b) { return std::fabs(a) >= std::fabs(b) ? a : b; }

Pad merged_locked() {
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

void game_keys_locked(uint8_t keys[256]) {
    memcpy(keys, g_keys, 256);
    Pad p = merged_locked();
    auto press = [&](int dik, bool on) {
        if (on) keys[dik] = 0x80;
    };
    // Keys are NFSU2's default keyboard bindings, taken from the DirectInput
    // action map the game registers (appdata: 1 throttle, 2 brake, 3/4 steer,
    // 5 handbrake, 6 nitrous, 7 shift up, 8 shift down, 9 look back, 10 camera,
    // 12 pause). The stick only steers (its Y axis must not touch the throttle).
    // In the driving action map the game marks Up as DIA_APPNOMAP for Throttle,
    // so throttle (and, symmetrically, brake) also press the WASD binding.
    press(0xC8, (p.buttons & kDpadUp) || p.rt > 0.3f);       // Up: throttle / menu up
    press(0x11, p.rt > 0.3f);                                // W: throttle (driving map)
    press(0xD0, (p.buttons & kDpadDown) || p.lt > 0.3f);     // Down: brake-reverse / menu down
    press(0x1F, p.lt > 0.3f);                                // S: brake-reverse (driving map)
    press(0xCB, (p.buttons & kDpadLeft) || p.lx < -0.35f);   // Left: steer / menu left
    press(0xCD, (p.buttons & kDpadRight) || p.lx > 0.35f);   // Right: steer / menu right
    press(0x1C, (p.buttons & kA) || (p.buttons & kOk));      // Enter: accept
    press(0x01, (p.buttons & kB) || (p.buttons & kStart));   // Esc: back / pause
    press(0x39, p.buttons & kX);                             // Space: handbrake
    press(0x38, p.buttons & kA);                             // Left Alt: nitrous
    press(0x2A, p.buttons & kRB);                            // Left Shift: shift up
    press(0x1D, p.buttons & kLB);                            // Left Ctrl: shift down
    press(0x30, p.buttons & kBack);                          // B: look back
    press(0x2E, p.buttons & kY);                             // C: camera
}

// Records transitions of the game-visible keyboard. Called with g_mu held.
void update_events_locked() {
    uint8_t now[256];
    game_keys_locked(now);
    for (int k = 1; k < 256; ++k) {
        if ((now[k] & 0x80) == (g_game_state[k] & 0x80)) continue;
        g_game_state[k] = now[k];
        g_events.push_back(KeyEvent{g_next_seq++, (uint8_t)k, (now[k] & 0x80) != 0});
        if (g_events.size() > 512) g_events.pop_front();
    }
}
}  // namespace

void set_pad(int source, const Pad& p) {
    if (source < 0 || source > 1) return;
    uint32_t before;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        before = g_pads[source].buttons;
        g_pads[source] = p;
        update_events_locked();
    }
    if (p.buttons != before)
        LOGI("input: %s buttons %05x", source == kSourceTouch ? "touch" : "controller", p.buttons);
}

Pad merged() {
    std::lock_guard<std::mutex> lk(g_mu);
    return merged_locked();
}

void set_key(int dik, bool down) {
    if (dik <= 0 || dik > 255) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_keys[dik] = down ? 0x80 : 0;
    update_events_locked();
}

void keyboard(uint8_t out[256]) {
    std::lock_guard<std::mutex> lk(g_mu);
    memcpy(out, g_keys, 256);
}

void game_keys(uint8_t keys[256]) {
    std::lock_guard<std::mutex> lk(g_mu);
    game_keys_locked(keys);
}

bool next_key_event(uint64_t* cursor, KeyEvent* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (*cursor == 0) *cursor = g_next_seq;  // new consumer: only future events
    for (const KeyEvent& e : g_events) {
        if (e.seq >= *cursor) {
            *out = e;
            *cursor = e.seq + 1;
            return true;
        }
    }
    return false;
}

uint32_t dik_to_vk(int dik) {
    static const uint8_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    for (int i = 0; i < 26; ++i)
        if (kLetters[i] == dik) return 'A' + (uint32_t)i;
    if (dik >= 0x02 && dik <= 0x0A) return '1' + (uint32_t)(dik - 0x02);
    if (dik == 0x0B) return '0';
    if (dik >= 0x3B && dik <= 0x44) return 0x70 + (uint32_t)(dik - 0x3B);  // F1-F10
    switch (dik) {
        case 0x01: return 0x1B;  // Escape
        case 0x0E: return 0x08;  // Backspace
        case 0x0F: return 0x09;  // Tab
        case 0x1C: return 0x0D;  // Enter
        case 0x1D: case 0x9D: return 0x11;  // Ctrl
        case 0x2A: case 0x36: return 0x10;  // Shift
        case 0x38: case 0xB8: return 0x12;  // Alt
        case 0x39: return 0x20;  // Space
        case 0x57: return 0x7A;  // F11
        case 0x58: return 0x7B;  // F12
        case 0xC7: return 0x24;  // Home
        case 0xC8: return 0x26;  // Up
        case 0xC9: return 0x21;  // PgUp
        case 0xCB: return 0x25;  // Left
        case 0xCD: return 0x27;  // Right
        case 0xCF: return 0x23;  // End
        case 0xD0: return 0x28;  // Down
        case 0xD1: return 0x22;  // PgDn
        case 0xD2: return 0x2D;  // Insert
        case 0xD3: return 0x2E;  // Delete
        default: return 0;
    }
}

}  // namespace input
