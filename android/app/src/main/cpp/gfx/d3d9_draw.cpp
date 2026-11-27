// D3D9 draw path on GLES 3.2: programs, vertex input, textures, render state.
//
// Frames are rendered top-down (row 0 = D3D top) because the vertex epilogue
// flips Y; D3D viewport/scissor/clear rectangles therefore map to GL
// coordinates unchanged, and Present flips once when scaling to the window.
#include <GLES3/gl32.h>

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#include "core/guest_heap.h"
#include "core/log.h"
#include "core/memory.h"
#include "gfx/d3d9_device.h"
#include "gfx/ffp.h"
#include "gfx/gl_context.h"
#include "gfx/shader_translator.h"
#include "gfx/texconv.h"

namespace d3d {

GfxStats g_stats;

namespace {
uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

constexpr uint32_t kRingVB = 8u << 20, kRingIB = 2u << 20;

struct Program {
    GLuint prog = 0;
    bool ffp_vs = false, ffp_ps = false;
    uint32_t vs_consts = 0, ps_consts = 0;
    GLint vc = -1, vi = -1, vb = -1, pc = -1, pi = -1, pb = -1, fix = -1, bgra = -1, alpha = -1, fog = -1,
          fogcolor = -1, bem = -1, beml = -1, rth = -1;
    // FFP
    GLint wv = -1, wvp = -1, nrm = -1, tex = -1, viewport = -1, mat = -1, ambient = -1, lpos = -1, ldir = -1,
          ldiff = -1, lspec = -1, lamb = -1, latt = -1, fogv = -1, tfactor = -1, tconst = -1;
};

struct VsStage {
    GLuint shader = 0;
    const ShaderInfo* info = nullptr;  // translated shader (null for FFP)
    FfpVsKey ffp{};
};
struct PsStage {
    GLuint shader = 0;
    const ShaderInfo* info = nullptr;
    FfpPsKey ffp{};
};
}  // namespace

struct DrawCache {
    std::unordered_map<std::string, GLuint> ffp_vs, ffp_ps;
    std::map<std::pair<GLuint, GLuint>, Program> programs;
    std::unordered_map<uint32_t, std::vector<VertexElement>> fvf_decls;
    std::unordered_map<uint64_t, GLuint> samplers;
    GLuint vao = 0, ring_vb = 0, ring_ib = 0;
    uint32_t ring_vb_off = 0, ring_ib_off = 0;
    uint32_t enabled = 0;  // enabled attribute locations
    GLuint dummy2d = 0, dummycube = 0;
    bool aniso = false;
    std::vector<uint8_t> scratch;
    std::map<std::string, bool> logged;
};

// ------------------------------------------------------------------ object cleanup
Surface::~Surface() {
    if (mem) gheap::free(mem);
    if (gl_tex && !gl_shared) glDeleteTextures(1, &gl_tex);
    if (gl_rb) glDeleteRenderbuffers(1, &gl_rb);
    if (upload_tex) glDeleteTextures(1, &upload_tex);
}
Texture::~Texture() {
    // A 2D render target's GL texture belongs to its level-0 surface; a cube render
    // target's cube map belongs to the texture (its faces only attach to it).
    bool surface_owned = (usage & USAGE_RENDERTARGET) && !cube;
    for (Surface* s : surfaces) com::destroy(s);
    if (gl && !surface_owned) glDeleteTextures(1, &gl);
}
Buffer::~Buffer() {
    if (mem) gheap::free(mem);
    if (gl) glDeleteBuffers(1, &gl);
}
Shader::~Shader() {
    for (auto& [k, info] : variants) delete info;
}

namespace {
void log_once(DrawCache* dc, const std::string& msg) {
    if (dc->logged[msg]) return;
    dc->logged[msg] = true;
    LOGW("%s", msg.c_str());
}

GLuint compile(GLenum type, const std::string& src, DrawCache* dc) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        LOGE("GLSL compile failed: %s", log);
        // Dump the source in chunks (logcat lines are limited).
        for (size_t i = 0; i < src.size(); i += 1500) LOGE("src: %s", src.substr(i, 1500).c_str());
        glDeleteShader(sh);
        (void)dc;
        return 0;
    }
    return sh;
}

GLenum gl_cmp(uint32_t f) {
    static const GLenum k[9] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f < 9 ? k[f] : GL_ALWAYS;
}
GLenum gl_blend(uint32_t b, bool alpha_part) {
    switch (b) {
        case 1: return GL_ZERO;
        case 2: return GL_ONE;
        case 3: return GL_SRC_COLOR;
        case 4: return GL_ONE_MINUS_SRC_COLOR;
        case 5: return GL_SRC_ALPHA;
        case 6: return GL_ONE_MINUS_SRC_ALPHA;
        case 7: return GL_DST_ALPHA;
        case 8: return GL_ONE_MINUS_DST_ALPHA;
        case 9: return GL_DST_COLOR;
        case 10: return GL_ONE_MINUS_DST_COLOR;
        case 11: return GL_SRC_ALPHA_SATURATE;
        case 12: return GL_SRC_ALPHA;
        case 13: return GL_ONE_MINUS_SRC_ALPHA;
        case 14: return alpha_part ? GL_CONSTANT_ALPHA : GL_CONSTANT_COLOR;
        case 15: return alpha_part ? GL_ONE_MINUS_CONSTANT_ALPHA : GL_ONE_MINUS_CONSTANT_COLOR;
        default: return GL_ONE;
    }
}
GLenum gl_blendop(uint32_t o) {
    switch (o) {
        case 2: return GL_FUNC_SUBTRACT;
        case 3: return GL_FUNC_REVERSE_SUBTRACT;
        case 4: return GL_MIN;
        case 5: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}
GLenum gl_stencil_op(uint32_t o) {
    static const GLenum k[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return o < 9 ? k[o] : GL_KEEP;
}
float rsf(const State& s, int i) {
    float f;
    memcpy(&f, &s.rs[i], 4);
    return f;
}
void argb_to_vec4(uint32_t c, float out[4]) {
    out[0] = ((c >> 16) & 255) / 255.f;
    out[1] = ((c >> 8) & 255) / 255.f;
    out[2] = (c & 255) / 255.f;
    out[3] = ((c >> 24) & 255) / 255.f;
}

// Row-vector (D3D) matrix product a*b.
void mat_mul(const float* a, const float* b, float* o) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, sizeof r);
}

void sync_buffer(Buffer* b, GLenum target) {
    if (!b->gl) {
        glGenBuffers(1, &b->gl);
        glBindBuffer(target, b->gl);
        glBufferData(target, b->size, b->mem ? mem::ptr(b->mem) : nullptr, GL_DYNAMIC_DRAW);
        b->dirty_lo = b->dirty_hi = 0;
        return;
    }
    glBindBuffer(target, b->gl);
    if (b->dirty_hi > b->dirty_lo && b->mem) {
        uint32_t lo = b->dirty_lo, hi = std::min(b->dirty_hi, b->size);
        g_stats.buf_uploads++;
        g_stats.buf_bytes += hi - lo;
        if (lo == 0 && hi == b->size)
            glBufferData(target, b->size, mem::ptr(b->mem), GL_DYNAMIC_DRAW);
        else
            glBufferSubData(target, lo, hi - lo, mem::ptr(b->mem + lo));
    }
    b->dirty_lo = b->dirty_hi = 0;
}

// Uploads `bytes` into a streaming ring buffer; returns the offset used.
// Regions are written once per ring cycle and the buffer is orphaned on wrap,
// so writes can be unsynchronized (glBufferSubData into a buffer the GPU is
// still reading makes the Mali driver stall or copy the whole buffer).
uint32_t ring_upload(GLuint buf, GLenum target, uint32_t cap, uint32_t& off, const void* data, uint32_t bytes) {
    glBindBuffer(target, buf);
    uint32_t aligned = (off + 15) & ~15u;
    if (aligned + bytes > cap) {
        glBufferData(target, cap, nullptr, GL_STREAM_DRAW);  // orphan
        aligned = 0;
        g_stats.orphans++;
    }
    g_stats.ring_bytes += bytes;
    void* p = glMapBufferRange(target, aligned, bytes,
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (p) {
        memcpy(p, data, bytes);
        glUnmapBuffer(target);
    } else {
        glBufferSubData(target, aligned, bytes, data);
    }
    off = aligned + bytes;
    return aligned;
}

const std::vector<VertexElement>& decl_for(Device* d, const State& s) {
    if (auto* vd = static_cast<VertexDecl*>(com::from_guest(s.vdecl))) return vd->elements;
    auto it = d->cache->fvf_decls.find(s.fvf);
    if (it == d->cache->fvf_decls.end()) it = d->cache->fvf_decls.emplace(s.fvf, fvf_to_decl(s.fvf)).first;
    return it->second;
}

Texture* bound_texture(const State& s, int i) {
    com::Object* o = com::from_guest(s.textures[i]);
    if (!o) return nullptr;
    auto* r = static_cast<Resource*>(o);
    if (r->type != RTYPE_TEXTURE && r->type != RTYPE_CUBETEXTURE) return nullptr;
    return static_cast<Texture*>(r);
}

uint8_t texture_dim(Texture* t) { return !t ? 0 : t->cube ? 3 : 2; }

// ---------------------------------------------------------------- stages
VsStage vs_stage(Device* d, const State& s, const std::vector<VertexElement>& decl) {
    VsStage v;
    if (auto* sh = static_cast<Shader*>(com::from_guest(s.vs))) {
        auto it = sh->variants.find(0);
        if (it == sh->variants.end()) {
            ShaderInfo* info = new ShaderInfo(translate_shader(sh->tokens, nullptr));
            if (!info->ok) LOGE("vertex shader translation failed: %s", info->error.c_str());
            else info->gl_shader = compile(GL_VERTEX_SHADER, info->glsl, d->cache);
            it = sh->variants.emplace(0, info).first;
        }
        v.info = it->second;
        v.shader = v.info->ok ? v.info->gl_shader : 0;
        return v;
    }
    FfpVsKey& k = v.ffp;
    memset(&k, 0, sizeof k);
    for (const VertexElement& e : decl) {
        switch (e.usage) {
            case 9: k.pretransformed = 1; break;
            case 0: if (e.usage_index == 0) k.pos_w = e.type == 3; break;
            case 3: k.has_normal = 1; break;
            case 10: (e.usage_index == 0 ? k.has_color0 : k.has_color1) = 1; break;
            case 5:
                if (e.usage_index < 8) {
                    static const uint8_t kDims[18] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4, 0};
                    k.tex_dims[e.usage_index] = e.type < 18 ? kDims[e.type] : 2;
                }
                break;
            default: break;
        }
    }
    const uint32_t* rs = s.rs;
    k.lighting = rs[137] ? 1 : 0;
    k.specular = rs[29] ? 1 : 0;
    k.local_viewer = rs[142] ? 1 : 0;
    k.color_vertex = rs[141] ? 1 : 0;
    k.diffuse_src = (uint8_t)rs[145];
    k.specular_src = (uint8_t)rs[146];
    k.ambient_src = (uint8_t)rs[147];
    k.emissive_src = (uint8_t)rs[148];
    if (k.lighting) {
        for (int i = 0; i < 16 && k.num_lights < 8; ++i) {
            if (!s.lights[i].enabled) continue;
            k.light_type[k.num_lights++] = (uint8_t)s.lights[i].data[0];
        }
    }
    for (int i = 0; i < 8; ++i) {
        uint32_t ti = s.tss[i][11];
        k.tc_index[i] = (uint8_t)(ti & 7);
        k.tc_gen[i] = (uint8_t)((ti >> 16) & 7);
        uint32_t tf = s.tss[i][24];
        k.tc_count[i] = (uint8_t)(tf & 7);
        k.tc_projected[i] = (tf & 256) ? 1 : 0;
    }
    if (rs[28] && rs[35] == 0) k.fog_mode = (uint8_t)rs[140];  // vertex fog
    k.range_fog = rs[48] ? 1 : 0;
    std::string key((const char*)&k, sizeof k);
    auto it = d->cache->ffp_vs.find(key);
    if (it == d->cache->ffp_vs.end())
        it = d->cache->ffp_vs.emplace(key, compile(GL_VERTEX_SHADER, ffp_vertex_glsl(k), d->cache)).first;
    v.shader = it->second;
    return v;
}

PsStage ps_stage(Device* d, const State& s) {
    PsStage p;
    if (auto* sh = static_cast<Shader*>(com::from_guest(s.ps))) {
        uint32_t ver = sh->tokens.empty() ? 0 : sh->tokens[0];
        uint32_t key = 0;
        uint8_t dims[16] = {};
        uint8_t proj[8] = {};
        if (((ver >> 8) & 0xFF) < 2) {  // ps_1_x: variant per bound texture types
            for (int i = 0; i < 8; ++i) {
                dims[i] = texture_dim(bound_texture(s, i));
                key |= (uint32_t)(dims[i] == 3 ? 1 : 0) << i;
            }
            // ps_1_1..1_3 honour D3DTTFF_PROJECTED on their texture fetches (ps_1_4 uses _dw).
            if ((ver & 0xFF) < 4) {
                for (int i = 0; i < 4; ++i) {
                    uint32_t tf = s.tss[i][24];
                    if (tf & 256) proj[i] = (uint8_t)((tf & 7) ? (tf & 7) : 4);
                    key |= (uint32_t)proj[i] << (8 + 3 * i);
                }
            }
            key |= 0x80000000u;
        }
        auto it = sh->variants.find(key);
        if (it == sh->variants.end()) {
            ShaderInfo* info = new ShaderInfo(translate_shader(sh->tokens, key ? dims : nullptr, key ? proj : nullptr));
            if (!info->ok) LOGE("pixel shader translation failed: %s", info->error.c_str());
            else info->gl_shader = compile(GL_FRAGMENT_SHADER, info->glsl, d->cache);
            it = sh->variants.emplace(key, info).first;
        }
        p.info = it->second;
        p.shader = p.info->ok ? p.info->gl_shader : 0;
        return p;
    }
    FfpPsKey& k = p.ffp;
    memset(&k, 0, sizeof k);
    k.specular = s.rs[29] ? 1 : 0;
    for (int i = 0; i < 8; ++i) {
        const uint32_t* t = s.tss[i];
        if (t[1] == 1 || t[1] == 0) break;  // COLOROP DISABLE ends the cascade
        FfpStage& st = k.st[i];
        st.cop = (uint8_t)t[1];
        st.ca1 = (uint8_t)t[2];
        st.ca2 = (uint8_t)t[3];
        st.aop = (uint8_t)t[4];
        st.aa1 = (uint8_t)t[5];
        st.aa2 = (uint8_t)t[6];
        st.ca0 = (uint8_t)t[26];
        st.aa0 = (uint8_t)t[27];
        st.result = (uint8_t)t[28];
        st.dim = texture_dim(bound_texture(s, i));
        uint32_t tf = t[24];
        st.projected = (tf & 256) ? 1 : 0;
        st.proj_comp = (uint8_t)(((tf & 7) ? (tf & 7) : 4) - 1);
        k.stages = (uint8_t)(i + 1);
    }
    std::string key((const char*)&k, sizeof k);
    auto it = d->cache->ffp_ps.find(key);
    if (it == d->cache->ffp_ps.end())
        it = d->cache->ffp_ps.emplace(key, compile(GL_FRAGMENT_SHADER, ffp_pixel_glsl(k), d->cache)).first;
    p.shader = it->second;
    return p;
}

Program* program_for(Device* d, const VsStage& v, const PsStage& p) {
    auto key = std::make_pair(v.shader, p.shader);
    auto it = d->cache->programs.find(key);
    if (it != d->cache->programs.end()) return it->second.prog ? &it->second : nullptr;
    Program pr;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v.shader);
    glAttachShader(prog, p.shader);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        LOGE("GLSL link failed: %s", log);
        glDeleteProgram(prog);
        d->cache->programs[key] = pr;
        return nullptr;
    }
    pr.prog = prog;
    pr.ffp_vs = v.info == nullptr;
    pr.ffp_ps = p.info == nullptr;
    pr.vs_consts = v.info ? v.info->const_count : 0;
    pr.ps_consts = p.info ? std::min<uint32_t>(p.info->const_count, 32) : 0;
    auto loc = [prog](const char* n) { return glGetUniformLocation(prog, n); };
    pr.vc = loc("vc"); pr.vi = loc("vi"); pr.vb = loc("vb");
    pr.pc = loc("pc"); pr.pi = loc("pi"); pr.pb = loc("pb");
    pr.fix = loc("u_fix"); pr.bgra = loc("u_bgra"); pr.alpha = loc("u_alpha"); pr.fog = loc("u_fog");
    pr.fogcolor = loc("u_fogcolor"); pr.bem = loc("u_bem"); pr.beml = loc("u_beml"); pr.rth = loc("u_rth");
    pr.wv = loc("u_wv"); pr.wvp = loc("u_wvp"); pr.nrm = loc("u_nrm"); pr.tex = loc("u_tex");
    pr.viewport = loc("u_viewport"); pr.mat = loc("u_mat"); pr.ambient = loc("u_ambient");
    pr.lpos = loc("u_lpos"); pr.ldir = loc("u_ldir"); pr.ldiff = loc("u_ldiff"); pr.lspec = loc("u_lspec");
    pr.lamb = loc("u_lamb"); pr.latt = loc("u_latt"); pr.fogv = loc("u_fogv");
    pr.tfactor = loc("u_tfactor"); pr.tconst = loc("u_tconst");
    glUseProgram(prog);
    for (int i = 0; i < 16; ++i) {
        std::string n = "s" + std::to_string(i);
        GLint l = loc(n.c_str());
        if (l >= 0) glUniform1i(l, i);
    }
    return &(d->cache->programs[key] = pr);
}

// ---------------------------------------------------------------- textures / samplers
GLint gl_address(uint32_t a) {
    switch (a) {
        case 2: case 5: return GL_MIRRORED_REPEAT;
        case 3: return GL_CLAMP_TO_EDGE;
        case 4: return GL_CLAMP_TO_BORDER;
        default: return GL_REPEAT;
    }
}

GLuint sampler_for(DrawCache* dc, const uint32_t* ss, bool has_mips) {
    uint32_t mip = has_mips ? ss[7] : 0;
    uint64_t key = (uint64_t)(ss[1] & 7) | ((uint64_t)(ss[2] & 7) << 3) | ((uint64_t)(ss[3] & 7) << 6) |
                   ((uint64_t)(ss[5] & 7) << 9) | ((uint64_t)(ss[6] & 7) << 12) | ((uint64_t)(mip & 7) << 15) |
                   ((uint64_t)(ss[10] & 31) << 18) | ((uint64_t)ss[4] << 32);
    auto it = dc->samplers.find(key);
    if (it != dc->samplers.end()) return it->second;
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, gl_address(ss[1]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, gl_address(ss[2]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, gl_address(ss[3]));
    bool mag_lin = ss[5] >= 2, min_lin = ss[6] >= 2;
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    GLint minf;
    if (mip == 0) minf = min_lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) minf = min_lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else minf = min_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minf);
    if (dc->aniso && (ss[6] == 3 || ss[5] == 3))
        glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)std::max<uint32_t>(1, std::min<uint32_t>(ss[10], 16)));
    float bc[4];
    argb_to_vec4(ss[4], bc);
    glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, bc);
    dc->samplers[key] = s;
    return s;
}

void bind_textures(Device* d, const State& s, const Program* pr, const VsStage&, const PsStage& p) {
    DrawCache* dc = d->cache;
    for (int i = 0; i < 16; ++i) {
        uint8_t dim;
        if (p.info) dim = p.info->sampler_dim[i];
        else dim = i < p.ffp.stages ? p.ffp.st[i].dim : 0;
        if (!dim) continue;
        Texture* t = bound_texture(s, i);
        glActiveTexture(GL_TEXTURE0 + i);
        GLenum target = dim == 3 ? GL_TEXTURE_CUBE_MAP : dim == 4 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
        if (t && texture_dim(t) == dim) {
            upload_texture(t);
            glBindTexture(target, t->gl);
            glBindSampler(i, sampler_for(dc, s.ss[i], t->levels > 1));
        } else {
            glBindTexture(target, dim == 3 ? dc->dummycube : dc->dummy2d);
            glBindSampler(i, 0);
        }
    }
    (void)pr;
}

// ---------------------------------------------------------------- render state
void apply_render_state(Device* d, const State& s) {
    const uint32_t* rs = s.rs;
    // Viewport (D3D coordinates map directly; see header comment).
    float minz, maxz;
    memcpy(&minz, &s.viewport[4], 4);
    memcpy(&maxz, &s.viewport[5], 4);
    glViewport((GLint)s.viewport[0], (GLint)s.viewport[1], (GLsizei)s.viewport[2], (GLsizei)s.viewport[3]);
    glDepthRangef(minz, maxz);
    if (rs[174]) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)s.scissor[0], (GLint)s.scissor[1], (GLsizei)(s.scissor[2] - s.scissor[0]),
                  (GLsizei)(s.scissor[3] - s.scissor[1]));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    bool depth = rs[7] && d->ds;
    if (depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(rs[23]));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(depth && rs[14] ? GL_TRUE : GL_FALSE);
    // Culling: Y is flipped, so D3D's clockwise front faces are clockwise in GL.
    glFrontFace(GL_CW);
    if (rs[22] == 1) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK);
    }
    if (rs[27]) {
        glEnable(GL_BLEND);
        uint32_t sb = rs[19], db = rs[20];
        if (sb == 12) db = 6;  // D3DBLEND_BOTHSRCALPHA
        if (sb == 13) db = 5;  // D3DBLEND_BOTHINVSRCALPHA
        uint32_t sa = sb, da = db, opa = rs[171];
        if (rs[206]) {
            sa = rs[207];
            da = rs[208];
            opa = rs[209];
        }
        glBlendFuncSeparate(gl_blend(sb, false), gl_blend(db, false), gl_blend(sa, true), gl_blend(da, true));
        glBlendEquationSeparate(gl_blendop(rs[171]), gl_blendop(opa));
        float bf[4];
        argb_to_vec4(rs[193], bf);
        glBlendColor(bf[0], bf[1], bf[2], bf[3]);
    } else {
        glDisable(GL_BLEND);
    }
    uint32_t cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    if (rs[52] && d->ds) {
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOpSeparate(GL_FRONT, gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        if (rs[185]) {
            glStencilFuncSeparate(GL_BACK, gl_cmp(rs[189]), (GLint)rs[57], rs[58]);
            glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[186]), gl_stencil_op(rs[187]), gl_stencil_op(rs[188]));
        } else {
            glStencilFuncSeparate(GL_BACK, gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
            glStencilOpSeparate(GL_BACK, gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        }
        glStencilMask(rs[59]);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    float bias = rsf(s, 195), slope = rsf(s, 175);
    if (bias != 0.f || slope != 0.f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(slope, bias * 16777215.f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

void set_uniforms(Device* d, const State& s, const Program* pr, uint32_t bgra) {
    const uint32_t* rs = s.rs;
    float vw = (float)std::max<uint32_t>(s.viewport[2], 1), vh = (float)std::max<uint32_t>(s.viewport[3], 1);
    if (pr->fix >= 0) glUniform4f(pr->fix, -1.f / vw, 1.f / vh, 0.f, 0.f);
    if (pr->bgra >= 0) glUniform1i(pr->bgra, (GLint)bgra);
    if (pr->vc >= 0 && pr->vs_consts) glUniform4fv(pr->vc, (GLsizei)pr->vs_consts, &s.vs_f[0][0]);
    if (pr->vi >= 0) glUniform4iv(pr->vi, 16, &s.vs_i[0][0]);
    if (pr->vb >= 0) glUniform1iv(pr->vb, 16, (const GLint*)s.vs_b);
    if (pr->pc >= 0 && pr->ps_consts) glUniform4fv(pr->pc, (GLsizei)pr->ps_consts, &s.ps_f[0][0]);
    if (pr->pi >= 0) glUniform4iv(pr->pi, 16, &s.ps_i[0][0]);
    if (pr->pb >= 0) glUniform1iv(pr->pb, 16, (const GLint*)s.ps_b);
    if (pr->rth >= 0) glUniform1f(pr->rth, d->rt[0] ? (float)d->rt[0]->h : 0.f);
    if (pr->alpha >= 0) glUniform4f(pr->alpha, rs[15] ? (float)rs[25] : 0.f, (float)(rs[24] & 255) / 255.f, 0.f, 0.f);
    if (pr->fog >= 0) {
        float mode = 0;
        if (rs[28]) {
            if (rs[35] == 3) mode = 2;
            else if (rs[35] == 1) mode = 3;
            else if (rs[35] == 2) mode = 4;
            else mode = 1;
        }
        glUniform4f(pr->fog, mode, rsf(s, 36), rsf(s, 37), rsf(s, 38));
        float fc[4];
        argb_to_vec4(rs[34], fc);
        glUniform4fv(pr->fogcolor, 1, fc);
    }
    if (pr->bem >= 0 || pr->beml >= 0) {
        float bem[8][4], beml[8][4];
        for (int i = 0; i < 8; ++i) {
            memcpy(bem[i], &s.tss[i][7], 16);
            memcpy(&beml[i][0], &s.tss[i][22], 4);
            memcpy(&beml[i][1], &s.tss[i][23], 4);
            beml[i][2] = beml[i][3] = 0;
        }
        if (pr->bem >= 0) glUniform4fv(pr->bem, 8, &bem[0][0]);
        if (pr->beml >= 0) glUniform4fv(pr->beml, 8, &beml[0][0]);
    }
    if (pr->tfactor >= 0) {
        float tf[4];
        argb_to_vec4(rs[60], tf);
        glUniform4fv(pr->tfactor, 1, tf);
    }
    if (pr->tconst >= 0) {
        float tc[8][4];
        for (int i = 0; i < 8; ++i) argb_to_vec4(s.tss[i][32], tc[i]);
        glUniform4fv(pr->tconst, 8, &tc[0][0]);
    }
    if (!pr->ffp_vs) return;
    if (pr->viewport >= 0) glUniform4f(pr->viewport, (float)s.viewport[0], (float)s.viewport[1], vw, vh);
    const float* world = s.transforms[24];
    const float* view = s.transforms[2];
    const float* proj = s.transforms[3];
    float wv[16], wvp[16];
    mat_mul(world, view, wv);
    mat_mul(wv, proj, wvp);
    if (pr->wv >= 0) glUniformMatrix4fv(pr->wv, 1, GL_FALSE, wv);
    if (pr->wvp >= 0) glUniformMatrix4fv(pr->wvp, 1, GL_FALSE, wvp);
    if (pr->nrm >= 0) {
        float n3[9] = {wv[0], wv[1], wv[2], wv[4], wv[5], wv[6], wv[8], wv[9], wv[10]};
        glUniformMatrix3fv(pr->nrm, 1, GL_FALSE, n3);
    }
    if (pr->tex >= 0) glUniformMatrix4fv(pr->tex, 8, GL_FALSE, s.transforms[16]);
    if (pr->mat >= 0) {
        float m[5][4];
        memcpy(m, s.material, 16 * 4);
        m[4][0] = s.material[16];
        m[4][1] = m[4][2] = m[4][3] = 0;
        glUniform4fv(pr->mat, 5, &m[0][0]);
    }
    if (pr->ambient >= 0) {
        float a[4];
        argb_to_vec4(rs[139], a);
        glUniform4fv(pr->ambient, 1, a);
    }
    if (pr->lpos >= 0 || pr->ldir >= 0) {
        float lpos[8][4] = {}, ldir[8][4] = {}, ldiff[8][4] = {}, lspec[8][4] = {}, lamb[8][4] = {}, latt[8][4] = {};
        int n = 0;
        for (int i = 0; i < 16 && n < 8; ++i) {
            if (!s.lights[i].enabled) continue;
            const float* L = (const float*)s.lights[i].data;
            memcpy(ldiff[n], L + 1, 16);
            memcpy(lspec[n], L + 5, 16);
            memcpy(lamb[n], L + 9, 16);
            const float* P = L + 13;
            const float* D = L + 16;
            for (int j = 0; j < 3; ++j) {
                lpos[n][j] = P[0] * view[j] + P[1] * view[4 + j] + P[2] * view[8 + j] + view[12 + j];
                ldir[n][j] = D[0] * view[j] + D[1] * view[4 + j] + D[2] * view[8 + j];
            }
            float len = std::sqrt(ldir[n][0] * ldir[n][0] + ldir[n][1] * ldir[n][1] + ldir[n][2] * ldir[n][2]);
            if (len > 0)
                for (int j = 0; j < 3; ++j) ldir[n][j] /= len;
            latt[n][0] = L[19];  // range
            latt[n][1] = L[21];
            latt[n][2] = L[22];
            latt[n][3] = L[23];
            ++n;
        }
        glUniform4fv(pr->lpos, 8, &lpos[0][0]);
        glUniform4fv(pr->ldir, 8, &ldir[0][0]);
        glUniform4fv(pr->ldiff, 8, &ldiff[0][0]);
        glUniform4fv(pr->lspec, 8, &lspec[0][0]);
        glUniform4fv(pr->lamb, 8, &lamb[0][0]);
        glUniform4fv(pr->latt, 8, &latt[0][0]);
    }
    if (pr->fogv >= 0) glUniform4f(pr->fogv, rsf(s, 36), rsf(s, 37), rsf(s, 38), 0.f);
}

// ---------------------------------------------------------------- vertex input
struct AttribFormat {
    GLint size;
    GLenum type;
    GLboolean norm;
    bool bgra;
};
AttribFormat attrib_format(uint8_t t) {
    switch (t) {
        case 0: return {1, GL_FLOAT, GL_FALSE, false};
        case 1: return {2, GL_FLOAT, GL_FALSE, false};
        case 2: return {3, GL_FLOAT, GL_FALSE, false};
        case 3: return {4, GL_FLOAT, GL_FALSE, false};
        case 4: return {4, GL_UNSIGNED_BYTE, GL_TRUE, true};
        case 5: return {4, GL_UNSIGNED_BYTE, GL_FALSE, false};
        case 6: return {2, GL_SHORT, GL_FALSE, false};
        case 7: return {4, GL_SHORT, GL_FALSE, false};
        case 8: return {4, GL_UNSIGNED_BYTE, GL_TRUE, false};
        case 9: return {2, GL_SHORT, GL_TRUE, false};
        case 10: return {4, GL_SHORT, GL_TRUE, false};
        case 11: return {2, GL_UNSIGNED_SHORT, GL_TRUE, false};
        case 12: return {4, GL_UNSIGNED_SHORT, GL_TRUE, false};
        case 13: return {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_FALSE, false};
        case 14: return {4, GL_INT_2_10_10_10_REV, GL_TRUE, false};
        case 15: return {2, GL_HALF_FLOAT, GL_FALSE, false};
        case 16: return {4, GL_HALF_FLOAT, GL_FALSE, false};
        default: return {0, GL_FLOAT, GL_FALSE, false};
    }
}

int ffp_location(const VertexElement& e) {
    switch (e.usage) {
        case 0: case 9: return e.usage_index == 0 ? kFfpLocPos : -1;
        case 3: return e.usage_index == 0 ? kFfpLocNormal : -1;
        case 10: return e.usage_index == 0 ? kFfpLocColor0 : e.usage_index == 1 ? kFfpLocColor1 : -1;
        case 5: return e.usage_index < 8 ? kFfpLocTex0 + e.usage_index : -1;
        default: return -1;
    }
}

// Sets up attribute pointers. `up_base` >= 0 means stream 0 comes from the
// ring buffer at that offset (DrawPrimitiveUP).
bool setup_vertices(Device* d, const State& s, const std::vector<VertexElement>& decl, const VsStage& v,
                    int64_t up_base, uint32_t up_stride, uint32_t* bgra_out) {
    DrawCache* dc = d->cache;
    uint32_t want = 0, bgra = 0;
    Buffer* synced[16] = {};
    for (const VertexElement& e : decl) {
        int loc = -1;
        if (v.info) {
            for (const ShaderInput& in : v.info->inputs) {
                if (in.usage == e.usage && in.usage_index == e.usage_index) {
                    loc = in.reg;
                    break;
                }
                // POSITIONT data feeding a POSITION input.
                if (e.usage == 9 && in.usage == 0 && in.usage_index == e.usage_index) loc = in.reg;
            }
        } else {
            loc = ffp_location(e);
        }
        if (loc < 0 || loc >= 16) continue;
        AttribFormat f = attrib_format(e.type);
        if (!f.size) continue;
        GLsizei stride;
        uintptr_t offset;
        if (e.stream == 0 && up_base >= 0) {
            glBindBuffer(GL_ARRAY_BUFFER, dc->ring_vb);
            stride = (GLsizei)up_stride;
            offset = (uintptr_t)up_base + e.offset;
        } else {
            if (e.stream >= 16) continue;
            const Stream& st = s.streams[e.stream];
            auto* b = static_cast<Buffer*>(com::from_guest(st.vb));
            if (!b) continue;
            if (!synced[e.stream]) {
                sync_buffer(b, GL_ARRAY_BUFFER);
                synced[e.stream] = b;
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            }
            stride = (GLsizei)st.stride;
            offset = st.offset + e.offset;
        }
        if (f.bgra) bgra |= 1u << loc;
        if (stride == 0 && f.type == GL_FLOAT) {
            // Constant attribute: D3D allows stride 0; GL treats 0 as "tightly packed".
            glDisableVertexAttribArray(loc);
            dc->enabled &= ~(1u << loc);
            want &= ~(1u << loc);
            continue;
        }
        if (f.type == GL_UNSIGNED_INT_2_10_10_10_REV || f.type == GL_INT_2_10_10_10_REV) f.size = 4;
        glVertexAttribPointer(loc, f.size, f.type, f.norm, stride, (const void*)offset);
        want |= 1u << loc;
    }
    // Inputs the shader reads but the declaration does not provide.
    uint32_t change = want ^ dc->enabled;
    for (int i = 0; i < 16; ++i) {
        if (!(change & (1u << i))) continue;
        if (want & (1u << i)) {
            glEnableVertexAttribArray(i);
        } else {
            glDisableVertexAttribArray(i);
        }
    }
    dc->enabled = want;
    for (int i = 0; i < 16; ++i)
        if (!(want & (1u << i))) glVertexAttrib4f(i, (i == kFfpLocColor0 && !v.info) ? 1.f : 0.f, 0.f, 0.f, 1.f);
    *bgra_out = bgra;
    return true;
}

GLenum gl_prim(uint32_t p) {
    switch (p) {
        case 1: return GL_POINTS;
        case 2: return GL_LINES;
        case 3: return GL_LINE_STRIP;
        case 5: return GL_TRIANGLE_STRIP;
        case 6: return GL_TRIANGLE_FAN;
        default: return GL_TRIANGLES;
    }
}
uint32_t vertex_count(uint32_t p, uint32_t n) {
    switch (p) {
        case 1: return n;
        case 2: return n * 2;
        case 3: return n + 1;
        case 4: return n * 3;
        default: return n + 2;
    }
}
}  // namespace

// ------------------------------------------------------------------ public
void draw_init(Device* d) {
    DrawCache* dc = new DrawCache();
    d->cache = dc;
    glGenVertexArrays(1, &dc->vao);
    glBindVertexArray(dc->vao);
    glGenBuffers(1, &dc->ring_vb);
    glBindBuffer(GL_ARRAY_BUFFER, dc->ring_vb);
    glBufferData(GL_ARRAY_BUFFER, kRingVB, nullptr, GL_STREAM_DRAW);
    glGenBuffers(1, &dc->ring_ib);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dc->ring_ib);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, kRingIB, nullptr, GL_STREAM_DRAW);
    const uint32_t black = 0xFF000000u;
    glGenTextures(1, &dc->dummy2d);
    glBindTexture(GL_TEXTURE_2D, dc->dummy2d);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &black);
    glGenTextures(1, &dc->dummycube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, dc->dummycube);
    for (int f = 0; f < 6; ++f)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &black);
    dc->aniso = gfx::has_extension("GL_EXT_texture_filter_anisotropic");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void bind_targets(Device* d) {
    if (!d->targets_dirty) return;
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    Surface* rt = d->rt[0];
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rt ? rt->gl_face : GL_TEXTURE_2D, rt ? rt->gl_tex : 0, 0);
    GLuint rb = d->ds ? d->ds->gl_rb : 0;
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) LOGW("render target FBO incomplete: %x", st);
    d->targets_dirty = false;
}

void upload_texture(Texture* t) {
    if (t->usage & USAGE_RENDERTARGET) {
        if (!t->gl_ready && !t->surfaces.empty()) {
            t->gl = t->surfaces[0]->gl_tex;
            glBindTexture(GL_TEXTURE_2D, t->gl);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            t->gl_ready = true;
        }
        return;
    }
    texconv::GLFormat gf;
    if (!texconv::describe(t->format, gf)) {
        static std::map<uint32_t, bool> warned;
        if (!warned[t->format]) {
            warned[t->format] = true;
            LOGW("unsupported texture format %08x", t->format);
        }
        gf = texconv::GLFormat{};
    }
    GLenum target = t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    if (!t->gl) {
        glGenTextures(1, &t->gl);
        glBindTexture(target, t->gl);
        glTexStorage2D(target, (GLsizei)t->levels, gf.internal, (GLsizei)t->w, (GLsizei)t->h);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, gf.swizzle[0]);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, gf.swizzle[1]);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, gf.swizzle[2]);
        glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, gf.swizzle[3]);
    } else {
        glBindTexture(target, t->gl);
    }
    bool any = false;
    static std::vector<uint8_t> buf;
    uint64_t t0 = 0;
    for (Surface* s : t->surfaces) {
        if (!s->dirty || !s->mem) continue;
        if (!t0) t0 = now_ns();  // only time actual uploads
        g_stats.tex_uploads++;
        g_stats.tex_bytes += (uint64_t)s->w * s->h * 4;
        texconv::convert(t->format, mem::ptr(s->mem), s->w, s->h, buf);
        GLenum face = t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + s->face : GL_TEXTURE_2D;
        glTexSubImage2D(face, (GLint)s->level, 0, 0, (GLsizei)s->w, (GLsizei)s->h, gf.format, gf.type, buf.data());
        s->dirty = false;
        any = true;
    }
    if (any && (t->usage & USAGE_AUTOGENMIPMAP) && t->levels > 1 && !gf.direct) glGenerateMipmap(target);
    if (t0) g_stats.ns_upload += now_ns() - t0;
}

GLuint surface_gl_texture(Surface* s) {
    if (s->gl_tex) return s->gl_tex;
    if (s->container) {
        auto* t = static_cast<Texture*>(s->container);
        if (s->level == 0 && !t->cube) {
            upload_texture(t);
            return t->gl;
        }
    }
    if (!s->mem) return 0;
    texconv::GLFormat gf;
    texconv::describe(s->format, gf);
    if (!s->upload_tex) {
        glGenTextures(1, &s->upload_tex);
        glBindTexture(GL_TEXTURE_2D, s->upload_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, gf.internal, (GLsizei)s->w, (GLsizei)s->h);
        s->dirty = true;
    }
    glBindTexture(GL_TEXTURE_2D, s->upload_tex);
    if (s->dirty) {
        static std::vector<uint8_t> buf;
        texconv::convert(s->format, mem::ptr(s->mem), s->w, s->h, buf);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)s->w, (GLsizei)s->h, gf.format, gf.type, buf.data());
        s->dirty = false;
    }
    return s->upload_tex;
}

void draw(Device* d, uint32_t prim, uint32_t start_vertex, uint32_t prim_count, bool indexed, int32_t base_vertex,
          uint32_t start_index, uint32_t up_vertices, uint32_t up_stride, uint32_t up_indices, uint32_t up_index_fmt,
          uint32_t up_vertex_count) {
    if (!prim_count || !gfx::make_current()) return;
    // Draw time is sampled on one draw in 16 (and scaled) to keep clock reads off the hot path.
    static uint32_t s_draw_seq = 0;
    struct Timer {
        uint64_t t0;
        explicit Timer(bool on) : t0(on ? now_ns() : 0) {}
        ~Timer() {
            if (t0) g_stats.ns_draw += (now_ns() - t0) * 16;
        }
    } timer((++s_draw_seq & 15) == 0);
    DrawCache* dc = d->cache;
    const State& s = d->st;
    glBindVertexArray(dc->vao);
    bind_targets(d);
    const std::vector<VertexElement>& decl = decl_for(d, s);
    VsStage v = vs_stage(d, s, decl);
    PsStage p = ps_stage(d, s);
    if (!v.shader || !p.shader) return;
    Program* pr = program_for(d, v, p);
    if (!pr) return;
    glUseProgram(pr->prog);

    uint32_t count = vertex_count(prim, prim_count);
    int64_t up_base = -1;
    if (up_vertices) {
        uint32_t nverts = indexed ? up_vertex_count : count;
        up_base = ring_upload(dc->ring_vb, GL_ARRAY_BUFFER, kRingVB, dc->ring_vb_off, mem::ptr(up_vertices),
                              nverts * up_stride);
    }
    uint32_t bgra = 0;
    setup_vertices(d, s, decl, v, up_base, up_stride, &bgra);
    set_uniforms(d, s, pr, bgra);
    bind_textures(d, s, pr, v, p);
    apply_render_state(d, s);

    GLenum mode = gl_prim(prim);
    if (!indexed) {
        glDrawArrays(mode, up_vertices ? 0 : (GLint)start_vertex, (GLsizei)count);
        return;
    }
    GLenum itype;
    uintptr_t ioff;
    if (up_indices) {
        uint32_t isz = up_index_fmt == FMT_INDEX32 ? 4 : 2;
        itype = isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        ioff = ring_upload(dc->ring_ib, GL_ELEMENT_ARRAY_BUFFER, kRingIB, dc->ring_ib_off, mem::ptr(up_indices), count * isz);
    } else {
        auto* ib = static_cast<Buffer*>(com::from_guest(s.indices));
        if (!ib) return;
        sync_buffer(ib, GL_ELEMENT_ARRAY_BUFFER);
        uint32_t isz = ib->format == FMT_INDEX32 ? 4 : 2;
        itype = isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        ioff = (uintptr_t)start_index * isz;
    }
    glDrawElementsBaseVertex(mode, (GLsizei)count, itype, (const void*)ioff, up_indices ? 0 : base_vertex);
}

std::vector<VertexElement> fvf_to_decl(uint32_t fvf) {
    std::vector<VertexElement> e;
    uint16_t off = 0;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t idx, uint16_t size) {
        e.push_back({0, off, type, 0, usage, idx});
        off = (uint16_t)(off + size);
    };
    uint32_t pos = fvf & 0x400E;
    switch (pos) {
        case 0x2: add(2, 0, 0, 12); break;                // XYZ
        case 0x4: add(3, 9, 0, 16); break;                // XYZRHW
        case 0x4002: add(3, 0, 0, 16); break;             // XYZW
        case 0x6: case 0x8: case 0xA: case 0xC: case 0xE: {
            add(2, 0, 0, 12);
            int betas = (int)((pos - 0x4) / 2);
            bool last_idx = fvf & 0x9000;
            int weights = last_idx ? betas - 1 : betas;
            if (weights > 0) add((uint8_t)(weights - 1), 1, 0, (uint16_t)(weights * 4));
            if (last_idx) add((fvf & 0x8000) ? 4 : 5, 2, 0, 4);
            break;
        }
        default: break;
    }
    if (fvf & 0x10) add(2, 3, 0, 12);
    if (fvf & 0x20) add(0, 4, 0, 4);
    if (fvf & 0x40) add(4, 10, 0, 4);
    if (fvf & 0x80) add(4, 10, 1, 4);
    uint32_t ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < ntex && i < 8; ++i) {
        uint32_t f = (fvf >> (16 + i * 2)) & 3;
        static const uint8_t kType[4] = {1, 2, 3, 0};
        static const uint16_t kSize[4] = {8, 12, 16, 4};
        add(kType[f], 5, (uint8_t)i, kSize[f]);
    }
    return e;
}

}  // namespace d3d
