#pragma once
// Direct3D 9 shader bytecode (vs_1_1..vs_2_x, ps_1_1..ps_2_x) -> GLSL ES 3.20.
//
// Conventions shared with the draw path (d3d9_draw.cpp):
//  * vertex inputs: `layout(location = N) in vec4 v_N` for input register vN;
//    uniform `u_bgra` (bit N) swaps .zyxw for D3DCOLOR attributes.
//  * varyings: D3D_D0, D3D_D1, D3D_T0..D3D_T7 (vec4), D3D_FOG (float).
//  * constants: vc[256]/vi[16]/vb[16] (VS), pc[32]/pi[16]/pb[16] (PS).
//  * samplers: s0..s15; texture unit N for sampler N.
//  * VS epilogue applies `u_fix` (half-pixel offset), flips Y (frames are
//    stored top-down) and maps D3D depth [0,1] to GL [-1,1].
//  * PS epilogue applies D3D fog (`u_fog`, `u_fogcolor`) and alpha test (`u_alpha`).
#include <cstdint>
#include <string>
#include <vector>

namespace d3d {

struct ShaderInput {
    uint8_t usage, usage_index, reg;
};

struct ShaderInfo {
    bool ok = false;
    bool pixel = false;
    uint32_t major = 0, minor = 0;
    std::string glsl;
    std::string error;
    std::vector<ShaderInput> inputs;  // VS input declarations
    uint8_t sampler_dim[16] = {};     // 0 unused, 2 = 2D, 3 = cube, 4 = volume
    uint32_t const_count = 0;         // float constants to upload (from register 0)
    bool writes_point_size = false;
    uint32_t gl_shader = 0;           // compiled GL shader object (owned by the draw cache)
};

// `ps1_dims` supplies sampler dimensions for ps_1_x shaders, which do not
// declare them (taken from the textures bound at draw time). May be null.
ShaderInfo translate_shader(const std::vector<uint32_t>& tokens, const uint8_t* ps1_dims);

// Shared GLSL snippets.
const char* glsl_vs_epilogue();   // void d3d_emit_position(vec4 p)
const char* glsl_ps_common();     // fog + alpha-test helpers: vec4 d3d_finish(vec4 c)

}  // namespace d3d
