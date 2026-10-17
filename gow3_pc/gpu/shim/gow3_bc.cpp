// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: BC1 and BC4 blocks for the HUD button pictures (gow3_bc.h).

#include "gow3_bc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace Gow3Bc {

namespace {

void Expand565(uint16_t c, uint8_t* rgb) {
    const uint32_t r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    rgb[0] = uint8_t(r << 3 | r >> 2);
    rgb[1] = uint8_t(g << 2 | g >> 4);
    rgb[2] = uint8_t(b << 3 | b >> 2);
}

uint16_t Quantize565(const float* rgb) {
    const auto q = [](float v, int bits) {
        const int max = (1 << bits) - 1;
        return std::clamp(int(std::lround(std::clamp(v, 0.0f, 255.0f) * max / 255.0f)), 0, max);
    };
    return uint16_t(q(rgb[0], 5) << 11 | q(rgb[1], 6) << 5 | q(rgb[2], 5));
}

// The four colours of a 4-colour block, as DecodeBc1 makes them.
void Palette4(uint16_t c0, uint16_t c1, uint8_t (*palette)[3]) {
    Expand565(c0, palette[0]);
    Expand565(c1, palette[1]);
    for (int k = 0; k < 3; ++k) {
        palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k] + 1) / 3);
        palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k] + 1) / 3);
    }
}

// The nearest palette colour of each texel; the total squared error.
uint32_t Assign(const float (*px)[3], uint16_t c0, uint16_t c1, uint8_t* indices) {
    uint8_t palette[4][3];
    Palette4(c0, c1, palette);
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) {
        uint32_t best = std::numeric_limits<uint32_t>::max();
        for (int p = 0; p < 4; ++p) {
            uint32_t e = 0;
            for (int k = 0; k < 3; ++k) {
                const float d = px[i][k] - palette[p][k];
                e += uint32_t(d * d);
            }
            if (e < best) {
                best = e;
                indices[i] = uint8_t(p);
            }
        }
        total += best;
    }
    return total;
}

// Endpoints that fit the texels best for these indices (least squares), or false when the
// indices leave them undetermined.
bool Refit(const float (*px)[3], const uint8_t* indices, float* e0, float* e1) {
    static constexpr float weight[4] = {1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
    float aa = 0, ab = 0, bb = 0, ax[3] = {}, bx[3] = {};
    for (int i = 0; i < 16; ++i) {
        const float a = weight[indices[i]], b = 1.0f - a;
        aa += a * a;
        ab += a * b;
        bb += b * b;
        for (int k = 0; k < 3; ++k) {
            ax[k] += a * px[i][k];
            bx[k] += b * px[i][k];
        }
    }
    const float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-6f) {
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        e0[k] = (ax[k] * bb - bx[k] * ab) / det;
        e1[k] = (bx[k] * aa - ax[k] * ab) / det;
    }
    return true;
}

void Store(uint8_t* block, uint16_t c0, uint16_t c1, const uint8_t* indices) {
    uint8_t remap[4] = {0, 1, 2, 3};
    if (c0 < c1) { // 4-colour mode needs c0 > c1: swap the ends and their indices
        std::swap(c0, c1);
        remap[0] = 1, remap[1] = 0, remap[2] = 3, remap[3] = 2;
    }
    uint32_t bits = 0;
    if (c0 != c1) {
        for (int i = 0; i < 16; ++i) bits |= uint32_t(remap[indices[i]]) << (2 * i);
    }
    block[0] = uint8_t(c0), block[1] = uint8_t(c0 >> 8);
    block[2] = uint8_t(c1), block[3] = uint8_t(c1 >> 8);
    for (int k = 0; k < 4; ++k) block[4 + k] = uint8_t(bits >> (8 * k));
}

} // namespace

void DecodeBc1(const uint8_t* block, uint8_t* rgba) {
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    uint8_t palette[4][4];
    Expand565(c0, palette[0]);
    Expand565(c1, palette[1]);
    palette[0][3] = palette[1][3] = 255;
    if (c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k] + 1) / 3);
            palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k] + 1) / 3);
        }
        palette[2][3] = palette[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            palette[2][k] = uint8_t((palette[0][k] + palette[1][k] + 1) / 2);
            palette[3][k] = 0;
        }
        palette[2][3] = 255;
        palette[3][3] = 0;
    }
    const uint32_t bits = uint32_t(block[4]) | uint32_t(block[5]) << 8 | uint32_t(block[6]) << 16 |
                          uint32_t(block[7]) << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(rgba + i * 4, palette[bits >> (2 * i) & 3], 4);
}

void DecodeBc4(const uint8_t* block, uint8_t* values) {
    const int r0 = block[0], r1 = block[1];
    uint8_t palette[8] = {uint8_t(r0), uint8_t(r1)};
    if (r0 > r1) {
        for (int i = 1; i <= 6; ++i) palette[i + 1] = uint8_t(((7 - i) * r0 + i * r1 + 3) / 7);
    } else {
        for (int i = 1; i <= 4; ++i) palette[i + 1] = uint8_t(((5 - i) * r0 + i * r1 + 2) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(block[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i) values[i] = palette[bits >> (3 * i) & 7];
}

void EncodeBc1(const uint8_t* rgba, uint8_t* block) {
    float px[16][3], mean[3] = {};
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 3; ++k) {
            px[i][k] = rgba[i * 4 + k];
            mean[k] += px[i][k] / 16.0f;
        }
    }
    // The principal axis of the colours (power iteration on their covariance).
    float cov[6] = {}; // rr rg rb gg gb bb
    for (const auto& p : px) {
        const float r = p[0] - mean[0], g = p[1] - mean[1], b = p[2] - mean[2];
        cov[0] += r * r, cov[1] += r * g, cov[2] += r * b, cov[3] += g * g, cov[4] += g * b, cov[5] += b * b;
    }
    float axis[3] = {1.0f, 1.0f, 1.0f};
    for (int iteration = 0; iteration < 8; ++iteration) {
        const float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        const float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        const float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-6f) {
            break;
        }
        axis[0] = x / length, axis[1] = y / length, axis[2] = z / length;
    }
    float lo = std::numeric_limits<float>::max(), hi = -lo;
    for (const auto& p : px) {
        const float t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    const float inset = (hi - lo) / 16.0f;
    float e0[3], e1[3];
    for (int k = 0; k < 3; ++k) {
        e0[k] = mean[k] + axis[k] * (hi - inset);
        e1[k] = mean[k] + axis[k] * (lo + inset);
    }
    uint16_t c0 = Quantize565(e0), c1 = Quantize565(e1);
    uint8_t indices[16];
    uint32_t error = Assign(px, c0, c1, indices);
    for (int pass = 0; pass < 2 && error > 0; ++pass) {
        float f0[3], f1[3];
        if (!Refit(px, indices, f0, f1)) {
            break;
        }
        const uint16_t d0 = Quantize565(f0), d1 = Quantize565(f1);
        uint8_t trial[16];
        const uint32_t e = Assign(px, d0, d1, trial);
        if (e >= error) {
            break;
        }
        error = e;
        c0 = d0, c1 = d1;
        std::memcpy(indices, trial, sizeof(indices));
    }
    Store(block, c0, c1, indices);
}

void EncodeBc4(const uint8_t* values, uint8_t* block) {
    const auto [lo, hi] = std::minmax_element(values, values + 16);
    block[0] = *hi;
    block[1] = *lo;
    uint64_t bits = 0;
    if (*hi != *lo) { // 8-value mode: r0 > r1
        const int r0 = *hi, r1 = *lo;
        uint8_t palette[8] = {uint8_t(r0), uint8_t(r1)};
        for (int i = 1; i <= 6; ++i) palette[i + 1] = uint8_t(((7 - i) * r0 + i * r1 + 3) / 7);
        for (int i = 0; i < 16; ++i) {
            int best = 0, best_error = 256;
            for (int p = 0; p < 8; ++p) {
                const int e = std::abs(int(values[i]) - palette[p]);
                if (e < best_error) {
                    best_error = e;
                    best = p;
                }
            }
            bits |= uint64_t(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k) block[2 + k] = uint8_t(bits >> (8 * k));
}

uint32_t Thin1dBlock(uint32_t x, uint32_t y, uint32_t pitch) {
    const uint32_t tiles = (pitch + 7) / 8;
    const uint32_t tile = y / 8 * tiles + x / 8;
    const uint32_t inside = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
    return tile * 64 + inside;
}

uint32_t Thin1dLevelBytes(uint32_t width, uint32_t height) {
    const uint32_t blocks_w = (width + 3) / 4, blocks_h = (height + 3) / 4;
    return (blocks_w + 7) / 8 * ((blocks_h + 7) / 8) * 64 * BlockBytes;
}

std::vector<uint8_t> DecodeThin1d(const uint8_t* tiled, uint32_t width, uint32_t height, Format format) {
    std::vector<uint8_t> out(size_t(width) * height * 4);
    const uint32_t blocks_w = (width + 3) / 4, blocks_h = (height + 3) / 4;
    for (uint32_t by = 0; by < blocks_h; ++by) {
        for (uint32_t bx = 0; bx < blocks_w; ++bx) {
            const uint8_t* block = tiled + size_t(Thin1dBlock(bx, by, blocks_w)) * BlockBytes;
            uint8_t texels[64];
            if (format == Format::Bc1) {
                DecodeBc1(block, texels);
            } else {
                uint8_t values[16];
                DecodeBc4(block, values);
                for (int i = 0; i < 16; ++i) {
                    std::memset(texels + i * 4, values[i], 3);
                    texels[i * 4 + 3] = 255;
                }
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    std::memcpy(&out[(size_t(by * 4 + y) * width + bx * 4 + x) * 4], texels + (y * 4 + x) * 4, 4);
                }
            }
        }
    }
    return out;
}

std::vector<uint8_t> EncodeLinear(const uint8_t* rgba, uint32_t width, uint32_t height, Format format) {
    const uint32_t blocks_w = (width + 3) / 4, blocks_h = (height + 3) / 4;
    std::vector<uint8_t> out(size_t(blocks_w) * blocks_h * BlockBytes);
    for (uint32_t by = 0; by < blocks_h; ++by) {
        for (uint32_t bx = 0; bx < blocks_w; ++bx) {
            uint8_t texels[64], values[16];
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                    std::memcpy(texels + (y * 4 + x) * 4, rgba + (size_t(sy) * width + sx) * 4, 4);
                    values[y * 4 + x] = texels[(y * 4 + x) * 4];
                }
            }
            uint8_t* block = &out[(size_t(by) * blocks_w + bx) * BlockBytes];
            if (format == Format::Bc1) {
                EncodeBc1(texels, block);
            } else {
                EncodeBc4(values, block);
            }
        }
    }
    return out;
}

} // namespace Gow3Bc
