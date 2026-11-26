#pragma once
// Internal object model of the D3D9 -> GLES layer, shared by the COM front end
// (d3d9.cpp), the draw path (d3d9_draw.cpp) and the shader generators.
#include <GLES3/gl32.h>

#include <bitset>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gfx/d3d9_types.h"
#include "hle/com.h"

namespace d3d {

struct Device;

struct Resource : com::Object {
    Device* dev = nullptr;
    uint32_t type = 0, usage = 0, pool = 0, format = 0, priority = 0;
};

struct Surface : Resource {
    uint32_t w = 0, h = 0;
    uint32_t mem = 0;                 // guest copy for LockRect (allocated on first lock)
    com::Object* container = nullptr; // owning texture, if any
    uint32_t face = 0, level = 0;
    bool dirty = false;               // guest memory changed since the last GL upload
    GLuint gl_tex = 0, gl_rb = 0;     // render-target texture / depth renderbuffer
    GLuint upload_tex = 0;            // GL copy of a plain (non-RT) surface, for StretchRect
    ~Surface() override;
};

struct Texture : Resource {
    uint32_t w = 0, h = 0, levels = 1;
    bool cube = false;
    std::vector<Surface*> surfaces;  // face-major: [face * levels + level]
    uint32_t lod = 0, autogen_filter = 2;
    GLuint gl = 0;                   // sampled GL texture (== level 0 gl_tex for render targets)
    bool gl_ready = false;
    ~Texture() override;
};

struct Buffer : Resource {
    uint32_t size = 0, fvf = 0;
    uint32_t mem = 0;
    bool index = false;
    uint32_t dirty_lo = 0, dirty_hi = 0;  // byte range written since last upload
    GLuint gl = 0;
    ~Buffer() override;
};

struct ShaderInfo;  // shader_translator.h
struct Shader : com::Object {
    Device* dev = nullptr;
    bool pixel = false;
    std::vector<uint32_t> tokens;
    // Translated lazily on first draw. ps_1_x shaders do not declare sampler
    // types, so they get one variant per combination of bound texture types.
    std::unordered_map<uint32_t, ShaderInfo*> variants;
    ~Shader() override;
};

struct VertexElement {
    uint16_t stream, offset;
    uint8_t type, method, usage, usage_index;
};

struct VertexDecl : com::Object {
    Device* dev = nullptr;
    std::vector<uint8_t> raw;              // D3DVERTEXELEMENT9 array including D3DDECL_END
    std::vector<VertexElement> elements;   // without the end marker
};

struct Query : com::Object {
    Device* dev = nullptr;
    uint32_t qtype = 0;
};

struct Stream {
    uint32_t vb = 0, offset = 0, stride = 0, freq = 1;
};

struct Light {
    uint32_t data[26] = {};  // D3DLIGHT9
    bool enabled = false;
};

// Transform slots: 0..23 = D3DTS_VIEW/PROJECTION/TEXTURE0-7 ids, 24..27 = WORLD0..3.
constexpr int kTransforms = 28;
inline int transform_slot(uint32_t ts) {
    if (ts < 24) return (int)ts;
    if (ts >= 256 && ts < 260) return 24 + (int)(ts - 256);
    return -1;
}

struct State {
    uint32_t rs[256] = {};
    uint32_t ss[20][14] = {};
    uint32_t tss[8][33] = {};
    uint32_t textures[20] = {};
    Stream streams[16];
    uint32_t indices = 0, vdecl = 0, fvf = 0, vs = 0, ps = 0;
    float vs_f[256][4] = {};
    int32_t vs_i[16][4] = {};
    uint32_t vs_b[16] = {};
    float ps_f[224][4] = {};
    int32_t ps_i[16][4] = {};
    uint32_t ps_b[16] = {};
    uint32_t viewport[6] = {};  // X Y W H MinZ MaxZ (floats as bits)
    uint32_t scissor[4] = {};
    float transforms[kTransforms][16] = {};
    float material[17] = {};
    Light lights[16];
    float clip_planes[6][4] = {};
};

// Which parts of a State a state block holds (and applies).
struct StateMask {
    std::bitset<256> rs;
    std::bitset<20 * 14> ss;
    std::bitset<8 * 33> tss;
    std::bitset<20> textures;
    std::bitset<16> streams;
    bool indices = false, vdecl = false, vs = false, ps = false, viewport = false, scissor = false,
         material = false;
    std::bitset<256> vs_f;
    std::bitset<16> vs_i, vs_b, ps_i, ps_b;
    std::bitset<224> ps_f;
    std::bitset<kTransforms> transforms;
    std::bitset<16> lights;
    std::bitset<6> clip;
    void set_all();
    void set_pixel();
    void set_vertex();
};

struct StateBlock : com::Object {
    Device* dev = nullptr;
    State state;
    StateMask mask;
};

struct SwapChain : com::Object {
    Device* dev = nullptr;
};

struct DrawCache;  // d3d9_draw.cpp

struct Device : com::Object {
    uint32_t d3d = 0;      // guest IDirect3D9
    uint32_t pp[14] = {};  // D3DPRESENT_PARAMETERS
    uint32_t behavior = 0, focus = 0;
    Surface* backbuffer = nullptr;
    Surface* auto_depth = nullptr;
    Surface* rt[4] = {};
    Surface* ds = nullptr;
    SwapChain* swapchain = nullptr;
    GLuint fbo = 0, bb_fbo = 0;
    bool targets_dirty = true;

    State st;
    StateBlock* recording = nullptr;  // between BeginStateBlock / EndStateBlock
    bool fixed_state_dirty = true;    // GL blend/depth/stencil/raster state needs re-applying
    bool in_scene = false;

    DrawCache* cache = nullptr;
    uint64_t frames = 0, draws = 0, frame_draws = 0;
    uint64_t stat_t0 = 0, stat_frames = 0;

    // State writes go to the recording block while one is open (D3D9 semantics).
    State& target() { return recording ? recording->state : st; }
    StateMask* mask() { return recording ? &recording->mask : nullptr; }
};

// Draw path (d3d9_draw.cpp).
void draw_init(Device* d);
void bind_targets(Device* d);
// prim: D3DPRIMITIVETYPE. Index data either from the bound IB (index_mem = 0)
// or user memory (DrawIndexedPrimitiveUP).
void draw(Device* d, uint32_t prim, uint32_t start_vertex, uint32_t prim_count, bool indexed,
          int32_t base_vertex, uint32_t start_index, uint32_t up_vertices, uint32_t up_stride,
          uint32_t up_indices, uint32_t up_index_fmt, uint32_t up_vertex_count);
void upload_texture(Texture* t);
GLuint surface_gl_texture(Surface* s);  // render target texture, or an uploaded copy

std::vector<VertexElement> fvf_to_decl(uint32_t fvf);

// Per-interval counters, logged with the FPS line.
struct GfxStats {
    uint64_t readbacks = 0, tex_uploads = 0, tex_bytes = 0, buf_uploads = 0, buf_bytes = 0, ring_bytes = 0,
             orphans = 0, programs = 0;
    uint64_t ns_draw = 0, ns_present = 0, ns_upload = 0;
};
extern GfxStats g_stats;

}  // namespace d3d
