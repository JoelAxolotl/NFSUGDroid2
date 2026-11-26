#pragma once
// Direct3D 9 fixed-function pipeline emulated with generated GLSL.
//
// Vertex attribute locations used by the generated vertex shader:
//   0 position, 1 normal, 2 color0, 3 color1, 4..11 texcoord0..7
#include <cstdint>
#include <string>

namespace d3d {

constexpr int kFfpLocPos = 0, kFfpLocNormal = 1, kFfpLocColor0 = 2, kFfpLocColor1 = 3, kFfpLocTex0 = 4;

#pragma pack(push, 1)
struct FfpVsKey {
    uint8_t pretransformed, has_normal, has_color0, has_color1;
    uint8_t tex_dims[8];  // components available per texcoord set (0 = absent)
    uint8_t lighting, specular, local_viewer, color_vertex;
    uint8_t diffuse_src, ambient_src, specular_src, emissive_src;
    uint8_t num_lights;
    uint8_t light_type[8];
    uint8_t tc_index[8], tc_gen[8], tc_count[8], tc_projected[8];
    uint8_t fog_mode;  // 0 none, 1 exp, 2 exp2, 3 linear (vertex fog)
    uint8_t range_fog;
    uint8_t pos_w;     // position has a w component (D3DFVF_XYZW / FLOAT4 position)
};

struct FfpStage {
    uint8_t cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, result, dim, projected, proj_comp;
};

struct FfpPsKey {
    uint8_t stages;
    uint8_t specular;
    FfpStage st[8];
};
#pragma pack(pop)

std::string ffp_vertex_glsl(const FfpVsKey& k);
std::string ffp_pixel_glsl(const FfpPsKey& k);

}  // namespace d3d
