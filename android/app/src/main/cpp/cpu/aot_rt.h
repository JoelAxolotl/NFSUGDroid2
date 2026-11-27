#pragma once
// Runtime support for statically recompiled code (generated/aot_*.cpp).
//
// Generated functions operate on cpu::Cpu exactly like the interpreter and use
// the same semantic helpers (cpu/ops.h, cpu/x87_ops.h), so results are
// bit-identical whichever engine executes an instruction.
#include <cstdint>

#include "core/memory.h"
#include "cpu/cpu.h"
#include "cpu/ops.h"
#include "cpu/x87_ops.h"
#include "hle/thunks.h"
#include "runtime/aot.h"

// Guest registers live in host locals R0..R7 inside a translated function and
// are synchronised with cpu::Cpu only where other code can observe them
// (calls, exits, interpreter fallbacks). The guest memory base is cached in M.
#define AOT_PROLOGUE                                                                          \
    cpu::check_suspend(c);                                                                    \
    uint8_t* const M = mem::base;                                                             \
    uint32_t R0 = c.r[0], R1 = c.r[1], R2 = c.r[2], R3 = c.r[3], R4 = c.r[4], R5 = c.r[5],     \
             R6 = c.r[6], R7 = c.r[7];                                                        \
    /* operands and result of the last flag producer, read by fused jcc/setcc/cmovcc */      \
    uint32_t FA = 0, FB = 0, FR = 0
#define AOT_SYNC_OUT \
    (c.r[0] = R0, c.r[1] = R1, c.r[2] = R2, c.r[3] = R3, c.r[4] = R4, c.r[5] = R5, c.r[6] = R6, c.r[7] = R7)
#define AOT_SYNC_IN \
    (R0 = c.r[0], R1 = c.r[1], R2 = c.r[2], R3 = c.r[3], R4 = c.r[4], R5 = c.r[5], R6 = c.r[6], R7 = c.r[7])
#define AOT_EXIT     \
    do {             \
        AOT_SYNC_OUT; \
        return;      \
    } while (0)

namespace aotrt {
template <typename T>
inline T rd(const uint8_t* m, uint32_t a) {
    T v;
    __builtin_memcpy(&v, m + a, sizeof(T));
    return v;
}
template <typename T>
inline void wr(uint8_t* m, uint32_t a, T v) {
    __builtin_memcpy(m + a, &v, sizeof(T));
}
}  // namespace aotrt

#define RD8(a) aotrt::rd<uint8_t>(M, (a))
#define RD16(a) aotrt::rd<uint16_t>(M, (a))
#define RD32(a) aotrt::rd<uint32_t>(M, (a))
#define WR8(a, v) aotrt::wr<uint8_t>(M, (a), (uint8_t)(v))
#define WR16(a, v) aotrt::wr<uint16_t>(M, (a), (uint16_t)(v))
#define WR32(a, v) aotrt::wr<uint32_t>(M, (a), (uint32_t)(v))

namespace aotrt {

// Calls guest code at `target`; the return address `ret` has been pushed and
// `sp0` is ESP before the push. Returns with EIP == ret (or c.stop set).
inline void call(cpu::Cpu& c, uint32_t target, uint32_t ret, uint32_t sp0) {
    c.eip = target;
    if (target - hle::kThunkBase < hle::kThunkSpan) {
        hle::dispatch(c);  // pops the return address itself
    } else if (aot::Fn f = aot::lookup(target)) {
        f(c);
    }
    // Returning to `ret` means the callee returned; stack probes (__chkstk)
    // legitimately come back with ESP below sp0.
    if (__builtin_expect(c.eip != ret, 0)) {
        if (c.stop != cpu::Stop::None) return;
        cpu::run_until(c, ret, sp0);
    }
}

// DIV/IDIV r/m32 on EDX:EAX. Returns false on divide error (zero divisor or
// quotient overflow) so the caller can let the interpreter raise the fault.
inline bool div32(uint32_t& eax, uint32_t& edx, uint32_t divisor, bool is_signed) {
    if (divisor == 0) return false;
    uint64_t n = ((uint64_t)edx << 32) | eax;
    if (!is_signed) {
        uint64_t q = n / divisor;
        if (q > 0xFFFFFFFFull) return false;
        edx = (uint32_t)(n % divisor);
        eax = (uint32_t)q;
        return true;
    }
    int64_t sn = (int64_t)n, d = (int32_t)divisor;
    if (d == -1 && sn == INT64_MIN) return false;
    int64_t q = sn / d;
    if (q > INT32_MAX || q < INT32_MIN) return false;
    edx = (uint32_t)(sn % d);
    eax = (uint32_t)q;
    return true;
}

// ---- Flag-free forms, used where the recompiler proved the flags dead ----------
inline uint32_t alu_nf(int op, uint32_t a, uint32_t b, int sz) {
    const uint32_t m = cpu::size_mask((uint8_t)sz);
    switch (op) {
        case 0: return (a + b) & m;
        case 1: return (a | b) & m;
        case 4: return (a & b) & m;
        case 6: return (a ^ b) & m;
        default: return (a - b) & m;  // SUB / CMP
    }
}
// SHL/SHR/SAR by a non-zero immediate.
inline uint32_t shift_nf(int op, uint32_t v, uint32_t cnt, int sz) {
    const uint32_t m = cpu::size_mask((uint8_t)sz);
    cnt &= 0x1F;
    switch (op) {
        case 5: return ((v & m) >> cnt) & m;
        case 7: return ((uint32_t)((int32_t)cpu::sext(v & m, sz) >> cnt)) & m;
        default: return (v << cnt) & m;  // SHL / SAL
    }
}
// INC/DEC when only OF/SF/ZF/AF/PF are live (CF is dead, so it need not be read).
inline uint32_t inc_dec_nocf(cpu::Cpu& c, uint32_t v, bool dec, int sz) {
    const uint32_t m = cpu::size_mask((uint8_t)sz);
    uint32_t r = (dec ? v - 1 : v + 1) & m;
    cpu::set_flags_lazy(c, dec ? cpu::LF_DEC : cpu::LF_INC, (uint8_t)sz, v & m, 1, r);
    c.lf_cf_in = 0;
    return r;
}

// ---- Fused condition codes: the flag producer's operands (a, b) and result r,
// all masked to sz bytes. cc follows the x86 encoding (0 O ... 15 G).
inline int32_t sx(uint32_t v, int sz) { return (int32_t)cpu::sext(v, sz); }
inline bool msb(uint32_t v, int sz) { return (v & cpu::sign_bit((uint8_t)sz)) != 0; }

// SUB / CMP: r = a - b.
inline bool fc_sub(int cc, uint32_t a, uint32_t b, uint32_t r, int sz) {
    switch (cc) {
        case 0: return msb((a ^ b) & (a ^ r), sz);
        case 1: return !msb((a ^ b) & (a ^ r), sz);
        case 2: return a < b;
        case 3: return a >= b;
        case 4: return r == 0;
        case 5: return r != 0;
        case 6: return a <= b;
        case 7: return a > b;
        case 8: return msb(r, sz);
        case 9: return !msb(r, sz);
        case 10: return cpu::parity8(r);
        case 11: return !cpu::parity8(r);
        case 12: return sx(a, sz) < sx(b, sz);
        case 13: return sx(a, sz) >= sx(b, sz);
        case 14: return sx(a, sz) <= sx(b, sz);
        default: return sx(a, sz) > sx(b, sz);
    }
}
// ADD: r = a + b.
inline bool fc_add(int cc, uint32_t a, uint32_t b, uint32_t r, int sz) {
    const bool of = msb(~(a ^ b) & (a ^ r), sz), cf = r < a, sf = msb(r, sz), zf = r == 0;
    switch (cc) {
        case 0: return of;
        case 1: return !of;
        case 2: return cf;
        case 3: return !cf;
        case 4: return zf;
        case 5: return !zf;
        case 6: return cf || zf;
        case 7: return !cf && !zf;
        case 8: return sf;
        case 9: return !sf;
        case 10: return cpu::parity8(r);
        case 11: return !cpu::parity8(r);
        case 12: return sf != of;
        case 13: return sf == of;
        case 14: return zf || sf != of;
        default: return !zf && sf == of;
    }
}
// AND / OR / XOR / TEST: CF = OF = 0.
inline bool fc_logic(int cc, uint32_t r, int sz) {
    const bool sf = msb(r, sz), zf = r == 0;
    switch (cc) {
        case 0: case 2: return false;
        case 1: case 3: return true;
        case 4: case 6: return zf;
        case 5: case 7: return !zf;
        case 8: case 12: return sf;
        case 9: case 13: return !sf;
        case 10: return cpu::parity8(r);
        case 11: return !cpu::parity8(r);
        case 14: return zf || sf;
        default: return !zf && !sf;
    }
}
// INC / DEC (CF is untouched, so the recompiler never fuses cc 2, 3, 6, 7 here).
inline bool fc_incdec(int cc, bool dec, uint32_t a, uint32_t r, int sz) {
    const uint32_t sb = cpu::sign_bit((uint8_t)sz);
    const bool of = dec ? a == sb : r == sb, sf = msb(r, sz), zf = r == 0;
    switch (cc) {
        case 0: return of;
        case 1: return !of;
        case 4: return zf;
        case 5: return !zf;
        case 8: return sf;
        case 9: return !sf;
        case 10: return cpu::parity8(r);
        case 11: return !cpu::parity8(r);
        case 12: return sf != of;
        case 13: return sf == of;
        case 14: return zf || sf != of;
        default: return !zf && sf == of;
    }
}

// SHLD/SHRD r/m32, r32, count (count masked to 5 bits; 0 leaves dst and flags alone).
// `flags` is false where the recompiler proved the flags dead.
inline uint32_t shxd32(cpu::Cpu& c, bool left, uint32_t dst, uint32_t src, uint32_t cnt, bool flags) {
    cnt &= 0x1F;
    if (!cnt) return dst;
    uint32_t r, cf;
    if (left) {
        r = (dst << cnt) | (src >> (32 - cnt));
        cf = (dst >> (32 - cnt)) & 1;
    } else {
        r = (dst >> cnt) | (src << (32 - cnt));
        cf = (dst >> (cnt - 1)) & 1;
    }
    if (flags) {
        uint32_t of = ((r ^ dst) >> 31) & 1;  // defined for count 1: sign changed
        cpu::put_flags(c, cpu::kArithFlags, cpu::szp_flags(r, 4) | (cf ? cpu::CF : 0u) | (of ? cpu::OF : 0u));
    }
    return r;
}

// Two/three-operand IMUL r32 (truncated product; CF = OF = overflow).
inline uint32_t imul32(cpu::Cpu& c, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a * (int64_t)(int32_t)b;
    uint32_t res = (uint32_t)r;
    bool ovf = (int64_t)(int32_t)res != r;
    cpu::put_flags(c, cpu::kArithFlags, cpu::szp_flags(res, 4) | (ovf ? (cpu::CF | cpu::OF) : 0));
    return res;
}

}  // namespace aotrt
