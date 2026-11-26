#pragma once
// x87 semantics shared by the interpreter (x87.cpp) and translated code.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "cpu/cpu.h"

namespace cpu {

constexpr uint16_t C0 = 1u << 8, C1 = 1u << 9, C2 = 1u << 10, C3 = 1u << 14;

inline double& st(Cpu& c, int i) { return c.fpr[(c.ftop + i) & 7]; }
inline bool st_empty(const Cpu& c, int i) { return (c.fempty >> ((c.ftop + i) & 7)) & 1; }

inline void fpush(Cpu& c, double v) {
    c.ftop = (c.ftop - 1) & 7;
    c.fpr[c.ftop] = v;
    c.fempty &= ~(1u << c.ftop);
}
inline void fpop(Cpu& c) {
    c.fempty |= 1u << c.ftop;
    c.ftop = (c.ftop + 1) & 7;
}
inline void set_tag(Cpu& c, int i, bool empty) {
    int phys = (c.ftop + i) & 7;
    if (empty)
        c.fempty |= 1u << phys;
    else
        c.fempty &= ~(1u << phys);
}

inline void set_cc(Cpu& c, uint16_t bits) { c.fsw = (c.fsw & ~(C0 | C1 | C2 | C3)) | bits; }

inline void compare(Cpu& c, double a, double b) {
    if (std::isnan(a) || std::isnan(b))
        set_cc(c, C0 | C2 | C3);
    else if (a > b)
        set_cc(c, 0);
    else if (a < b)
        set_cc(c, C0);
    else
        set_cc(c, C3);
}

// FCOMI/FUCOMI write ZF/PF/CF in EFLAGS.
inline void compare_eflags(Cpu& c, double a, double b) {
    uint32_t f = materialize_flags(c) & ~(ZF | PF | CF | OF | SF | AF);
    if (std::isnan(a) || std::isnan(b))
        f |= ZF | PF | CF;
    else if (a < b)
        f |= CF;
    else if (a == b)
        f |= ZF;
    c.eflags = f;
}

inline double round_rc(const Cpu& c, double v) {
    switch ((c.fcw >> 10) & 3) {
        case 0: return std::nearbyint(v);  // host default is round-to-nearest-even
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
    }
}

template <typename I>
inline I to_int(const Cpu& c, double v, bool truncate) {
    double r = truncate ? std::trunc(v) : round_rc(c, v);
    if (std::isnan(r) || r < (double)std::numeric_limits<I>::min() ||
        r > (double)std::numeric_limits<I>::max())
        return std::numeric_limits<I>::min();  // "integer indefinite"
    return (I)r;
}

inline double load_f80(uint32_t a) {
    uint64_t mant = mem::rd<uint64_t>(a);
    uint16_t se = mem::rd16(a + 8);
    int sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) {
        v = 0.0;
    } else if (exp == 0x7FFF) {
        v = (mant << 1) ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    } else {
        v = std::ldexp((double)mant, exp - 16383 - 63);
    }
    return sign ? -v : v;
}

inline void store_f80(uint32_t a, double v) {
    uint64_t mant = 0;
    uint16_t se = 0;
    if (std::signbit(v)) se = 0x8000;
    double av = std::fabs(v);
    if (std::isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (std::isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (av != 0.0) {
        int e;
        double f = std::frexp(av, &e);  // av = f * 2^e, f in [0.5, 1)
        mant = (uint64_t)std::ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    mem::wr<uint64_t>(a, mant);
    mem::wr16(a + 8, se);
}

inline float rdf32(uint32_t a) { return mem::rd<float>(a); }
inline double rdf64(uint32_t a) { return mem::rd<double>(a); }

// Shared arithmetic for D8/DC/DA/DE memory and register forms.
// op: 0 add 1 mul 2 com 3 comp 4 sub 5 subr 6 div 7 divr. dst = dst op src.
inline double arith(int op, double dst, double src) {
    switch (op) {
        case 0: return dst + src;
        case 1: return dst * src;
        case 4: return dst - src;
        case 5: return src - dst;
        case 6: return dst / src;
        default: return src / dst;
    }
}


}  // namespace cpu
