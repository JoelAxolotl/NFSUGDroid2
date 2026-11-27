// DirectInput 8: system keyboard, system mouse and one virtual gamepad fed by
// the Android touch overlay and physical controllers.
//
// The gamepad is exposed like the Xbox 360 controller's DirectInput driver:
// X/Y = left stick, Rx/Ry = right stick, Z = triggers (LT up, RT down),
// buttons 0..9 = A B X Y LB RB Back Start LS RS, POV 0 = D-pad.
// The pad is also mirrored onto keyboard keys so menus and the game's default
// keyboard driving controls work without configuring a controller.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "hle/com.h"
#include "hle/hle.h"
#include "hle/win_util.h"
#include "platform/input.h"

using hle::arg;
using hle::ret;

namespace {
constexpr uint32_t DI_OK = 0, DI_NOEFFECT = 1, DIERR_INVALIDPARAM = 0x80070057, DIERR_NOTFOUND = 0x80070002,
                   DIERR_UNSUPPORTED = 0x80004001, DIERR_NOTACQUIRED = 0x8007000C;

// GUIDs as little-endian dwords.
const uint32_t GUID_SysKeyboard[4] = {0x6F1D2B61, 0x11CFD5A0, 0x4544C7BF, 0x00005453};
const uint32_t GUID_SysMouse[4] = {0x6F1D2B60, 0x11CFD5A0, 0x4544C7BF, 0x00005453};
const uint32_t GUID_Pad[4] = {0x4E534655, 0x11EF0002, 0x4F52C7BF, 0x44494F44};     // "UFSN..ANDROID" instance
const uint32_t GUID_PadProd[4] = {0x028E045E, 0x00000000, 0x49500000, 0x44494456};  // VID_045E PID_028E style
const uint32_t GUID_XAxis[4] = {0xA36D02E0, 0x11CFC9F3, 0x4544C7BF, 0x00005453};
const uint32_t GUID_YAxis[4] = {0xA36D02E1, 0x11CFC9F3, 0x4544C7BF, 0x00005453};
const uint32_t GUID_ZAxis[4] = {0xA36D02E2, 0x11CFC9F3, 0x4544C7BF, 0x00005453};
const uint32_t GUID_RxAxis[4] = {0xA36D02F4, 0x11CFC9F3, 0x4544C7BF, 0x00005453};
const uint32_t GUID_RyAxis[4] = {0xA36D02F5, 0x11CFC9F3, 0x4544C7BF, 0x00005453};
const uint32_t GUID_Button[4] = {0xA36D02F0, 0x11CFC9F3, 0x4544C7BF, 0x00005453};
const uint32_t GUID_POV[4] = {0xA36D02F2, 0x11CFC9F3, 0x4544C7BF, 0x00005453};

enum class Kind { Keyboard, Mouse, Pad };

// Input mode. The touch overlay and physical controllers drive the game through
// its keyboard bindings (platform/input.cpp game_keys). The DirectInput joystick
// stays hidden until its axes/buttons are matched to the game's controller
// configuration: with default mappings its centred axes read as part throttle.
constexpr bool kExposeGamepad = false;

struct DInput : com::Object {};

// One action bound by SetActionMap: the game's uAppData for a device object.
struct BoundAction {
    uint32_t app_data;
    uint32_t object;  // keyboard: DIK code
};

struct Device : com::Object {
    Kind kind = Kind::Keyboard;
    bool acquired = false;
    uint32_t data_size = 0;  // from SetDataFormat
    int32_t range_min[8], range_max[8];
    uint32_t deadzone = 0;
    uint64_t key_cursor = 0;  // position in the input::next_key_event log
    uint32_t sequence = 0;
    uint32_t buffer_size = 0;
    std::vector<BoundAction> actions;  // action-mapped devices (SetActionMap)
    bool action_mapped = false;
    Device() {
        for (int i = 0; i < 8; ++i) {
            range_min[i] = 0;
            range_max[i] = 65535;
        }
    }
};

void write_guid(uint32_t p, const uint32_t g[4]) { memcpy(mem::ptr(p), g, 16); }

int32_t scale_axis(const Device* d, int axis, float v /* -1..1 */) {
    float t = (v + 1.0f) * 0.5f;
    return d->range_min[axis] + (int32_t)std::lround(t * (float)(d->range_max[axis] - d->range_min[axis]));
}

void pad_state(const Device* d, uint32_t out, uint32_t size) {
    memset(mem::ptr(out), 0, size);
    input::Pad p = input::merged();
    // DIJOYSTATE: lX lY lZ lRx lRy lRz rglSlider[2] rgdwPOV[4] rgbButtons[32]
    mem::wr32(out + 0, (uint32_t)scale_axis(d, 0, p.lx));
    mem::wr32(out + 4, (uint32_t)scale_axis(d, 1, p.ly));
    mem::wr32(out + 8, (uint32_t)scale_axis(d, 2, p.lt - p.rt));
    mem::wr32(out + 12, (uint32_t)scale_axis(d, 3, p.rx));
    mem::wr32(out + 16, (uint32_t)scale_axis(d, 4, p.ry));
    mem::wr32(out + 20, (uint32_t)scale_axis(d, 5, 0.f));
    uint32_t pov = 0xFFFFFFFF;
    bool u = p.buttons & input::kDpadUp, dn = p.buttons & input::kDpadDown;
    bool l = p.buttons & input::kDpadLeft, r = p.buttons & input::kDpadRight;
    if (u && r) pov = 4500;
    else if (r && dn) pov = 13500;
    else if (dn && l) pov = 22500;
    else if (l && u) pov = 31500;
    else if (u) pov = 0;
    else if (r) pov = 9000;
    else if (dn) pov = 18000;
    else if (l) pov = 27000;
    for (int i = 0; i < 4; ++i) mem::wr32(out + 32 + i * 4, i == 0 ? pov : 0xFFFFFFFF);
    static const uint32_t kOrder[10] = {input::kA,  input::kB,    input::kX,     input::kY,      input::kLB,
                                        input::kRB, input::kBack, input::kStart, input::kLThumb, input::kRThumb};
    for (int i = 0; i < 10; ++i)
        if (p.buttons & kOrder[i]) mem::wr8(out + 48 + i, 0x80);
}

uint32_t dinput_vtable();
uint32_t device_vtable();
uint32_t effect_vtable();

// Force-feedback effects are accepted and tracked (rumble -> Android vibrator
// is a later addition); the game requires valid effect objects.
struct Effect : com::Object {
    uint32_t guid[4] = {};
    bool playing = false;
};

// ---------------------------------------------------------------- IDirectInput8A
void di_CreateDevice(cpu::Cpu& c) {
    uint32_t g = arg(c, 1), out = arg(c, 2);
    Device* d = new Device();
    if (com::guid_eq(g, GUID_SysKeyboard)) {
        d->kind = Kind::Keyboard;
    } else if (com::guid_eq(g, GUID_SysMouse)) {
        d->kind = Kind::Mouse;
    } else if (com::guid_eq(g, GUID_Pad)) {
        d->kind = Kind::Pad;
    } else {
        delete d;
        mem::wr32(out, 0);
        return ret(c, DIERR_NOTFOUND);
    }
    com::wrap(d, device_vtable());
    mem::wr32(out, d->guest);
    LOGI("DirectInput: created %s device",
         d->kind == Kind::Keyboard ? "keyboard" : d->kind == Kind::Mouse ? "mouse" : "gamepad");
    ret(c, DI_OK);
}

// DIDEVICEINSTANCEA (DirectInput >= 5): 0x244 bytes. Older callers pass the
// 0x234-byte DirectInput 3 layout, so never write past the caller's dwSize.
constexpr uint32_t kDevInstSize = 0x244;

void fill_instance(uint32_t p, const uint32_t inst[4], const uint32_t prod[4], uint32_t type, const char* name,
                   uint32_t cap = kDevInstSize) {
    uint8_t tmp[kDevInstSize] = {};
    auto put32 = [&](uint32_t off, uint32_t v) { memcpy(tmp + off, &v, 4); };
    put32(0, cap);
    memcpy(tmp + 4, inst, 16);
    memcpy(tmp + 20, prod, 16);
    put32(36, type);
    strncpy((char*)tmp + 40, name, 259);
    strncpy((char*)tmp + 300, name, 259);
    uint16_t page = 1, usage = (type & 0xFF) == 0x13 ? 6 : 5;  // generic desktop: keyboard / gamepad
    memcpy(tmp + 0x240, &page, 2);
    memcpy(tmp + 0x242, &usage, 2);
    memcpy(mem::ptr(p), tmp, std::min(cap, kDevInstSize));
}

void di_EnumDevices(cpu::Cpu& c) {
    uint32_t type = arg(c, 1), cb = arg(c, 2), ref = arg(c, 3);
    static const uint32_t inst = mem::alloc_system(kDevInstSize);
    struct Dev {
        const uint32_t* g;
        const uint32_t* prod;
        uint32_t devtype;
        uint32_t cls;
        const char* name;
    };
    const Dev devs[] = {
        {GUID_SysKeyboard, GUID_SysKeyboard, 0x0413, 3, "Keyboard"},
        {GUID_SysMouse, GUID_SysMouse, 0x0212, 2, "Mouse"},
        {GUID_Pad, GUID_PadProd, 0x0115, 4, "Controller (Android Gamepad)"},
    };
    for (const Dev& d : devs) {
        uint32_t t = type & 0xFF;
        bool match = t == 0 || t == d.cls || t == (d.devtype & 0xFF);
        if (!match) continue;
        if (d.g == GUID_Pad && !kExposeGamepad) continue;
        fill_instance(inst, d.g, d.prod, d.devtype, d.name);
        uint32_t r = hle::call_guest(c, cb, {inst, ref});
        if (c.stop != cpu::Stop::None) return;
        if (r == 0) break;  // DIENUM_STOP
    }
    ret(c, DI_OK);
}
void di_GetDeviceStatus(cpu::Cpu& c) { ret(c, DI_OK); }
void di_EnumDevicesBySemantics(cpu::Cpu& c);
void di_Initialize(cpu::Cpu& c) { ret(c, DI_OK); }

const com::Method kDInputMethods[] = {
    {"IDirectInput8A::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirectInput8A::AddRef", 4, com::AddRef, 0},
    {"IDirectInput8A::Release", 4, com::Release, 0},
    {"IDirectInput8A::CreateDevice", 16, di_CreateDevice, 0},
    {"IDirectInput8A::EnumDevices", 20, di_EnumDevices, 0},
    {"IDirectInput8A::GetDeviceStatus", 8, di_GetDeviceStatus, 0},
    {"IDirectInput8A::RunControlPanel", 12, nullptr, 0},
    {"IDirectInput8A::Initialize", 12, di_Initialize, 0},
    {"IDirectInput8A::FindDevice", 16, nullptr, DIERR_NOTFOUND},
    {"IDirectInput8A::EnumDevicesBySemantics", 24, di_EnumDevicesBySemantics, 0},
    {"IDirectInput8A::ConfigureDevices", 20, nullptr, DIERR_UNSUPPORTED},
};

// ---------------------------------------------------------------- IDirectInputDevice8A
Device* dv(cpu::Cpu& c) { return static_cast<Device*>(com::from_guest(arg(c, 0))); }

void dev_GetCapabilities(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t p = arg(c, 1);
    uint32_t sz = mem::rd32(p);
    memset(mem::ptr(p + 4), 0, sz > 4 ? sz - 4 : 0);
    mem::wr32(p + 4, 0x5);  // DIDC_ATTACHED | DIDC_EMULATED
    switch (d->kind) {
        case Kind::Keyboard: mem::wr32(p + 8, 0x0413); mem::wr32(p + 16, 128); break;
        case Kind::Mouse: mem::wr32(p + 8, 0x0212); mem::wr32(p + 12, 3); mem::wr32(p + 16, 3); break;
        case Kind::Pad:
            mem::wr32(p + 8, 0x0115);
            mem::wr32(p + 12, 5);   // axes
            mem::wr32(p + 16, 10);  // buttons
            mem::wr32(p + 20, 1);   // POVs
            break;
    }
    ret(c, DI_OK);
}

void dev_EnumObjects(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t cb = arg(c, 1), ref = arg(c, 2), flags = arg(c, 3);
    if (d->kind != Kind::Pad) return ret(c, DI_OK);
    static const uint32_t oi = mem::alloc_system(0x140);
    struct Obj {
        const uint32_t* g;
        uint32_t ofs, type;
        const char* name;
    };
    const Obj objs[] = {
        {GUID_XAxis, 0, 0x00000002 | (0 << 8), "X Axis"},   {GUID_YAxis, 4, 0x00000002 | (1 << 8), "Y Axis"},
        {GUID_ZAxis, 8, 0x00000002 | (2 << 8), "Z Axis"},   {GUID_RxAxis, 12, 0x00000002 | (3 << 8), "X Rotation"},
        {GUID_RyAxis, 16, 0x00000002 | (4 << 8), "Y Rotation"}, {GUID_POV, 32, 0x00000010 | (0 << 8), "Hat Switch"},
    };
    auto emit = [&](const uint32_t* g, uint32_t ofs, uint32_t type, const char* name) -> bool {
        uint32_t want = flags & 0xFF;  // DIDFT_AXIS 3, DIDFT_BUTTON 0xC, DIDFT_POV 0x10, ALL 0
        if (want && !(want & type & 0xFF)) return true;
        memset(mem::ptr(oi), 0, 0x13C);
        mem::wr32(oi, 0x13C);
        write_guid(oi + 4, g);
        mem::wr32(oi + 20, ofs);
        mem::wr32(oi + 24, type);
        mem::write_cstr(oi + 32, name, 260);
        uint32_t r = hle::call_guest(c, cb, {oi, ref});
        return c.stop == cpu::Stop::None && r != 0;
    };
    for (const Obj& o : objs)
        if (!emit(o.g, o.ofs, o.type, o.name)) return c.stop == cpu::Stop::None ? ret(c, DI_OK) : void();
    static const char* kBtn[10] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS"};
    for (uint32_t i = 0; i < 10; ++i)
        if (!emit(GUID_Button, 48 + i, 0x0000000C | (i << 8), kBtn[i]))
            return c.stop == cpu::Stop::None ? ret(c, DI_OK) : void();
    ret(c, DI_OK);
}

int axis_from_how(uint32_t obj, uint32_t how) {
    if (how == 1) return (int)(obj / 4);              // DIPH_BYOFFSET
    if (how == 2) return (int)((obj >> 8) & 0xFFFF);  // DIPH_BYID
    return -1;                                        // DIPH_DEVICE
}

void dev_SetProperty(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t prop = arg(c, 1), ph = arg(c, 2);
    uint32_t obj = mem::rd32(ph + 8), how = mem::rd32(ph + 12);
    int ax = axis_from_how(obj, how);
    switch (prop) {
        case 1:  // DIPROP_BUFFERSIZE
            d->buffer_size = mem::rd32(ph + 16);
            break;
        case 4: {  // DIPROP_RANGE
            int32_t lo = (int32_t)mem::rd32(ph + 16), hi = (int32_t)mem::rd32(ph + 20);
            for (int i = 0; i < 8; ++i) {
                if (ax < 0 || ax == i) {
                    d->range_min[i] = lo;
                    d->range_max[i] = hi;
                }
            }
            break;
        }
        case 5:  // DIPROP_DEADZONE (handled on the Android side)
            d->deadzone = mem::rd32(ph + 16);
            break;
        default: break;
    }
    ret(c, DI_OK);
}
void dev_GetProperty(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t prop = arg(c, 1), ph = arg(c, 2);
    int ax = axis_from_how(mem::rd32(ph + 8), mem::rd32(ph + 12));
    if (ax < 0 || ax > 7) ax = 0;
    switch (prop) {
        case 1: mem::wr32(ph + 16, d->buffer_size); return ret(c, DI_OK);
        case 4:
            mem::wr32(ph + 16, (uint32_t)d->range_min[ax]);
            mem::wr32(ph + 20, (uint32_t)d->range_max[ax]);
            return ret(c, DI_OK);
        case 5: mem::wr32(ph + 16, d->deadzone); return ret(c, DI_OK);
        default: return ret(c, DIERR_UNSUPPORTED);
    }
}
void dev_Acquire(cpu::Cpu& c) {
    Device* d = dv(c);
    bool was = d->acquired;
    d->acquired = true;
    ret(c, was ? DI_NOEFFECT : DI_OK);
}
void dev_Unacquire(cpu::Cpu& c) {
    dv(c)->acquired = false;
    ret(c, DI_OK);
}
void dev_GetDeviceState(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t size = arg(c, 1), out = arg(c, 2);
    // The game reads its joystick without ever calling Acquire (and only re-acquires
    // on DIERR_INPUTLOST), so reading acquires implicitly; nothing else owns the device.
    d->acquired = true;
    switch (d->kind) {
        case Kind::Keyboard: {
            uint8_t keys[256];
            input::game_keys(keys);
            memcpy(mem::ptr(out), keys, std::min<uint32_t>(size, 256));
            break;
        }
        case Kind::Mouse: memset(mem::ptr(out), 0, size); break;
        case Kind::Pad: pad_state(d, out, size); break;
    }
    ret(c, DI_OK);
}
void dev_GetDeviceData(cpu::Cpu& c) {
    // Buffered input: keyboard transitions from the input event log. For an
    // action-mapped device each event carries the game's uAppData (one event
    // per action bound to that key); unmapped keys are not reported.
    Device* d = dv(c);
    uint32_t objsize = arg(c, 1), out = arg(c, 2), pinout = arg(c, 3), flags = arg(c, 4);
    // The game reads its joystick without ever calling Acquire (and only re-acquires
    // on DIERR_INPUTLOST), so reading acquires implicitly; nothing else owns the device.
    d->acquired = true;
    uint32_t cap = mem::rd32(pinout), n = 0;
    if (d->kind == Kind::Keyboard) {
        uint32_t now = (uint32_t)(winu::mono_ns() / 1000000ull);
        bool peek = flags & 1;  // DIGDD_PEEK
        uint64_t cursor = d->key_cursor;
        input::KeyEvent ev;
        auto emit = [&](uint32_t ofs, uint32_t data, uint32_t app) {
            if (out) {
                uint32_t e = out + n * objsize;
                memset(mem::ptr(e), 0, objsize);
                mem::wr32(e + 0, ofs);
                mem::wr32(e + 4, data);
                mem::wr32(e + 8, now);
                mem::wr32(e + 12, d->sequence++);
                if (objsize >= 20) mem::wr32(e + 16, app);
            }
            ++n;
        };
        while ((n < cap || !out) && input::next_key_event(&cursor, &ev)) {
            uint32_t data = ev.down ? 0x80 : 0;
            if (d->action_mapped) {
                for (const BoundAction& a : d->actions)
                    if (a.object == ev.dik && (n < cap || !out)) emit(ev.dik, data, a.app_data);
            } else {
                emit(ev.dik, data, 0xFFFFFFFFu);
            }
            LOGI("input: dinput kbd %p event dik=%02x %s (mapped=%d, delivered=%u, peek=%d)", (void*)d, ev.dik,
                 ev.down ? "down" : "up", d->action_mapped ? 1 : 0, n, peek ? 1 : 0);
        }
        if (!peek) d->key_cursor = cursor;
    }
    mem::wr32(pinout, n);
    ret(c, DI_OK);
}
void dev_SetDataFormat(cpu::Cpu& c) {
    Device* d = dv(c);
    d->data_size = mem::rd32(arg(c, 1) + 12);
    ret(c, DI_OK);
}
void dev_GetDeviceInfo(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t p = arg(c, 1);
    uint32_t cap = mem::rd32(p);
    if (cap != kDevInstSize && cap != 0x234) return ret(c, DIERR_INVALIDPARAM);
    switch (d->kind) {
        case Kind::Keyboard: fill_instance(p, GUID_SysKeyboard, GUID_SysKeyboard, 0x0413, "Keyboard", cap); break;
        case Kind::Mouse: fill_instance(p, GUID_SysMouse, GUID_SysMouse, 0x0212, "Mouse", cap); break;
        case Kind::Pad:
            fill_instance(p, GUID_Pad, GUID_PadProd, 0x0115, "Controller (Android Gamepad)", cap);
            break;
    }
    ret(c, DI_OK);
}
void dev_ok(cpu::Cpu& c) { ret(c, DI_OK); }

// ---------------------------------------------------------------- objects / action mapping
const uint32_t GUID_Key[4] = {0x55728220, 0x11CFD33C, 0x4544C7BF, 0x00005453};

struct PadObject {
    const uint32_t* guid;
    uint32_t ofs, type;
    const char* name;
};
const PadObject kPadObjects[] = {
    {GUID_XAxis, 0, 0x00000002 | (0 << 8), "X Axis"},      {GUID_YAxis, 4, 0x00000002 | (1 << 8), "Y Axis"},
    {GUID_ZAxis, 8, 0x00000002 | (2 << 8), "Z Axis"},      {GUID_RxAxis, 12, 0x00000002 | (3 << 8), "X Rotation"},
    {GUID_RyAxis, 16, 0x00000002 | (4 << 8), "Y Rotation"}, {GUID_POV, 32, 0x00000010 | (0 << 8), "Hat Switch"},
    {GUID_Button, 48, 0x0000000C | (0 << 8), "A"},          {GUID_Button, 49, 0x0000000C | (1 << 8), "B"},
    {GUID_Button, 50, 0x0000000C | (2 << 8), "X"},          {GUID_Button, 51, 0x0000000C | (3 << 8), "Y"},
    {GUID_Button, 52, 0x0000000C | (4 << 8), "LB"},         {GUID_Button, 53, 0x0000000C | (5 << 8), "RB"},
    {GUID_Button, 54, 0x0000000C | (6 << 8), "Back"},       {GUID_Button, 55, 0x0000000C | (7 << 8), "Start"},
    {GUID_Button, 56, 0x0000000C | (8 << 8), "LS"},         {GUID_Button, 57, 0x0000000C | (9 << 8), "RS"},
};

void fill_object(uint32_t p, const uint32_t* guid, uint32_t ofs, uint32_t type, const char* name) {
    uint32_t cap = mem::rd32(p);
    if (cap < 0x13C) cap = 0x13C;
    memset(mem::ptr(p + 4), 0, std::min<uint32_t>(cap, 0x13C) - 4);
    write_guid(p + 4, guid);
    mem::wr32(p + 20, ofs);
    mem::wr32(p + 24, type);
    mem::write_cstr(p + 32, name, 260);
}

void dev_GetObjectInfo(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t p = arg(c, 1), obj = arg(c, 2), how = arg(c, 3);
    if (d->kind == Kind::Keyboard) {
        uint32_t dik = how == 1 ? obj : (obj >> 8) & 0xFF;
        if (dik == 0 || dik > 255) return ret(c, DIERR_NOTFOUND);
        fill_object(p, GUID_Key, dik, 0x0000000C | (dik << 8), "Key");
        return ret(c, DI_OK);
    }
    if (d->kind == Kind::Pad) {
        for (const PadObject& o : kPadObjects) {
            // DIPH_BYOFFSET: data-format offset; DIPH_BYID: same instance and type class.
            bool match = how == 1 ? o.ofs == obj
                                  : ((o.type >> 8) & 0xFFFF) == ((obj >> 8) & 0xFFFF) && (o.type & obj & 0xFF);
            if (match) {
                fill_object(p, o.guid, o.ofs, o.type, o.name);
                return ret(c, DI_OK);
            }
        }
    }
    ret(c, DIERR_NOTFOUND);
}

// DIACTIONFORMATA: dwSize, dwActionSize, dwDataSize, dwNumActions, rgoAction, guidActionMap(16),
// dwGenre @36, dwBufferSize @40. DIACTIONA (40 bytes): uAppData, dwSemantic, dwFlags, name,
// guidInstance @16, dwObjID @32, dwHow @36.
constexpr uint32_t DIA_APPMAPPED = 0x2, DIA_APPNOMAP = 0x4, DIAH_APPREQUESTED = 0x1, DIAH_DEFAULT = 0x20;

void log_action_format(uint32_t f) {
    uint32_t n = mem::rd32(f + 12), acts = mem::rd32(f + 16), asz = mem::rd32(f + 4);
    LOGI("DirectInput action format: genre %08x, %u actions, buffer %u", mem::rd32(f + 36), n, mem::rd32(f + 40));
    for (uint32_t i = 0; i < n && i < 128; ++i) {
        uint32_t a = acts + i * asz;
        LOGI("  action %2u: appdata %08x semantic %08x flags %x", i, mem::rd32(a), mem::rd32(a + 4), mem::rd32(a + 8));
    }
}

// Binds keyboard semantics (DIKEYBOARD_* = 0x81000400 | DIK) to this keyboard.
uint32_t build_keyboard_map(uint32_t f) {
    uint32_t n = mem::rd32(f + 12), acts = mem::rd32(f + 16), asz = mem::rd32(f + 4), mapped = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = acts + i * asz;
        uint32_t sem = mem::rd32(a + 4), fl = mem::rd32(a + 8);
        if (fl & DIA_APPNOMAP) continue;
        if ((fl & DIA_APPMAPPED) && com::guid_eq(a + 16, GUID_SysKeyboard)) {
            mem::wr32(a + 36, DIAH_APPREQUESTED);
            ++mapped;
            continue;
        }
        if ((sem & 0xFF000000u) != 0x81000000u) continue;
        uint32_t dik = sem & 0xFF;
        if (!dik) continue;
        write_guid(a + 16, GUID_SysKeyboard);
        mem::wr32(a + 32, (dik << 8) | 0x0C);
        mem::wr32(a + 36, DIAH_DEFAULT);
        ++mapped;
    }
    return mapped;
}

void dev_BuildActionMap(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t f = arg(c, 1);
    {
        static std::vector<uint32_t> seen;
        if (std::find(seen.begin(), seen.end(), f) == seen.end()) {
            seen.push_back(f);
            LOGI("DirectInput BuildActionMap format @%08x:", f);
            log_action_format(f);
        }
    }
    uint32_t mapped = d->kind == Kind::Keyboard ? build_keyboard_map(f) : 0;
    LOGI("DirectInput BuildActionMap: %u actions mapped to the %s", mapped,
         d->kind == Kind::Keyboard ? "keyboard" : "device");
    ret(c, mapped ? DI_OK : DI_NOEFFECT);
}

void dev_SetActionMap(cpu::Cpu& c) {
    Device* d = dv(c);
    uint32_t f = arg(c, 1);
    uint32_t n = mem::rd32(f + 12), acts = mem::rd32(f + 16), asz = mem::rd32(f + 4);
    d->actions.clear();
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = acts + i * asz;
        if (!mem::rd32(a + 36)) continue;  // dwHow: not mapped
        if (d->kind == Kind::Keyboard && com::guid_eq(a + 16, GUID_SysKeyboard))
            d->actions.push_back({mem::rd32(a), (mem::rd32(a + 32) >> 8) & 0xFF});
    }
    d->action_mapped = true;
    d->buffer_size = mem::rd32(f + 40);
    LOGI("DirectInput SetActionMap: %zu keyboard bindings active", d->actions.size());
    ret(c, DI_OK);
}

// EnumDevicesBySemantics(this, user, actionFormat, callback, ref, flags): offers the
// system keyboard (created by DirectInput, passed to the callback) for action mapping.
// The gamepad is read by the game through its own DirectInput joystick path.
void di_EnumDevicesBySemantics(cpu::Cpu& c) {
    uint32_t f = arg(c, 2), cb = arg(c, 3), ref = arg(c, 4);
    if (f) log_action_format(f);
    static const uint32_t inst = mem::alloc_system(kDevInstSize);
    fill_instance(inst, GUID_SysKeyboard, GUID_SysKeyboard, 0x0413, "Keyboard");
    Device* kb = new Device();
    kb->kind = Kind::Keyboard;
    com::wrap(kb, device_vtable());
    uint32_t r = hle::call_guest(c, cb, {inst, kb->guest, 1u /* DIEDBS_MAPPEDPRI1 */, 0u, ref});
    if (c.stop != cpu::Stop::None) return;
    com::release(kb);  // the game AddRef'd it if it keeps the device
    (void)r;
    ret(c, DI_OK);
}

void dev_CreateEffect(cpu::Cpu& c) {
    uint32_t g = arg(c, 1), out = arg(c, 3);
    Effect* e = new Effect();
    if (g) memcpy(e->guid, mem::ptr(g), 16);
    mem::wr32(out, com::wrap(e, effect_vtable()));
    ret(c, DI_OK);
}

Effect* fx(cpu::Cpu& c) { return static_cast<Effect*>(com::from_guest(arg(c, 0))); }
void fx_GetEffectGuid(cpu::Cpu& c) {
    memcpy(mem::ptr(arg(c, 1)), fx(c)->guid, 16);
    ret(c, DI_OK);
}
void fx_Start(cpu::Cpu& c) {
    fx(c)->playing = true;
    ret(c, DI_OK);
}
void fx_Stop(cpu::Cpu& c) {
    fx(c)->playing = false;
    ret(c, DI_OK);
}
void fx_GetEffectStatus(cpu::Cpu& c) {
    mem::wr32(arg(c, 1), fx(c)->playing ? 1 : 0);
    ret(c, DI_OK);
}

const com::Method kEffectMethods[] = {
    {"IDirectInputEffect::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirectInputEffect::AddRef", 4, com::AddRef, 0},
    {"IDirectInputEffect::Release", 4, com::Release, 0},
    {"IDirectInputEffect::Initialize", 16, dev_ok, 0},
    {"IDirectInputEffect::GetEffectGuid", 8, fx_GetEffectGuid, 0},
    {"IDirectInputEffect::GetParameters", 12, dev_ok, 0},
    {"IDirectInputEffect::SetParameters", 12, dev_ok, 0},
    {"IDirectInputEffect::Start", 12, fx_Start, 0},
    {"IDirectInputEffect::Stop", 4, fx_Stop, 0},
    {"IDirectInputEffect::GetEffectStatus", 8, fx_GetEffectStatus, 0},
    {"IDirectInputEffect::Download", 4, dev_ok, 0},
    {"IDirectInputEffect::Unload", 4, dev_ok, 0},
    {"IDirectInputEffect::Escape", 8, nullptr, DIERR_UNSUPPORTED},
};

const com::Method kDeviceMethods[] = {
    {"IDirectInputDevice8A::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirectInputDevice8A::AddRef", 4, com::AddRef, 0},
    {"IDirectInputDevice8A::Release", 4, com::Release, 0},
    {"IDirectInputDevice8A::GetCapabilities", 8, dev_GetCapabilities, 0},
    {"IDirectInputDevice8A::EnumObjects", 16, dev_EnumObjects, 0},
    {"IDirectInputDevice8A::GetProperty", 12, dev_GetProperty, 0},
    {"IDirectInputDevice8A::SetProperty", 12, dev_SetProperty, 0},
    {"IDirectInputDevice8A::Acquire", 4, dev_Acquire, 0},
    {"IDirectInputDevice8A::Unacquire", 4, dev_Unacquire, 0},
    {"IDirectInputDevice8A::GetDeviceState", 12, dev_GetDeviceState, 0},
    {"IDirectInputDevice8A::GetDeviceData", 20, dev_GetDeviceData, 0},
    {"IDirectInputDevice8A::SetDataFormat", 8, dev_SetDataFormat, 0},
    {"IDirectInputDevice8A::SetEventNotification", 8, dev_ok, 0},
    {"IDirectInputDevice8A::SetCooperativeLevel", 12, dev_ok, 0},
    {"IDirectInputDevice8A::GetObjectInfo", 16, dev_GetObjectInfo, 0},
    {"IDirectInputDevice8A::GetDeviceInfo", 8, dev_GetDeviceInfo, 0},
    {"IDirectInputDevice8A::RunControlPanel", 12, nullptr, 0},
    {"IDirectInputDevice8A::Initialize", 16, dev_ok, 0},
    {"IDirectInputDevice8A::CreateEffect", 20, dev_CreateEffect, 0},
    {"IDirectInputDevice8A::EnumEffects", 16, dev_ok, 0},
    {"IDirectInputDevice8A::GetEffectInfo", 12, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::GetForceFeedbackState", 8, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::SendForceFeedbackCommand", 8, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::EnumCreatedEffectObjects", 16, dev_ok, 0},
    {"IDirectInputDevice8A::Escape", 8, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::Poll", 4, dev_ok, 0},
    {"IDirectInputDevice8A::SendDeviceData", 20, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::EnumEffectsInFile", 20, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::WriteEffectToFile", 20, nullptr, DIERR_UNSUPPORTED},
    {"IDirectInputDevice8A::BuildActionMap", 16, dev_BuildActionMap, 0},
    {"IDirectInputDevice8A::SetActionMap", 16, dev_SetActionMap, 0},
    {"IDirectInputDevice8A::GetImageInfo", 8, nullptr, DIERR_UNSUPPORTED},
};

std::mutex g_vt_mu;
uint32_t dinput_vtable() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kDInputMethods, sizeof kDInputMethods / sizeof kDInputMethods[0]);
    return vt;
}
uint32_t device_vtable() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kDeviceMethods, sizeof kDeviceMethods / sizeof kDeviceMethods[0]);
    return vt;
}
uint32_t effect_vtable() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kEffectMethods, sizeof kEffectMethods / sizeof kEffectMethods[0]);
    return vt;
}
}  // namespace

HLE_EXPORT(dinput8, DirectInput8Create, 20) {
    uint32_t out = arg(c, 3);
    DInput* di = new DInput();
    mem::wr32(out, com::wrap(di, dinput_vtable()));
    ret(c, DI_OK);
}
