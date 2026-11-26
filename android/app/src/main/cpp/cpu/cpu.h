#pragma once
// x86-32 guest CPU state shared by the interpreter and (later) AOT-translated
// code. Arithmetic flags are evaluated lazily: the last flag-producing
// operation's operands/result are kept and individual flags are derived only
// when a consumer (Jcc, SETcc, ADC, PUSHF...) needs them.
#include <cstdint>

#include "core/memory.h"

namespace cpu {

enum Reg { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

enum Flag : uint32_t {
    CF = 1u << 0,
    PF = 1u << 2,
    AF = 1u << 4,
    ZF = 1u << 6,
    SF = 1u << 7,
    TF = 1u << 8,
    IF = 1u << 9,
    DF = 1u << 10,
    OF = 1u << 11,
    ID = 1u << 21,
};
constexpr uint32_t kArithFlags = CF | PF | AF | ZF | SF | OF;

enum LazyOp : uint8_t { LF_NONE, LF_ADD, LF_ADC, LF_SUB, LF_SBB, LF_LOGIC, LF_INC, LF_DEC, LF_NEG };

union Xmm {
    float f[4];
    double d[2];
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    int16_t i16[8];
    int32_t i32[4];
};

enum class Stop : uint8_t {
    None,
    ReturnToHost,   // reached the host-call return thunk at the expected depth
    ThreadExit,     // ExitThread / thread routine returned
    ProcessExit,
    Fault,
};

struct alignas(16) Cpu {
    uint32_t r[8];
    uint32_t eip;
    uint32_t eflags;  // non-arith bits always valid; arith bits valid when lf_op == LF_NONE

    // Lazy flags.
    uint32_t lf_a, lf_b, lf_res, lf_cf_in;
    uint8_t lf_op, lf_size;  // size in bytes: 1, 2, 4

    uint32_t fs_base;  // guest TEB

    // x87: st(i) = fpr[(ftop + i) & 7]. 80-bit precision is approximated with double.
    double fpr[8];
    uint32_t ftop;
    uint16_t fcw;
    uint16_t fsw;      // C0..C3 and exception bits; TOP is synthesized from ftop
    uint8_t fempty;    // bit i set = physical register i is empty

    uint64_t mm[8];
    Xmm xmm[8];
    uint32_t mxcsr;

    // Runtime bookkeeping (not architectural).
    Stop stop;
    uint32_t host_call_depth;
    uint32_t thread_id;
    uint64_t icount;
};

inline uint32_t& reg(Cpu& c, int i) { return c.r[i]; }

// --- Lazy flag evaluation -------------------------------------------------
inline uint32_t size_mask(uint8_t sz) { return sz == 4 ? 0xFFFFFFFFu : (sz == 2 ? 0xFFFFu : 0xFFu); }
inline uint32_t sign_bit(uint8_t sz) { return sz == 4 ? 0x80000000u : (sz == 2 ? 0x8000u : 0x80u); }

inline bool parity8(uint32_t v) { return !__builtin_parity(v & 0xFF); }

inline bool get_cf(const Cpu& c) {
    switch (c.lf_op) {
        case LF_NONE: return c.eflags & CF;
        case LF_ADD: return c.lf_res < c.lf_a;
        case LF_ADC: return c.lf_cf_in ? c.lf_res <= c.lf_a : c.lf_res < c.lf_a;
        case LF_SUB: return c.lf_a < c.lf_b;
        case LF_SBB: return c.lf_a < c.lf_b || (c.lf_cf_in && c.lf_a == c.lf_b);
        case LF_LOGIC: return false;
        case LF_INC:
        case LF_DEC: return c.lf_cf_in;
        case LF_NEG: return c.lf_b != 0;
    }
    return false;
}
inline bool get_zf(const Cpu& c) { return c.lf_op == LF_NONE ? (c.eflags & ZF) : c.lf_res == 0; }
inline bool get_sf(const Cpu& c) {
    return c.lf_op == LF_NONE ? (c.eflags & SF) : (c.lf_res & sign_bit(c.lf_size));
}
inline bool get_pf(const Cpu& c) { return c.lf_op == LF_NONE ? (c.eflags & PF) : parity8(c.lf_res); }
inline bool get_of(const Cpu& c) {
    uint32_t sb = sign_bit(c.lf_size);
    switch (c.lf_op) {
        case LF_NONE: return c.eflags & OF;
        case LF_ADD:
        case LF_ADC: return ((c.lf_a ^ c.lf_res) & (c.lf_b ^ c.lf_res)) & sb;
        case LF_SUB:
        case LF_SBB: return ((c.lf_a ^ c.lf_b) & (c.lf_a ^ c.lf_res)) & sb;
        case LF_LOGIC: return false;
        case LF_INC: return c.lf_res == sb;
        case LF_DEC: return c.lf_res == sb - 1;
        case LF_NEG: return c.lf_b == sb;
    }
    return false;
}
inline bool get_af(const Cpu& c) {
    switch (c.lf_op) {
        case LF_NONE: return c.eflags & AF;
        case LF_LOGIC: return false;
        case LF_INC: return (c.lf_res & 0xF) == 0;
        case LF_DEC: return (c.lf_res & 0xF) == 0xF;
        default: return (c.lf_a ^ c.lf_b ^ c.lf_res) & 0x10;
    }
}

inline uint32_t materialize_flags(Cpu& c) {
    if (c.lf_op != LF_NONE) {
        uint32_t f = c.eflags & ~kArithFlags;
        if (get_cf(c)) f |= CF;
        if (get_pf(c)) f |= PF;
        if (get_af(c)) f |= AF;
        if (get_zf(c)) f |= ZF;
        if (get_sf(c)) f |= SF;
        if (get_of(c)) f |= OF;
        c.eflags = f;
        c.lf_op = LF_NONE;
    }
    return c.eflags | 0x2;  // bit 1 is always set
}

inline void set_flags_lazy(Cpu& c, LazyOp op, uint8_t sz, uint32_t a, uint32_t b, uint32_t res) {
    c.lf_op = op;
    c.lf_size = sz;
    c.lf_a = a;
    c.lf_b = b;
    c.lf_res = res;
}

inline bool cond(const Cpu& c, int cc) {
    switch (cc & 0xF) {
        case 0x0: return get_of(c);
        case 0x1: return !get_of(c);
        case 0x2: return get_cf(c);
        case 0x3: return !get_cf(c);
        case 0x4: return get_zf(c);
        case 0x5: return !get_zf(c);
        case 0x6: return get_cf(c) || get_zf(c);
        case 0x7: return !get_cf(c) && !get_zf(c);
        case 0x8: return get_sf(c);
        case 0x9: return !get_sf(c);
        case 0xA: return get_pf(c);
        case 0xB: return !get_pf(c);
        case 0xC: return get_sf(c) != get_of(c);
        case 0xD: return get_sf(c) == get_of(c);
        case 0xE: return get_zf(c) || (get_sf(c) != get_of(c));
        default: return !get_zf(c) && (get_sf(c) == get_of(c));
    }
}

// --- Stack ---------------------------------------------------------------
inline void push32(Cpu& c, uint32_t v) {
    c.r[ESP] -= 4;
    mem::wr32(c.r[ESP], v);
}
inline uint32_t pop32(Cpu& c) {
    uint32_t v = mem::rd32(c.r[ESP]);
    c.r[ESP] += 4;
    return v;
}

void reset(Cpu& c);

// Runs guest code until `c.stop` becomes non-None. Returns the stop reason
// (and clears it for ReturnToHost so nested host->guest calls compose).
Stop run(Cpu& c);

// The Cpu executing on the calling host thread (null on non-guest threads).
Cpu* current();
void set_current(Cpu* c);

// x87 helpers implemented in x87.cpp, used by the interpreter.
bool exec_x87(Cpu& c, uint8_t op, uint32_t& pc, uint32_t seg_base);
// MMX / SSE-integer-on-MMX; `op` is the byte after 0x0F.
bool exec_mmx(Cpu& c, uint8_t op, uint32_t& pc, uint32_t seg_base);
void fpu_reset(Cpu& c);

// Global lock shared by LOCK-prefixed guest instructions and HLE Interlocked*.
void bus_lock();
void bus_unlock();

// Formats a register dump for fault reports.
void dump(const Cpu& c, char* out, size_t cap);

}  // namespace cpu
