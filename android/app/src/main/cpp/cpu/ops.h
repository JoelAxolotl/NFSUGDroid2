#pragma once
// Instruction semantics shared by the interpreter (interp.cpp) and the
// ahead-of-time translated code (generated/*.cpp), so both engines produce
// bit-identical results.
#include <cstdint>

#include "cpu/cpu.h"
#include "cpu/decode.h"

namespace cpu {

// Serializes LOCK-prefixed read-modify-writes, XCHG with memory and HLE Interlocked*.
struct BusLock {
    bool held;
    explicit BusLock(bool take) : held(take) {
        if (held) bus_lock();
    }
    ~BusLock() {
        if (held) bus_unlock();
    }
};

// --- ALU ------------------------------------------------------------------
// op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns masked result.
inline uint32_t alu(Cpu& c, int op, uint32_t a, uint32_t b, int sz) {
    uint32_t mask = size_mask((uint8_t)sz);
    a &= mask;
    b &= mask;
    uint32_t r;
    switch (op) {
        case 0:
            r = (a + b) & mask;
            set_flags_lazy(c, LF_ADD, (uint8_t)sz, a, b, r);
            return r;
        case 1:
            r = a | b;
            set_flags_lazy(c, LF_LOGIC, (uint8_t)sz, a, b, r);
            return r;
        case 2: {
            uint32_t cin = get_cf(c);
            r = (a + b + cin) & mask;
            set_flags_lazy(c, LF_ADC, (uint8_t)sz, a, b, r);
            c.lf_cf_in = cin;
            return r;
        }
        case 3: {
            uint32_t cin = get_cf(c);
            r = (a - b - cin) & mask;
            set_flags_lazy(c, LF_SBB, (uint8_t)sz, a, b, r);
            c.lf_cf_in = cin;
            return r;
        }
        case 4:
            r = a & b;
            set_flags_lazy(c, LF_LOGIC, (uint8_t)sz, a, b, r);
            return r;
        case 6:
            r = a ^ b;
            set_flags_lazy(c, LF_LOGIC, (uint8_t)sz, a, b, r);
            return r;
        default:  // SUB / CMP
            r = (a - b) & mask;
            set_flags_lazy(c, LF_SUB, (uint8_t)sz, a, b, r);
            return r;
    }
}

inline uint32_t inc_dec(Cpu& c, uint32_t v, bool dec, int sz) {
    uint32_t mask = size_mask((uint8_t)sz);
    uint32_t cf = get_cf(c);
    uint32_t r = (dec ? v - 1 : v + 1) & mask;
    set_flags_lazy(c, dec ? LF_DEC : LF_INC, (uint8_t)sz, v & mask, 1, r);
    c.lf_cf_in = cf;
    return r;
}

// Writes the given arithmetic flags eagerly, leaving the others intact.
inline void put_flags(Cpu& c, uint32_t which, uint32_t values) {
    if ((which & kArithFlags) == kArithFlags) {
        // Every arithmetic flag is overwritten: the pending lazy flags are dead.
        c.eflags = (c.eflags & ~which) | (values & which);
        c.lf_op = LF_NONE;
        return;
    }
    uint32_t f = materialize_flags(c);
    c.eflags = (f & ~which) | (values & which);
}

inline uint32_t szp_flags(uint32_t r, int sz) {
    uint32_t f = 0;
    if ((r & size_mask((uint8_t)sz)) == 0) f |= ZF;
    if (r & sign_bit((uint8_t)sz)) f |= SF;
    if (parity8(r)) f |= PF;
    return f;
}

// Group 2: 0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 6 SAL 7 SAR
inline uint32_t shift(Cpu& c, int op, uint32_t v, uint32_t cnt, int sz) {
    cnt &= 0x1F;
    if (cnt == 0) return v;
    const int bits = sz * 8;
    const uint32_t mask = size_mask((uint8_t)sz);
    const uint32_t msb = sign_bit((uint8_t)sz);
    v &= mask;
    uint32_t r, f;
    switch (op) {
        case 0: {  // ROL
            uint32_t n = cnt % bits;
            r = n ? ((v << n) | (v >> (bits - n))) & mask : v;
            uint32_t cf = r & 1;
            f = (cf ? CF : 0) | ((((r & msb) != 0) ^ cf) ? OF : 0);
            put_flags(c, CF | OF, f);
            return r;
        }
        case 1: {  // ROR
            uint32_t n = cnt % bits;
            r = n ? ((v >> n) | (v << (bits - n))) & mask : v;
            uint32_t cf = (r & msb) != 0;
            uint32_t of = cf ^ ((r & (msb >> 1)) != 0);
            f = (cf ? CF : 0) | (of ? OF : 0);
            put_flags(c, CF | OF, f);
            return r;
        }
        case 2: {  // RCL
            uint32_t n = cnt % (bits + 1);
            uint32_t cf = get_cf(c);
            r = v;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t out = (r & msb) != 0;
                r = ((r << 1) | cf) & mask;
                cf = out;
            }
            f = (cf ? CF : 0) | ((((r & msb) != 0) ^ cf) ? OF : 0);
            put_flags(c, CF | OF, f);
            return r;
        }
        case 3: {  // RCR
            uint32_t n = cnt % (bits + 1);
            uint32_t cf = get_cf(c);
            r = v;
            uint32_t of = ((r & msb) != 0) ^ cf;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t out = r & 1;
                r = (r >> 1) | (cf ? msb : 0);
                cf = out;
            }
            f = (cf ? CF : 0) | (of ? OF : 0);
            put_flags(c, CF | OF, f);
            return r;
        }
        case 4:
        case 6: {  // SHL
            uint64_t w = (uint64_t)v << cnt;
            r = (uint32_t)w & mask;
            uint32_t cf = cnt <= (uint32_t)bits ? (uint32_t)((w >> bits) & 1) : 0;
            f = szp_flags(r, sz) | (cf ? CF : 0) | ((((r & msb) != 0) ^ cf) ? OF : 0);
            put_flags(c, kArithFlags, f);
            return r;
        }
        case 5: {  // SHR
            uint32_t cf = cnt <= (uint32_t)bits ? (v >> (cnt - 1)) & 1 : 0;
            r = cnt < 32 ? (v >> cnt) : 0;
            f = szp_flags(r, sz) | (cf ? CF : 0) | ((v & msb) ? OF : 0);
            put_flags(c, kArithFlags, f);
            return r;
        }
        default: {  // SAR
            int32_t sv = (int32_t)sext(v, sz);
            uint32_t sh = cnt > 31 ? 31 : cnt;
            uint32_t cf = (uint32_t)(sv >> (sh - 1)) & 1;
            r = (uint32_t)(sv >> sh) & mask;
            f = szp_flags(r, sz) | (cf ? CF : 0);
            put_flags(c, kArithFlags, f);
            return r;
        }
    }
}

inline bool divide(Cpu& c, bool is_signed, uint32_t divisor, int sz) {
    if (divisor == 0) return false;
    if (sz == 1) {
        uint32_t ax = c.r[EAX] & 0xFFFF;
        if (!is_signed) {
            uint32_t q = ax / divisor, rem = ax % divisor;
            if (q > 0xFF) return false;
            c.r[EAX] = (c.r[EAX] & 0xFFFF0000u) | (rem << 8) | q;
        } else {
            int32_t n = (int16_t)ax, d = (int8_t)divisor;
            int32_t q = n / d, rem = n % d;
            if (q > 127 || q < -128) return false;
            c.r[EAX] = (c.r[EAX] & 0xFFFF0000u) | ((rem & 0xFF) << 8) | (q & 0xFF);
        }
    } else if (sz == 2) {
        uint32_t n = ((c.r[EDX] & 0xFFFF) << 16) | (c.r[EAX] & 0xFFFF);
        if (!is_signed) {
            uint32_t q = n / divisor, rem = n % divisor;
            if (q > 0xFFFF) return false;
            wr_reg(c, EAX, 2, q);
            wr_reg(c, EDX, 2, rem);
        } else {
            int32_t sn = (int32_t)n, d = (int16_t)divisor;
            int32_t q = sn / d, rem = sn % d;
            if (q > 32767 || q < -32768) return false;
            wr_reg(c, EAX, 2, (uint32_t)q);
            wr_reg(c, EDX, 2, (uint32_t)rem);
        }
    } else {
        uint64_t n = ((uint64_t)c.r[EDX] << 32) | c.r[EAX];
        if (!is_signed) {
            uint64_t q = n / divisor, rem = n % divisor;
            if (q > 0xFFFFFFFFull) return false;
            c.r[EAX] = (uint32_t)q;
            c.r[EDX] = (uint32_t)rem;
        } else {
            int64_t sn = (int64_t)n, d = (int32_t)divisor;
            if (d == -1 && sn == INT64_MIN) return false;
            int64_t q = sn / d, rem = sn % d;
            if (q > INT32_MAX || q < INT32_MIN) return false;
            c.r[EAX] = (uint32_t)q;
            c.r[EDX] = (uint32_t)rem;
        }
    }
    return true;
}


// String instruction (MOVS/STOS/LODS/CMPS/SCAS, op = one-byte opcode) with optional REP prefix.
void string_op(Cpu& c, uint8_t op, int sz, uint8_t rep, uint32_t seg);

// Executes exactly one instruction at c.eip with the interpreter.
void step_one(Cpu& c);

// Runs guest code (interpreter + translated functions) until EIP == ret_eip
// with ESP == ret_esp, or until c.stop is set. Used when translated code has
// to hand control to code it cannot run itself.
void run_until(Cpu& c, uint32_t ret_eip, uint32_t ret_esp);

}  // namespace cpu
