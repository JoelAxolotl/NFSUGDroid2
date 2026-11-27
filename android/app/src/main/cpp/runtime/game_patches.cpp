#include "runtime/game_patches.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/memory.h"
#include "cpu/x87_ops.h"
#include "runtime/aot.h"

namespace game {

namespace {
// Width / height for each entry of the game's resolution option (it has six slots).
constexpr uint32_t kResolutions[6][2] = {
    {854, 480}, {960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
};
constexpr uint32_t kRacingResolution = 0x00870D1C;  // g_RacingResolution (option index)

// Horizontal projection scale chosen by patch_projection_aspect for the view being set up
// (the game sets up views on its main thread only).
double g_hscale = 1.0;

bool bytes_match(uint32_t addr, const char* hex) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        sscanf(hex + i * 2, "%2x", &v);
        if (mem::rd8(addr + (uint32_t)i) != v) return false;
    }
    return true;
}
}  // namespace

// GetRacingResolution(int* width, int* height), stdcall.
void patch_racing_resolution(cpu::Cpu& c) {
    uint32_t sp = c.r[cpu::ESP];
    uint32_t ret = mem::rd32(sp), pw = mem::rd32(sp + 4), ph = mem::rd32(sp + 8);
    uint32_t idx = mem::rd32(kRacingResolution);
    if (idx < 6) {
        mem::wr32(pw, kResolutions[idx][0]);
        mem::wr32(ph, kResolutions[idx][1]);
    }
    c.r[cpu::ESP] = sp + 12;
    c.eip = ret;
}

// Replaces `fild [eax+18h]; mov [esp+20h], 1.0; fidiv [eax+14h]` (viewport height / width,
// the vertical field-of-view factor; ecx = view type, eax = view). The game keeps the
// horizontal field of view and derives the vertical one from the viewport (Vert-). For a
// viewport wider than 4:3 this keeps the 4:3 vertical view instead and widens the
// horizontal one (Hor+). The rear-view mirror (type 3) keeps the original behaviour.
void patch_projection_aspect(cpu::Cpu& c) {
    const uint32_t view = c.r[cpu::EAX];
    const int32_t w = (int32_t)mem::rd32(view + 0x14), h = (int32_t)mem::rd32(view + 0x18);
    mem::wr32(c.r[cpu::ESP] + 0x20, 0x3F800000u);  // 1.0f
    double ratio = (double)h / (double)w;
    g_hscale = 1.0;
    if (c.r[cpu::ECX] != 3 && w > 0 && h > 0 && (int64_t)w * 3 > (int64_t)h * 4 + w / 64) {
        ratio = 0.75;
        g_hscale = (4.0 * h) / (3.0 * w);
    }
    cpu::fpush(c, ratio);
    c.eip = 0x005C7F64;
}

// Replaces `fdivr dword ptr [1.0f]` (horizontal scale = 1 / tan(half angle)).
void patch_projection_hscale(cpu::Cpu& c) {
    cpu::st(c, 0) = g_hscale / cpu::st(c, 0);
    c.eip = 0x005C7FEE;
}

int install_patches() {
    struct P {
        uint32_t addr;
        aot::Fn fn;
        const char* bytes;
        const char* name;
    };
    static const P kPatches[] = {
#define PATCH(addr, resume, fn, bytes) {addr, fn, bytes, #fn},
#include "runtime/game_patches.def"
#undef PATCH
    };
    int n = 0;
    for (const P& p : kPatches) {
        if (!bytes_match(p.addr, p.bytes)) {
            LOGW("game patch %s: unexpected code at %08x, not applied", p.name, p.addr);
            continue;
        }
        aot::register_patch(p.addr, p.fn);
        ++n;
    }
    LOGI("game patches: %d applied (16:9 resolutions, Hor+ projection)", n);
    return n;
}

}  // namespace game
