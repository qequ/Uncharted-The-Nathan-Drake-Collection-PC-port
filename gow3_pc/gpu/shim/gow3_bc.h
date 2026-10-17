// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// gow3: BC1 and BC4 blocks for the games' HUD button pictures (gow3_glyphs.h): decoding the
// games' own pictures as the GPU reads them (1D-thin tiled), and encoding the replacements.

#include <cstdint>
#include <vector>

namespace Gow3Bc {

enum class Format : uint8_t { Bc1, Bc4 };

/// Bytes of one 4x4 block (both formats use 8).
constexpr uint32_t BlockBytes = 8;

/// 16 RGBA8 texels, rows of 4, from a BC1 block (4-colour or 3-colour + transparent mode).
void DecodeBc1(const uint8_t* block, uint8_t* rgba);
/// 16 values from a BC4 (unsigned) block.
void DecodeBc4(const uint8_t* block, uint8_t* values);
/// An opaque 4-colour BC1 block from 16 RGBA8 texels (alpha is ignored).
void EncodeBc1(const uint8_t* rgba, uint8_t* block);
/// A BC4 block from 16 values.
void EncodeBc4(const uint8_t* values, uint8_t* block);

/// Index of block (x, y) in a 1D-thin tiled surface `pitch` blocks wide: tiles of 8x8 blocks in
/// rows, the blocks of a tile in Morton order (x0 y0 x1 y1 x2 y2).
uint32_t Thin1dBlock(uint32_t x, uint32_t y, uint32_t pitch);
/// Bytes of a 1D-thin tiled level of `width` x `height` texels (whole tiles).
uint32_t Thin1dLevelBytes(uint32_t width, uint32_t height);

/// RGBA8 texels (rows top to bottom as stored) of a 1D-thin tiled level. BC4 values go to R, G
/// and B, with alpha 255.
std::vector<uint8_t> DecodeThin1d(const uint8_t* tiled, uint32_t width, uint32_t height, Format format);
/// Blocks in rows (linear, as Vulkan uploads take them) for RGBA8 texels of any size; a partial
/// block repeats its last row and column. BC4 takes the R channel.
std::vector<uint8_t> EncodeLinear(const uint8_t* rgba, uint32_t width, uint32_t height, Format format);

} // namespace Gow3Bc
