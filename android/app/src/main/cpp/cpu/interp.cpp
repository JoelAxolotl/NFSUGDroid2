// x86-32 reference interpreter. It is the correctness baseline for the AOT
// translator and the fallback for code the translator has not covered.
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/decode.h"
#include "cpu/ops.h"
#include "hle/thunks.h"
#include "runtime/aot.h"
#include "runtime/process.h"

namespace cpu {

namespace {
thread_local Cpu* t_current = nullptr;

std::atomic_flag g_bus_lock = ATOMIC_FLAG_INIT;

struct Prefix {
    int osz = 4;        // operand size for "v" operands
    uint32_t seg = 0;   // segment base (FS) or 0
    uint8_t rep = 0;    // 0, 0xF2, 0xF3
    bool lock = false;
};

void fault(Cpu& c, uint32_t at, const char* what) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = mem::rd8(at + i);
    LOGE("CPU fault at %08x: %s [%02x %02x %02x %02x %02x %02x %02x %02x]", at, what, b[0], b[1], b[2],
         b[3], b[4], b[5], b[6], b[7]);
    char d[512];
    dump(c, d, sizeof d);
    LOGE("%s", d);
    c.eip = at;
    c.stop = Stop::Fault;
}

// --- String instructions ----------------------------------------------------
void string_op_impl(Cpu& c, uint8_t op, int sz, uint8_t rep, uint32_t seg) {
    const int32_t step = (c.eflags & DF) ? -sz : sz;
    const uint32_t src_seg = seg;  // only DS-relative source may be overridden
    auto once = [&]() -> bool {    // returns false when a REPE/REPNE condition ends the loop
        switch (op) {
            case 0xA4:
            case 0xA5:
                wr_mem(c.r[EDI], sz, rd_mem(c.r[ESI] + src_seg, sz));
                c.r[ESI] += step;
                c.r[EDI] += step;
                return true;
            case 0xAA:
            case 0xAB:
                wr_mem(c.r[EDI], sz, c.r[EAX]);
                c.r[EDI] += step;
                return true;
            case 0xAC:
            case 0xAD:
                wr_reg(c, EAX, sz, rd_mem(c.r[ESI] + src_seg, sz));
                c.r[ESI] += step;
                return true;
            case 0xA6:
            case 0xA7: {
                uint32_t a = rd_mem(c.r[ESI] + src_seg, sz), b = rd_mem(c.r[EDI], sz);
                alu(c, 7, a, b, sz);
                c.r[ESI] += step;
                c.r[EDI] += step;
                return true;
            }
            default: {  // SCAS
                uint32_t b = rd_mem(c.r[EDI], sz);
                alu(c, 7, c.r[EAX], b, sz);
                c.r[EDI] += step;
                return true;
            }
        }
    };
    if (!rep) {
        once();
        return;
    }
    const bool cmp = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    // Fast paths for forward, non-overlapping MOVS / STOS.
    if (!cmp && step > 0 && src_seg == 0) {
        uint32_t n = c.r[ECX];
        if (n && (op == 0xA4 || op == 0xA5)) {
            uint32_t bytes = n * sz, s = c.r[ESI], d = c.r[EDI];
            if (d <= s || d >= s + bytes) {
                memmove(mem::ptr(d), mem::ptr(s), bytes);
                c.r[ESI] += bytes;
                c.r[EDI] += bytes;
                c.r[ECX] = 0;
                return;
            }
        } else if (n && (op == 0xAA || op == 0xAB)) {
            uint32_t d = c.r[EDI];
            if (sz == 1) {
                memset(mem::ptr(d), (int)(c.r[EAX] & 0xFF), n);
            } else if (sz == 4) {
                uint32_t v = c.r[EAX];
                if (v == 0) {
                    memset(mem::ptr(d), 0, (size_t)n * 4);
                } else {
                    for (uint32_t i = 0; i < n; ++i) mem::wr32(d + i * 4, v);
                }
            } else {
                for (uint32_t i = 0; i < n; ++i) mem::wr16(d + i * 2, (uint16_t)c.r[EAX]);
            }
            c.r[EDI] += n * sz;
            c.r[ECX] = 0;
            return;
        }
    }
    while (c.r[ECX] != 0) {
        once();
        c.r[ECX]--;
        if (cmp) {
            bool zf = get_zf(c);
            if (rep == 0xF3 && !zf) break;
            if (rep == 0xF2 && zf) break;
        }
    }
}

// --- CPUID ------------------------------------------------------------------
// Reports a P6-class CPU with x87, TSC, CX8 and CMOV but no MMX/SSE/3DNow!, so
// the game and D3DX take their plain x87 code paths.
void cpuid(Cpu& c) {
    uint32_t leaf = c.r[EAX];
    switch (leaf) {
        case 0:
            c.r[EAX] = 1;
            c.r[EBX] = 0x756E6547;  // "GenuineIntel"
            c.r[EDX] = 0x49656E69;
            c.r[ECX] = 0x6C65746E;
            break;
        case 1:
            c.r[EAX] = 0x00000686;  // family 6 model 8
            c.r[EBX] = 0;
            c.r[ECX] = 0;
            c.r[EDX] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15);
            break;
        case 0x80000000:
            c.r[EAX] = 0x80000000;
            c.r[EBX] = c.r[ECX] = c.r[EDX] = 0;
            break;
        default:
            c.r[EAX] = c.r[EBX] = c.r[ECX] = c.r[EDX] = 0;
            break;
    }
}

uint64_t rdtsc_value() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // Pretend to be a 2 GHz part.
    return ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * 2;
}

void do_cmpxchg8b(Cpu& c, const ModRM& m) {
    BusLock lk(true);
    uint64_t cur = mem::rd<uint64_t>(m.ea);
    uint64_t expect = ((uint64_t)c.r[EDX] << 32) | c.r[EAX];
    uint32_t f = materialize_flags(c);
    if (cur == expect) {
        mem::wr<uint64_t>(m.ea, ((uint64_t)c.r[ECX] << 32) | c.r[EBX]);
        c.eflags = f | ZF;
    } else {
        c.r[EAX] = (uint32_t)cur;
        c.r[EDX] = (uint32_t)(cur >> 32);
        c.eflags = f & ~ZF;
    }
}

// Executes the 0x0F-prefixed opcode at pc. Returns false on unsupported opcode.
bool exec_0f(Cpu& c, Prefix& p, uint32_t& pc, uint32_t start) {
    uint8_t op = fetch8(pc);
    ModRM m;
    const int osz = p.osz;
    if (op >= 0x80 && op <= 0x8F) {  // Jcc rel32
        int32_t rel = (int32_t)fetch32(pc);
        c.eip = cond(c, op & 0xF) ? pc + rel : pc;
        return true;
    }
    if (op >= 0x90 && op <= 0x9F) {  // SETcc
        decode_modrm(c, pc, p.seg, m);
        wr_rm(c, m, 1, cond(c, op & 0xF) ? 1 : 0);
        c.eip = pc;
        return true;
    }
    if (op >= 0x40 && op <= 0x4F) {  // CMOVcc
        decode_modrm(c, pc, p.seg, m);
        uint32_t v = rd_rm(c, m, osz);
        if (cond(c, op & 0xF)) wr_reg(c, m.reg, osz, v);
        c.eip = pc;
        return true;
    }
    if (op >= 0xC8 && op <= 0xCF) {  // BSWAP
        c.r[op & 7] = __builtin_bswap32(c.r[op & 7]);
        c.eip = pc;
        return true;
    }
    switch (op) {
        case 0x0B:  // UD2
            return false;
        case 0x0D:  // prefetch (3DNow!) / 0F 18 prefetchnta.. / 0F 1F NOP Ev
        case 0x18:
        case 0x19:
        case 0x1A:
        case 0x1B:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
            decode_modrm(c, pc, p.seg, m);
            break;
        case 0x31: {  // RDTSC
            uint64_t t = rdtsc_value();
            c.r[EAX] = (uint32_t)t;
            c.r[EDX] = (uint32_t)(t >> 32);
            break;
        }
        case 0xA2:
            cpuid(c);
            break;
        case 0xA3:  // BT Ev,Gv
        case 0xAB:  // BTS
        case 0xB3:  // BTR
        case 0xBB:  // BTC
        case 0xBA: {  // group 8: BT/BTS/BTR/BTC Ev,Ib
            decode_modrm(c, pc, p.seg, m);
            int kind;
            uint32_t bit;
            uint32_t ea = m.ea;
            if (op == 0xBA) {
                kind = m.reg - 4;
                bit = fetch8(pc) & (osz * 8 - 1);
                if (kind < 0) return false;
            } else {
                kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
                uint32_t off = rd_reg(c, m.reg, osz);
                if (!m.isreg) {
                    int32_t soff = osz == 2 ? (int16_t)off : (int32_t)off;
                    ea += (uint32_t)((soff >> (osz == 2 ? 4 : 5)) * osz);
                    bit = (uint32_t)soff & (osz * 8 - 1);
                } else {
                    bit = off & (osz * 8 - 1);
                }
            }
            ModRM mm = m;
            mm.ea = ea;
            BusLock lk(p.lock && !m.isreg);
            uint32_t v = rd_rm(c, mm, osz);
            uint32_t cf = (v >> bit) & 1;
            put_flags(c, CF, cf ? CF : 0);
            if (kind == 1) wr_rm(c, mm, osz, v | (1u << bit));
            if (kind == 2) wr_rm(c, mm, osz, v & ~(1u << bit));
            if (kind == 3) wr_rm(c, mm, osz, v ^ (1u << bit));
            break;
        }
        case 0xA4:  // SHLD Ev,Gv,Ib
        case 0xA5:  // SHLD Ev,Gv,CL
        case 0xAC:  // SHRD Ev,Gv,Ib
        case 0xAD: {  // SHRD Ev,Gv,CL
            decode_modrm(c, pc, p.seg, m);
            uint32_t cnt = (op == 0xA4 || op == 0xAC) ? fetch8(pc) : c.r[ECX];
            cnt &= 31;
            if (cnt && osz == 4) {
                uint32_t d = rd_rm(c, m, 4), s = rd_reg(c, m.reg, 4), r, cf;
                if (op <= 0xA5) {
                    r = (d << cnt) | (s >> (32 - cnt));
                    cf = (d >> (32 - cnt)) & 1;
                } else {
                    r = (d >> cnt) | (s << (32 - cnt));
                    cf = (d >> (cnt - 1)) & 1;
                }
                uint32_t of = ((r ^ d) & 0x80000000u) ? OF : 0;
                put_flags(c, kArithFlags, szp_flags(r, 4) | (cf ? CF : 0) | of);
                wr_rm(c, m, 4, r);
            } else if (cnt) {
                return false;  // 16-bit double shifts are not used by the game
            }
            break;
        }
        case 0xAF: {  // IMUL Gv,Ev
            decode_modrm(c, pc, p.seg, m);
            int64_t a = (int32_t)sext(rd_reg(c, m.reg, osz), osz);
            int64_t b = (int32_t)sext(rd_rm(c, m, osz), osz);
            int64_t r = a * b;
            uint32_t res = (uint32_t)r & size_mask((uint8_t)osz);
            bool ovf = (int64_t)(int32_t)sext(res, osz) != r;
            put_flags(c, kArithFlags, szp_flags(res, osz) | (ovf ? (CF | OF) : 0));
            wr_reg(c, m.reg, osz, res);
            break;
        }
        case 0xB0:  // CMPXCHG Eb,Gb
        case 0xB1: {  // CMPXCHG Ev,Gv
            int sz = op == 0xB0 ? 1 : osz;
            decode_modrm(c, pc, p.seg, m);
            BusLock lk(!m.isreg);
            uint32_t d = rd_rm(c, m, sz);
            uint32_t acc = rd_reg(c, EAX, sz);
            alu(c, 7, acc, d, sz);
            if ((acc & size_mask((uint8_t)sz)) == d)
                wr_rm(c, m, sz, rd_reg(c, m.reg, sz));
            else
                wr_reg(c, EAX, sz, d);
            break;
        }
        case 0xB6:  // MOVZX Gv,Eb
        case 0xB7:  // MOVZX Gv,Ew
        case 0xBE:  // MOVSX Gv,Eb
        case 0xBF: {  // MOVSX Gv,Ew
            decode_modrm(c, pc, p.seg, m);
            int ssz = (op & 1) ? 2 : 1;
            uint32_t v = rd_rm(c, m, ssz);
            if (op >= 0xBE) v = sext(v, ssz);
            wr_reg(c, m.reg, osz, v);
            break;
        }
        case 0xBC:  // BSF
        case 0xBD: {  // BSR
            decode_modrm(c, pc, p.seg, m);
            uint32_t v = rd_rm(c, m, osz);
            if (v == 0) {
                put_flags(c, ZF, ZF);
            } else {
                uint32_t idx = op == 0xBC ? (uint32_t)__builtin_ctz(v) : 31u - (uint32_t)__builtin_clz(v);
                put_flags(c, ZF, 0);
                wr_reg(c, m.reg, osz, idx);
            }
            break;
        }
        case 0xC0:  // XADD Eb,Gb
        case 0xC1: {  // XADD Ev,Gv
            int sz = op == 0xC0 ? 1 : osz;
            decode_modrm(c, pc, p.seg, m);
            BusLock lk(p.lock && !m.isreg);
            uint32_t d = rd_rm(c, m, sz), s = rd_reg(c, m.reg, sz);
            uint32_t r = alu(c, 0, d, s, sz);
            wr_reg(c, m.reg, sz, d);
            wr_rm(c, m, sz, r);
            break;
        }
        case 0xC7:  // CMPXCHG8B
            decode_modrm(c, pc, p.seg, m);
            if (m.reg != 1 || m.isreg) return false;
            do_cmpxchg8b(c, m);
            break;
        case 0xAE:  // fences / LDMXCSR / STMXCSR
            decode_modrm(c, pc, p.seg, m);
            if (m.isreg) break;  // LFENCE/MFENCE/SFENCE
            if (m.reg == 2) {
                c.mxcsr = mem::rd32(m.ea);
            } else if (m.reg == 3) {
                mem::wr32(m.ea, c.mxcsr);
            } else {
                return false;
            }
            break;
        case 0x77:  // EMMS
            c.fempty = 0xFF;
            break;
        default:
            // MMX forms have no 66/F2/F3 prefix (those select SSE2/SSE encodings).
            if (osz == 4 && !p.rep && exec_mmx(c, op, pc, p.seg)) break;
            return false;
    }
    c.eip = pc;
    return true;
}

// Executes one instruction at c.eip.
void step(Cpu& c) {
    const uint32_t start = c.eip;
    uint32_t pc = start;
    Prefix p;
    uint8_t op;
    for (;;) {
        op = fetch8(pc);
        switch (op) {
            case 0x66: p.osz = 2; continue;
            case 0xF0: p.lock = true; continue;
            case 0xF2:
            case 0xF3: p.rep = op; continue;
            case 0x64: p.seg = c.fs_base; continue;
            case 0x26:
            case 0x2E:
            case 0x36:
            case 0x3E:
            case 0x65: continue;  // flat segments
            case 0x67:
                fault(c, start, "16-bit addressing (0x67) unsupported");
                return;
            default: break;
        }
        break;
    }
    ModRM m;
    const int osz = p.osz;

    // ALU block 00-3F (except prefixes / BCD / segment push-pop)
    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3;
        int form = op & 7;
        int sz = (form & 1) ? osz : 1;
        switch (form) {
            case 0:
            case 1: {  // Eb,Gb / Ev,Gv
                decode_modrm(c, pc, p.seg, m);
                BusLock lk(p.lock && !m.isreg);
                uint32_t r = alu(c, aop, rd_rm(c, m, sz), rd_reg(c, m.reg, sz), sz);
                if (aop != 7) wr_rm(c, m, sz, r);
                break;
            }
            case 2:
            case 3: {  // Gb,Eb / Gv,Ev
                decode_modrm(c, pc, p.seg, m);
                uint32_t r = alu(c, aop, rd_reg(c, m.reg, sz), rd_rm(c, m, sz), sz);
                if (aop != 7) wr_reg(c, m.reg, sz, r);
                break;
            }
            case 4: {  // AL,Ib
                uint32_t r = alu(c, aop, c.r[EAX], fetch8(pc), 1);
                if (aop != 7) wr_reg(c, EAX, 1, r);
                break;
            }
            default: {  // eAX,Iz
                uint32_t imm = osz == 2 ? fetch16(pc) : fetch32(pc);
                uint32_t r = alu(c, aop, c.r[EAX], imm, osz);
                if (aop != 7) wr_reg(c, EAX, osz, r);
                break;
            }
        }
        c.eip = pc;
        return;
    }

    switch (op) {
        case 0x0F:
            if (!exec_0f(c, p, pc, start)) fault(c, start, "unsupported 0F opcode");
            return;
        case 0x27:
        case 0x2F:
        case 0x37:
        case 0x3F:
            fault(c, start, "BCD adjust");
            return;
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            wr_reg(c, op & 7, osz, inc_dec(c, rd_reg(c, op & 7, osz), false, osz));
            break;
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            wr_reg(c, op & 7, osz, inc_dec(c, rd_reg(c, op & 7, osz), true, osz));
            break;
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            if (osz == 2) {
                c.r[ESP] -= 2;
                mem::wr16(c.r[ESP], (uint16_t)c.r[op & 7]);
            } else {
                push32(c, c.r[op & 7]);  // PUSH ESP pushes the pre-decrement value
            }
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            if (osz == 2) {
                uint16_t v = mem::rd16(c.r[ESP]);
                c.r[ESP] += 2;
                wr_reg(c, op & 7, 2, v);
            } else {
                uint32_t v = pop32(c);
                c.r[op & 7] = v;
            }
            break;
        case 0x60: {  // PUSHAD
            uint32_t sp = c.r[ESP];
            for (int i = 0; i < 8; ++i) push32(c, i == ESP ? sp : c.r[i]);
            break;
        }
        case 0x61:  // POPAD
            for (int i = 7; i >= 0; --i) {
                uint32_t v = pop32(c);
                if (i != ESP) c.r[i] = v;
            }
            break;
        case 0x68:
            push32(c, osz == 2 ? fetch16(pc) : fetch32(pc));
            break;
        case 0x6A:
            push32(c, (uint32_t)(int32_t)(int8_t)fetch8(pc));
            break;
        case 0x69:
        case 0x6B: {  // IMUL Gv,Ev,imm
            decode_modrm(c, pc, p.seg, m);
            int64_t a = (int32_t)sext(rd_rm(c, m, osz), osz);
            int64_t b = op == 0x6B ? (int8_t)fetch8(pc) : (int32_t)sext(osz == 2 ? fetch16(pc) : fetch32(pc), osz);
            int64_t r = a * b;
            uint32_t res = (uint32_t)r & size_mask((uint8_t)osz);
            bool ovf = (int64_t)(int32_t)sext(res, osz) != r;
            put_flags(c, kArithFlags, szp_flags(res, osz) | (ovf ? (CF | OF) : 0));
            wr_reg(c, m.reg, osz, res);
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            int32_t rel = (int8_t)fetch8(pc);
            c.eip = cond(c, op & 0xF) ? pc + rel : pc;
            return;
        }
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83: {  // group 1
            int sz = (op == 0x81 || op == 0x83) ? osz : 1;
            decode_modrm(c, pc, p.seg, m);
            uint32_t imm;
            if (op == 0x81)
                imm = osz == 2 ? fetch16(pc) : fetch32(pc);
            else if (op == 0x83)
                imm = (uint32_t)(int32_t)(int8_t)fetch8(pc);
            else
                imm = fetch8(pc);
            BusLock lk(p.lock && !m.isreg);
            uint32_t r = alu(c, m.reg, rd_rm(c, m, sz), imm, sz);
            if (m.reg != 7) wr_rm(c, m, sz, r);
            break;
        }
        case 0x84:
        case 0x85: {  // TEST
            int sz = op == 0x84 ? 1 : osz;
            decode_modrm(c, pc, p.seg, m);
            alu(c, 4, rd_rm(c, m, sz), rd_reg(c, m.reg, sz), sz);
            break;
        }
        case 0x86:
        case 0x87: {  // XCHG (implicitly locked with memory)
            int sz = op == 0x86 ? 1 : osz;
            decode_modrm(c, pc, p.seg, m);
            BusLock lk(!m.isreg);
            uint32_t a = rd_rm(c, m, sz), b = rd_reg(c, m.reg, sz);
            wr_rm(c, m, sz, b);
            wr_reg(c, m.reg, sz, a);
            break;
        }
        case 0x88:
            decode_modrm(c, pc, p.seg, m);
            wr_rm(c, m, 1, rd_reg(c, m.reg, 1));
            break;
        case 0x89:
            decode_modrm(c, pc, p.seg, m);
            wr_rm(c, m, osz, rd_reg(c, m.reg, osz));
            break;
        case 0x8A:
            decode_modrm(c, pc, p.seg, m);
            wr_reg(c, m.reg, 1, rd_rm(c, m, 1));
            break;
        case 0x8B:
            decode_modrm(c, pc, p.seg, m);
            wr_reg(c, m.reg, osz, rd_rm(c, m, osz));
            break;
        case 0x8C:  // MOV Ew,Sreg: flat model, report plausible selectors
            decode_modrm(c, pc, p.seg, m);
            wr_rm(c, m, m.isreg ? osz : 2, m.reg == 4 ? 0x3B : (m.reg == 1 ? 0x1B : 0x23));
            break;
        case 0x8D:  // LEA (segment base must not apply)
            decode_modrm(c, pc, 0, m);
            if (m.isreg) {
                fault(c, start, "LEA with register operand");
                return;
            }
            wr_reg(c, m.reg, osz, m.ea);
            break;
        case 0x8E:  // MOV Sreg,Ew: ignored in the flat model
            decode_modrm(c, pc, p.seg, m);
            break;
        case 0x8F: {  // POP Ev
            uint32_t v = pop32(c);
            decode_modrm(c, pc, p.seg, m);  // ESP-relative addressing uses the incremented ESP
            wr_rm(c, m, osz, v);
            break;
        }
        case 0x90:
            break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint32_t a = rd_reg(c, EAX, osz), b = rd_reg(c, op & 7, osz);
            wr_reg(c, EAX, osz, b);
            wr_reg(c, op & 7, osz, a);
            break;
        }
        case 0x98:  // CWDE / CBW
            if (osz == 2)
                wr_reg(c, EAX, 2, sext(c.r[EAX] & 0xFF, 1));
            else
                c.r[EAX] = sext(c.r[EAX] & 0xFFFF, 2);
            break;
        case 0x99:  // CDQ / CWD
            if (osz == 2)
                wr_reg(c, EDX, 2, (c.r[EAX] & 0x8000) ? 0xFFFF : 0);
            else
                c.r[EDX] = (c.r[EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
            break;
        case 0x9B:  // WAIT
            break;
        case 0x9C:  // PUSHFD
            push32(c, materialize_flags(c) & 0x00FCFFFF);
            break;
        case 0x9D: {  // POPFD
            uint32_t v = pop32(c);
            c.lf_op = LF_NONE;
            c.eflags = (v & (kArithFlags | DF | TF | ID | 0x00040000)) | IF | 0x2;
            break;
        }
        case 0x9E: {  // SAHF
            uint32_t ah = (c.r[EAX] >> 8) & 0xFF;
            put_flags(c, CF | PF | AF | ZF | SF, ah);
            break;
        }
        case 0x9F:  // LAHF
            set_r8(c, 4, materialize_flags(c) & 0xFF);
            break;
        case 0xA0:
            set_r8(c, 0, mem::rd8(fetch32(pc) + p.seg));
            break;
        case 0xA1:
            wr_reg(c, EAX, osz, rd_mem(fetch32(pc) + p.seg, osz));
            break;
        case 0xA2:
            mem::wr8(fetch32(pc) + p.seg, (uint8_t)c.r[EAX]);
            break;
        case 0xA3:
            wr_mem(fetch32(pc) + p.seg, osz, c.r[EAX]);
            break;
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
            string_op_impl(c, op, (op & 1) ? osz : 1, p.rep, p.seg);
            break;
        case 0xA8:
            alu(c, 4, c.r[EAX], fetch8(pc), 1);
            break;
        case 0xA9:
            alu(c, 4, c.r[EAX], osz == 2 ? fetch16(pc) : fetch32(pc), osz);
            break;
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            set_r8(c, op & 7, fetch8(pc));
            break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            wr_reg(c, op & 7, osz, osz == 2 ? fetch16(pc) : fetch32(pc));
            break;
        case 0xC0:
        case 0xC1:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3: {  // group 2
            int sz = (op & 1) ? osz : 1;
            decode_modrm(c, pc, p.seg, m);
            uint32_t cnt = (op <= 0xC1) ? fetch8(pc) : (op <= 0xD1 ? 1 : (c.r[ECX] & 0xFF));
            uint32_t r = shift(c, m.reg, rd_rm(c, m, sz), cnt, sz);
            wr_rm(c, m, sz, r);
            break;
        }
        case 0xC2: {  // RET imm16
            uint16_t n = fetch16(pc);
            c.eip = pop32(c);
            c.r[ESP] += n;
            return;
        }
        case 0xC3:
            c.eip = pop32(c);
            return;
        case 0xC6:
            decode_modrm(c, pc, p.seg, m);
            wr_rm(c, m, 1, fetch8(pc));
            break;
        case 0xC7:
            decode_modrm(c, pc, p.seg, m);
            wr_rm(c, m, osz, osz == 2 ? fetch16(pc) : fetch32(pc));
            break;
        case 0xC8: {  // ENTER
            uint16_t size = fetch16(pc);
            uint8_t level = fetch8(pc) & 31;
            push32(c, c.r[EBP]);
            uint32_t frame = c.r[ESP];
            if (level > 0) {
                uint32_t bp = c.r[EBP];
                for (int i = 1; i < level; ++i) {
                    bp -= 4;
                    push32(c, mem::rd32(bp));
                }
                push32(c, frame);
            }
            c.r[EBP] = frame;
            c.r[ESP] -= size;
            break;
        }
        case 0xC9:  // LEAVE
            c.r[ESP] = c.r[EBP];
            c.r[EBP] = pop32(c);
            break;
        case 0xCC:
            fault(c, start, "INT3");
            return;
        case 0xCD:
            fault(c, start, "INT n");
            return;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            if (!exec_x87(c, op, pc, p.seg)) {
                fault(c, start, "unsupported x87 opcode");
                return;
            }
            break;
        case 0xE0:
        case 0xE1:
        case 0xE2: {  // LOOPNE / LOOPE / LOOP
            int32_t rel = (int8_t)fetch8(pc);
            c.r[ECX]--;
            bool take = c.r[ECX] != 0;
            if (op == 0xE0) take = take && !get_zf(c);
            if (op == 0xE1) take = take && get_zf(c);
            c.eip = take ? pc + rel : pc;
            return;
        }
        case 0xE3: {  // JECXZ
            int32_t rel = (int8_t)fetch8(pc);
            c.eip = c.r[ECX] == 0 ? pc + rel : pc;
            return;
        }
        case 0xE8: {
            int32_t rel = (int32_t)fetch32(pc);
            push32(c, pc);
            c.eip = pc + rel;
            aot::note_call(c.eip);
            return;
        }
        case 0xE9: {
            int32_t rel = (int32_t)fetch32(pc);
            c.eip = pc + rel;
            return;
        }
        case 0xEB: {
            int32_t rel = (int8_t)fetch8(pc);
            c.eip = pc + rel;
            return;
        }
        case 0xF4:
            fault(c, start, "HLT");
            return;
        case 0xF5:
            put_flags(c, CF, get_cf(c) ? 0 : CF);
            break;
        case 0xF6:
        case 0xF7: {  // group 3
            int sz = op == 0xF6 ? 1 : osz;
            decode_modrm(c, pc, p.seg, m);
            switch (m.reg) {
                case 0:
                case 1: {
                    uint32_t imm = sz == 1 ? fetch8(pc) : (sz == 2 ? fetch16(pc) : fetch32(pc));
                    alu(c, 4, rd_rm(c, m, sz), imm, sz);
                    break;
                }
                case 2: {
                    BusLock lk(p.lock && !m.isreg);
                    wr_rm(c, m, sz, ~rd_rm(c, m, sz));
                    break;
                }
                case 3: {  // NEG
                    BusLock lk(p.lock && !m.isreg);
                    uint32_t v = rd_rm(c, m, sz) & size_mask((uint8_t)sz);
                    uint32_t r = (0 - v) & size_mask((uint8_t)sz);
                    set_flags_lazy(c, LF_NEG, (uint8_t)sz, 0, v, r);
                    wr_rm(c, m, sz, r);
                    break;
                }
                case 4:
                case 5: {  // MUL / IMUL
                    uint32_t v = rd_rm(c, m, sz);
                    bool ovf;
                    uint32_t lo;
                    if (m.reg == 4) {
                        uint64_t a = rd_reg(c, EAX, sz);
                        uint64_t r = a * v;
                        if (sz == 1) {
                            wr_reg(c, EAX, 2, (uint32_t)r);
                            ovf = (r >> 8) != 0;
                        } else if (sz == 2) {
                            wr_reg(c, EAX, 2, (uint32_t)r);
                            wr_reg(c, EDX, 2, (uint32_t)(r >> 16));
                            ovf = (r >> 16) != 0;
                        } else {
                            c.r[EAX] = (uint32_t)r;
                            c.r[EDX] = (uint32_t)(r >> 32);
                            ovf = (r >> 32) != 0;
                        }
                        lo = (uint32_t)r;
                    } else {
                        int64_t a = (int32_t)sext(rd_reg(c, EAX, sz), sz);
                        int64_t r = a * (int32_t)sext(v, sz);
                        if (sz == 1) {
                            wr_reg(c, EAX, 2, (uint32_t)r);
                            ovf = r != (int8_t)r;
                        } else if (sz == 2) {
                            wr_reg(c, EAX, 2, (uint32_t)r);
                            wr_reg(c, EDX, 2, (uint32_t)(r >> 16));
                            ovf = r != (int16_t)r;
                        } else {
                            c.r[EAX] = (uint32_t)r;
                            c.r[EDX] = (uint32_t)((uint64_t)r >> 32);
                            ovf = r != (int32_t)r;
                        }
                        lo = (uint32_t)r;
                    }
                    put_flags(c, kArithFlags, szp_flags(lo, sz) | (ovf ? (CF | OF) : 0));
                    break;
                }
                default: {  // DIV / IDIV
                    uint32_t v = rd_rm(c, m, sz) & size_mask((uint8_t)sz);
                    if (!divide(c, m.reg == 7, v, sz)) {
                        fault(c, start, "integer divide error");
                        return;
                    }
                    break;
                }
            }
            break;
        }
        case 0xF8: put_flags(c, CF, 0); break;
        case 0xF9: put_flags(c, CF, CF); break;
        case 0xFA:
        case 0xFB: break;  // CLI/STI: no-op in user mode emulation
        case 0xFC: c.eflags &= ~DF; break;
        case 0xFD: c.eflags |= DF; break;
        case 0xFE: {  // INC/DEC Eb
            decode_modrm(c, pc, p.seg, m);
            if (m.reg > 1) {
                fault(c, start, "bad FE group");
                return;
            }
            BusLock lk(p.lock && !m.isreg);
            wr_rm(c, m, 1, inc_dec(c, rd_rm(c, m, 1), m.reg == 1, 1));
            break;
        }
        case 0xFF: {  // group 5
            decode_modrm(c, pc, p.seg, m);
            switch (m.reg) {
                case 0:
                case 1: {
                    BusLock lk(p.lock && !m.isreg);
                    wr_rm(c, m, osz, inc_dec(c, rd_rm(c, m, osz), m.reg == 1, osz));
                    break;
                }
                case 2: {  // CALL Ev
                    uint32_t t = rd_rm(c, m, 4);
                    push32(c, pc);
                    c.eip = t;
                    aot::note_call(t);
                    return;
                }
                case 4:  // JMP Ev
                    c.eip = rd_rm(c, m, 4);
                    return;
                case 6:  // PUSH Ev
                    push32(c, rd_rm(c, m, 4));
                    break;
                default:
                    fault(c, start, "far call/jmp");
                    return;
            }
            break;
        }
        default:
            fault(c, start, "unsupported opcode");
            return;
    }
    c.eip = pc;
}
}  // namespace

void bus_lock() {
    while (g_bus_lock.test_and_set(std::memory_order_acquire)) {
    }
}
void bus_unlock() { g_bus_lock.clear(std::memory_order_release); }

Cpu* current() { return t_current; }
void set_current(Cpu* c) { t_current = c; }

void reset(Cpu& c) {
    memset(&c, 0, sizeof c);
    c.eflags = IF | 0x2;
    c.lf_op = LF_NONE;
    c.mxcsr = 0x1F80;
    fpu_reset(c);
}

thread_local bool t_interpreting = false;

// One dispatch step: HLE thunk, translated function, or one interpreted instruction.
inline void dispatch_one(Cpu& c) {
    check_suspend(c);
    if (c.eip - hle::kThunkBase < hle::kThunkSpan) {
        hle::dispatch(c);
        t_interpreting = false;
    } else if (aot::Fn fn = aot::lookup(c.eip)) {
        fn(c);
        t_interpreting = false;
    } else {
        if (!t_interpreting) {
            aot::note_interp_entry(c.eip);
            t_interpreting = true;
        }
        aot::note_interpreted(c.eip);
        step(c);
        ++c.icount;
    }
}

Stop run(Cpu& c) {
    Cpu* prev = t_current;
    t_current = &c;
    for (;;) {
        dispatch_one(c);
        if (__builtin_expect(c.stop != Stop::None, 0)) break;
    }
    Stop s = c.stop;
    if (s == Stop::ReturnToHost) c.stop = Stop::None;
    t_current = prev;
    return s;
}

void run_until(Cpu& c, uint32_t ret_eip, uint32_t ret_esp) {
    // A stdcall callee pops its arguments, so the caller's frame is reached
    // with ESP >= ret_esp; recursive returns to the same address are deeper.
    while (!(c.eip == ret_eip && c.r[ESP] >= ret_esp)) {
        dispatch_one(c);
        if (c.stop != Stop::None) return;
    }
}

void step_one(Cpu& c) {
    aot::note_interpreted(c.eip);
    step(c);
    ++c.icount;
}

void string_op(Cpu& c, uint8_t op, int sz, uint8_t rep, uint32_t seg) { string_op_impl(c, op, sz, rep, seg); }

void dump(const Cpu& c, char* out, size_t cap) {
    Cpu tmp = c;
    uint32_t f = materialize_flags(tmp);
    snprintf(out, cap,
             "EAX=%08x ECX=%08x EDX=%08x EBX=%08x ESP=%08x EBP=%08x ESI=%08x EDI=%08x EIP=%08x "
             "EFL=%08x FTOP=%u icount=%" PRIu64,
             c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.eip, f, c.ftop, c.icount);
}

}  // namespace cpu
