// Direct3D 9 on GLES 3.2.
//
// Phase 1 (this file): full COM surface of IDirect3D9 / IDirect3DDevice9 and
// the resource interfaces, adapter enumeration and caps, device creation on
// the guest's render thread, resources backed by guest memory for Lock,
// complete state tracking, render targets, Clear and Present.
// Phase 2: vertex/index upload, D3D shader bytecode -> GLSL ES translation,
// fixed-function emulation, texture upload (DXT decode where unsupported) and
// draw submission.
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/guest_heap.h"
#include "gfx/d3d9_types.h"
#include "gfx/gl_context.h"
#include "hle/com.h"
#include "hle/hle.h"
#include "hle/user32.h"
#include "hle/win_util.h"
#include "runtime/process.h"

using namespace d3d;
using hle::arg;
using hle::ret;

namespace {

struct Device;

// ------------------------------------------------------------------ objects
struct Resource : com::Object {
    Device* dev = nullptr;
    uint32_t type = 0, usage = 0, pool = 0, format = 0, priority = 0;
};

struct Surface : Resource {
    uint32_t w = 0, h = 0;
    uint32_t mem = 0;               // guest copy for LockRect (allocated on first lock)
    com::Object* container = nullptr;
    uint32_t level = 0;
    GLuint gl_tex = 0, gl_rb = 0;   // render target / depth backing
    ~Surface() override {
        if (mem) gheap::free(mem);
        if (gl_tex) glDeleteTextures(1, &gl_tex);
        if (gl_rb) glDeleteRenderbuffers(1, &gl_rb);
    }
};

struct Texture : Resource {
    uint32_t w = 0, h = 0, levels = 1;
    bool cube = false;
    std::vector<Surface*> surfaces;  // levels (x6 faces for cube maps)
    uint32_t lod = 0, autogen_filter = 2;
    ~Texture() override {
        for (Surface* s : surfaces) com::release(s);
    }
};

struct Buffer : Resource {
    uint32_t size = 0, fvf = 0;
    uint32_t mem = 0;
    bool index = false;
    bool dirty = true;
    GLuint gl = 0;
    ~Buffer() override {
        if (mem) gheap::free(mem);
        if (gl) glDeleteBuffers(1, &gl);
    }
};

struct Shader : com::Object {
    Device* dev = nullptr;
    bool pixel = false;
    std::vector<uint32_t> tokens;
};

struct VertexDecl : com::Object {
    Device* dev = nullptr;
    std::vector<uint8_t> elements;  // D3DVERTEXELEMENT9 array including D3DDECL_END
};

struct Query : com::Object {
    Device* dev = nullptr;
    uint32_t qtype = 0;
};

struct StateBlock : com::Object {
    Device* dev = nullptr;
};

struct SwapChain : com::Object {
    Device* dev = nullptr;
};

struct Direct3D : com::Object {};

struct Stream {
    uint32_t vb = 0, offset = 0, stride = 0, freq = 1;
};

struct Device : com::Object {
    uint32_t d3d = 0;             // guest IDirect3D9
    uint32_t pp[14] = {};         // D3DPRESENT_PARAMETERS
    uint32_t behavior = 0, focus = 0;
    Surface* backbuffer = nullptr;
    Surface* auto_depth = nullptr;
    Surface* rt[4] = {};
    Surface* ds = nullptr;
    SwapChain* swapchain = nullptr;
    GLuint fbo = 0, bb_fbo = 0;
    bool targets_dirty = true;

    uint32_t rs[256] = {};
    uint32_t ss[20][14] = {};
    uint32_t tss[8][33] = {};
    uint32_t textures[20] = {};
    Stream streams[16];
    uint32_t indices = 0, vdecl = 0, fvf = 0, vs = 0, ps = 0;
    float vs_f[256][4] = {}, ps_f[224][4] = {};
    int32_t vs_i[16][4] = {}, ps_i[16][4] = {};
    uint32_t vs_b[16] = {}, ps_b[16] = {};
    uint32_t viewport[6] = {};    // X, Y, Width, Height, MinZ(float), MaxZ(float)
    uint32_t scissor[4] = {};
    std::map<uint32_t, std::vector<float>> transforms;
    uint32_t material[17] = {};
    std::map<uint32_t, std::vector<uint32_t>> lights;
    std::map<uint32_t, bool> light_enable;
    float clip_planes[6][4] = {};
    bool in_scene = false;

    uint64_t frames = 0, draws = 0, frame_draws = 0;
    uint64_t stat_t0 = 0, stat_frames = 0;
};

// ------------------------------------------------------------------ helpers
std::mutex g_vt_mu;

template <typename T>
T* obj(cpu::Cpu& c, int i = 0) {
    return static_cast<T*>(com::from_guest(arg(c, i)));
}

float as_f(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
uint32_t as_u(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

void write_ptr(uint32_t out, com::Object* o) {
    if (out) mem::wr32(out, o ? o->guest : 0);
}

GLenum gl_internal_rt(uint32_t fmt) {
    switch (fmt) {
        case FMT_R5G6B5: return GL_RGB565;
        case FMT_A16B16G16R16F: return GL_RGBA16F;
        case FMT_R32F: return GL_R32F;
        default: return GL_RGBA8;
    }
}

// Surface vtables are built on first use (they need thunks).
uint32_t surface_vtable();
uint32_t texture_vtable();
uint32_t cube_vtable();
uint32_t vb_vtable();
uint32_t ib_vtable();
uint32_t shader_vtable(bool pixel);
uint32_t vdecl_vtable();
uint32_t query_vtable();
uint32_t stateblock_vtable();
uint32_t swapchain_vtable();
uint32_t device_vtable();

// {85C31227-3DE5-4F00-9B3A-F11AC38C18B5}, {580CA87E-1D3C-4D54-991D-B7D3E3C298CE} (base texture)
const uint32_t kTextureIIDs[8] = {0x85C31227, 0x4F003DE5, 0x1AF13A9B, 0xB5188CC3,
                                  0x580CA87E, 0x4D541D3C, 0xD3B71D99, 0xCE98C2E3};
// {FFF32F81-D953-473A-9223-93D652ABA93F}, base texture
const uint32_t kCubeIIDs[8] = {0xFFF32F81, 0x473AD953, 0xD6932392, 0x3FA9AB52,
                               0x580CA87E, 0x4D541D3C, 0xD3B71D99, 0xCE98C2E3};

Surface* new_surface(Device* d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                     com::Object* container, uint32_t level) {
    Surface* s = new Surface();
    s->dev = d;
    s->type = RTYPE_SURFACE;
    s->w = w;
    s->h = h;
    s->format = fmt;
    s->usage = usage;
    s->pool = pool;
    s->container = container;
    s->level = level;
    com::wrap(s, surface_vtable());
    if (usage & USAGE_RENDERTARGET) {
        glGenTextures(1, &s->gl_tex);
        glBindTexture(GL_TEXTURE_2D, s->gl_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, gl_internal_rt(fmt), (GLsizei)w, (GLsizei)h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else if (usage & USAGE_DEPTHSTENCIL) {
        glGenRenderbuffers(1, &s->gl_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, s->gl_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
    }
    return s;
}

void bind_targets(Device* d) {
    if (!d->targets_dirty) return;
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    Surface* rt = d->rt[0];
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt ? rt->gl_tex : 0, 0);
    GLuint rb = d->ds ? d->ds->gl_rb : 0;
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) LOGW("render target FBO incomplete: %x", st);
    d->targets_dirty = false;
}

void set_default_viewport(Device* d) {
    Surface* rt = d->rt[0];
    d->viewport[0] = 0;
    d->viewport[1] = 0;
    d->viewport[2] = rt ? rt->w : 0;
    d->viewport[3] = rt ? rt->h : 0;
    d->viewport[4] = as_u(0.0f);
    d->viewport[5] = as_u(1.0f);
}

void set_default_states(Device* d) {
    uint32_t* rs = d->rs;
    rs[7] = 1;                 // ZENABLE
    rs[8] = 3;                 // FILLMODE solid
    rs[9] = 2;                 // SHADEMODE gouraud
    rs[14] = 1;                // ZWRITEENABLE
    rs[19] = 2;                // SRCBLEND one
    rs[20] = 1;                // DESTBLEND zero
    rs[22] = 2;                // CULLMODE ccw
    rs[23] = 4;                // ZFUNC lessequal
    rs[25] = 8;                // ALPHAFUNC always
    rs[56] = 8;                // STENCILFUNC always
    rs[58] = 0xFFFFFFFF;       // STENCILMASK
    rs[59] = 0xFFFFFFFF;       // STENCILWRITEMASK
    rs[60] = 0xFFFFFFFF;       // TEXTUREFACTOR
    rs[137] = 1;               // LIGHTING
    rs[168] = 0xF;             // COLORWRITEENABLE
    rs[171] = 1;               // BLENDOP add
    for (auto& s : d->ss) {
        s[1] = 1;  // ADDRESSU wrap
        s[2] = 1;
        s[3] = 1;
        s[5] = 1;  // MAGFILTER point
        s[6] = 1;  // MINFILTER point
        s[7] = 0;  // MIPFILTER none
        s[10] = 1; // MAXANISOTROPY
    }
    for (int i = 0; i < 8; ++i) {
        d->tss[i][1] = i == 0 ? 4 : 1;  // COLOROP modulate / disable
        d->tss[i][2] = 2;               // COLORARG1 texture
        d->tss[i][3] = 1;               // COLORARG2 current
        d->tss[i][4] = i == 0 ? 2 : 1;  // ALPHAOP selectarg1 / disable
        d->tss[i][5] = 2;
        d->tss[i][6] = 1;
        d->tss[i][11] = (uint32_t)i;    // TEXCOORDINDEX
    }
}

bool create_backbuffer(Device* d) {
    uint32_t w = d->pp[0] ? d->pp[0] : user32::kDesktopWidth;
    uint32_t h = d->pp[1] ? d->pp[1] : user32::kDesktopHeight;
    d->pp[0] = w;
    d->pp[1] = h;
    if (d->pp[2] == FMT_UNKNOWN) d->pp[2] = FMT_X8R8G8B8;
    if (d->backbuffer) com::release(d->backbuffer);
    if (d->auto_depth) com::release(d->auto_depth);
    d->backbuffer = new_surface(d, w, h, d->pp[2], USAGE_RENDERTARGET, 0, nullptr, 0);
    d->auto_depth = nullptr;
    if (d->pp[9]) d->auto_depth = new_surface(d, w, h, d->pp[10], USAGE_DEPTHSTENCIL, 0, nullptr, 0);
    for (auto& r : d->rt) r = nullptr;
    d->rt[0] = d->backbuffer;
    d->ds = d->auto_depth;
    d->targets_dirty = true;
    glBindFramebuffer(GL_FRAMEBUFFER, d->bb_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->backbuffer->gl_tex, 0);
    set_default_viewport(d);
    LOGI("back buffer %ux%u fmt %u, auto depth %s fmt %u", w, h, d->pp[2], d->pp[9] ? "yes" : "no", d->pp[10]);
    return true;
}

// ------------------------------------------------------------------ IDirect3D9
struct Mode {
    uint32_t w, h;
};
const Mode kModes[] = {{640, 480},  {800, 600},  {1024, 768}, {1280, 720},
                       {1280, 960}, {1600, 720}, {1920, 864}};
constexpr uint32_t kModeCount = sizeof kModes / sizeof kModes[0];

bool display_format(uint32_t f) { return f == FMT_X8R8G8B8 || f == FMT_R5G6B5 || f == FMT_A8R8G8B8; }

bool texture_format_ok(uint32_t f) {
    switch (f) {
        case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
        case FMT_A4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_DXT1: case FMT_DXT2:
        case FMT_DXT3: case FMT_DXT4: case FMT_DXT5: case FMT_A8B8G8R8: case FMT_X8B8G8R8:
        case FMT_V8U8: case FMT_Q8W8V8U8: case FMT_A16B16G16R16F: case FMT_R32F: case FMT_G16R16:
            return true;
        default: return false;
    }
}

void fill_caps(uint32_t p) {
    memset(mem::ptr(p), 0, 304);
    auto w = [p](uint32_t off, uint32_t v) { mem::wr32(p + off, v); };
    auto wf = [p](uint32_t off, float v) { mem::wr<float>(p + off, v); };
    w(0, 1);              // D3DDEVTYPE_HAL
    w(8, 0x00020000);     // Caps: READ_SCANLINE
    w(12, 0x60020000);    // Caps2: FULLSCREENGAMMA | DYNAMICTEXTURES | CANAUTOGENMIPMAP
    w(16, 0x00000320);    // Caps3
    w(20, 0x8000000F);    // PresentationIntervals
    w(24, 0x1);           // CursorCaps
    w(28, 0x001BBEF0);    // DevCaps (HW T&L, pure device, ...)
    w(32, 0x000E2FF2);    // PrimitiveMiscCaps
    w(36, 0x07322191);    // RasterCaps
    w(40, 0xFF);          // ZCmpCaps
    w(44, 0x3FFF);        // SrcBlendCaps
    w(48, 0x3FFF);        // DestBlendCaps
    w(52, 0xFF);          // AlphaCmpCaps
    w(56, 0x00084208);    // ShadeCaps
    w(60, 0x0001EC05);    // TextureCaps (no POW2 restriction)
    w(64, 0x03030700);    // TextureFilterCaps
    w(68, 0x03030700);
    w(72, 0x03030300);
    w(76, 0x3F);          // TextureAddressCaps
    w(80, 0x3F);
    w(84, 0x1F);          // LineCaps
    w(88, 4096);
    w(92, 4096);
    w(96, 2048);
    w(100, 8192);
    w(104, 4096);
    w(108, 16);
    wf(112, 1e10f);
    wf(116, -32768.f);
    wf(120, -32768.f);
    wf(124, 32768.f);
    wf(128, 32768.f);
    w(136, 0x1FF);        // StencilCaps
    w(140, 0x00180008);   // FVFCaps
    w(144, 0x03FEFFFF);   // TextureOpCaps
    w(148, 8);
    w(152, 8);
    w(156, 0x17B);        // VertexProcessingCaps
    w(160, 8);
    w(164, 6);
    w(168, 4);
    w(172, 0);
    wf(176, 256.f);
    w(180, 0x000FFFFF);
    w(184, 0x00FFFFFF);
    w(188, 16);
    w(192, 508);
    w(196, 0xFFFE0200);   // vs_2_0
    w(200, 256);
    w(204, 0xFFFF0200);   // ps_2_0
    wf(208, 8.f);
    w(212, 0x51);         // DevCaps2
    w(232, 1);            // NumberOfAdaptersInGroup
    w(236, 0x30F);        // DeclTypes
    w(240, 1);            // NumSimultaneousRTs
    w(244, 0x03000300);   // StretchRectFilterCaps
    w(248, 1);            // VS20Caps
    w(252, 24);
    w(256, 12);
    w(260, 4);
    w(264, 0);            // PS20Caps
    w(272, 12);
    w(280, 96);
    w(288, 65535);
    w(292, 96);
}

void d3d_GetAdapterCount(cpu::Cpu& c) { ret(c, 1); }

void d3d_GetAdapterIdentifier(cpu::Cpu& c) {
    uint32_t p = arg(c, 3);
    memset(mem::ptr(p), 0, 1100);
    mem::write_cstr(p, "ati2dvag.dll", 512);
    mem::write_cstr(p + 512, "RADEON 9800 PRO (GLES translation layer)", 512);
    mem::write_cstr(p + 1024, "\\\\.\\DISPLAY1", 32);
    mem::wr32(p + 1056, 0x00060E00);
    mem::wr32(p + 1060, 0x00060000);
    mem::wr32(p + 1064, 0x1002);   // VendorId: ATI
    mem::wr32(p + 1068, 0x4E48);   // DeviceId: Radeon 9800 Pro
    mem::wr32(p + 1080, 0xD7B71EE2);
    mem::wr32(p + 1096, 1);
    ret(c, D3D_OK);
}

void d3d_GetAdapterModeCount(cpu::Cpu& c) { ret(c, display_format(arg(c, 2)) ? kModeCount : 0); }

void d3d_EnumAdapterModes(cpu::Cpu& c) {
    uint32_t fmt = arg(c, 2), i = arg(c, 3), out = arg(c, 4);
    if (!display_format(fmt) || i >= kModeCount) return ret(c, D3DERR_INVALIDCALL);
    mem::wr32(out + 0, kModes[i].w);
    mem::wr32(out + 4, kModes[i].h);
    mem::wr32(out + 8, 60);
    mem::wr32(out + 12, fmt);
    ret(c, D3D_OK);
}

void d3d_GetAdapterDisplayMode(cpu::Cpu& c) {
    uint32_t out = arg(c, 2);
    mem::wr32(out + 0, user32::kDesktopWidth);
    mem::wr32(out + 4, user32::kDesktopHeight);
    mem::wr32(out + 8, 60);
    mem::wr32(out + 12, FMT_X8R8G8B8);
    ret(c, D3D_OK);
}

void d3d_CheckDeviceType(cpu::Cpu& c) {
    ret(c, display_format(arg(c, 3)) || arg(c, 4) == FMT_UNKNOWN || display_format(arg(c, 4)) ? D3D_OK
                                                                                              : D3DERR_NOTAVAILABLE);
}

void d3d_CheckDeviceFormat(cpu::Cpu& c) {
    uint32_t usage = arg(c, 4), rtype = arg(c, 5), fmt = arg(c, 6);
    bool ok;
    if (usage & USAGE_DEPTHSTENCIL)
        ok = fmt == FMT_D16 || fmt == FMT_D24S8 || fmt == FMT_D24X8 || fmt == FMT_D32;
    else if (usage & USAGE_RENDERTARGET)
        ok = fmt == FMT_A8R8G8B8 || fmt == FMT_X8R8G8B8 || fmt == FMT_R5G6B5 || fmt == FMT_A16B16G16R16F ||
             fmt == FMT_R32F;
    else if (rtype == RTYPE_VERTEXBUFFER || rtype == RTYPE_INDEXBUFFER)
        ok = true;
    else
        ok = texture_format_ok(fmt);
    ret(c, ok ? D3D_OK : D3DERR_NOTAVAILABLE);
}

void d3d_CheckDeviceMultiSampleType(cpu::Cpu& c) {
    uint32_t type = arg(c, 5), quality = arg(c, 6);
    if (quality) mem::wr32(quality, 1);
    ret(c, type == 0 ? D3D_OK : D3DERR_NOTAVAILABLE);
}

void d3d_CheckDepthStencilMatch(cpu::Cpu& c) { ret(c, D3D_OK); }

void d3d_GetDeviceCaps(cpu::Cpu& c) {
    fill_caps(arg(c, 3));
    ret(c, D3D_OK);
}

void d3d_GetAdapterMonitor(cpu::Cpu& c) { ret(c, 0x00050001); }

void d3d_CreateDevice(cpu::Cpu& c) {
    uint32_t type = arg(c, 2), focus = arg(c, 3), flags = arg(c, 4), ppp = arg(c, 5), out = arg(c, 6);
    LOGI("CreateDevice(type=%u, hwnd=%08x, flags=%x): %ux%u fmt %u, windowed %u, autodepth %u/%u, interval %x",
         type, focus, flags, mem::rd32(ppp), mem::rd32(ppp + 4), mem::rd32(ppp + 8), mem::rd32(ppp + 32),
         mem::rd32(ppp + 36), mem::rd32(ppp + 40), mem::rd32(ppp + 52));
    if (!gfx::make_current()) return ret(c, D3DERR_NOTAVAILABLE);
    Device* d = new Device();
    d->d3d = arg(c, 0);
    d->focus = focus;
    d->behavior = flags;
    for (int i = 0; i < 14; ++i) d->pp[i] = mem::rd32(ppp + i * 4);
    com::wrap(d, device_vtable());
    com::add_ref(com::from_guest(d->d3d));
    glGenFramebuffers(1, &d->fbo);
    glGenFramebuffers(1, &d->bb_fbo);
    create_backbuffer(d);
    for (int i = 0; i < 14; ++i) mem::wr32(ppp + i * 4, d->pp[i]);
    set_default_states(d);
    d->swapchain = new SwapChain();
    d->swapchain->dev = d;
    com::wrap(d->swapchain, swapchain_vtable());
    d->stat_t0 = winu::mono_ns();
    mem::wr32(out, d->guest);
    proc::set_status("D3D9 device created (" + std::to_string(d->pp[0]) + "x" + std::to_string(d->pp[1]) + ")");
    ret(c, D3D_OK);
}

#define D3D_M(name, bytes, fn) {"IDirect3D9::" name, bytes, fn, 0}
const com::Method kD3D9Methods[] = {
    D3D_M("QueryInterface", 12, com::QueryInterface),
    D3D_M("AddRef", 4, com::AddRef),
    D3D_M("Release", 4, com::Release),
    D3D_M("RegisterSoftwareDevice", 8, nullptr),
    D3D_M("GetAdapterCount", 4, d3d_GetAdapterCount),
    D3D_M("GetAdapterIdentifier", 16, d3d_GetAdapterIdentifier),
    D3D_M("GetAdapterModeCount", 12, d3d_GetAdapterModeCount),
    D3D_M("EnumAdapterModes", 20, d3d_EnumAdapterModes),
    D3D_M("GetAdapterDisplayMode", 12, d3d_GetAdapterDisplayMode),
    D3D_M("CheckDeviceType", 24, d3d_CheckDeviceType),
    D3D_M("CheckDeviceFormat", 28, d3d_CheckDeviceFormat),
    D3D_M("CheckDeviceMultiSampleType", 28, d3d_CheckDeviceMultiSampleType),
    D3D_M("CheckDepthStencilMatch", 24, d3d_CheckDepthStencilMatch),
    D3D_M("CheckDeviceFormatConversion", 20, d3d_CheckDepthStencilMatch),
    D3D_M("GetDeviceCaps", 16, d3d_GetDeviceCaps),
    D3D_M("GetAdapterMonitor", 8, d3d_GetAdapterMonitor),
    D3D_M("CreateDevice", 28, d3d_CreateDevice),
};

// ------------------------------------------------------------------ IDirect3DResource9 common
void res_GetDevice(cpu::Cpu& c) {
    Resource* r = obj<Resource>(c);
    com::add_ref(r->dev);
    write_ptr(arg(c, 1), r->dev);
    ret(c, D3D_OK);
}
void res_SetPriority(cpu::Cpu& c) {
    Resource* r = obj<Resource>(c);
    uint32_t old = r->priority;
    r->priority = arg(c, 1);
    ret(c, old);
}
void res_GetPriority(cpu::Cpu& c) { ret(c, obj<Resource>(c)->priority); }
void res_PreLoad(cpu::Cpu& c) { ret(c, 0); }
void res_GetType(cpu::Cpu& c) { ret(c, obj<Resource>(c)->type); }
void res_SetPrivateData(cpu::Cpu& c) { ret(c, D3D_OK); }
void res_GetPrivateData(cpu::Cpu& c) { ret(c, 0x88760866); }  // D3DERR_NOTFOUND
void res_FreePrivateData(cpu::Cpu& c) { ret(c, D3D_OK); }

#define RESOURCE_METHODS(P)                                    \
    {P "QueryInterface", 12, com::QueryInterface, 0},          \
        {P "AddRef", 4, com::AddRef, 0},                       \
        {P "Release", 4, com::Release, 0},                     \
        {P "GetDevice", 8, res_GetDevice, 0},                  \
        {P "SetPrivateData", 20, res_SetPrivateData, 0},       \
        {P "GetPrivateData", 16, res_GetPrivateData, 0},       \
        {P "FreePrivateData", 8, res_FreePrivateData, 0},      \
        {P "SetPriority", 8, res_SetPriority, 0},              \
        {P "GetPriority", 4, res_GetPriority, 0},              \
        {P "PreLoad", 4, res_PreLoad, 0},                      \
        {P "GetType", 4, res_GetType, 0}

// ------------------------------------------------------------------ IDirect3DSurface9
void write_surface_desc(Surface* s, uint32_t p) {
    mem::wr32(p + 0, s->format);
    mem::wr32(p + 4, RTYPE_SURFACE);
    mem::wr32(p + 8, s->usage);
    mem::wr32(p + 12, s->pool);
    mem::wr32(p + 16, 0);
    mem::wr32(p + 20, 0);
    mem::wr32(p + 24, s->w);
    mem::wr32(p + 28, s->h);
}

uint32_t surface_lock(Surface* s, uint32_t locked, uint32_t rect) {
    uint32_t pitch = level_pitch(s->format, s->w);
    if (!s->mem) s->mem = gheap::alloc(level_size(s->format, s->w, s->h), true);
    if (!s->mem) return D3DERR_OUTOFVIDEOMEMORY;
    uint32_t off = 0;
    if (rect) {
        uint32_t left = mem::rd32(rect), top = mem::rd32(rect + 4);
        if (is_dxt(s->format))
            off = (top / 4) * pitch + (left / 4) * (s->format == FMT_DXT1 ? 8 : 16);
        else
            off = top * pitch + left * bytes_per_pixel(s->format);
    }
    mem::wr32(locked, pitch);
    mem::wr32(locked + 4, s->mem + off);
    return D3D_OK;
}

void surf_GetContainer(cpu::Cpu& c) {
    Surface* s = obj<Surface>(c);
    com::Object* owner = s->container ? s->container : static_cast<com::Object*>(s->dev);
    com::add_ref(owner);
    write_ptr(arg(c, 2), owner);
    ret(c, D3D_OK);
}
void surf_GetDesc(cpu::Cpu& c) {
    write_surface_desc(obj<Surface>(c), arg(c, 1));
    ret(c, D3D_OK);
}
void surf_LockRect(cpu::Cpu& c) { ret(c, surface_lock(obj<Surface>(c), arg(c, 1), arg(c, 2))); }
void surf_UnlockRect(cpu::Cpu& c) { ret(c, D3D_OK); }

const com::Method kSurfaceMethods[] = {
    RESOURCE_METHODS("IDirect3DSurface9::"),
    {"IDirect3DSurface9::GetContainer", 12, surf_GetContainer, 0},
    {"IDirect3DSurface9::GetDesc", 8, surf_GetDesc, 0},
    {"IDirect3DSurface9::LockRect", 16, surf_LockRect, 0},
    {"IDirect3DSurface9::UnlockRect", 4, surf_UnlockRect, 0},
    {"IDirect3DSurface9::GetDC", 8, nullptr, D3DERR_INVALIDCALL},
    {"IDirect3DSurface9::ReleaseDC", 8, nullptr, D3DERR_INVALIDCALL},
};

// ------------------------------------------------------------------ textures
void tex_SetLOD(cpu::Cpu& c) {
    Texture* t = obj<Texture>(c);
    uint32_t old = t->lod;
    t->lod = arg(c, 1);
    ret(c, old);
}
void tex_GetLOD(cpu::Cpu& c) { ret(c, obj<Texture>(c)->lod); }
void tex_GetLevelCount(cpu::Cpu& c) { ret(c, obj<Texture>(c)->levels); }
void tex_SetAutoGenFilterType(cpu::Cpu& c) {
    obj<Texture>(c)->autogen_filter = arg(c, 1);
    ret(c, D3D_OK);
}
void tex_GetAutoGenFilterType(cpu::Cpu& c) { ret(c, obj<Texture>(c)->autogen_filter); }
void tex_GenerateMipSubLevels(cpu::Cpu& c) { ret(c, 0); }

Surface* tex_level(Texture* t, uint32_t face, uint32_t level) {
    uint32_t i = face * t->levels + level;
    return i < t->surfaces.size() ? t->surfaces[i] : nullptr;
}

void tex_GetLevelDesc(cpu::Cpu& c) {
    Surface* s = tex_level(obj<Texture>(c), 0, arg(c, 1));
    if (!s) return ret(c, D3DERR_INVALIDCALL);
    write_surface_desc(s, arg(c, 2));
    ret(c, D3D_OK);
}
void tex_GetSurfaceLevel(cpu::Cpu& c) {
    Surface* s = tex_level(obj<Texture>(c), 0, arg(c, 1));
    if (!s) return ret(c, D3DERR_INVALIDCALL);
    com::add_ref(s);
    write_ptr(arg(c, 2), s);
    ret(c, D3D_OK);
}
void tex_LockRect(cpu::Cpu& c) {
    Surface* s = tex_level(obj<Texture>(c), 0, arg(c, 1));
    if (!s) return ret(c, D3DERR_INVALIDCALL);
    ret(c, surface_lock(s, arg(c, 2), arg(c, 3)));
}
void tex_UnlockRect(cpu::Cpu& c) { ret(c, D3D_OK); }
void tex_AddDirtyRect(cpu::Cpu& c) { ret(c, D3D_OK); }

void cube_GetCubeMapSurface(cpu::Cpu& c) {
    Surface* s = tex_level(obj<Texture>(c), arg(c, 1), arg(c, 2));
    if (!s) return ret(c, D3DERR_INVALIDCALL);
    com::add_ref(s);
    write_ptr(arg(c, 3), s);
    ret(c, D3D_OK);
}
void cube_LockRect(cpu::Cpu& c) {
    Surface* s = tex_level(obj<Texture>(c), arg(c, 1), arg(c, 2));
    if (!s) return ret(c, D3DERR_INVALIDCALL);
    ret(c, surface_lock(s, arg(c, 3), arg(c, 4)));
}

#define BASETEX_METHODS(P)                                              \
    RESOURCE_METHODS(P), {P "SetLOD", 8, tex_SetLOD, 0}, {P "GetLOD", 4, tex_GetLOD, 0}, \
        {P "GetLevelCount", 4, tex_GetLevelCount, 0},                   \
        {P "SetAutoGenFilterType", 8, tex_SetAutoGenFilterType, 0},     \
        {P "GetAutoGenFilterType", 4, tex_GetAutoGenFilterType, 0},     \
        {P "GenerateMipSubLevels", 4, tex_GenerateMipSubLevels, 0}

const com::Method kTextureMethods[] = {
    BASETEX_METHODS("IDirect3DTexture9::"),
    {"IDirect3DTexture9::GetLevelDesc", 12, tex_GetLevelDesc, 0},
    {"IDirect3DTexture9::GetSurfaceLevel", 12, tex_GetSurfaceLevel, 0},
    {"IDirect3DTexture9::LockRect", 20, tex_LockRect, 0},
    {"IDirect3DTexture9::UnlockRect", 8, tex_UnlockRect, 0},
    {"IDirect3DTexture9::AddDirtyRect", 8, tex_AddDirtyRect, 0},
};

const com::Method kCubeMethods[] = {
    BASETEX_METHODS("IDirect3DCubeTexture9::"),
    {"IDirect3DCubeTexture9::GetLevelDesc", 12, tex_GetLevelDesc, 0},
    {"IDirect3DCubeTexture9::GetCubeMapSurface", 16, cube_GetCubeMapSurface, 0},
    {"IDirect3DCubeTexture9::LockRect", 24, cube_LockRect, 0},
    {"IDirect3DCubeTexture9::UnlockRect", 12, tex_UnlockRect, 0},
    {"IDirect3DCubeTexture9::AddDirtyRect", 12, tex_AddDirtyRect, 0},
};

// ------------------------------------------------------------------ buffers
void buf_Lock(cpu::Cpu& c) {
    Buffer* b = obj<Buffer>(c);
    uint32_t off = arg(c, 1), out = arg(c, 3);
    if (!b->mem) b->mem = gheap::alloc(b->size, true);
    if (!b->mem) return ret(c, D3DERR_OUTOFVIDEOMEMORY);
    b->dirty = true;
    mem::wr32(out, b->mem + std::min(off, b->size));
    ret(c, D3D_OK);
}
void buf_Unlock(cpu::Cpu& c) { ret(c, D3D_OK); }
void vb_GetDesc(cpu::Cpu& c) {
    Buffer* b = obj<Buffer>(c);
    uint32_t p = arg(c, 1);
    mem::wr32(p + 0, 0x64);  // D3DFMT_VERTEXDATA
    mem::wr32(p + 4, RTYPE_VERTEXBUFFER);
    mem::wr32(p + 8, b->usage);
    mem::wr32(p + 12, b->pool);
    mem::wr32(p + 16, b->size);
    mem::wr32(p + 20, b->fvf);
    ret(c, D3D_OK);
}
void ib_GetDesc(cpu::Cpu& c) {
    Buffer* b = obj<Buffer>(c);
    uint32_t p = arg(c, 1);
    mem::wr32(p + 0, b->format);
    mem::wr32(p + 4, RTYPE_INDEXBUFFER);
    mem::wr32(p + 8, b->usage);
    mem::wr32(p + 12, b->pool);
    mem::wr32(p + 16, b->size);
    ret(c, D3D_OK);
}

const com::Method kVBMethods[] = {
    RESOURCE_METHODS("IDirect3DVertexBuffer9::"),
    {"IDirect3DVertexBuffer9::Lock", 20, buf_Lock, 0},
    {"IDirect3DVertexBuffer9::Unlock", 4, buf_Unlock, 0},
    {"IDirect3DVertexBuffer9::GetDesc", 8, vb_GetDesc, 0},
};
const com::Method kIBMethods[] = {
    RESOURCE_METHODS("IDirect3DIndexBuffer9::"),
    {"IDirect3DIndexBuffer9::Lock", 20, buf_Lock, 0},
    {"IDirect3DIndexBuffer9::Unlock", 4, buf_Unlock, 0},
    {"IDirect3DIndexBuffer9::GetDesc", 8, ib_GetDesc, 0},
};

// ------------------------------------------------------------------ shaders / declarations / queries
template <typename T>
void dev_of(cpu::Cpu& c) {
    T* o = obj<T>(c);
    com::add_ref(o->dev);
    write_ptr(arg(c, 1), o->dev);
    ret(c, D3D_OK);
}
void shader_GetFunction(cpu::Cpu& c) {
    Shader* s = obj<Shader>(c);
    uint32_t data = arg(c, 1), psize = arg(c, 2);
    uint32_t bytes = (uint32_t)s->tokens.size() * 4;
    if (data) memcpy(mem::ptr(data), s->tokens.data(), bytes);
    mem::wr32(psize, bytes);
    ret(c, D3D_OK);
}
const com::Method kVSMethods[] = {
    {"IDirect3DVertexShader9::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirect3DVertexShader9::AddRef", 4, com::AddRef, 0},
    {"IDirect3DVertexShader9::Release", 4, com::Release, 0},
    {"IDirect3DVertexShader9::GetDevice", 8, dev_of<Shader>, 0},
    {"IDirect3DVertexShader9::GetFunction", 12, shader_GetFunction, 0},
};
const com::Method kPSMethods[] = {
    {"IDirect3DPixelShader9::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirect3DPixelShader9::AddRef", 4, com::AddRef, 0},
    {"IDirect3DPixelShader9::Release", 4, com::Release, 0},
    {"IDirect3DPixelShader9::GetDevice", 8, dev_of<Shader>, 0},
    {"IDirect3DPixelShader9::GetFunction", 12, shader_GetFunction, 0},
};

void vdecl_GetDeclaration(cpu::Cpu& c) {
    VertexDecl* v = obj<VertexDecl>(c);
    uint32_t out = arg(c, 1), pn = arg(c, 2);
    if (out) memcpy(mem::ptr(out), v->elements.data(), v->elements.size());
    mem::wr32(pn, (uint32_t)v->elements.size() / 8);
    ret(c, D3D_OK);
}
const com::Method kVDeclMethods[] = {
    {"IDirect3DVertexDeclaration9::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirect3DVertexDeclaration9::AddRef", 4, com::AddRef, 0},
    {"IDirect3DVertexDeclaration9::Release", 4, com::Release, 0},
    {"IDirect3DVertexDeclaration9::GetDevice", 8, dev_of<VertexDecl>, 0},
    {"IDirect3DVertexDeclaration9::GetDeclaration", 12, vdecl_GetDeclaration, 0},
};

void query_GetType(cpu::Cpu& c) { ret(c, obj<Query>(c)->qtype); }
void query_GetDataSize(cpu::Cpu& c) {
    uint32_t t = obj<Query>(c)->qtype;
    ret(c, t == 8 ? 4 : t == 9 ? 4 : 4);  // EVENT: BOOL, OCCLUSION: DWORD
}
void query_Issue(cpu::Cpu& c) { ret(c, D3D_OK); }
void query_GetData(cpu::Cpu& c) {
    Query* q = obj<Query>(c);
    uint32_t data = arg(c, 1), size = arg(c, 2);
    if (data && size >= 4) mem::wr32(data, q->qtype == 9 ? 1000 : 1);  // occlusion: visible
    ret(c, D3D_OK);
}
const com::Method kQueryMethods[] = {
    {"IDirect3DQuery9::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirect3DQuery9::AddRef", 4, com::AddRef, 0},
    {"IDirect3DQuery9::Release", 4, com::Release, 0},
    {"IDirect3DQuery9::GetDevice", 8, dev_of<Query>, 0},
    {"IDirect3DQuery9::GetType", 4, query_GetType, 0},
    {"IDirect3DQuery9::GetDataSize", 4, query_GetDataSize, 0},
    {"IDirect3DQuery9::Issue", 8, query_Issue, 0},
    {"IDirect3DQuery9::GetData", 16, query_GetData, 0},
};

const com::Method kStateBlockMethods[] = {
    {"IDirect3DStateBlock9::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirect3DStateBlock9::AddRef", 4, com::AddRef, 0},
    {"IDirect3DStateBlock9::Release", 4, com::Release, 0},
    {"IDirect3DStateBlock9::GetDevice", 8, dev_of<StateBlock>, 0},
    {"IDirect3DStateBlock9::Capture", 4, nullptr, 0},
    {"IDirect3DStateBlock9::Apply", 4, nullptr, 0},
};

void present_frame(Device* d);

void sc_Present(cpu::Cpu& c) {
    present_frame(obj<SwapChain>(c)->dev);
    ret(c, D3D_OK);
}
void sc_GetBackBuffer(cpu::Cpu& c) {
    Device* d = obj<SwapChain>(c)->dev;
    com::add_ref(d->backbuffer);
    write_ptr(arg(c, 3), d->backbuffer);
    ret(c, D3D_OK);
}
void sc_GetPresentParameters(cpu::Cpu& c) {
    Device* d = obj<SwapChain>(c)->dev;
    for (int i = 0; i < 14; ++i) mem::wr32(arg(c, 1) + i * 4, d->pp[i]);
    ret(c, D3D_OK);
}
const com::Method kSwapChainMethods[] = {
    {"IDirect3DSwapChain9::QueryInterface", 12, com::QueryInterface, 0},
    {"IDirect3DSwapChain9::AddRef", 4, com::AddRef, 0},
    {"IDirect3DSwapChain9::Release", 4, com::Release, 0},
    {"IDirect3DSwapChain9::Present", 24, sc_Present, 0},
    {"IDirect3DSwapChain9::GetFrontBufferData", 8, nullptr, 0},
    {"IDirect3DSwapChain9::GetBackBuffer", 16, sc_GetBackBuffer, 0},
    {"IDirect3DSwapChain9::GetRasterStatus", 8, nullptr, 0},
    {"IDirect3DSwapChain9::GetDisplayMode", 8, d3d_GetAdapterDisplayMode, 0},
    {"IDirect3DSwapChain9::GetDevice", 8, dev_of<SwapChain>, 0},
    {"IDirect3DSwapChain9::GetPresentParameters", 8, sc_GetPresentParameters, 0},
};

// ------------------------------------------------------------------ IDirect3DDevice9
Device* dev(cpu::Cpu& c) { return obj<Device>(c); }

void present_frame(Device* d) {
    gfx::present(d->bb_fbo, (int)d->backbuffer->w, (int)d->backbuffer->h);
    d->targets_dirty = true;  // present rebinds framebuffers
    d->frames++;
    d->stat_frames++;
    d->frame_draws = 0;
    uint64_t now = winu::mono_ns();
    if (now - d->stat_t0 >= 5000000000ull) {
        double secs = (double)(now - d->stat_t0) / 1e9;
        LOGI("presented %llu frames, %.1f FPS, %llu draws total", (unsigned long long)d->frames,
             (double)d->stat_frames / secs, (unsigned long long)d->draws);
        char buf[96];
        snprintf(buf, sizeof buf, "Running: %.1f FPS (%llu frames)", (double)d->stat_frames / secs,
                 (unsigned long long)d->frames);
        proc::set_status(buf);
        d->stat_t0 = now;
        d->stat_frames = 0;
    }
}

void dev_TestCooperativeLevel(cpu::Cpu& c) { ret(c, D3D_OK); }
void dev_GetAvailableTextureMem(cpu::Cpu& c) { ret(c, 256u << 20); }
void dev_EvictManagedResources(cpu::Cpu& c) { ret(c, D3D_OK); }
void dev_GetDirect3D(cpu::Cpu& c) {
    Device* d = dev(c);
    com::add_ref(com::from_guest(d->d3d));
    mem::wr32(arg(c, 1), d->d3d);
    ret(c, D3D_OK);
}
void dev_GetDeviceCaps(cpu::Cpu& c) {
    fill_caps(arg(c, 1));
    ret(c, D3D_OK);
}
void dev_GetDisplayMode(cpu::Cpu& c) {
    Device* d = dev(c);
    uint32_t out = arg(c, 2);
    mem::wr32(out + 0, d->pp[0]);
    mem::wr32(out + 4, d->pp[1]);
    mem::wr32(out + 8, 60);
    mem::wr32(out + 12, d->pp[2]);
    ret(c, D3D_OK);
}
void dev_GetCreationParameters(cpu::Cpu& c) {
    Device* d = dev(c);
    uint32_t p = arg(c, 1);
    mem::wr32(p + 0, 0);
    mem::wr32(p + 4, 1);
    mem::wr32(p + 8, d->focus);
    mem::wr32(p + 12, d->behavior);
    ret(c, D3D_OK);
}
void dev_ShowCursor(cpu::Cpu& c) { ret(c, 0); }
void dev_GetSwapChain(cpu::Cpu& c) {
    Device* d = dev(c);
    com::add_ref(d->swapchain);
    write_ptr(arg(c, 2), d->swapchain);
    ret(c, D3D_OK);
}
void dev_GetNumberOfSwapChains(cpu::Cpu& c) { ret(c, 1); }
void dev_Reset(cpu::Cpu& c) {
    Device* d = dev(c);
    uint32_t ppp = arg(c, 1);
    for (int i = 0; i < 14; ++i) d->pp[i] = mem::rd32(ppp + i * 4);
    LOGI("Reset: %ux%u", d->pp[0], d->pp[1]);
    create_backbuffer(d);
    for (int i = 0; i < 14; ++i) mem::wr32(ppp + i * 4, d->pp[i]);
    set_default_states(d);
    ret(c, D3D_OK);
}
void dev_Present(cpu::Cpu& c) {
    present_frame(dev(c));
    ret(c, D3D_OK);
}
void dev_GetBackBuffer(cpu::Cpu& c) {
    Device* d = dev(c);
    com::add_ref(d->backbuffer);
    write_ptr(arg(c, 4), d->backbuffer);
    ret(c, D3D_OK);
}
void dev_GetRasterStatus(cpu::Cpu& c) {
    uint32_t p = arg(c, 2);
    mem::wr32(p, 0);
    mem::wr32(p + 4, 0);
    ret(c, D3D_OK);
}

Texture* new_texture(Device* d, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt,
                     uint32_t pool, bool cube) {
    if (levels == 0) {
        levels = 1;
        for (uint32_t s = std::max(w, h); s > 1; s >>= 1) ++levels;
    }
    if (usage & USAGE_AUTOGENMIPMAP) levels = 1;
    Texture* t = new Texture();
    t->dev = d;
    t->type = cube ? RTYPE_CUBETEXTURE : RTYPE_TEXTURE;
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->usage = usage;
    t->format = fmt;
    t->pool = pool;
    t->cube = cube;
    t->iids = cube ? kCubeIIDs : kTextureIIDs;
    t->iid_count = 2;
    com::wrap(t, cube ? cube_vtable() : texture_vtable());
    for (uint32_t f = 0; f < (cube ? 6u : 1u); ++f) {
        for (uint32_t l = 0; l < levels; ++l) {
            uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
            t->surfaces.push_back(new_surface(d, lw, lh, fmt, l == 0 ? usage : (usage & ~USAGE_RENDERTARGET), pool, t, l));
        }
    }
    return t;
}

void dev_CreateTexture(cpu::Cpu& c) {
    Texture* t = new_texture(dev(c), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), false);
    write_ptr(arg(c, 7), t);
    ret(c, D3D_OK);
}
void dev_CreateCubeTexture(cpu::Cpu& c) {
    Texture* t = new_texture(dev(c), arg(c, 1), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), true);
    write_ptr(arg(c, 6), t);
    ret(c, D3D_OK);
}

Buffer* new_buffer(Device* d, bool index, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool) {
    Buffer* b = new Buffer();
    b->dev = d;
    b->index = index;
    b->type = index ? RTYPE_INDEXBUFFER : RTYPE_VERTEXBUFFER;
    b->size = len;
    b->usage = usage;
    b->pool = pool;
    if (index)
        b->format = fmt_or_fvf;
    else
        b->fvf = fmt_or_fvf;
    com::wrap(b, index ? ib_vtable() : vb_vtable());
    return b;
}
void dev_CreateVertexBuffer(cpu::Cpu& c) {
    write_ptr(arg(c, 5), new_buffer(dev(c), false, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
    ret(c, D3D_OK);
}
void dev_CreateIndexBuffer(cpu::Cpu& c) {
    write_ptr(arg(c, 5), new_buffer(dev(c), true, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
    ret(c, D3D_OK);
}
void dev_CreateRenderTarget(cpu::Cpu& c) {
    Surface* s = new_surface(dev(c), arg(c, 1), arg(c, 2), arg(c, 3), USAGE_RENDERTARGET, 0, nullptr, 0);
    write_ptr(arg(c, 7), s);
    ret(c, D3D_OK);
}
void dev_CreateDepthStencilSurface(cpu::Cpu& c) {
    Surface* s = new_surface(dev(c), arg(c, 1), arg(c, 2), arg(c, 3), USAGE_DEPTHSTENCIL, 0, nullptr, 0);
    write_ptr(arg(c, 7), s);
    ret(c, D3D_OK);
}
void dev_CreateOffscreenPlainSurface(cpu::Cpu& c) {
    Surface* s = new_surface(dev(c), arg(c, 1), arg(c, 2), arg(c, 3), 0, arg(c, 4), nullptr, 0);
    write_ptr(arg(c, 5), s);
    ret(c, D3D_OK);
}

void dev_SetRenderTarget(cpu::Cpu& c) {
    Device* d = dev(c);
    uint32_t i = arg(c, 1);
    if (i >= 4) return ret(c, D3DERR_INVALIDCALL);
    d->rt[i] = obj<Surface>(c, 2);
    d->targets_dirty = true;
    if (i == 0) {
        set_default_viewport(d);
        if (d->rt[0]) {
            d->scissor[0] = d->scissor[1] = 0;
            d->scissor[2] = d->rt[0]->w;
            d->scissor[3] = d->rt[0]->h;
        }
    }
    ret(c, D3D_OK);
}
void dev_GetRenderTarget(cpu::Cpu& c) {
    Device* d = dev(c);
    uint32_t i = arg(c, 1);
    Surface* s = i < 4 ? d->rt[i] : nullptr;
    if (s) com::add_ref(s);
    write_ptr(arg(c, 2), s);
    ret(c, s ? D3D_OK : 0x88760866);
}
void dev_SetDepthStencilSurface(cpu::Cpu& c) {
    Device* d = dev(c);
    d->ds = obj<Surface>(c, 1);
    d->targets_dirty = true;
    ret(c, D3D_OK);
}
void dev_GetDepthStencilSurface(cpu::Cpu& c) {
    Device* d = dev(c);
    if (d->ds) com::add_ref(d->ds);
    write_ptr(arg(c, 1), d->ds);
    ret(c, d->ds ? D3D_OK : 0x88760866);
}
void dev_BeginScene(cpu::Cpu& c) {
    dev(c)->in_scene = true;
    ret(c, D3D_OK);
}
void dev_EndScene(cpu::Cpu& c) {
    dev(c)->in_scene = false;
    ret(c, D3D_OK);
}

void dev_Clear(cpu::Cpu& c) {
    Device* d = dev(c);
    uint32_t count = arg(c, 1), rects = arg(c, 2), flags = arg(c, 3), color = arg(c, 4);
    float z = as_f(arg(c, 5));
    uint32_t stencil = arg(c, 6);
    if (!gfx::make_current()) return ret(c, D3D_OK);
    bind_targets(d);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearColor(((color >> 16) & 0xFF) / 255.f, ((color >> 8) & 0xFF) / 255.f, (color & 0xFF) / 255.f,
                 ((color >> 24) & 0xFF) / 255.f);
    glClearDepthf(z);
    glClearStencil((GLint)stencil);
    GLbitfield mask = 0;
    if (flags & 1) mask |= GL_COLOR_BUFFER_BIT;
    if ((flags & 2) && d->ds) mask |= GL_DEPTH_BUFFER_BIT;
    if ((flags & 4) && d->ds) mask |= GL_STENCIL_BUFFER_BIT;
    uint32_t rth = d->rt[0] ? d->rt[0]->h : 0;
    glEnable(GL_SCISSOR_TEST);
    auto clear_rect = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
        // D3D is top-left origin; GL is bottom-left.
        glScissor(x1, (GLint)rth - y2, x2 - x1, y2 - y1);
        glClear(mask);
    };
    if (count && rects) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t r = rects + i * 16;
            clear_rect((int32_t)mem::rd32(r), (int32_t)mem::rd32(r + 4), (int32_t)mem::rd32(r + 8),
                       (int32_t)mem::rd32(r + 12));
        }
    } else {
        int32_t x = (int32_t)d->viewport[0], y = (int32_t)d->viewport[1];
        clear_rect(x, y, x + (int32_t)d->viewport[2], y + (int32_t)d->viewport[3]);
    }
    glDisable(GL_SCISSOR_TEST);
    ret(c, D3D_OK);
}

void dev_SetTransform(cpu::Cpu& c) {
    std::vector<float> m(16);
    memcpy(m.data(), mem::ptr(arg(c, 2)), 64);
    dev(c)->transforms[arg(c, 1)] = std::move(m);
    ret(c, D3D_OK);
}
void dev_GetTransform(cpu::Cpu& c) {
    Device* d = dev(c);
    auto it = d->transforms.find(arg(c, 1));
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(mem::ptr(arg(c, 2)), it == d->transforms.end() ? kIdentity : it->second.data(), 64);
    ret(c, D3D_OK);
}
void dev_SetViewport(cpu::Cpu& c) {
    memcpy(dev(c)->viewport, mem::ptr(arg(c, 1)), 24);
    ret(c, D3D_OK);
}
void dev_GetViewport(cpu::Cpu& c) {
    memcpy(mem::ptr(arg(c, 1)), dev(c)->viewport, 24);
    ret(c, D3D_OK);
}
void dev_SetMaterial(cpu::Cpu& c) {
    memcpy(dev(c)->material, mem::ptr(arg(c, 1)), 68);
    ret(c, D3D_OK);
}
void dev_GetMaterial(cpu::Cpu& c) {
    memcpy(mem::ptr(arg(c, 1)), dev(c)->material, 68);
    ret(c, D3D_OK);
}
void dev_SetLight(cpu::Cpu& c) {
    std::vector<uint32_t> l(26);
    memcpy(l.data(), mem::ptr(arg(c, 2)), 104);
    dev(c)->lights[arg(c, 1)] = std::move(l);
    ret(c, D3D_OK);
}
void dev_LightEnable(cpu::Cpu& c) {
    dev(c)->light_enable[arg(c, 1)] = arg(c, 2) != 0;
    ret(c, D3D_OK);
}
void dev_SetClipPlane(cpu::Cpu& c) {
    uint32_t i = arg(c, 1);
    if (i < 6) memcpy(dev(c)->clip_planes[i], mem::ptr(arg(c, 2)), 16);
    ret(c, D3D_OK);
}
void dev_SetRenderState(cpu::Cpu& c) {
    uint32_t s = arg(c, 1);
    if (s < 256) dev(c)->rs[s] = arg(c, 2);
    ret(c, D3D_OK);
}
void dev_GetRenderState(cpu::Cpu& c) {
    uint32_t s = arg(c, 1);
    mem::wr32(arg(c, 2), s < 256 ? dev(c)->rs[s] : 0);
    ret(c, D3D_OK);
}

void dev_CreateStateBlock(cpu::Cpu& c) {
    StateBlock* sb = new StateBlock();
    sb->dev = dev(c);
    com::wrap(sb, stateblock_vtable());
    write_ptr(arg(c, 2), sb);
    LOG_ONCE("state blocks are accepted but do not capture state yet");
    ret(c, D3D_OK);
}
void dev_BeginStateBlock(cpu::Cpu& c) { ret(c, D3D_OK); }
void dev_EndStateBlock(cpu::Cpu& c) {
    StateBlock* sb = new StateBlock();
    sb->dev = dev(c);
    com::wrap(sb, stateblock_vtable());
    write_ptr(arg(c, 1), sb);
    ret(c, D3D_OK);
}

uint32_t sampler_index(uint32_t s) { return s >= 257 ? 16 + (s - 257) : s; }  // D3DDMAPSAMPLER / vertex samplers

void dev_GetTexture(cpu::Cpu& c) {
    uint32_t s = sampler_index(arg(c, 1));
    uint32_t t = s < 20 ? dev(c)->textures[s] : 0;
    if (com::Object* o = com::from_guest(t)) com::add_ref(o);
    mem::wr32(arg(c, 2), t);
    ret(c, D3D_OK);
}
void dev_SetTexture(cpu::Cpu& c) {
    uint32_t s = sampler_index(arg(c, 1));
    if (s < 20) dev(c)->textures[s] = arg(c, 2);
    ret(c, D3D_OK);
}
void dev_GetTextureStageState(cpu::Cpu& c) {
    uint32_t st = arg(c, 1), t = arg(c, 2);
    mem::wr32(arg(c, 3), st < 8 && t < 33 ? dev(c)->tss[st][t] : 0);
    ret(c, D3D_OK);
}
void dev_SetTextureStageState(cpu::Cpu& c) {
    uint32_t st = arg(c, 1), t = arg(c, 2);
    if (st < 8 && t < 33) dev(c)->tss[st][t] = arg(c, 3);
    ret(c, D3D_OK);
}
void dev_GetSamplerState(cpu::Cpu& c) {
    uint32_t s = sampler_index(arg(c, 1)), t = arg(c, 2);
    mem::wr32(arg(c, 3), s < 20 && t < 14 ? dev(c)->ss[s][t] : 0);
    ret(c, D3D_OK);
}
void dev_SetSamplerState(cpu::Cpu& c) {
    uint32_t s = sampler_index(arg(c, 1)), t = arg(c, 2);
    if (s < 20 && t < 14) dev(c)->ss[s][t] = arg(c, 3);
    ret(c, D3D_OK);
}
void dev_ValidateDevice(cpu::Cpu& c) {
    if (arg(c, 1)) mem::wr32(arg(c, 1), 1);
    ret(c, D3D_OK);
}
void dev_SetScissorRect(cpu::Cpu& c) {
    memcpy(dev(c)->scissor, mem::ptr(arg(c, 1)), 16);
    ret(c, D3D_OK);
}
void dev_GetScissorRect(cpu::Cpu& c) {
    memcpy(mem::ptr(arg(c, 1)), dev(c)->scissor, 16);
    ret(c, D3D_OK);
}
void dev_GetSoftwareVertexProcessing(cpu::Cpu& c) { ret(c, 0); }

void count_draw(Device* d, const char* what) {
    d->draws++;
    d->frame_draws++;
    LOG_ONCE("draw calls are accepted but not rendered yet (shader translator pending)");
    (void)what;
}
void dev_DrawPrimitive(cpu::Cpu& c) {
    count_draw(dev(c), "DrawPrimitive");
    ret(c, D3D_OK);
}
void dev_DrawIndexedPrimitive(cpu::Cpu& c) {
    count_draw(dev(c), "DrawIndexedPrimitive");
    ret(c, D3D_OK);
}
void dev_DrawPrimitiveUP(cpu::Cpu& c) {
    Device* d = dev(c);
    count_draw(d, "DrawPrimitiveUP");
    d->streams[0] = Stream{};  // D3D9 resets stream 0 after *UP draws
    d->indices = 0;
    ret(c, D3D_OK);
}
void dev_DrawIndexedPrimitiveUP(cpu::Cpu& c) {
    Device* d = dev(c);
    count_draw(d, "DrawIndexedPrimitiveUP");
    d->streams[0] = Stream{};
    d->indices = 0;
    ret(c, D3D_OK);
}

void dev_CreateVertexDeclaration(cpu::Cpu& c) {
    VertexDecl* v = new VertexDecl();
    v->dev = dev(c);
    uint32_t p = arg(c, 1);
    for (uint32_t i = 0; i < 64; ++i, p += 8) {
        const uint8_t* e = mem::ptr(p);
        v->elements.insert(v->elements.end(), e, e + 8);
        if (mem::rd16(p) == 0xFF) break;
    }
    com::wrap(v, vdecl_vtable());
    write_ptr(arg(c, 2), v);
    ret(c, D3D_OK);
}
void dev_SetVertexDeclaration(cpu::Cpu& c) {
    Device* d = dev(c);
    d->vdecl = arg(c, 1);
    ret(c, D3D_OK);
}
void dev_GetVertexDeclaration(cpu::Cpu& c) {
    uint32_t v = dev(c)->vdecl;
    if (com::Object* o = com::from_guest(v)) com::add_ref(o);
    mem::wr32(arg(c, 1), v);
    ret(c, D3D_OK);
}
void dev_SetFVF(cpu::Cpu& c) {
    Device* d = dev(c);
    d->fvf = arg(c, 1);
    d->vdecl = 0;
    ret(c, D3D_OK);
}
void dev_GetFVF(cpu::Cpu& c) {
    mem::wr32(arg(c, 1), dev(c)->fvf);
    ret(c, D3D_OK);
}

Shader* new_shader(Device* d, uint32_t fn, bool pixel) {
    Shader* s = new Shader();
    s->dev = d;
    s->pixel = pixel;
    for (uint32_t p = fn;; p += 4) {
        uint32_t t = mem::rd32(p);
        s->tokens.push_back(t);
        if (t == 0x0000FFFF || s->tokens.size() > 65536) break;
    }
    com::wrap(s, shader_vtable(pixel));
    return s;
}
void dev_CreateVertexShader(cpu::Cpu& c) {
    write_ptr(arg(c, 2), new_shader(dev(c), arg(c, 1), false));
    ret(c, D3D_OK);
}
void dev_CreatePixelShader(cpu::Cpu& c) {
    write_ptr(arg(c, 2), new_shader(dev(c), arg(c, 1), true));
    ret(c, D3D_OK);
}
void dev_SetVertexShader(cpu::Cpu& c) {
    dev(c)->vs = arg(c, 1);
    ret(c, D3D_OK);
}
void dev_GetVertexShader(cpu::Cpu& c) {
    uint32_t v = dev(c)->vs;
    if (com::Object* o = com::from_guest(v)) com::add_ref(o);
    mem::wr32(arg(c, 1), v);
    ret(c, D3D_OK);
}
void dev_SetPixelShader(cpu::Cpu& c) {
    dev(c)->ps = arg(c, 1);
    ret(c, D3D_OK);
}
void dev_GetPixelShader(cpu::Cpu& c) {
    uint32_t v = dev(c)->ps;
    if (com::Object* o = com::from_guest(v)) com::add_ref(o);
    mem::wr32(arg(c, 1), v);
    ret(c, D3D_OK);
}

template <int Bank, bool Set>
void dev_ShaderConst(cpu::Cpu& c) {
    // Bank: 0 VS float, 1 VS int, 2 VS bool, 3 PS float, 4 PS int, 5 PS bool
    Device* d = dev(c);
    uint32_t start = arg(c, 1), data = arg(c, 2), count = arg(c, 3);
    void* base;
    uint32_t elem, limit;
    switch (Bank) {
        case 0: base = d->vs_f; elem = 16; limit = 256; break;
        case 1: base = d->vs_i; elem = 16; limit = 16; break;
        case 2: base = d->vs_b; elem = 4; limit = 16; break;
        case 3: base = d->ps_f; elem = 16; limit = 224; break;
        case 4: base = d->ps_i; elem = 16; limit = 16; break;
        default: base = d->ps_b; elem = 4; limit = 16; break;
    }
    if (start >= limit) return ret(c, D3DERR_INVALIDCALL);
    if (start + count > limit) count = limit - start;
    uint8_t* p = (uint8_t*)base + start * elem;
    if (Set)
        memcpy(p, mem::ptr(data), count * elem);
    else
        memcpy(mem::ptr(data), p, count * elem);
    ret(c, D3D_OK);
}

void dev_SetStreamSource(cpu::Cpu& c) {
    uint32_t n = arg(c, 1);
    if (n >= 16) return ret(c, D3DERR_INVALIDCALL);
    Stream& s = dev(c)->streams[n];
    s.vb = arg(c, 2);
    s.offset = arg(c, 3);
    s.stride = arg(c, 4);
    ret(c, D3D_OK);
}
void dev_GetStreamSource(cpu::Cpu& c) {
    uint32_t n = arg(c, 1);
    if (n >= 16) return ret(c, D3DERR_INVALIDCALL);
    Stream& s = dev(c)->streams[n];
    if (com::Object* o = com::from_guest(s.vb)) com::add_ref(o);
    mem::wr32(arg(c, 2), s.vb);
    mem::wr32(arg(c, 3), s.offset);
    mem::wr32(arg(c, 4), s.stride);
    ret(c, D3D_OK);
}
void dev_SetStreamSourceFreq(cpu::Cpu& c) {
    uint32_t n = arg(c, 1);
    if (n < 16) dev(c)->streams[n].freq = arg(c, 2);
    ret(c, D3D_OK);
}
void dev_SetIndices(cpu::Cpu& c) {
    dev(c)->indices = arg(c, 1);
    ret(c, D3D_OK);
}
void dev_GetIndices(cpu::Cpu& c) {
    uint32_t v = dev(c)->indices;
    if (com::Object* o = com::from_guest(v)) com::add_ref(o);
    mem::wr32(arg(c, 1), v);
    ret(c, D3D_OK);
}
void dev_CreateQuery(cpu::Cpu& c) {
    uint32_t type = arg(c, 1), out = arg(c, 2);
    if (type != 8 && type != 9) return ret(c, D3DERR_NOTAVAILABLE);  // EVENT, OCCLUSION only
    if (!out) return ret(c, D3D_OK);  // support check
    Query* q = new Query();
    q->dev = dev(c);
    q->qtype = type;
    com::wrap(q, query_vtable());
    write_ptr(out, q);
    ret(c, D3D_OK);
}

void dev_StretchRect(cpu::Cpu& c) {
    Device* d = dev(c);
    Surface* src = obj<Surface>(c, 1);
    Surface* dst = obj<Surface>(c, 3);
    if (!src || !dst || !src->gl_tex || !dst->gl_tex) {
        LOG_ONCE("StretchRect between non-render-target surfaces is not implemented");
        return ret(c, D3D_OK);
    }
    auto rect = [](uint32_t r, Surface* s, int32_t out[4]) {
        if (r) {
            for (int i = 0; i < 4; ++i) out[i] = (int32_t)mem::rd32(r + i * 4);
        } else {
            out[0] = out[1] = 0;
            out[2] = (int32_t)s->w;
            out[3] = (int32_t)s->h;
        }
    };
    int32_t sr[4], dr[4];
    rect(arg(c, 2), src, sr);
    rect(arg(c, 4), dst, dr);
    GLuint fbos[2];
    glGenFramebuffers(2, fbos);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src->gl_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->gl_tex, 0);
    // Flip Y: D3D rects are top-left based.
    glBlitFramebuffer(sr[0], (GLint)src->h - sr[3], sr[2], (GLint)src->h - sr[1], dr[0], (GLint)dst->h - dr[3], dr[2],
                      (GLint)dst->h - dr[1], GL_COLOR_BUFFER_BIT, arg(c, 5) == 1 ? GL_NEAREST : GL_LINEAR);
    glDeleteFramebuffers(2, fbos);
    d->targets_dirty = true;
    ret(c, D3D_OK);
}

void dev_SetGammaRamp(cpu::Cpu& c) { ret(c, 0); }
void dev_GetGammaRamp(cpu::Cpu& c) {
    uint32_t p = arg(c, 2);
    for (uint32_t ch = 0; ch < 3; ++ch)
        for (uint32_t i = 0; i < 256; ++i) mem::wr16(p + (ch * 256 + i) * 2, (uint16_t)(i * 257));
    ret(c, 0);
}

#define DEV_M(name, bytes, fn) {"IDirect3DDevice9::" name, bytes, fn, 0}
const com::Method kDeviceMethods[] = {
    DEV_M("QueryInterface", 12, com::QueryInterface),
    DEV_M("AddRef", 4, com::AddRef),
    DEV_M("Release", 4, com::Release),
    DEV_M("TestCooperativeLevel", 4, dev_TestCooperativeLevel),
    DEV_M("GetAvailableTextureMem", 4, dev_GetAvailableTextureMem),
    DEV_M("EvictManagedResources", 4, dev_EvictManagedResources),
    DEV_M("GetDirect3D", 8, dev_GetDirect3D),
    DEV_M("GetDeviceCaps", 8, dev_GetDeviceCaps),
    DEV_M("GetDisplayMode", 12, dev_GetDisplayMode),
    DEV_M("GetCreationParameters", 8, dev_GetCreationParameters),
    DEV_M("SetCursorProperties", 16, nullptr),
    DEV_M("SetCursorPosition", 16, nullptr),
    DEV_M("ShowCursor", 8, dev_ShowCursor),
    DEV_M("CreateAdditionalSwapChain", 12, nullptr),
    DEV_M("GetSwapChain", 12, dev_GetSwapChain),
    DEV_M("GetNumberOfSwapChains", 4, dev_GetNumberOfSwapChains),
    DEV_M("Reset", 8, dev_Reset),
    DEV_M("Present", 20, dev_Present),
    DEV_M("GetBackBuffer", 20, dev_GetBackBuffer),
    DEV_M("GetRasterStatus", 12, dev_GetRasterStatus),
    DEV_M("SetDialogBoxMode", 8, nullptr),
    DEV_M("SetGammaRamp", 16, dev_SetGammaRamp),
    DEV_M("GetGammaRamp", 12, dev_GetGammaRamp),
    DEV_M("CreateTexture", 36, dev_CreateTexture),
    {"IDirect3DDevice9::CreateVolumeTexture", 40, nullptr, D3DERR_NOTAVAILABLE},
    DEV_M("CreateCubeTexture", 32, dev_CreateCubeTexture),
    DEV_M("CreateVertexBuffer", 28, dev_CreateVertexBuffer),
    DEV_M("CreateIndexBuffer", 28, dev_CreateIndexBuffer),
    DEV_M("CreateRenderTarget", 36, dev_CreateRenderTarget),
    DEV_M("CreateDepthStencilSurface", 36, dev_CreateDepthStencilSurface),
    DEV_M("UpdateSurface", 20, nullptr),
    DEV_M("UpdateTexture", 12, nullptr),
    DEV_M("GetRenderTargetData", 12, nullptr),
    DEV_M("GetFrontBufferData", 12, nullptr),
    DEV_M("StretchRect", 24, dev_StretchRect),
    DEV_M("ColorFill", 16, nullptr),
    DEV_M("CreateOffscreenPlainSurface", 28, dev_CreateOffscreenPlainSurface),
    DEV_M("SetRenderTarget", 12, dev_SetRenderTarget),
    DEV_M("GetRenderTarget", 12, dev_GetRenderTarget),
    DEV_M("SetDepthStencilSurface", 8, dev_SetDepthStencilSurface),
    DEV_M("GetDepthStencilSurface", 8, dev_GetDepthStencilSurface),
    DEV_M("BeginScene", 4, dev_BeginScene),
    DEV_M("EndScene", 4, dev_EndScene),
    DEV_M("Clear", 28, dev_Clear),
    DEV_M("SetTransform", 12, dev_SetTransform),
    DEV_M("GetTransform", 12, dev_GetTransform),
    DEV_M("MultiplyTransform", 12, nullptr),
    DEV_M("SetViewport", 8, dev_SetViewport),
    DEV_M("GetViewport", 8, dev_GetViewport),
    DEV_M("SetMaterial", 8, dev_SetMaterial),
    DEV_M("GetMaterial", 8, dev_GetMaterial),
    DEV_M("SetLight", 12, dev_SetLight),
    DEV_M("GetLight", 12, nullptr),
    DEV_M("LightEnable", 12, dev_LightEnable),
    DEV_M("GetLightEnable", 12, nullptr),
    DEV_M("SetClipPlane", 12, dev_SetClipPlane),
    DEV_M("GetClipPlane", 12, nullptr),
    DEV_M("SetRenderState", 12, dev_SetRenderState),
    DEV_M("GetRenderState", 12, dev_GetRenderState),
    DEV_M("CreateStateBlock", 12, dev_CreateStateBlock),
    DEV_M("BeginStateBlock", 4, dev_BeginStateBlock),
    DEV_M("EndStateBlock", 8, dev_EndStateBlock),
    DEV_M("SetClipStatus", 8, nullptr),
    DEV_M("GetClipStatus", 8, nullptr),
    DEV_M("GetTexture", 12, dev_GetTexture),
    DEV_M("SetTexture", 12, dev_SetTexture),
    DEV_M("GetTextureStageState", 16, dev_GetTextureStageState),
    DEV_M("SetTextureStageState", 16, dev_SetTextureStageState),
    DEV_M("GetSamplerState", 16, dev_GetSamplerState),
    DEV_M("SetSamplerState", 16, dev_SetSamplerState),
    DEV_M("ValidateDevice", 8, dev_ValidateDevice),
    DEV_M("SetPaletteEntries", 12, nullptr),
    DEV_M("GetPaletteEntries", 12, nullptr),
    DEV_M("SetCurrentTexturePalette", 8, nullptr),
    DEV_M("GetCurrentTexturePalette", 8, nullptr),
    DEV_M("SetScissorRect", 8, dev_SetScissorRect),
    DEV_M("GetScissorRect", 8, dev_GetScissorRect),
    DEV_M("SetSoftwareVertexProcessing", 8, nullptr),
    DEV_M("GetSoftwareVertexProcessing", 4, dev_GetSoftwareVertexProcessing),
    DEV_M("SetNPatchMode", 8, nullptr),
    DEV_M("GetNPatchMode", 4, nullptr),
    DEV_M("DrawPrimitive", 16, dev_DrawPrimitive),
    DEV_M("DrawIndexedPrimitive", 28, dev_DrawIndexedPrimitive),
    DEV_M("DrawPrimitiveUP", 20, dev_DrawPrimitiveUP),
    DEV_M("DrawIndexedPrimitiveUP", 36, dev_DrawIndexedPrimitiveUP),
    DEV_M("ProcessVertices", 28, nullptr),
    DEV_M("CreateVertexDeclaration", 12, dev_CreateVertexDeclaration),
    DEV_M("SetVertexDeclaration", 8, dev_SetVertexDeclaration),
    DEV_M("GetVertexDeclaration", 8, dev_GetVertexDeclaration),
    DEV_M("SetFVF", 8, dev_SetFVF),
    DEV_M("GetFVF", 8, dev_GetFVF),
    DEV_M("CreateVertexShader", 12, dev_CreateVertexShader),
    DEV_M("SetVertexShader", 8, dev_SetVertexShader),
    DEV_M("GetVertexShader", 8, dev_GetVertexShader),
    DEV_M("SetVertexShaderConstantF", 16, (dev_ShaderConst<0, true>)),
    DEV_M("GetVertexShaderConstantF", 16, (dev_ShaderConst<0, false>)),
    DEV_M("SetVertexShaderConstantI", 16, (dev_ShaderConst<1, true>)),
    DEV_M("GetVertexShaderConstantI", 16, (dev_ShaderConst<1, false>)),
    DEV_M("SetVertexShaderConstantB", 16, (dev_ShaderConst<2, true>)),
    DEV_M("GetVertexShaderConstantB", 16, (dev_ShaderConst<2, false>)),
    DEV_M("SetStreamSource", 20, dev_SetStreamSource),
    DEV_M("GetStreamSource", 20, dev_GetStreamSource),
    DEV_M("SetStreamSourceFreq", 12, dev_SetStreamSourceFreq),
    DEV_M("GetStreamSourceFreq", 12, nullptr),
    DEV_M("SetIndices", 8, dev_SetIndices),
    DEV_M("GetIndices", 8, dev_GetIndices),
    DEV_M("CreatePixelShader", 12, dev_CreatePixelShader),
    DEV_M("SetPixelShader", 8, dev_SetPixelShader),
    DEV_M("GetPixelShader", 8, dev_GetPixelShader),
    DEV_M("SetPixelShaderConstantF", 16, (dev_ShaderConst<3, true>)),
    DEV_M("GetPixelShaderConstantF", 16, (dev_ShaderConst<3, false>)),
    DEV_M("SetPixelShaderConstantI", 16, (dev_ShaderConst<4, true>)),
    DEV_M("GetPixelShaderConstantI", 16, (dev_ShaderConst<4, false>)),
    DEV_M("SetPixelShaderConstantB", 16, (dev_ShaderConst<5, true>)),
    DEV_M("GetPixelShaderConstantB", 16, (dev_ShaderConst<5, false>)),
    DEV_M("DrawRectPatch", 16, nullptr),
    DEV_M("DrawTriPatch", 16, nullptr),
    DEV_M("DeletePatch", 8, nullptr),
    DEV_M("CreateQuery", 12, dev_CreateQuery),
};
static_assert(sizeof kDeviceMethods / sizeof kDeviceMethods[0] == 119, "IDirect3DDevice9 has 119 methods");
static_assert(sizeof kD3D9Methods / sizeof kD3D9Methods[0] == 17, "IDirect3D9 has 17 methods");

#define VTABLE_GETTER(fn, table)                                         \
    uint32_t fn() {                                                      \
        static uint32_t vt = 0;                                          \
        std::lock_guard<std::mutex> lk(g_vt_mu);                         \
        if (!vt) vt = com::make_vtable(table, sizeof table / sizeof table[0]); \
        return vt;                                                       \
    }
VTABLE_GETTER(surface_vtable, kSurfaceMethods)
VTABLE_GETTER(texture_vtable, kTextureMethods)
VTABLE_GETTER(cube_vtable, kCubeMethods)
VTABLE_GETTER(vb_vtable, kVBMethods)
VTABLE_GETTER(ib_vtable, kIBMethods)
VTABLE_GETTER(vs_vtable, kVSMethods)
VTABLE_GETTER(ps_vtable, kPSMethods)
VTABLE_GETTER(vdecl_vtable, kVDeclMethods)
VTABLE_GETTER(query_vtable, kQueryMethods)
VTABLE_GETTER(stateblock_vtable, kStateBlockMethods)
VTABLE_GETTER(swapchain_vtable, kSwapChainMethods)
VTABLE_GETTER(device_vtable, kDeviceMethods)
VTABLE_GETTER(d3d9_vtable, kD3D9Methods)
uint32_t shader_vtable(bool pixel) { return pixel ? ps_vtable() : vs_vtable(); }

}  // namespace

// D3DX (statically linked into the game) probes this on every call.
HLE_EXPORT(d3d9, DebugSetMute, 0) { ret(c, 0); }

HLE_EXPORT(d3d9, Direct3DCreate9, 4) {
    LOGI("Direct3DCreate9(SDK %u)", arg(c, 0));
    Direct3D* d = new Direct3D();
    ret(c, com::wrap(d, d3d9_vtable()));
}
