// XMF texture loader. Game art is stored in the proprietary XMF container as Xbox
// D3D resource headers + DXT (S3TC) compressed pixel data. This decodes the first
// texture in a file to 32-bit RGBA that the runtime can upload via Device::CreateTexture.
//
// Format (reverse-engineered from the game's XMF loader, default.xbe FUN @0x60160, and
// the Xbox D3DPixelContainer layout):
//   * a resource header has Common with the texture-type bit (0x00040000) and a packed
//     Format dword at +0xC: format=bits8-15, mips=16-19, USize(log2 w)=20-23, VSize=24-27.
//   * DXT blocks are stored linearly at the end of the file (largest mip first).
//   * Formats seen: DXT1 (0x0C), DXT23 (0x0E), DXT45 (0x0F).
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace tj::assets {

struct DecodedTexture {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba;   // width*height, 0xAARRGGBB
    bool ok = false;
    bool hasAlpha = false;        // any pixel with alpha < 250
    std::string format;           // e.g. "DXT1"
};

// Decode the first texture found in an XMF/XBD file. Handles single-texture files
// (e.g. the front-end backgrounds); multi-texture files decode their first texture.
DecodedTexture LoadFirstTexture(const std::wstring& path);

// Decode a linear DXT mip (fmtCode 0x0C=DXT1, 0x0E=DXT23, 0x0F=DXT45) of WxH from
// `pixels` (avail bytes). Used by the mesh loader, which knows exact pixel offsets
// from the container's resource headers.
DecodedTexture DecodeDxtPixels(const uint8_t* pixels, size_t avail, int fmtCode, int W, int H);

// Decode a swizzled (Morton-order) 8-bit paletted Xbox texture (D3DFMT_P8, 0x0B).
// `palette` is 256 D3DCOLOR (ARGB) entries; null -> grayscale.
DecodedTexture DecodeP8Swizzled(const uint8_t* pixels, size_t avail,
                                const uint32_t* palette, int W, int H);

// Decode any supported Xbox texture format (DXT, P8, swizzled/linear 8/16/32-bit)
// given the packed-format code from the resource header. Returns !ok if the
// format is unknown. `palette` is only used for P8.
DecodedTexture DecodeXboxTexture(int fmtCode, const uint8_t* pixels, size_t avail,
                                 const uint32_t* palette, int W, int H);

// Fill in DecodedTexture::hasAlpha by scanning the decoded pixels.
void ComputeAlphaFlag(DecodedTexture& t);

} // namespace tj::assets
