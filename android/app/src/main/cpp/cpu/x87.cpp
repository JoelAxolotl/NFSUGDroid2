// x87 FPU. Registers hold doubles: the game is built for 24/53-bit precision
// (D3D sets the FPU to single precision), so 80-bit extended precision is not
// needed for correct results. Memory m80 operands are converted exactly where
// the value fits in a double.
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/decode.h"
#include "cpu/x87_ops.h"

namespace cpu {

void fsave_env(Cpu& c, uint32_t a) {
    // 32-bit protected-mode layout (28 bytes).
    mem::wr32(a + 0, 0xFFFF0000u | c.fcw);
    mem::wr32(a + 4, 0xFFFF0000u | ((c.fsw & ~0x3800) | ((c.ftop & 7) << 11)));
    uint32_t tag = 0;
    for (int i = 0; i < 8; ++i) tag |= ((c.fempty >> i) & 1 ? 3u : 0u) << (i * 2);
    mem::wr32(a + 8, 0xFFFF0000u | tag);
    for (int i = 3; i < 7; ++i) mem::wr32(a + i * 4, 0);
}

void fload_env(Cpu& c, uint32_t a) {
    c.fcw = mem::rd16(a + 0);
    uint16_t sw = mem::rd16(a + 4);
    c.ftop = (sw >> 11) & 7;
    c.fsw = sw & ~0x3800;
    uint16_t tag = mem::rd16(a + 8);
    c.fempty = 0;
    for (int i = 0; i < 8; ++i)
        if (((tag >> (i * 2)) & 3) == 3) c.fempty |= 1u << i;
}

void fpu_reset(Cpu& c) {
    c.fcw = 0x027F;  // Windows default: 53-bit precision, all exceptions masked
    c.fsw = 0;
    c.ftop = 0;
    c.fempty = 0xFF;
    for (double& d : c.fpr) d = 0.0;
}

bool exec_x87(Cpu& c, uint8_t op, uint32_t& pc, uint32_t seg) {
    uint8_t modrm = mem::rd8(pc);
    uint32_t ea = 0;
    if (modrm < 0xC0) {
        ModRM m;
        decode_modrm(c, pc, seg, m);
        ea = m.ea;
    } else {
        pc++;
    }
    return x87_core(c, op, modrm, ea);
}

bool x87_core(Cpu& c, uint8_t op, uint8_t modrm, uint32_t ea) { return x87_exec(c, op, modrm, ea); }

}  // namespace cpu
