// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// gow3: the HUD button pictures in a Naughty Dog pak (Uncharted 1-3 on PS4), found by name so
// that the texture cache can recognise them when the game uploads them (gow3_glyphs.h).
//
// A pak (magic 0xA79) keeps its records first and the GPU data last: the dword at 0x20 is the
// size of the GPU part at the end of the file. A texture is a VRAM_DESC record: an item header
// ending in the marker 0x12345, then dwords [4] the data's offset in the GPU part, [6] its size,
// [9] its DXGI format (71 BC1, 72 BC1 sRGB, 80 BC4), [11] mip levels, [12] width and
// [13] height; after them come the cooked picture's path and "VRAM_DESC". The data is stored as
// the GPU reads it (1D-thin tiled), so a hash of it is what the texture cache sees.

#include <cstdint>
#include <string>
#include <vector>

namespace Gow3Pak {

struct Texture {
    std::string name;  ///< the picture's file name, lower case, without ".tga"
    uint32_t width = 0, height = 0, levels = 0;
    uint32_t dxgi = 0; ///< 71 BC1, 72 BC1 sRGB, 80 BC4
    uint32_t size = 0; ///< bytes of all levels
    uint64_t offset = 0; ///< of the data in the file
    uint64_t hash = 0;   ///< XXH3 of level 0 as stored
};

/// Bytes of level 0 of a BC1 or BC4 texture (1D-thin tiled: whole 8x8-block tiles).
uint32_t Level0Bytes(uint32_t width, uint32_t height);
/// The textures under ".../hud/buttons/" of the pak at `path`, each once. Empty, with `error`
/// set, when the file cannot be read or is not a pak.
std::vector<Texture> ReadButtons(const std::string& path, std::string* error = nullptr);
/// The stored bytes of a texture's level 0.
bool ReadLevel0(const std::string& path, const Texture& texture, std::vector<uint8_t>& out);

} // namespace Gow3Pak
