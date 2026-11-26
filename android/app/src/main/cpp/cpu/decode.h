#pragma once
// Shared x86-32 decoding helpers (ModRM/SIB, register views, operand access).
#include "cpu/cpu.h"

namespace cpu {

struct ModRM {
    uint8_t mod, reg, rm;
    bool isreg;
    uint32_t ea;
};

inline uint8_t fetch8(uint32_t& pc) { return mem::rd8(pc++); }
inline uint16_t fetch16(uint32_t& pc) {
    uint16_t v = mem::rd16(pc);
    pc += 2;
    return v;
}
inline uint32_t fetch32(uint32_t& pc) {
    uint32_t v = mem::rd32(pc);
    pc += 4;
    return v;
}

inline void decode_modrm(const Cpu& c, uint32_t& pc, uint32_t seg_base, ModRM& m) {
    uint8_t b = fetch8(pc);
    m.mod = b >> 6;
    m.reg = (b >> 3) & 7;
    m.rm = b & 7;
    m.isreg = m.mod == 3;
    if (m.isreg) {
        m.ea = 0;
        return;
    }
    uint32_t ea;
    if (m.rm == 4) {
        uint8_t sib = fetch8(pc);
        uint8_t scale = sib >> 6, index = (sib >> 3) & 7, basereg = sib & 7;
        if (basereg == 5 && m.mod == 0) {
            ea = fetch32(pc);
        } else {
            ea = c.r[basereg];
        }
        if (index != 4) ea += c.r[index] << scale;
    } else if (m.rm == 5 && m.mod == 0) {
        ea = fetch32(pc);
    } else {
        ea = c.r[m.rm];
    }
    if (m.mod == 1) {
        ea += (uint32_t)(int32_t)(int8_t)fetch8(pc);
    } else if (m.mod == 2) {
        ea += fetch32(pc);
    }
    m.ea = ea + seg_base;
}

inline uint32_t get_r8(const Cpu& c, int i) {
    return i < 4 ? (c.r[i] & 0xFF) : ((c.r[i - 4] >> 8) & 0xFF);
}
inline void set_r8(Cpu& c, int i, uint32_t v) {
    if (i < 4)
        c.r[i] = (c.r[i] & ~0xFFu) | (v & 0xFF);
    else
        c.r[i - 4] = (c.r[i - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
}

inline uint32_t rd_reg(const Cpu& c, int r, int sz) {
    if (sz == 4) return c.r[r];
    if (sz == 2) return c.r[r] & 0xFFFF;
    return get_r8(c, r);
}
inline void wr_reg(Cpu& c, int r, int sz, uint32_t v) {
    if (sz == 4)
        c.r[r] = v;
    else if (sz == 2)
        c.r[r] = (c.r[r] & 0xFFFF0000u) | (v & 0xFFFF);
    else
        set_r8(c, r, v);
}

inline uint32_t rd_mem(uint32_t a, int sz) {
    if (sz == 4) return mem::rd32(a);
    if (sz == 2) return mem::rd16(a);
    return mem::rd8(a);
}
inline void wr_mem(uint32_t a, int sz, uint32_t v) {
    if (sz == 4)
        mem::wr32(a, v);
    else if (sz == 2)
        mem::wr16(a, (uint16_t)v);
    else
        mem::wr8(a, (uint8_t)v);
}

inline uint32_t rd_rm(const Cpu& c, const ModRM& m, int sz) {
    return m.isreg ? rd_reg(c, m.rm, sz) : rd_mem(m.ea, sz);
}
inline void wr_rm(Cpu& c, const ModRM& m, int sz, uint32_t v) {
    if (m.isreg)
        wr_reg(c, m.rm, sz, v);
    else
        wr_mem(m.ea, sz, v);
}

inline uint32_t sext(uint32_t v, int sz) {
    if (sz == 1) return (uint32_t)(int32_t)(int8_t)v;
    if (sz == 2) return (uint32_t)(int32_t)(int16_t)v;
    return v;
}

}  // namespace cpu
