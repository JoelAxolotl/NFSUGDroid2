#include "gfx/ffp.h"

#include "gfx/shader_translator.h"

namespace d3d {

namespace {
std::string n(int v) { return std::to_string(v); }
}  // namespace

std::string ffp_vertex_glsl(const FfpVsKey& k) {
    std::string s = "#version 320 es\nprecision highp float;\nprecision highp int;\n";
    s += "layout(location = 0) in vec4 a_pos;\nlayout(location = 1) in vec4 a_normal;\n"
         "layout(location = 2) in vec4 a_color0;\nlayout(location = 3) in vec4 a_color1;\n";
    for (int i = 0; i < 8; ++i) s += "layout(location = " + n(4 + i) + ") in vec4 a_tex" + n(i) + ";\n";
    s += "uniform int u_bgra;\nuniform mat4 u_wv;\nuniform mat4 u_wvp;\nuniform mat3 u_nrm;\nuniform mat4 u_tex[8];\n"
         "uniform vec4 u_viewport;\nuniform vec4 u_mat[5];\nuniform vec4 u_ambient;\n"
         "uniform vec4 u_lpos[8];\nuniform vec4 u_ldir[8];\nuniform vec4 u_ldiff[8];\nuniform vec4 u_lspec[8];\n"
         "uniform vec4 u_lamb[8];\nuniform vec4 u_latt[8];\nuniform vec4 u_fogv;\n";
    s += "out vec4 D3D_D0;\nout vec4 D3D_D1;\n";
    for (int i = 0; i < 8; ++i) s += "out vec4 D3D_T" + n(i) + ";\n";
    s += "out float D3D_FOG;\n";
    s += glsl_vs_epilogue();
    s += "void main() {\n";
    s += "  vec4 c0 = ((u_bgra & 4) != 0) ? a_color0.zyxw : a_color0;\n";
    s += "  vec4 c1 = ((u_bgra & 8) != 0) ? a_color1.zyxw : a_color1;\n";
    s += k.has_color0 ? "" : "  c0 = vec4(1.0);\n";
    s += k.has_color1 ? "" : "  c1 = vec4(0.0);\n";
    s += "  vec3 ep = vec3(0.0);\n  vec3 N = vec3(0.0, 0.0, 1.0);\n";
    if (k.pretransformed) {
        s += "  float w = (a_pos.w == 0.0) ? 1.0 : 1.0 / a_pos.w;\n"
             "  vec2 ndc = ((a_pos.xy + 0.5) - u_viewport.xy) / u_viewport.zw * 2.0 - 1.0;\n"
             "  gl_Position = vec4(ndc * w, (a_pos.z * 2.0 - 1.0) * w, w);\n";
    } else {
        s += k.pos_w ? "  vec4 P = a_pos;\n" : "  vec4 P = vec4(a_pos.xyz, 1.0);\n";
        s += "  ep = (u_wv * P).xyz;\n  d3d_emit_position(u_wvp * P);\n";
        if (k.has_normal) s += "  N = normalize(u_nrm * a_normal.xyz);\n";
    }

    // Colours.
    if (k.lighting && !k.pretransformed) {
        auto src = [&](int which) -> std::string {
            if (!k.color_vertex || which == 0) return "";
            if (which == 1 && k.has_color0) return "c0";
            if (which == 2 && k.has_color1) return "c1";
            return "";
        };
        std::string md = src(k.diffuse_src), ma = src(k.ambient_src), ms = src(k.specular_src), me = src(k.emissive_src);
        s += "  vec4 mdiff = " + (md.empty() ? "u_mat[0]" : md) + ";\n";
        s += "  vec4 mamb = " + (ma.empty() ? "u_mat[1]" : ma) + ";\n";
        s += "  vec4 mspec = " + (ms.empty() ? "u_mat[2]" : ms) + ";\n";
        s += "  vec4 memis = " + (me.empty() ? "u_mat[3]" : me) + ";\n";
        s += "  vec3 la = vec3(0.0), ld = vec3(0.0), ls = vec3(0.0);\n";
        s += "  vec3 V = " + std::string(k.local_viewer ? "normalize(-ep)" : "vec3(0.0, 0.0, -1.0)") + ";\n";
        for (int i = 0; i < k.num_lights; ++i) {
            std::string I = n(i);
            s += "  {\n    vec3 L; float att = 1.0;\n";
            if (k.light_type[i] == 3) {
                s += "    L = -u_ldir[" + I + "].xyz;\n";
            } else {
                s += "    vec3 lv = u_lpos[" + I + "].xyz - ep; float d = length(lv); L = lv / max(d, 1e-6);\n"
                     "    att = d > u_latt[" + I + "].x ? 0.0 : 1.0 / max(u_latt[" + I + "].y + u_latt[" + I +
                     "].z * d + u_latt[" + I + "].w * d * d, 1e-6);\n";
            }
            s += "    float nl = max(dot(N, L), 0.0);\n"
                 "    la += u_lamb[" + I + "].rgb * att;\n"
                 "    ld += u_ldiff[" + I + "].rgb * nl * att;\n";
            if (k.specular)
                s += "    if (nl > 0.0) ls += u_lspec[" + I + "].rgb * pow(max(dot(N, normalize(L + V)), 0.0), u_mat[4].x) * att;\n";
            s += "  }\n";
        }
        s += "  D3D_D0 = clamp(vec4(memis.rgb + mamb.rgb * (u_ambient.rgb + la) + mdiff.rgb * ld, mdiff.a), 0.0, 1.0);\n";
        s += k.specular ? "  D3D_D1 = clamp(vec4(mspec.rgb * ls, mspec.a), 0.0, 1.0);\n" : "  D3D_D1 = vec4(0.0);\n";
    } else {
        s += "  D3D_D0 = clamp(c0, 0.0, 1.0);\n  D3D_D1 = clamp(c1, 0.0, 1.0);\n";
    }

    // Texture coordinates.
    for (int i = 0; i < 8; ++i) {
        std::string I = n(i);
        int idx = k.tc_index[i] & 7, gen = k.tc_gen[i];
        std::string t;
        int dims = 4;
        switch (gen) {
            case 1: t = "vec4(N, 1.0)"; dims = 3; break;                               // CAMERASPACENORMAL
            case 2: t = "vec4(ep, 1.0)"; dims = 3; break;                              // CAMERASPACEPOSITION
            case 3: t = "vec4(reflect(normalize(ep), N), 1.0)"; dims = 3; break;       // CAMERASPACEREFLECTIONVECTOR
            case 4: t = "vec4(reflect(normalize(ep), N).xy * 0.5 + 0.5, 0.0, 1.0)"; dims = 2; break;  // SPHEREMAP
            default:
                dims = k.tex_dims[idx];
                t = dims ? "a_tex" + n(idx) : "vec4(0.0, 0.0, 0.0, 1.0)";
                if (!dims) dims = 2;
                break;
        }
        if (k.tc_count[i] && !k.pretransformed) {
            // D3D pads the input vector with a 1 after its last component.
            std::string in;
            if (dims == 1) in = "vec4((" + t + ").x, 1.0, 0.0, 0.0)";
            else if (dims == 2) in = "vec4((" + t + ").xy, 1.0, 0.0)";
            else if (dims == 3) in = "vec4((" + t + ").xyz, 1.0)";
            else in = t;
            t = "(u_tex[" + I + "] * " + in + ")";
        }
        s += "  D3D_T" + I + " = " + t + ";\n";
    }

    // Fog.
    if (k.pretransformed) {
        s += "  D3D_FOG = c1.a;\n";
    } else if (k.fog_mode) {
        s += std::string("  float fz = ") + (k.range_fog ? "length(ep)" : "abs(ep.z)") + ";\n";
        if (k.fog_mode == 3) s += "  D3D_FOG = (u_fogv.y - fz) / max(u_fogv.y - u_fogv.x, 1e-6);\n";
        else if (k.fog_mode == 1) s += "  D3D_FOG = exp(-u_fogv.z * fz);\n";
        else s += "  D3D_FOG = exp(-(u_fogv.z * fz) * (u_fogv.z * fz));\n";
    } else {
        s += "  D3D_FOG = 1.0;\n";
    }
    s += "}\n";
    return s;
}

namespace {
std::string arg(const FfpStage& st, int i, uint8_t a) {
    std::string b;
    switch (a & 0xF) {
        case 0: b = "D3D_D0"; break;
        case 1: b = "cur"; break;
        case 2: b = "tex" + n(i); break;
        case 3: b = "u_tfactor"; break;
        case 4: b = "D3D_D1"; break;
        case 5: b = "tmp"; break;
        case 6: b = "u_tconst[" + n(i) + "]"; break;
        default: b = "cur"; break;
    }
    if (a & 0x20) b = "vec4((" + b + ").a)";
    if (a & 0x10) b = "(1.0 - " + b + ")";
    (void)st;
    return b;
}

std::string op_expr(int op, const std::string& a1, const std::string& a2, const std::string& a0, int i) {
    std::string I = n(i);
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2.0";
        case 6: return a1 + " * " + a2 + " * 4.0";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + " - 0.5";
        case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
        case 12: return "mix(" + a2 + ", " + a1 + ", D3D_D0.a)";
        case 13: return "mix(" + a2 + ", " + a1 + ", tex" + I + ".a)";
        case 14: return "mix(" + a2 + ", " + a1 + ", u_tfactor.a)";
        case 15: return a1 + " + " + a2 + " * (1.0 - tex" + I + ".a)";
        case 16: return "mix(" + a2 + ", " + a1 + ", cur.a)";
        case 17: return a1;
        case 18: return a1 + " + (" + a1 + ").a * " + a2;
        case 19: return a1 + " * " + a2 + " + (" + a1 + ").a";
        case 20: return "(1.0 - (" + a1 + ").a) * " + a2 + " + " + a1;
        case 21: return "(1.0 - " + a1 + ") * " + a2 + " + (" + a1 + ").a";
        case 24: return "vec4(dot((" + a1 + ").rgb - 0.5, (" + a2 + ").rgb - 0.5) * 4.0)";
        case 25: return a0 + " + " + a1 + " * " + a2;
        case 26: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
        default: return "cur";  // BUMPENVMAP*: handled through texture coordinates
    }
}
}  // namespace

std::string ffp_pixel_glsl(const FfpPsKey& k) {
    std::string s = "#version 320 es\nprecision highp float;\nprecision highp int;\n"
                    "precision highp sampler2D;\nprecision highp samplerCube;\nprecision highp sampler3D;\n";
    s += "in vec4 D3D_D0;\nin vec4 D3D_D1;\n";
    for (int i = 0; i < 8; ++i) s += "in vec4 D3D_T" + n(i) + ";\n";
    s += "in float D3D_FOG;\nlayout(location = 0) out vec4 fragColor;\n";
    s += "uniform vec4 u_tfactor;\nuniform vec4 u_tconst[8];\nuniform vec4 u_bem[8];\n";
    for (int i = 0; i < k.stages; ++i) {
        int d = k.st[i].dim;
        if (!d) continue;
        s += std::string("uniform ") + (d == 3 ? "samplerCube" : d == 4 ? "sampler3D" : "sampler2D") + " s" + n(i) + ";\n";
    }
    s += glsl_ps_common();
    s += "void main() {\n  vec4 cur = D3D_D0;\n  vec4 tmp = vec4(0.0);\n  vec2 bump = vec2(0.0);\n";
    for (int i = 0; i < k.stages; ++i) {
        const FfpStage& st = k.st[i];
        std::string I = n(i);
        if (st.dim) {
            std::string c = "D3D_T" + I;
            std::string coord;
            if (st.dim == 2) {
                coord = st.projected ? "(" + c + ".xy / " + c + "." + "xyzw"[st.proj_comp & 3] + ")" : c + ".xy";
                coord = "(" + coord + " + bump)";
            } else {
                coord = st.projected ? "(" + c + ".xyz / " + c + ".w)" : c + ".xyz";
            }
            s += "  vec4 tex" + I + " = texture(s" + I + ", " + coord + ");\n";
        } else {
            s += "  vec4 tex" + I + " = vec4(1.0);\n";
        }
        if (i > 0) s += "  bump = vec2(0.0);\n";
        if (st.cop == 22 || st.cop == 23) {
            // BUMPENVMAP: perturb the next stage's coordinates with this stage's (du, dv).
            s += "  bump = vec2(u_bem[" + I + "].x * tex" + I + ".x + u_bem[" + I + "].z * tex" + I + ".y, u_bem[" + I +
                 "].y * tex" + I + ".x + u_bem[" + I + "].w * tex" + I + ".y);\n";
            continue;
        }
        std::string col = op_expr(st.cop, arg(st, i, st.ca1), arg(st, i, st.ca2), arg(st, i, st.ca0), i);
        std::string alp = st.aop == 1 ? "cur" : op_expr(st.aop, arg(st, i, st.aa1), arg(st, i, st.aa2), arg(st, i, st.aa0), i);
        std::string dst = (st.result & 0xF) == 5 ? "tmp" : "cur";
        s += "  " + dst + " = clamp(vec4((" + col + ").rgb, (" + alp + ").a), 0.0, 1.0);\n";
    }
    if (k.specular) s += "  cur.rgb = clamp(cur.rgb + D3D_D1.rgb, 0.0, 1.0);\n";
    s += "  fragColor = d3d_finish(cur);\n}\n";
    return s;
}

}  // namespace d3d
