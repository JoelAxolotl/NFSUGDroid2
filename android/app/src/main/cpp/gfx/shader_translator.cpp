#include "gfx/shader_translator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace d3d {

namespace {

enum Op {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT,
    OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2,
    OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM,
    OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB,
    OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_TEXBEM, OP_TEXBEML, OP_TEXREG2AR, OP_TEXREG2GB, OP_TEXM3x2PAD,
    OP_TEXM3x2TEX, OP_TEXM3x3PAD, OP_TEXM3x3TEX, OP_RESERVED0, OP_TEXM3x3SPEC, OP_TEXM3x3VSPEC, OP_EXPP,
    OP_LOGP, OP_CND, OP_DEF, OP_TEXREG2RGB, OP_TEXDP3TEX, OP_TEXM3x2DEPTH, OP_TEXDP3, OP_TEXM3x3, OP_TEXDEPTH,
    OP_CMP, OP_BEM, OP_DP2ADD, OP_DSX, OP_DSY, OP_TEXLDD, OP_SETP, OP_TEXLDL, OP_BREAKP,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};

enum RegType {
    R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3 /* VS a0, PS t# */, R_RASTOUT = 4, R_ATTROUT = 5,
    R_TEXCRDOUT = 6, R_CONSTINT = 7, R_COLOROUT = 8, R_DEPTHOUT = 9, R_SAMPLER = 10, R_CONST2 = 11,
    R_CONST3 = 12, R_CONST4 = 13, R_CONSTBOOL = 14, R_LOOP = 15, R_TEMPFLOAT16 = 16, R_MISCTYPE = 17,
    R_LABEL = 18, R_PREDICATE = 19,
};

inline int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
inline int reg_num(uint32_t t) { return (int)(t & 0x7FF); }

// Parameter count (dst + src) for shader model 1 bytecode, which has no length field.
int sm1_length(int op, bool pixel, int major, int minor) {
    switch (op) {
        case OP_NOP: case OP_RET: case OP_ENDLOOP: case OP_ENDREP: case OP_ELSE: case OP_ENDIF: case OP_BREAK:
        case OP_PHASE:
            return 0;
        case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC: case OP_ABS:
        case OP_NRM: case OP_MOVA: case OP_EXPP: case OP_LOGP: case OP_DSX: case OP_DSY:
        case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXM3x2PAD:
        case OP_TEXM3x2TEX: case OP_TEXM3x3PAD: case OP_TEXM3x3TEX: case OP_TEXM3x3VSPEC: case OP_TEXREG2RGB:
        case OP_TEXDP3TEX: case OP_TEXM3x2DEPTH: case OP_TEXDP3: case OP_TEXM3x3:
            return 2;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT:
        case OP_SGE: case OP_DST: case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2:
        case OP_POW: case OP_CRS: case OP_TEXM3x3SPEC: case OP_BEM:
            return 3;
        case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: case OP_DP2ADD: case OP_SGN:
            return 4;
        case OP_DCL: return 2;
        case OP_DEF: return 5;
        case OP_TEXCOORD: case OP_TEX:
            return (pixel && major == 1 && minor >= 4) ? 2 : 1;
        case OP_TEXKILL: case OP_TEXDEPTH: return 1;
        case OP_SINCOS: return 4;
        default: return -1;
    }
}

const char kComp[] = "xyzw";

std::string fmt_float(float f) {
    char b[48];
    if (f != f) return "0.0";
    if (f > 3.0e38f) return "3.0e38";
    if (f < -3.0e38f) return "-3.0e38";
    snprintf(b, sizeof b, "%.9g", f);
    std::string s = b;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

struct Param {
    uint32_t tok = 0;
    uint32_t rel = 0;
    bool has_rel = false;
};

class Translator {
public:
    Translator(const std::vector<uint32_t>& t, const uint8_t* dims) : t_(t), dims_(dims) {}

    ShaderInfo run() {
        info_.ok = false;
        if (t_.empty()) return fail("empty shader");
        uint32_t ver = t_[0];
        pixel_ = (ver >> 16) == 0xFFFF;
        if (!pixel_ && (ver >> 16) != 0xFFFE) return fail("bad version token");
        major_ = (ver >> 8) & 0xFF;
        minor_ = ver & 0xFF;
        info_.pixel = pixel_;
        info_.major = major_;
        info_.minor = minor_;
        if (major_ > 2) return fail("shader model 3 is not supported");
        ps_tex_is_coord_ = pixel_ && (major_ >= 2 || minor_ >= 4);
        cur_ = &main_;
        // Pass 1: definitions and declarations (DEF values are needed before use).
        if (!scan()) return info_;
        // Pass 2: code.
        size_t p = 1;
        while (p < t_.size()) {
            uint32_t tok = t_[p];
            int op = (int)(tok & 0xFFFF);
            if (op == OP_END) break;
            if (op == OP_COMMENT) {
                p += 1 + ((tok >> 16) & 0x7FFF);
                continue;
            }
            int len = major_ >= 2 ? (int)((tok >> 24) & 0xF) : sm1_length(op, pixel_, major_, minor_);
            if (len < 0) return fail("unknown opcode " + std::to_string(op));
            size_t end = p + 1 + (size_t)len;
            if (end > t_.size()) return fail("truncated instruction");
            // SM1 relative addressing has no extra token; SM2 adds one after a relative source.
            if (!emit(tok, op, p + 1, end)) return info_;
            p = end;
        }
        if (!error_.empty()) return fail(error_);
        build();
        info_.ok = true;
        return info_;
    }

private:
    const std::vector<uint32_t>& t_;
    const uint8_t* dims_;
    bool pixel_ = false;
    bool ps_tex_is_coord_ = false;
    uint32_t major_ = 0, minor_ = 0;
    ShaderInfo info_;
    std::string error_;
    std::string main_, funcs_, protos_;
    std::string* cur_;
    int depth_ = 1;
    int loop_id_ = 0;
    bool in_func_ = false;
    std::map<int, std::string> defs_;   // float constant literals
    std::map<int, std::string> defi_;
    std::map<int, std::string> defb_;
    std::set<int> temps_, inputs_, textures_, outputs_t_, colorouts_;
    int max_const_ = -1;
    bool rel_const_ = false;
    bool uses_a0_ = false, uses_pad_ = false;

    ShaderInfo fail(const std::string& e) {
        info_.ok = false;
        info_.error = e;
        return info_;
    }

    void line(const std::string& s) {
        cur_->append((size_t)depth_ * 2, ' ');
        *cur_ += s;
        *cur_ += '\n';
    }

    // Reads params [p, end) into dst / srcs.
    std::vector<Param> params(size_t p, size_t end) {
        std::vector<Param> out;
        while (p < end) {
            Param pr;
            pr.tok = t_[p++];
            if (major_ >= 2 && (pr.tok & (1u << 13)) && p < end && !out.empty()) {
                pr.has_rel = true;
                pr.rel = t_[p++];
            } else if ((pr.tok & (1u << 13)) && !out.empty()) {
                pr.has_rel = true;  // SM1: implicit a0.x
                pr.rel = 0;
            }
            out.push_back(pr);
        }
        return out;
    }

    bool scan() {
        size_t p = 1;
        while (p < t_.size()) {
            uint32_t tok = t_[p];
            int op = (int)(tok & 0xFFFF);
            if (op == OP_END) break;
            if (op == OP_COMMENT) {
                p += 1 + ((tok >> 16) & 0x7FFF);
                continue;
            }
            int len = major_ >= 2 ? (int)((tok >> 24) & 0xF) : sm1_length(op, pixel_, major_, minor_);
            if (len < 0) {
                fail("unknown opcode " + std::to_string(op) + " in scan");
                return false;
            }
            if (op == OP_DEF && p + 5 < t_.size() + 1) {
                int n = reg_num(t_[p + 1]);
                float f[4];
                memcpy(f, &t_[p + 2], 16);
                defs_[n] = "vec4(" + fmt_float(f[0]) + ", " + fmt_float(f[1]) + ", " + fmt_float(f[2]) + ", " +
                           fmt_float(f[3]) + ")";
            } else if (op == OP_DEFI) {
                int n = reg_num(t_[p + 1]);
                int32_t v[4];
                memcpy(v, &t_[p + 2], 16);
                defi_[n] = "ivec4(" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", " +
                           std::to_string(v[2]) + ", " + std::to_string(v[3]) + ")";
            } else if (op == OP_DEFB) {
                defb_[reg_num(t_[p + 1])] = t_[p + 2] ? "true" : "false";
            } else if (op == OP_DCL) {
                uint32_t dcl = t_[p + 1], reg = t_[p + 2];
                int type = reg_type(reg), n = reg_num(reg);
                if (!pixel_ && type == R_INPUT) {
                    info_.inputs.push_back({(uint8_t)(dcl & 0x1F), (uint8_t)((dcl >> 16) & 0xF), (uint8_t)n});
                    inputs_.insert(n);
                } else if (pixel_ && type == R_SAMPLER && n < 16) {
                    uint32_t tt = (dcl >> 27) & 0xF;
                    info_.sampler_dim[n] = (uint8_t)(tt == 3 ? 3 : tt == 4 ? 4 : 2);
                }
            }
            p += 1 + (size_t)len;
        }
        return true;
    }

    std::string swizzle(uint32_t sw) {
        if (sw == 0xE4) return "";
        std::string s = ".";
        for (int i = 0; i < 4; ++i) s += kComp[(sw >> (i * 2)) & 3];
        return s;
    }

    std::string rel_index(const Param& p) {
        if (major_ < 2) {
            uses_a0_ = true;
            return "a0.x";
        }
        int rt = reg_type(p.rel);
        int comp = (int)((p.rel >> 16) & 3);
        if (rt == R_LOOP) return "aL";
        uses_a0_ = true;
        return std::string("a0.") + kComp[comp];
    }

    std::string const_ref(int n, const Param& p, bool vs_bank) {
        if (!p.has_rel) {
            if (defs_.count(n)) return defs_[n];
            max_const_ = std::max(max_const_, n);
            return std::string(vs_bank ? "vc[" : "pc[") + std::to_string(n) + "]";
        }
        rel_const_ = true;
        return std::string(vs_bank ? "vc[clamp(" : "pc[clamp(") + rel_index(p) + " + " + std::to_string(n) +
               (vs_bank ? ", 0, 255)]" : ", 0, 31)]");
    }

    // Name of a register used as a source (before swizzle/modifiers), as vec4.
    std::string reg_read(const Param& p, int offset = 0) {
        int type = reg_type(p.tok), n = reg_num(p.tok) + offset;
        switch (type) {
            case R_TEMP:
                temps_.insert(n);
                return "r" + std::to_string(n);
            case R_INPUT:
                if (pixel_) return n == 0 ? "D3D_D0" : n == 1 ? "D3D_D1" : "vec4(0.0)";
                inputs_.insert(n);
                return "v" + std::to_string(n);
            case R_CONST:
                return const_ref(n, p, !pixel_);
            case R_CONST2: return const_ref(n + 2048, p, !pixel_);
            case R_CONST3: return const_ref(n + 4096, p, !pixel_);
            case R_CONST4: return const_ref(n + 6144, p, !pixel_);
            case R_ADDR:
                if (!pixel_) {
                    uses_a0_ = true;
                    return "vec4(a0)";
                }
                if (ps_tex_is_coord_) return "D3D_T" + std::to_string(n);
                textures_.insert(n);
                return "t" + std::to_string(n);
            case R_CONSTINT:
                if (defi_.count(n)) return "vec4(" + defi_[n] + ")";
                return std::string("vec4(") + (pixel_ ? "pi[" : "vi[") + std::to_string(n) + "])";
            case R_CONSTBOOL:
                return "vec4(" + bool_ref(n) + " ? 1.0 : 0.0)";
            case R_LOOP: return "vec4(float(aL))";
            case R_MISCTYPE:
                return n == 0 ? "vec4(gl_FragCoord.x - 0.5, u_rth - gl_FragCoord.y - 0.5, 0.0, 0.0)"
                              : "vec4(gl_FrontFacing ? -1.0 : 1.0)";
            case R_TEXCRDOUT:
                return "oT" + std::to_string(n);
            case R_ATTROUT:
                return "oD" + std::to_string(n);
            case R_RASTOUT:
                return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
            case R_COLOROUT:
                return "oC" + std::to_string(n);
            default:
                error_ = "unsupported source register type " + std::to_string(type);
                return "vec4(0.0)";
        }
    }

    std::string bool_ref(int n) {
        if (defb_.count(n)) return defb_[n];
        return std::string(pixel_ ? "pb[" : "vb[") + std::to_string(n) + "]";
    }

    std::string src(const Param& p, int offset = 0) {
        std::string r = reg_read(p, offset);
        uint32_t sw = (p.tok >> 16) & 0xFF;
        r += swizzle(sw);
        switch ((p.tok >> 24) & 0xF) {
            case 0: return r;
            case 1: return "(-" + r + ")";
            case 2: return "(" + r + " - 0.5)";
            case 3: return "(0.5 - " + r + ")";
            case 4: return "(" + r + " * 2.0 - 1.0)";
            case 5: return "(1.0 - " + r + " * 2.0)";
            case 6: return "(1.0 - " + r + ")";
            case 7: return "(" + r + " * 2.0)";
            case 8: return "(" + r + " * -2.0)";
            case 9: return "(" + r + " / " + r + ".z)";
            case 10: return "(" + r + " / " + r + ".w)";
            case 11: return "abs(" + r + ")";
            case 12: return "(-abs(" + r + "))";
            default: return r;
        }
    }

    std::string mask_str(uint32_t tok) {
        uint32_t m = (tok >> 16) & 0xF;
        if (m == 0xF || m == 0) return "";
        std::string s = ".";
        for (int i = 0; i < 4; ++i)
            if (m & (1u << i)) s += kComp[i];
        return s;
    }

    std::string dst_name(const Param& p) {
        int type = reg_type(p.tok), n = reg_num(p.tok);
        switch (type) {
            case R_TEMP:
                temps_.insert(n);
                return "r" + std::to_string(n);
            case R_ADDR:
                if (!pixel_) {
                    uses_a0_ = true;
                    return "a0";
                }
                textures_.insert(n);
                return "t" + std::to_string(n);
            case R_RASTOUT:
                if (n == 2) info_.writes_point_size = true;
                return n == 0 ? "oPos" : n == 1 ? "oFog" : "oPts";
            case R_ATTROUT: return "oD" + std::to_string(n);
            case R_TEXCRDOUT:
                outputs_t_.insert(n);
                return "oT" + std::to_string(n);
            case R_COLOROUT:
                colorouts_.insert(n);
                return "oC" + std::to_string(n);
            case R_DEPTHOUT: return "oDepth";
            default:
                error_ = "unsupported destination register type " + std::to_string(type);
                return "r_bad";
        }
    }

    // Emits `dst.mask = modifiers(expr).mask;`
    void assign(const Param& d, const std::string& expr) {
        std::string name = dst_name(d);
        std::string m = mask_str(d.tok);
        std::string e = "(" + expr + ")";
        int shift = (int)((d.tok >> 24) & 0xF);
        if (shift) {
            static const char* kScale[16] = {"1.0", "2.0", "4.0", "8.0", "16.0", "32.0", "64.0", "128.0",
                                             "0.00390625", "0.0078125", "0.015625", "0.03125", "0.0625",
                                             "0.125", "0.25", "0.5"};
            e = "(" + e + " * " + kScale[shift] + ")";
        }
        if ((d.tok >> 20) & 1) e = "clamp(" + e + ", 0.0, 1.0)";
        if (name == "a0") {
            // vs_1_1 `mov a0` floors; vs_2_0 `mova` rounds to nearest.
            std::string conv = major_ >= 2 ? "ivec4(floor(" + e + " + 0.5))" : "ivec4(floor(" + e + "))";
            line("a0" + m + " = " + conv + m + ";");
            return;
        }
        if (name == "oDepth") {
            line("gl_FragDepth = clamp(" + e + ".x, 0.0, 1.0);");
            return;
        }
        line(name + m + " = " + e + m + ";");
    }

    static std::string w_of(const std::string& s) { return "(" + s + ").w"; }

    std::string cmp_op(uint32_t tok, const std::string& a, const std::string& b) {
        static const char* kOps[8] = {"false", ">", "==", ">=", "<", "!=", "<=", "true"};
        int c = (int)((tok >> 16) & 7);
        if (c == 0 || c == 7) return kOps[c];
        return "(" + a + ").x " + kOps[c] + " (" + b + ").x";
    }

    std::string sampler_expr(int s, const std::string& coord, int mode /*0 plain,1 proj,2 bias,3 lod*/) {
        int dim = s < 16 ? info_.sampler_dim[s] : 2;
        if (!dim) {
            dim = dims_ ? (dims_[s] ? dims_[s] : 2) : 2;
            info_.sampler_dim[s] = (uint8_t)dim;
        }
        std::string sn = "s" + std::to_string(s);
        std::string c = "(" + coord + ")";
        const char* comps = dim == 2 ? ".xy" : ".xyz";
        switch (mode) {
            case 1:
                if (dim == 2) return "texture(" + sn + ", " + c + ".xy / " + c + ".w)";
                return "texture(" + sn + ", " + c + ".xyz / " + c + ".w)";
            case 2: return "texture(" + sn + ", " + c + comps + ", " + c + ".w)";
            case 3: return "textureLod(" + sn + ", " + c + comps + ", " + c + ".w)";
            default: return "texture(" + sn + ", " + c + comps + ")";
        }
    }

    bool emit(uint32_t tok, int op, size_t p, size_t end) {
        std::vector<Param> a = params(p, end);
        auto need = [&](size_t n) {
            if (a.size() < n) {
                error_ = "malformed instruction (opcode " + std::to_string(op) + ")";
                return false;
            }
            return true;
        };
        switch (op) {
            case OP_NOP: case OP_PHASE: case OP_DCL: case OP_DEF: case OP_DEFI: case OP_DEFB:
                return true;
            case OP_MOV: if (!need(2)) return false; assign(a[0], src(a[1])); return true;
            case OP_MOVA: if (!need(2)) return false; assign(a[0], src(a[1])); return true;
            case OP_ADD: if (!need(3)) return false; assign(a[0], src(a[1]) + " + " + src(a[2])); return true;
            case OP_SUB: if (!need(3)) return false; assign(a[0], src(a[1]) + " - " + src(a[2])); return true;
            case OP_MUL: if (!need(3)) return false; assign(a[0], src(a[1]) + " * " + src(a[2])); return true;
            case OP_MAD:
                if (!need(4)) return false;
                assign(a[0], src(a[1]) + " * " + src(a[2]) + " + " + src(a[3]));
                return true;
            case OP_RCP: if (!need(2)) return false; assign(a[0], "vec4(1.0 / " + w_of(src(a[1])) + ")"); return true;
            case OP_RSQ:
                if (!need(2)) return false;
                assign(a[0], "vec4(inversesqrt(abs(" + w_of(src(a[1])) + ")))");
                return true;
            case OP_DP3:
                if (!need(3)) return false;
                assign(a[0], "vec4(dot((" + src(a[1]) + ").xyz, (" + src(a[2]) + ").xyz))");
                return true;
            case OP_DP4:
                if (!need(3)) return false;
                assign(a[0], "vec4(dot(" + src(a[1]) + ", " + src(a[2]) + "))");
                return true;
            case OP_MIN: if (!need(3)) return false; assign(a[0], "min(" + src(a[1]) + ", " + src(a[2]) + ")"); return true;
            case OP_MAX: if (!need(3)) return false; assign(a[0], "max(" + src(a[1]) + ", " + src(a[2]) + ")"); return true;
            case OP_SLT:
                if (!need(3)) return false;
                assign(a[0], "vec4(lessThan(" + src(a[1]) + ", " + src(a[2]) + "))");
                return true;
            case OP_SGE:
                if (!need(3)) return false;
                assign(a[0], "vec4(greaterThanEqual(" + src(a[1]) + ", " + src(a[2]) + "))");
                return true;
            case OP_EXP: if (!need(2)) return false; assign(a[0], "vec4(exp2(" + w_of(src(a[1])) + "))"); return true;
            case OP_LOG:
                if (!need(2)) return false;
                assign(a[0], "vec4(log2(abs(" + w_of(src(a[1])) + ")))");
                return true;
            case OP_EXPP:
                if (!need(2)) return false;
                if (major_ >= 2) {
                    assign(a[0], "vec4(exp2(" + w_of(src(a[1])) + "))");
                } else {
                    std::string s = w_of(src(a[1]));
                    assign(a[0], "vec4(exp2(floor(" + s + ")), fract(" + s + "), exp2(" + s + "), 1.0)");
                }
                return true;
            case OP_LOGP:
                if (!need(2)) return false;
                assign(a[0], "vec4(log2(abs(" + w_of(src(a[1])) + ")))");
                return true;
            case OP_LIT: if (!need(2)) return false; assign(a[0], "d3d_lit(" + src(a[1]) + ")"); return true;
            case OP_DST: {
                if (!need(3)) return false;
                std::string s0 = src(a[1]), s1 = src(a[2]);
                assign(a[0], "vec4(1.0, (" + s0 + ").y * (" + s1 + ").y, (" + s0 + ").z, (" + s1 + ").w)");
                return true;
            }
            case OP_LRP:
                if (!need(4)) return false;
                assign(a[0], "mix(" + src(a[3]) + ", " + src(a[2]) + ", " + src(a[1]) + ")");
                return true;
            case OP_FRC: if (!need(2)) return false; assign(a[0], "fract(" + src(a[1]) + ")"); return true;
            case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
                if (!need(3)) return false;
                int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M4x3 || op == OP_M3x3 ? 3 : 2;
                bool three = op == OP_M3x4 || op == OP_M3x3 || op == OP_M3x2;
                std::string v = src(a[1]);
                std::string e = "vec4(";
                for (int i = 0; i < 4; ++i) {
                    if (i) e += ", ";
                    if (i >= rows) {
                        e += "0.0";
                        continue;
                    }
                    std::string row = src(a[2], i);
                    e += three ? "dot((" + v + ").xyz, (" + row + ").xyz)" : "dot(" + v + ", " + row + ")";
                }
                assign(a[0], e + ")");
                return true;
            }
            case OP_POW:
                if (!need(3)) return false;
                assign(a[0], "vec4(pow(abs(" + w_of(src(a[1])) + "), " + w_of(src(a[2])) + "))");
                return true;
            case OP_CRS:
                if (!need(3)) return false;
                assign(a[0], "vec4(cross((" + src(a[1]) + ").xyz, (" + src(a[2]) + ").xyz), 0.0)");
                return true;
            case OP_SGN: if (!need(2)) return false; assign(a[0], "sign(" + src(a[1]) + ")"); return true;
            case OP_ABS: if (!need(2)) return false; assign(a[0], "abs(" + src(a[1]) + ")"); return true;
            case OP_NRM: {
                if (!need(2)) return false;
                std::string s = src(a[1]);
                assign(a[0], s + " * inversesqrt(max(dot((" + s + ").xyz, (" + s + ").xyz), 1e-30))");
                return true;
            }
            case OP_SINCOS: {
                if (!need(2)) return false;
                std::string s = w_of(src(a[1]));
                assign(a[0], "vec4(cos(" + s + "), sin(" + s + "), 0.0, 0.0)");
                return true;
            }
            case OP_CMP:
                if (!need(4)) return false;
                assign(a[0], "mix(" + src(a[3]) + ", " + src(a[2]) + ", greaterThanEqual(" + src(a[1]) +
                                 ", vec4(0.0)))");
                return true;
            case OP_CND:
                if (!need(4)) return false;
                assign(a[0], "mix(" + src(a[3]) + ", " + src(a[2]) + ", greaterThan(" + src(a[1]) +
                                 ", vec4(0.5)))");
                return true;
            case OP_DP2ADD:
                if (!need(4)) return false;
                assign(a[0], "vec4(dot((" + src(a[1]) + ").xy, (" + src(a[2]) + ").xy) + " + w_of(src(a[3])) + ")");
                return true;
            case OP_DSX: if (!need(2)) return false; assign(a[0], "dFdx(" + src(a[1]) + ")"); return true;
            case OP_DSY: if (!need(2)) return false; assign(a[0], "(-dFdy(" + src(a[1]) + "))"); return true;
            case OP_TEXKILL: {
                if (!need(1)) return false;
                Param k = a[0];
                std::string v;
                int type = reg_type(k.tok), n = reg_num(k.tok);
                if (type == R_ADDR) v = ps_tex_is_coord_ ? "D3D_T" + std::to_string(n) : "t" + std::to_string(n);
                else v = "r" + std::to_string(n);
                if (major_ < 2 && type == R_ADDR && !ps_tex_is_coord_) v = "D3D_T" + std::to_string(n);
                line(std::string("if (any(lessThan(") + v + (major_ >= 2 ? ", vec4(0.0)" : ".xyz, vec3(0.0)") +
                     "))) discard;");
                return true;
            }
            case OP_TEX: {
                if (!pixel_) {
                    error_ = "texld in vertex shader";
                    return false;
                }
                if (major_ >= 2) {
                    if (!need(3)) return false;
                    int s = reg_num(a[2].tok);
                    int mode = (int)((tok >> 16) & 3);  // 1 = project, 2 = bias
                    std::string e = sampler_expr(s, src(a[1]), mode == 1 ? 1 : mode == 2 ? 2 : 0);
                    e = "(" + e + ")" + swizzle((a[2].tok >> 16) & 0xFF);
                    assign(a[0], e);
                } else if (minor_ >= 4) {  // ps_1_4: texld rN, tM|rM
                    if (!need(2)) return false;
                    int s = reg_num(a[0].tok);
                    assign(a[0], sampler_expr(s, src(a[1]), 0));
                } else {  // ps_1_1..1_3: tex tN
                    if (!need(1)) return false;
                    int n = reg_num(a[0].tok);
                    textures_.insert(n);
                    line("t" + std::to_string(n) + " = " + sampler_expr(n, "D3D_T" + std::to_string(n), 0) + ";");
                }
                return true;
            }
            case OP_TEXLDL:
                if (!need(3)) return false;
                assign(a[0], "(" + sampler_expr(reg_num(a[2].tok), src(a[1]), 3) + ")" +
                                 swizzle((a[2].tok >> 16) & 0xFF));
                return true;
            case OP_TEXLDD: {
                if (!need(5)) return false;
                int s = reg_num(a[2].tok);
                int dim = info_.sampler_dim[s] ? info_.sampler_dim[s] : 2;
                const char* c = dim == 2 ? ".xy" : ".xyz";
                assign(a[0], "textureGrad(s" + std::to_string(s) + ", (" + src(a[1]) + ")" + c + ", (" + src(a[3]) +
                                 ")" + c + ", (" + src(a[4]) + ")" + c + ")");
                return true;
            }
            case OP_TEXCOORD: {
                if (minor_ >= 4 && major_ == 1) {  // texcrd rN, tM
                    if (!need(2)) return false;
                    assign(a[0], "vec4((" + src(a[1]) + ").xyz, 1.0)");
                } else {
                    if (!need(1)) return false;
                    int n = reg_num(a[0].tok);
                    textures_.insert(n);
                    line("t" + std::to_string(n) + " = clamp(vec4(D3D_T" + std::to_string(n) + ".xyz, 1.0), 0.0, 1.0);");
                }
                return true;
            }
            case OP_TEXBEM:
            case OP_TEXBEML: {
                if (!need(2)) return false;
                int m = reg_num(a[0].tok), n = reg_num(a[1].tok);
                textures_.insert(m);
                std::string ms = std::to_string(m), tn = "t" + std::to_string(n);
                line("{ vec2 uv = D3D_T" + ms + ".xy + vec2(u_bem[" + ms + "].x * " + tn + ".x + u_bem[" + ms + "].z * " +
                     tn + ".y, u_bem[" + ms + "].y * " + tn + ".x + u_bem[" + ms + "].w * " + tn + ".y);");
                line("  t" + ms + " = " + sampler_expr(m, "vec4(uv, 0.0, 1.0)", 0) + ";");
                if (op == OP_TEXBEML)
                    line("  t" + ms + ".rgb *= clamp(" + tn + ".z * u_beml[" + ms + "].x + u_beml[" + ms + "].y, 0.0, 1.0);");
                line("}");
                return true;
            }
            case OP_TEXM3x2PAD:
            case OP_TEXM3x3PAD: {
                if (!need(2)) return false;
                int m = reg_num(a[0].tok), n = reg_num(a[1].tok);
                uses_pad_ = true;
                // The first pad row goes to pad.x, the second (3x3 only) to pad.y.
                std::string slot = (op == OP_TEXM3x3PAD && pad_rows_ == 1) ? "y" : "x";
                pad_base_ = pad_rows_ == 0 ? m : pad_base_;
                ++pad_rows_;
                line("pad." + slot + " = dot(D3D_T" + std::to_string(m) + ".xyz, t" + std::to_string(n) + ".xyz);");
                return true;
            }
            case OP_TEXM3x2TEX: {
                if (!need(2)) return false;
                int m = reg_num(a[0].tok), n = reg_num(a[1].tok);
                textures_.insert(m);
                line("t" + std::to_string(m) + " = " +
                     sampler_expr(m, "vec4(pad.x, dot(D3D_T" + std::to_string(m) + ".xyz, t" + std::to_string(n) +
                                         ".xyz), 0.0, 1.0)", 0) + ";");
                pad_rows_ = 0;
                return true;
            }
            case OP_TEXM3x3TEX:
            case OP_TEXM3x3:
            case OP_TEXM3x3SPEC:
            case OP_TEXM3x3VSPEC: {
                if (!need(2)) return false;
                int m = reg_num(a[0].tok), n = reg_num(a[1].tok);
                textures_.insert(m);
                std::string N = "vec3(pad.x, pad.y, dot(D3D_T" + std::to_string(m) + ".xyz, t" + std::to_string(n) + ".xyz))";
                std::string tm = "t" + std::to_string(m);
                if (op == OP_TEXM3x3) {
                    line(tm + " = vec4(" + N + ", 1.0);");
                } else if (op == OP_TEXM3x3TEX) {
                    if (!dims_ || dims_[m] != 4) info_.sampler_dim[m] = 3;
                    line(tm + " = " + sampler_expr(m, "vec4(" + N + ", 1.0)", 0) + ";");
                } else {
                    info_.sampler_dim[m] = 3;
                    std::string E;
                    if (op == OP_TEXM3x3SPEC) {
                        if (!need(3)) return false;
                        E = "(" + src(a[2]) + ").xyz";
                    } else {
                        int b = pad_base_;
                        E = "vec3(D3D_T" + std::to_string(b) + ".w, D3D_T" + std::to_string(b + 1) + ".w, D3D_T" +
                            std::to_string(b + 2) + ".w)";
                    }
                    line("{ vec3 nn = " + N + "; vec3 ee = " + E + ";");
                    line("  vec3 rr = 2.0 * nn * dot(nn, ee) / max(dot(nn, nn), 1e-30) - ee;");
                    line("  " + tm + " = " + sampler_expr(m, "vec4(rr, 1.0)", 0) + "; }");
                }
                pad_rows_ = 0;
                return true;
            }
            case OP_TEXREG2AR:
            case OP_TEXREG2GB:
            case OP_TEXREG2RGB: {
                if (!need(2)) return false;
                int m = reg_num(a[0].tok), n = reg_num(a[1].tok);
                textures_.insert(m);
                std::string tn = "t" + std::to_string(n);
                std::string uv = op == OP_TEXREG2AR ? tn + ".wx" : op == OP_TEXREG2GB ? tn + ".yz" : tn + ".xyz";
                line("t" + std::to_string(m) + " = " + sampler_expr(m, "vec4(" + uv + (op == OP_TEXREG2RGB ? ", 1.0)" : ", 0.0, 1.0)"), 0) + ";");
                return true;
            }
            case OP_TEXDP3:
            case OP_TEXDP3TEX: {
                if (!need(2)) return false;
                int m = reg_num(a[0].tok), n = reg_num(a[1].tok);
                textures_.insert(m);
                std::string d = "dot(D3D_T" + std::to_string(m) + ".xyz, t" + std::to_string(n) + ".xyz)";
                if (op == OP_TEXDP3)
                    line("t" + std::to_string(m) + " = vec4(" + d + ");");
                else
                    line("t" + std::to_string(m) + " = " + sampler_expr(m, "vec4(" + d + ", 0.0, 0.0, 1.0)", 0) + ";");
                return true;
            }
            case OP_TEXDEPTH:
                line("gl_FragDepth = clamp(r5.x / r5.y, 0.0, 1.0);");
                temps_.insert(5);
                return true;
            case OP_BEM: {
                if (!need(3)) return false;
                int m = reg_num(a[0].tok);
                std::string ms = std::to_string(m), s0 = src(a[1]), s1 = src(a[2]);
                assign(a[0], "vec4((" + s0 + ").x + u_bem[" + ms + "].x * (" + s1 + ").x + u_bem[" + ms + "].z * (" + s1 +
                                 ").y, (" + s0 + ").y + u_bem[" + ms + "].y * (" + s1 + ").x + u_bem[" + ms + "].w * (" + s1 +
                                 ").y, 0.0, 0.0)");
                return true;
            }
            // ---- control flow (SM2)
            case OP_IF:
                if (!need(1)) return false;
                line("if (" + bool_ref(reg_num(a[0].tok)) + ") {");
                ++depth_;
                return true;
            case OP_IFC:
                if (!need(2)) return false;
                line("if (" + cmp_op(tok, src(a[0]), src(a[1])) + ") {");
                ++depth_;
                return true;
            case OP_ELSE:
                --depth_;
                line("} else {");
                ++depth_;
                return true;
            case OP_ENDIF:
            case OP_ENDREP:
            case OP_ENDLOOP:
                --depth_;
                line("}");
                return true;
            case OP_REP: {
                if (!need(1)) return false;
                int n = reg_num(a[0].tok);
                std::string cnt = defi_.count(n) ? defi_[n] + ".x" : std::string(pixel_ ? "pi[" : "vi[") + std::to_string(n) + "].x";
                std::string k = "rep" + std::to_string(loop_id_++);
                line("for (int " + k + " = 0; " + k + " < " + cnt + "; ++" + k + ") {");
                ++depth_;
                return true;
            }
            case OP_LOOP: {
                if (!need(2)) return false;
                int n = reg_num(a[1].tok);
                std::string iv = defi_.count(n) ? defi_[n] : std::string(pixel_ ? "pi[" : "vi[") + std::to_string(n) + "]";
                std::string k = "lp" + std::to_string(loop_id_++);
                line("for (int " + k + " = 0, aL = " + iv + ".y; " + k + " < " + iv + ".x; ++" + k + ", aL += " + iv + ".z) {");
                ++depth_;
                return true;
            }
            case OP_BREAK: line("break;"); return true;
            case OP_BREAKC:
                if (!need(2)) return false;
                line("if (" + cmp_op(tok, src(a[0]), src(a[1])) + ") break;");
                return true;
            case OP_CALL:
                if (!need(1)) return false;
                line("L" + std::to_string(reg_num(a[0].tok)) + "();");
                return true;
            case OP_CALLNZ:
                if (!need(2)) return false;
                line("if (" + bool_ref(reg_num(a[1].tok)) + ") L" + std::to_string(reg_num(a[0].tok)) + "();");
                return true;
            case OP_LABEL: {
                if (!need(1)) return false;
                std::string name = "L" + std::to_string(reg_num(a[0].tok));
                protos_ += "void " + name + "();\n";
                cur_ = &funcs_;
                in_func_ = true;
                depth_ = 0;
                line("void " + name + "() {");
                depth_ = 1;
                return true;
            }
            case OP_RET:
                if (in_func_ && depth_ <= 1) {
                    depth_ = 0;
                    line("}");
                    depth_ = 1;
                    in_func_ = false;
                } else if (depth_ > 1) {
                    line("return;");
                }
                return true;
            default:
                error_ = "unsupported opcode " + std::to_string(op);
                return false;
        }
    }

    int pad_rows_ = 0;
    int pad_base_ = 0;

    void build() {
        std::string s = "#version 320 es\nprecision highp float;\nprecision highp int;\n";
        info_.const_count = rel_const_ ? (pixel_ ? 32u : 256u) : (uint32_t)(max_const_ + 1);
        if (!pixel_) {
            s += "uniform vec4 vc[256];\nuniform ivec4 vi[16];\nuniform bool vb[16];\nuniform int u_bgra;\n";
            for (const ShaderInput& in : info_.inputs)
                s += "layout(location = " + std::to_string(in.reg) + ") in vec4 v_" + std::to_string(in.reg) + ";\n";
            s += "out vec4 D3D_D0;\nout vec4 D3D_D1;\n";
            for (int i = 0; i < 8; ++i) s += "out vec4 D3D_T" + std::to_string(i) + ";\n";
            s += "out float D3D_FOG;\n";
            s += glsl_vs_epilogue();
            s += "vec4 oPos = vec4(0.0), oFog = vec4(0.0), oPts = vec4(1.0), oD0 = vec4(0.0), oD1 = vec4(0.0);\n";
            for (int i = 0; i < 8; ++i) s += "vec4 oT" + std::to_string(i) + " = vec4(0.0);\n";
            s += "ivec4 a0 = ivec4(0);\n";
            for (int n : inputs_)
                if (!std::count_if(info_.inputs.begin(), info_.inputs.end(), [n](const ShaderInput& i) { return i.reg == n; }))
                    s += "vec4 v" + std::to_string(n) + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
            for (const ShaderInput& in : info_.inputs) s += "vec4 v" + std::to_string(in.reg) + ";\n";
        } else {
            s += "precision highp sampler2D;\nprecision highp samplerCube;\nprecision highp sampler3D;\n";
            s += "uniform vec4 pc[32];\nuniform ivec4 pi[16];\nuniform bool pb[16];\nuniform vec4 u_bem[8];\nuniform vec4 u_beml[8];\nuniform float u_rth;\n";
            for (int i = 0; i < 16; ++i) {
                int d = info_.sampler_dim[i];
                if (!d) continue;
                s += std::string("uniform ") + (d == 3 ? "samplerCube" : d == 4 ? "sampler3D" : "sampler2D") + " s" +
                     std::to_string(i) + ";\n";
            }
            s += "in vec4 D3D_D0;\nin vec4 D3D_D1;\n";
            for (int i = 0; i < 8; ++i) s += "in vec4 D3D_T" + std::to_string(i) + ";\n";
            s += "in float D3D_FOG;\nlayout(location = 0) out vec4 fragColor;\n";
            s += glsl_ps_common();
            for (int i = 0; i < 4; ++i) s += "vec4 oC" + std::to_string(i) + " = vec4(0.0);\n";
            for (int n : textures_) s += "vec4 t" + std::to_string(n) + ";\n";
            if (uses_pad_) s += "vec2 pad = vec2(0.0);\n";
        }
        for (int n : temps_) s += "vec4 r" + std::to_string(n) + " = vec4(0.0);\n";
        if (!pixel_)
            s += "vec4 d3d_lit(vec4 s) { float d = max(s.x, 0.0); float p = clamp(s.w, -127.9961, 127.9961);\n"
                 "  float sp = (s.x > 0.0 && s.y > 0.0) ? pow(s.y, p) : 0.0; return vec4(1.0, d, sp, 1.0); }\n";
        s += protos_;
        s += funcs_;
        s += "void main() {\n";
        if (!pixel_) {
            for (const ShaderInput& in : info_.inputs) {
                std::string r = std::to_string(in.reg);
                s += "  v" + r + " = ((u_bgra & " + std::to_string(1 << in.reg) + ") != 0) ? v_" + r + ".zyxw : v_" + r + ";\n";
            }
        } else {
            for (int n : textures_) s += "  t" + std::to_string(n) + " = D3D_T" + std::to_string(n) + ";\n";
        }
        s += main_;
        if (!pixel_) {
            s += "  D3D_D0 = clamp(oD0, 0.0, 1.0);\n  D3D_D1 = clamp(oD1, 0.0, 1.0);\n";
            for (int i = 0; i < 8; ++i) s += "  D3D_T" + std::to_string(i) + " = oT" + std::to_string(i) + ";\n";
            s += "  D3D_FOG = oFog.x;\n";
            if (info_.writes_point_size) s += "  gl_PointSize = max(oPts.x, 1.0);\n";
            s += "  d3d_emit_position(oPos);\n";
        } else {
            bool ps1 = major_ < 2;
            s += std::string("  fragColor = d3d_finish(") + (ps1 ? "r0" : "oC0") + ");\n";
            if (ps1) temps_.insert(0);
        }
        s += "}\n";
        // ps_1_x writes r0 as the output; make sure it is declared.
        if (pixel_ && major_ < 2 && s.find("vec4 r0 = ") == std::string::npos) {
            size_t pos = s.find("void main()");
            s.insert(pos, "vec4 r0 = vec4(0.0);\n");
        }
        info_.glsl = s;
    }
};

}  // namespace

const char* glsl_vs_epilogue() {
    return "uniform vec4 u_fix;\n"
           "void d3d_emit_position(vec4 p) {\n"
           "  p.xy += u_fix.xy * p.w;\n"
           "  p.y = -p.y;\n"
           "  p.z = p.z * 2.0 - p.w;\n"
           "  gl_Position = p;\n"
           "}\n";
}

const char* glsl_ps_common() {
    // u_alpha: x = D3DCMPFUNC (0 = disabled), y = reference [0,1]
    // u_fog: x = mode (0 off, 1 vertex factor, 2 linear, 3 exp, 4 exp2), y start, z end, w density
    return "uniform vec4 u_alpha;\nuniform vec4 u_fog;\nuniform vec4 u_fogcolor;\n"
           "vec4 d3d_finish(vec4 c) {\n"
           "  int mode = int(u_fog.x);\n"
           "  if (mode != 0) {\n"
           "    float f;\n"
           "    if (mode == 1) f = D3D_FOG;\n"
           "    else {\n"
           "      float z = 1.0 / gl_FragCoord.w;\n"
           "      if (mode == 2) f = (u_fog.z - z) / max(u_fog.z - u_fog.y, 1e-6);\n"
           "      else if (mode == 3) f = exp(-u_fog.w * z);\n"
           "      else f = exp(-(u_fog.w * z) * (u_fog.w * z));\n"
           "    }\n"
           "    c.rgb = mix(u_fogcolor.rgb, c.rgb, clamp(f, 0.0, 1.0));\n"
           "  }\n"
           "  int af = int(u_alpha.x);\n"
           "  if (af != 0 && af != 8) {\n"
           "    float a = c.a, r = u_alpha.y;\n"
           "    bool pass = af == 2 ? a < r : af == 3 ? a == r : af == 4 ? a <= r : af == 5 ? a > r :\n"
           "                af == 6 ? a != r : af == 7 ? a >= r : false;\n"
           "    if (!pass) discard;\n"
           "  }\n"
           "  return c;\n"
           "}\n";
}

ShaderInfo translate_shader(const std::vector<uint32_t>& tokens, const uint8_t* ps1_dims) {
    Translator tr(tokens, ps1_dims);
    return tr.run();
}

}  // namespace d3d
