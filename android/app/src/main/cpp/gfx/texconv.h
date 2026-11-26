#pragma once
// D3D9 texture formats -> GLES upload formats.
#include <GLES3/gl32.h>

#include <cstdint>
#include <vector>

namespace texconv {

struct GLFormat {
    GLenum internal = GL_RGBA8, format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    bool direct = false;  // D3D bytes can be uploaded as-is
    uint32_t out_bpp = 4;
};

// Returns false for unsupported formats.
bool describe(uint32_t d3d_format, GLFormat& out);

// Converts one w x h level (D3D layout, tightly pitched) into `out` in the
// layout described by describe(). For direct formats this is a plain copy.
void convert(uint32_t d3d_format, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

}  // namespace texconv
