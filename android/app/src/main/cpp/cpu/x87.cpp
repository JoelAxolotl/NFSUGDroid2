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

namespace cpu {

namespace {
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

void compare(Cpu& c, double a, double b) {
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
void compare_eflags(Cpu& c, double a, double b) {
    uint32_t f = materialize_flags(c) & ~(ZF | PF | CF | OF | SF | AF);
    if (std::isnan(a) || std::isnan(b))
        f |= ZF | PF | CF;
    else if (a < b)
        f |= CF;
    else if (a == b)
        f |= ZF;
    c.eflags = f;
}

double round_rc(const Cpu& c, double v) {
    switch ((c.fcw >> 10) & 3) {
        case 0: return std::nearbyint(v);  // host default is round-to-nearest-even
        case 1: return std::floor(v);
        case 2: return std::ceil(v);
        default: return std::trunc(v);
    }
}

template <typename I>
I to_int(const Cpu& c, double v, bool truncate) {
    double r = truncate ? std::trunc(v) : round_rc(c, v);
    if (std::isnan(r) || r < (double)std::numeric_limits<I>::min() ||
        r > (double)std::numeric_limits<I>::max())
        return std::numeric_limits<I>::min();  // "integer indefinite"
    return (I)r;
}

double load_f80(uint32_t a) {
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

void store_f80(uint32_t a, double v) {
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
double arith(int op, double dst, double src) {
    switch (op) {
        case 0: return dst + src;
        case 1: return dst * src;
        case 4: return dst - src;
        case 5: return src - dst;
        case 6: return dst / src;
        default: return src / dst;
    }
}

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
}  // namespace

void fpu_reset(Cpu& c) {
    c.fcw = 0x027F;  // Windows default: 53-bit precision, all exceptions masked
    c.fsw = 0;
    c.ftop = 0;
    c.fempty = 0xFF;
    for (double& d : c.fpr) d = 0.0;
}

bool exec_x87(Cpu& c, uint8_t op, uint32_t& pc, uint32_t seg) {
    uint8_t modrm = mem::rd8(pc);
    if (modrm < 0xC0) {
        ModRM m;
        decode_modrm(c, pc, seg, m);
        const uint32_t a = m.ea;
        const int r = m.reg;
        switch (op) {
            case 0xD8:  // m32fp arithmetic
            case 0xDC:  // m64fp arithmetic
            case 0xDA:  // m32int arithmetic
            case 0xDE: {  // m16int arithmetic
                double src = op == 0xD8 ? (double)rdf32(a)
                           : op == 0xDC ? rdf64(a)
                           : op == 0xDA ? (double)(int32_t)mem::rd32(a)
                                        : (double)(int16_t)mem::rd16(a);
                if (r == 2 || r == 3) {
                    compare(c, st(c, 0), src);
                    if (r == 3) fpop(c);
                } else {
                    st(c, 0) = arith(r, st(c, 0), src);
                }
                return true;
            }
            case 0xD9:
                switch (r) {
                    case 0: fpush(c, rdf32(a)); return true;
                    case 2: mem::wr<float>(a, (float)st(c, 0)); return true;
                    case 3: mem::wr<float>(a, (float)st(c, 0)); fpop(c); return true;
                    case 4: fload_env(c, a); return true;
                    case 5: c.fcw = mem::rd16(a); return true;
                    case 6: fsave_env(c, a); return true;
                    case 7: mem::wr16(a, c.fcw); return true;
                    default: return false;
                }
            case 0xDB:
                switch (r) {
                    case 0: fpush(c, (double)(int32_t)mem::rd32(a)); return true;
                    case 1: mem::wr32(a, (uint32_t)to_int<int32_t>(c, st(c, 0), true)); fpop(c); return true;
                    case 2: mem::wr32(a, (uint32_t)to_int<int32_t>(c, st(c, 0), false)); return true;
                    case 3: mem::wr32(a, (uint32_t)to_int<int32_t>(c, st(c, 0), false)); fpop(c); return true;
                    case 5: fpush(c, load_f80(a)); return true;
                    case 7: store_f80(a, st(c, 0)); fpop(c); return true;
                    default: return false;
                }
            case 0xDD:
                switch (r) {
                    case 0: fpush(c, rdf64(a)); return true;
                    case 1: mem::wr<int64_t>(a, to_int<int64_t>(c, st(c, 0), true)); fpop(c); return true;
                    case 2: mem::wr<double>(a, st(c, 0)); return true;
                    case 3: mem::wr<double>(a, st(c, 0)); fpop(c); return true;
                    case 4:  // FRSTOR
                        fload_env(c, a);
                        for (int i = 0; i < 8; ++i) st(c, i) = load_f80(a + 28 + i * 10);
                        return true;
                    case 6:  // FNSAVE
                        fsave_env(c, a);
                        for (int i = 0; i < 8; ++i) store_f80(a + 28 + i * 10, st(c, i));
                        fpu_reset(c);
                        return true;
                    case 7: mem::wr16(a, (c.fsw & ~0x3800) | ((c.ftop & 7) << 11)); return true;
                    default: return false;
                }
            case 0xDF:
                switch (r) {
                    case 0: fpush(c, (double)(int16_t)mem::rd16(a)); return true;
                    case 1: mem::wr16(a, (uint16_t)to_int<int16_t>(c, st(c, 0), true)); fpop(c); return true;
                    case 2: mem::wr16(a, (uint16_t)to_int<int16_t>(c, st(c, 0), false)); return true;
                    case 3: mem::wr16(a, (uint16_t)to_int<int16_t>(c, st(c, 0), false)); fpop(c); return true;
                    case 5: fpush(c, (double)mem::rd<int64_t>(a)); return true;
                    case 7: mem::wr<int64_t>(a, to_int<int64_t>(c, st(c, 0), false)); fpop(c); return true;
                    default: return false;  // FBLD / FBSTP
                }
        }
        return false;
    }

    // Register forms.
    pc++;
    const int i = modrm & 7;
    const int r = (modrm >> 3) & 7;
    switch (op) {
        case 0xD8:
            if (r == 2 || r == 3) {
                compare(c, st(c, 0), st(c, i));
                if (r == 3) fpop(c);
            } else {
                st(c, 0) = arith(r, st(c, 0), st(c, i));
            }
            return true;
        case 0xDC:  // st(i) = st(i) op st(0); note sub/subr and div/divr are swapped
            if (r == 2 || r == 3) {
                compare(c, st(c, 0), st(c, i));
                if (r == 3) fpop(c);
                return true;
            }
            {
                static const int kSwap[8] = {0, 1, 2, 3, 5, 4, 7, 6};
                st(c, i) = arith(kSwap[r], st(c, i), st(c, 0));
            }
            return true;
        case 0xDE:
            if (r == 3 && i == 1) {  // FCOMPP
                compare(c, st(c, 0), st(c, 1));
                fpop(c);
                fpop(c);
                return true;
            }
            if (r == 2 || r == 3) {
                compare(c, st(c, 0), st(c, i));
                fpop(c);
                return true;
            }
            {
                static const int kSwap[8] = {0, 1, 2, 3, 5, 4, 7, 6};
                st(c, i) = arith(kSwap[r], st(c, i), st(c, 0));
            }
            fpop(c);
            return true;
        case 0xD9:
            switch (modrm) {
                case 0xD0: return true;  // FNOP
                case 0xE0: st(c, 0) = -st(c, 0); return true;
                case 0xE1: st(c, 0) = std::fabs(st(c, 0)); return true;
                case 0xE4: compare(c, st(c, 0), 0.0); return true;  // FTST
                case 0xE5: {  // FXAM
                    uint16_t cc = std::signbit(st(c, 0)) ? C1 : 0;
                    double v = st(c, 0);
                    if (st_empty(c, 0))
                        cc |= C3 | C0;
                    else if (std::isnan(v))
                        cc |= C0;
                    else if (std::isinf(v))
                        cc |= C2 | C0;
                    else if (v == 0.0)
                        cc |= C3;
                    else if (std::fpclassify(v) == FP_SUBNORMAL)
                        cc |= C3 | C2;
                    else
                        cc |= C2;
                    set_cc(c, cc);
                    return true;
                }
                case 0xE8: fpush(c, 1.0); return true;
                case 0xE9: fpush(c, 3.321928094887362347870319429489390175864831393); return true;
                case 0xEA: fpush(c, 1.442695040888963407359924681001892137426645954); return true;
                case 0xEB: fpush(c, M_PI); return true;
                case 0xEC: fpush(c, 0.301029995663981195213738894724493026768189881); return true;
                case 0xED: fpush(c, M_LN2); return true;
                case 0xEE: fpush(c, 0.0); return true;
                case 0xF0: st(c, 0) = std::exp2(st(c, 0)) - 1.0; return true;  // F2XM1
                case 0xF1:  // FYL2X
                    st(c, 1) = st(c, 1) * std::log2(st(c, 0));
                    fpop(c);
                    return true;
                case 0xF2:  // FPTAN
                    st(c, 0) = std::tan(st(c, 0));
                    fpush(c, 1.0);
                    set_cc(c, 0);
                    return true;
                case 0xF3:  // FPATAN
                    st(c, 1) = std::atan2(st(c, 1), st(c, 0));
                    fpop(c);
                    return true;
                case 0xF4: {  // FXTRACT
                    double v = st(c, 0);
                    int e = 0;
                    double f = std::frexp(v, &e);
                    st(c, 0) = (double)(e - 1);
                    fpush(c, f * 2.0);
                    return true;
                }
                case 0xF5:  // FPREM1
                case 0xF8: {  // FPREM
                    double a = st(c, 0), b = st(c, 1);
                    double q = modrm == 0xF8 ? std::trunc(a / b) : std::nearbyint(a / b);
                    st(c, 0) = a - q * b;
                    int64_t qi = (int64_t)std::fabs(q);
                    set_cc(c, ((qi & 1) ? C1 : 0) | ((qi & 2) ? C3 : 0) | ((qi & 4) ? C0 : 0));
                    return true;
                }
                case 0xF6: c.ftop = (c.ftop - 1) & 7; return true;
                case 0xF7: c.ftop = (c.ftop + 1) & 7; return true;
                case 0xF9:  // FYL2XP1
                    st(c, 1) = st(c, 1) * std::log2(st(c, 0) + 1.0);
                    fpop(c);
                    return true;
                case 0xFA: st(c, 0) = std::sqrt(st(c, 0)); return true;
                case 0xFB: {  // FSINCOS
                    double v = st(c, 0);
                    st(c, 0) = std::sin(v);
                    fpush(c, std::cos(v));
                    set_cc(c, 0);
                    return true;
                }
                case 0xFC: st(c, 0) = round_rc(c, st(c, 0)); return true;
                case 0xFD: st(c, 0) = std::ldexp(st(c, 0), (int)std::trunc(st(c, 1))); return true;
                case 0xFE: st(c, 0) = std::sin(st(c, 0)); set_cc(c, 0); return true;
                case 0xFF: st(c, 0) = std::cos(st(c, 0)); set_cc(c, 0); return true;
                default: break;
            }
            if (r == 0) {  // FLD st(i)
                double v = st(c, i);
                fpush(c, v);
                return true;
            }
            if (r == 1) {  // FXCH
                double t = st(c, 0);
                st(c, 0) = st(c, i);
                st(c, i) = t;
                bool e0 = st_empty(c, 0), ei = st_empty(c, i);
                set_tag(c, 0, ei);
                set_tag(c, i, e0);
                return true;
            }
            if (r == 3) {  // FSTP1 (undocumented alias)
                st(c, i) = st(c, 0);
                fpop(c);
                return true;
            }
            return false;
        case 0xDA:
            if (modrm == 0xE9) {  // FUCOMPP
                compare(c, st(c, 0), st(c, 1));
                fpop(c);
                fpop(c);
                return true;
            }
            {
                bool take;
                switch (r) {
                    case 0: take = get_cf(c); break;
                    case 1: take = get_zf(c); break;
                    case 2: take = get_cf(c) || get_zf(c); break;
                    case 3: take = get_pf(c); break;
                    default: return false;
                }
                if (take) st(c, 0) = st(c, i);
            }
            return true;
        case 0xDB:
            if (modrm == 0xE2) {  // FNCLEX
                c.fsw &= 0x7F00;
                return true;
            }
            if (modrm == 0xE3) {  // FNINIT
                fpu_reset(c);
                return true;
            }
            if (r == 5 || r == 6) {  // FUCOMI / FCOMI
                compare_eflags(c, st(c, 0), st(c, i));
                return true;
            }
            {
                bool take;
                switch (r) {
                    case 0: take = !get_cf(c); break;
                    case 1: take = !get_zf(c); break;
                    case 2: take = !get_cf(c) && !get_zf(c); break;
                    case 3: take = !get_pf(c); break;
                    default: return false;
                }
                if (take) st(c, 0) = st(c, i);
            }
            return true;
        case 0xDD:
            switch (r) {
                case 0: set_tag(c, i, true); return true;  // FFREE
                case 2: st(c, i) = st(c, 0); set_tag(c, i, false); return true;
                case 3: st(c, i) = st(c, 0); set_tag(c, i, false); fpop(c); return true;
                case 4: compare(c, st(c, 0), st(c, i)); return true;
                case 5: compare(c, st(c, 0), st(c, i)); fpop(c); return true;
                default: return false;
            }
        case 0xDF:
            if (modrm == 0xE0) {  // FNSTSW AX
                uint16_t sw = (c.fsw & ~0x3800) | ((c.ftop & 7) << 11);
                c.r[EAX] = (c.r[EAX] & 0xFFFF0000u) | sw;
                return true;
            }
            if (r == 5 || r == 6) {  // FUCOMIP / FCOMIP
                compare_eflags(c, st(c, 0), st(c, i));
                fpop(c);
                return true;
            }
            if (r == 0) {  // FFREEP
                set_tag(c, i, true);
                fpop(c);
                return true;
            }
            return false;
    }
    return false;
}

}  // namespace cpu
