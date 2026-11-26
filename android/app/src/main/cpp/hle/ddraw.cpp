// DirectDraw: only used by the game to probe video memory and the adapter.
#include <cstring>
#include <mutex>

#include "hle/com.h"
#include "hle/hle.h"

using hle::arg;
using hle::ret;

namespace {
constexpr uint32_t DD_OK = 0, DDERR_UNSUPPORTED = 0x80004001, E_NOINTERFACE = 0x80004002;
constexpr uint32_t kVidMem = 256u << 20;

// IDirectDraw2 {B3A6F3E0-2B43-11CF-A2DE-00AA00B93356}, IDirectDraw4 {9C59509A-39BD-11D1-8C4A-00C04FD930C5},
// IDirectDraw7 {15E65EC0-3B9C-11D2-B92F-00609797EA5B}
const uint32_t kIIDs2plus[12] = {0xB3A6F3E0, 0x11CF2B43, 0xAA00DEA2, 0x5633B900,
                                 0x9C59509A, 0x11D139BD, 0xC0004A8C, 0xC530D94F,
                                 0x15E65EC0, 0x11D23B9C, 0x60002FB9, 0x5BEA9797};

struct DDraw : com::Object {};

uint32_t vtable_v1();
uint32_t vtable_v2();

void dd_QueryInterface(cpu::Cpu& c) {
    uint32_t riid = arg(c, 1), out = arg(c, 2);
    for (int i = 0; i < 3; ++i) {
        if (com::guid_eq(riid, kIIDs2plus + i * 4)) {
            // IDirectDraw2/4/7 differ from IDirectDraw in method signatures
            // (SetDisplayMode), so they get their own object and vtable.
            DDraw* v2 = new DDraw();
            mem::wr32(out, com::wrap(v2, vtable_v2()));
            return ret(c, DD_OK);
        }
    }
    mem::wr32(out, 0);
    ret(c, E_NOINTERFACE);
}

void dd_GetCaps(cpu::Cpu& c) {
    for (int i = 1; i <= 2; ++i) {
        uint32_t p = arg(c, i);
        if (!p) continue;
        uint32_t sz = mem::rd32(p);
        memset(mem::ptr(p + 4), 0, sz > 4 ? sz - 4 : 0);
        if (sz >= 68) {
            mem::wr32(p + 60, kVidMem);
            mem::wr32(p + 64, kVidMem);
        }
    }
    ret(c, DD_OK);
}
void dd_GetAvailableVidMem(cpu::Cpu& c) {
    if (arg(c, 2)) mem::wr32(arg(c, 2), kVidMem);
    if (arg(c, 3)) mem::wr32(arg(c, 3), kVidMem);
    ret(c, DD_OK);
}
void dd_GetDeviceIdentifier(cpu::Cpu& c) {
    uint32_t p = arg(c, 1);
    memset(mem::ptr(p), 0, 1072);
    mem::write_cstr(p, "ati2dvag.dll", 512);
    mem::write_cstr(p + 512, "RADEON 9800 PRO (GLES translation layer)", 512);
    mem::wr32(p + 1032, 0x1002);
    mem::wr32(p + 1036, 0x4E48);
    ret(c, DD_OK);
}
void dd_ok(cpu::Cpu& c) { ret(c, DD_OK); }

#define DD_COMMON(P)                                                    \
    {P "QueryInterface", 12, dd_QueryInterface, 0}, {P "AddRef", 4, com::AddRef, 0},   \
        {P "Release", 4, com::Release, 0}, {P "Compact", 4, dd_ok, 0},                  \
        {P "CreateClipper", 16, nullptr, DDERR_UNSUPPORTED},                            \
        {P "CreatePalette", 20, nullptr, DDERR_UNSUPPORTED},                            \
        {P "CreateSurface", 16, nullptr, DDERR_UNSUPPORTED},                            \
        {P "DuplicateSurface", 12, nullptr, DDERR_UNSUPPORTED},                         \
        {P "EnumDisplayModes", 20, dd_ok, 0}, {P "EnumSurfaces", 20, dd_ok, 0},         \
        {P "FlipToGDISurface", 4, dd_ok, 0}, {P "GetCaps", 12, dd_GetCaps, 0},          \
        {P "GetDisplayMode", 8, nullptr, DDERR_UNSUPPORTED},                            \
        {P "GetFourCCCodes", 12, nullptr, DDERR_UNSUPPORTED},                           \
        {P "GetGDISurface", 8, nullptr, DDERR_UNSUPPORTED},                             \
        {P "GetMonitorFrequency", 8, nullptr, DDERR_UNSUPPORTED},                       \
        {P "GetScanLine", 8, nullptr, DDERR_UNSUPPORTED},                               \
        {P "GetVerticalBlankStatus", 8, nullptr, DDERR_UNSUPPORTED},                    \
        {P "Initialize", 8, dd_ok, 0}, {P "RestoreDisplayMode", 4, dd_ok, 0},           \
        {P "SetCooperativeLevel", 12, dd_ok, 0}

const com::Method kV1[] = {
    DD_COMMON("IDirectDraw::"),
    {"IDirectDraw::SetDisplayMode", 16, dd_ok, 0},
    {"IDirectDraw::WaitForVerticalBlank", 12, dd_ok, 0},
};
const com::Method kV2[] = {
    DD_COMMON("IDirectDraw7::"),
    {"IDirectDraw7::SetDisplayMode", 24, dd_ok, 0},
    {"IDirectDraw7::WaitForVerticalBlank", 12, dd_ok, 0},
    {"IDirectDraw7::GetAvailableVidMem", 16, dd_GetAvailableVidMem, 0},
    {"IDirectDraw7::GetSurfaceFromDC", 12, nullptr, DDERR_UNSUPPORTED},
    {"IDirectDraw7::RestoreAllSurfaces", 4, dd_ok, 0},
    {"IDirectDraw7::TestCooperativeLevel", 4, dd_ok, 0},
    {"IDirectDraw7::GetDeviceIdentifier", 12, dd_GetDeviceIdentifier, 0},
    {"IDirectDraw7::StartModeTest", 16, nullptr, DDERR_UNSUPPORTED},
    {"IDirectDraw7::EvaluateMode", 12, nullptr, DDERR_UNSUPPORTED},
};

std::mutex g_vt_mu;
uint32_t vtable_v1() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kV1, sizeof kV1 / sizeof kV1[0]);
    return vt;
}
uint32_t vtable_v2() {
    static uint32_t vt = 0;
    std::lock_guard<std::mutex> lk(g_vt_mu);
    if (!vt) vt = com::make_vtable(kV2, sizeof kV2 / sizeof kV2[0]);
    return vt;
}
}  // namespace

HLE_EXPORT(ddraw, DirectDrawCreate, 12) {
    DDraw* d = new DDraw();
    mem::wr32(arg(c, 1), com::wrap(d, vtable_v1()));
    LOGI("DirectDrawCreate");
    ret(c, DD_OK);
}
