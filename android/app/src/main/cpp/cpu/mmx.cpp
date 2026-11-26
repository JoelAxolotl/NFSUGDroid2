// MMX (and the SSE integer extensions to MMX). Some of the game's code uses
// MMX unconditionally, regardless of the CPUID feature bits we report.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#include "cpu/cpu.h"
#include "cpu/decode.h"

namespace cpu {

namespace {
template <typename T>
T lane(uint64_t v, int i) {
    T r;
    memcpy(&r, (const uint8_t*)&v + i * sizeof(T), sizeof(T));
    return r;
}
template <typename T>
void set_lane(uint64_t& v, int i, T x) {
    memcpy((uint8_t*)&v + i * sizeof(T), &x, sizeof(T));
}

template <typename T, typename F>
uint64_t map2(uint64_t a, uint64_t b, F f) {
    uint64_t r = 0;
    for (int i = 0; i < (int)(8 / sizeof(T)); ++i) set_lane<T>(r, i, f(lane<T>(a, i), lane<T>(b, i)));
    return r;
}

template <typename T>
T sat(int64_t v) {
    if (v < (int64_t)std::numeric_limits<T>::min()) return std::numeric_limits<T>::min();
    if (v > (int64_t)std::numeric_limits<T>::max()) return std::numeric_limits<T>::max();
    return (T)v;
}

template <typename T>
uint64_t unpack_lo(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    const int n = 4 / sizeof(T);
    for (int i = 0; i < n; ++i) {
        set_lane<T>(r, 2 * i, lane<T>(a, i));
        set_lane<T>(r, 2 * i + 1, lane<T>(b, i));
    }
    return r;
}
template <typename T>
uint64_t unpack_hi(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    const int n = 4 / sizeof(T);
    for (int i = 0; i < n; ++i) {
        set_lane<T>(r, 2 * i, lane<T>(a, n + i));
        set_lane<T>(r, 2 * i + 1, lane<T>(b, n + i));
    }
    return r;
}

template <typename T>
uint64_t shift_lanes(uint64_t a, uint64_t cnt, int kind /*0 srl,1 sra,2 sll*/) {
    const uint64_t bits = sizeof(T) * 8;
    uint64_t r = 0;
    for (int i = 0; i < (int)(8 / sizeof(T)); ++i) {
        using U = std::make_unsigned_t<T>;
        using S = std::make_signed_t<T>;
        U v = (U)lane<T>(a, i);
        U out;
        if (kind == 0)
            out = cnt >= bits ? 0 : (U)(v >> cnt);
        else if (kind == 1)
            out = (U)((S)v >> (cnt >= bits ? bits - 1 : cnt));
        else
            out = cnt >= bits ? 0 : (U)(v << cnt);
        set_lane<T>(r, i, (T)out);
    }
    return r;
}
}  // namespace

// Returns false if `op` (the byte after 0x0F) is not an MMX instruction.
bool exec_mmx(Cpu& c, uint8_t op, uint32_t& pc, uint32_t seg) {
    ModRM m;
    auto src64 = [&]() -> uint64_t { return m.isreg ? c.mm[m.rm] : mem::rd<uint64_t>(m.ea); };
    switch (op) {
        case 0x6E:  // MOVD mm, r/m32
            decode_modrm(c, pc, seg, m);
            c.mm[m.reg] = rd_rm(c, m, 4);
            return true;
        case 0x7E:  // MOVD r/m32, mm
            decode_modrm(c, pc, seg, m);
            wr_rm(c, m, 4, (uint32_t)c.mm[m.reg]);
            return true;
        case 0x6F:  // MOVQ mm, mm/m64
            decode_modrm(c, pc, seg, m);
            c.mm[m.reg] = src64();
            return true;
        case 0x7F:  // MOVQ mm/m64, mm
        case 0xE7:  // MOVNTQ
            decode_modrm(c, pc, seg, m);
            if (m.isreg)
                c.mm[m.rm] = c.mm[m.reg];
            else
                mem::wr<uint64_t>(m.ea, c.mm[m.reg]);
            return true;
        case 0x71:
        case 0x72:
        case 0x73: {  // shift by immediate
            decode_modrm(c, pc, seg, m);
            uint64_t cnt = mem::rd8(pc++);
            uint64_t& r = c.mm[m.rm];
            int kind = m.reg == 2 ? 0 : m.reg == 4 ? 1 : m.reg == 6 ? 2 : -1;
            if (kind < 0) return false;
            if (op == 0x71) r = shift_lanes<int16_t>(r, cnt, kind);
            else if (op == 0x72) r = shift_lanes<int32_t>(r, cnt, kind);
            else if (kind == 1) return false;
            else r = shift_lanes<int64_t>(r, cnt, kind);
            return true;
        }
        case 0x70: {  // PSHUFW mm, mm/m64, imm8
            decode_modrm(c, pc, seg, m);
            uint64_t s = src64();
            uint8_t imm = mem::rd8(pc++);
            uint64_t r = 0;
            for (int i = 0; i < 4; ++i) set_lane<uint16_t>(r, i, lane<uint16_t>(s, (imm >> (i * 2)) & 3));
            c.mm[m.reg] = r;
            return true;
        }
        case 0xC4: {  // PINSRW mm, r32/m16, imm8
            decode_modrm(c, pc, seg, m);
            uint16_t v = (uint16_t)rd_rm(c, m, 2);
            uint8_t imm = mem::rd8(pc++);
            set_lane<uint16_t>(c.mm[m.reg], imm & 3, v);
            return true;
        }
        case 0xC5: {  // PEXTRW r32, mm, imm8
            decode_modrm(c, pc, seg, m);
            uint8_t imm = mem::rd8(pc++);
            c.r[m.reg] = lane<uint16_t>(c.mm[m.rm], imm & 3);
            return true;
        }
        case 0xD7: {  // PMOVMSKB r32, mm
            decode_modrm(c, pc, seg, m);
            uint32_t r = 0;
            for (int i = 0; i < 8; ++i) r |= ((lane<uint8_t>(c.mm[m.rm], i) >> 7) & 1u) << i;
            c.r[m.reg] = r;
            return true;
        }
        default: break;
    }

    // Two-operand forms: mm, mm/m64.
    decode_modrm(c, pc, seg, m);
    const uint64_t b = src64();
    uint64_t& a = c.mm[m.reg];
    switch (op) {
        case 0x60: a = unpack_lo<uint8_t>(a, b); break;
        case 0x61: a = unpack_lo<uint16_t>(a, b); break;
        case 0x62: a = unpack_lo<uint32_t>(a, b); break;
        case 0x68: a = unpack_hi<uint8_t>(a, b); break;
        case 0x69: a = unpack_hi<uint16_t>(a, b); break;
        case 0x6A: a = unpack_hi<uint32_t>(a, b); break;
        case 0x63: {  // PACKSSWB
            uint64_t r = 0;
            for (int i = 0; i < 4; ++i) set_lane<int8_t>(r, i, sat<int8_t>(lane<int16_t>(a, i)));
            for (int i = 0; i < 4; ++i) set_lane<int8_t>(r, 4 + i, sat<int8_t>(lane<int16_t>(b, i)));
            a = r;
            break;
        }
        case 0x67: {  // PACKUSWB
            uint64_t r = 0;
            for (int i = 0; i < 4; ++i) set_lane<uint8_t>(r, i, sat<uint8_t>(lane<int16_t>(a, i)));
            for (int i = 0; i < 4; ++i) set_lane<uint8_t>(r, 4 + i, sat<uint8_t>(lane<int16_t>(b, i)));
            a = r;
            break;
        }
        case 0x6B: {  // PACKSSDW
            uint64_t r = 0;
            for (int i = 0; i < 2; ++i) set_lane<int16_t>(r, i, sat<int16_t>(lane<int32_t>(a, i)));
            for (int i = 0; i < 2; ++i) set_lane<int16_t>(r, 2 + i, sat<int16_t>(lane<int32_t>(b, i)));
            a = r;
            break;
        }
        case 0x64: a = map2<int8_t>(a, b, [](int8_t x, int8_t y) { return (int8_t)(x > y ? -1 : 0); }); break;
        case 0x65: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return (int16_t)(x > y ? -1 : 0); }); break;
        case 0x66: a = map2<int32_t>(a, b, [](int32_t x, int32_t y) { return (int32_t)(x > y ? -1 : 0); }); break;
        case 0x74: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return (uint8_t)(x == y ? 0xFF : 0); }); break;
        case 0x75: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return (uint16_t)(x == y ? 0xFFFF : 0); }); break;
        case 0x76: a = map2<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x == y ? 0xFFFFFFFFu : 0u; }); break;
        case 0xD1: a = shift_lanes<int16_t>(a, b, 0); break;
        case 0xD2: a = shift_lanes<int32_t>(a, b, 0); break;
        case 0xD3: a = shift_lanes<int64_t>(a, b, 0); break;
        case 0xE1: a = shift_lanes<int16_t>(a, b, 1); break;
        case 0xE2: a = shift_lanes<int32_t>(a, b, 1); break;
        case 0xF1: a = shift_lanes<int16_t>(a, b, 2); break;
        case 0xF2: a = shift_lanes<int32_t>(a, b, 2); break;
        case 0xF3: a = shift_lanes<int64_t>(a, b, 2); break;
        case 0xD5: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return (int16_t)(x * y); }); break;
        case 0xE5: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return (int16_t)(((int32_t)x * y) >> 16); }); break;
        case 0xE4: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return (uint16_t)(((uint32_t)x * y) >> 16); }); break;
        case 0xF5: {  // PMADDWD
            uint64_t r = 0;
            for (int i = 0; i < 2; ++i) {
                int32_t s = (int32_t)lane<int16_t>(a, 2 * i) * lane<int16_t>(b, 2 * i) +
                            (int32_t)lane<int16_t>(a, 2 * i + 1) * lane<int16_t>(b, 2 * i + 1);
                set_lane<int32_t>(r, i, s);
            }
            a = r;
            break;
        }
        case 0xD8: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return sat<uint8_t>((int)x - y); }); break;
        case 0xD9: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return sat<uint16_t>((int)x - y); }); break;
        case 0xDC: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return sat<uint8_t>((int)x + y); }); break;
        case 0xDD: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return sat<uint16_t>((int)x + y); }); break;
        case 0xE8: a = map2<int8_t>(a, b, [](int8_t x, int8_t y) { return sat<int8_t>((int)x - y); }); break;
        case 0xE9: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return sat<int16_t>((int)x - y); }); break;
        case 0xEC: a = map2<int8_t>(a, b, [](int8_t x, int8_t y) { return sat<int8_t>((int)x + y); }); break;
        case 0xED: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return sat<int16_t>((int)x + y); }); break;
        case 0xF8: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return (uint8_t)(x - y); }); break;
        case 0xF9: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return (uint16_t)(x - y); }); break;
        case 0xFA: a = map2<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x - y; }); break;
        case 0xFB: a = a - b; break;  // PSUBQ
        case 0xFC: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return (uint8_t)(x + y); }); break;
        case 0xFD: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return (uint16_t)(x + y); }); break;
        case 0xFE: a = map2<uint32_t>(a, b, [](uint32_t x, uint32_t y) { return x + y; }); break;
        case 0xD4: a = a + b; break;  // PADDQ
        case 0xDB: a &= b; break;
        case 0xDF: a = ~a & b; break;
        case 0xEB: a |= b; break;
        case 0xEF: a ^= b; break;
        case 0xDA: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return std::min(x, y); }); break;
        case 0xDE: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return std::max(x, y); }); break;
        case 0xEA: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return std::min(x, y); }); break;
        case 0xEE: a = map2<int16_t>(a, b, [](int16_t x, int16_t y) { return std::max(x, y); }); break;
        case 0xE0: a = map2<uint8_t>(a, b, [](uint8_t x, uint8_t y) { return (uint8_t)((x + y + 1) >> 1); }); break;
        case 0xE3: a = map2<uint16_t>(a, b, [](uint16_t x, uint16_t y) { return (uint16_t)((x + y + 1) >> 1); }); break;
        case 0xF6: {  // PSADBW
            uint32_t s = 0;
            for (int i = 0; i < 8; ++i) s += (uint32_t)std::abs((int)lane<uint8_t>(a, i) - (int)lane<uint8_t>(b, i));
            a = s;
            break;
        }
        default: return false;
    }
    return true;
}

}  // namespace cpu
