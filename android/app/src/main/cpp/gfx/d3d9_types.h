#pragma once
// Direct3D 9 constants and format helpers.
#include <cstdint>

namespace d3d {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) |
           ((uint32_t)(uint8_t)d << 24);
}

enum Format : uint32_t {
    FMT_UNKNOWN = 0,
    FMT_R8G8B8 = 20,
    FMT_A8R8G8B8 = 21,
    FMT_X8R8G8B8 = 22,
    FMT_R5G6B5 = 23,
    FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25,
    FMT_A4R4G4B4 = 26,
    FMT_A8 = 28,
    FMT_A8B8G8R8 = 32,
    FMT_X8B8G8R8 = 33,
    FMT_G16R16 = 34,
    FMT_A2R10G10B10 = 35,
    FMT_L8 = 50,
    FMT_A8L8 = 51,
    FMT_V8U8 = 60,
    FMT_Q8W8V8U8 = 63,
    FMT_D16_LOCKABLE = 70,
    FMT_D32 = 71,
    FMT_D24S8 = 75,
    FMT_D24X8 = 77,
    FMT_D16 = 80,
    FMT_INDEX16 = 101,
    FMT_INDEX32 = 102,
    FMT_A16B16G16R16F = 113,
    FMT_R32F = 114,
    FMT_DXT1 = fourcc('D', 'X', 'T', '1'),
    FMT_DXT2 = fourcc('D', 'X', 'T', '2'),
    FMT_DXT3 = fourcc('D', 'X', 'T', '3'),
    FMT_DXT4 = fourcc('D', 'X', 'T', '4'),
    FMT_DXT5 = fourcc('D', 'X', 'T', '5'),
};

enum Usage : uint32_t {
    USAGE_RENDERTARGET = 0x1,
    USAGE_DEPTHSTENCIL = 0x2,
    USAGE_WRITEONLY = 0x8,
    USAGE_DYNAMIC = 0x200,
    USAGE_AUTOGENMIPMAP = 0x400,
};

enum ResourceType : uint32_t {
    RTYPE_SURFACE = 1,
    RTYPE_VOLUME = 2,
    RTYPE_TEXTURE = 3,
    RTYPE_VOLUMETEXTURE = 4,
    RTYPE_CUBETEXTURE = 5,
    RTYPE_VERTEXBUFFER = 6,
    RTYPE_INDEXBUFFER = 7,
};

constexpr uint32_t D3D_OK = 0;
constexpr uint32_t D3DERR_NOTAVAILABLE = 0x8876086A;
constexpr uint32_t D3DERR_INVALIDCALL = 0x8876086C;
constexpr uint32_t D3DERR_OUTOFVIDEOMEMORY = 0x8876017C;
constexpr uint32_t D3DERR_DEVICELOST = 0x88760868;
constexpr uint32_t D3DERR_DEVICENOTRESET = 0x88760869;

inline bool is_dxt(uint32_t f) {
    return f == FMT_DXT1 || f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5;
}
inline bool is_depth(uint32_t f) {
    return f == FMT_D16 || f == FMT_D16_LOCKABLE || f == FMT_D24S8 || f == FMT_D24X8 || f == FMT_D32;
}

inline uint32_t bytes_per_pixel(uint32_t f) {
    switch (f) {
        case FMT_A8:
        case FMT_L8: return 1;
        case FMT_R5G6B5:
        case FMT_X1R5G5B5:
        case FMT_A1R5G5B5:
        case FMT_A4R4G4B4:
        case FMT_A8L8:
        case FMT_V8U8:
        case FMT_D16:
        case FMT_D16_LOCKABLE: return 2;
        case FMT_R8G8B8: return 3;
        case FMT_A16B16G16R16F: return 8;
        default: return 4;
    }
}

// Row pitch in bytes and number of rows (block rows for DXT) of one level.
inline uint32_t level_pitch(uint32_t f, uint32_t w) {
    if (is_dxt(f)) return ((w + 3) / 4) * (f == FMT_DXT1 ? 8 : 16);
    return w * bytes_per_pixel(f);
}
inline uint32_t level_rows(uint32_t f, uint32_t h) { return is_dxt(f) ? (h + 3) / 4 : h; }
inline uint32_t level_size(uint32_t f, uint32_t w, uint32_t h) { return level_pitch(f, w) * level_rows(f, h); }

}  // namespace d3d
