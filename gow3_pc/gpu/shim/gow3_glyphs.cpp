// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: button icons that follow the player's keys and gamepad (gow3_glyphs.h).

#include "gow3_glyphs.h"
#include "../gow3gpu.h"

#include <SDL3/SDL.h>
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

// Text on the icons: stb_truetype (with Dear ImGui), its own static copy.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#pragma clang diagnostic pop

// gow3_overlay.cpp: DejaVu Sans, the text font when Windows' bold fonts are missing.
extern "C" const unsigned char gow3_font_ttf[];
extern "C" const unsigned char gow3_font_ttf_end[];

namespace Gow3Glyphs {

namespace {

constexpr int AtlasSize = 512;
constexpr uint32_t AtlasBytes = AtlasSize * AtlasSize * 4;

// The icon font's textures as the games upload them: XXH3 of the 1D-tiled RGBA8 data. The
// content is symbol3_00.tga from each game's data.psarc, top row first. Uncharted 3 keeps the
// alpha apart: its colour texture has alpha 0, and symbol_alpha.tga is a second texture (grey,
// alpha 0). Another game version with other files needs its hashes here: tile the TGA data as
// Detile reads it and hash it.
struct Known {
    uint64_t hash;
    Role role;
    const char* game;
};
constexpr Known known[] = {
    {0xad5ada34fd34c76bull, RoleRgba, "Uncharted: Drake's Fortune"},
    {0x97779d0906f16551ull, RoleRgba, "Uncharted 2"},
    {0x8d03f2ff457f032dull, RoleColor, "Uncharted 3"},
    {0xae0c06306763d424ull, RoleAlpha, "Uncharted 3"},
};

// 1D-thin tiling of 32-bit texels: 8x8 tiles in rows, the texels of a tile in Morton order
// (x0 y0 x1 y1 x2 y2).
void Detile(const uint8_t* tiled, uint8_t* linear) {
    constexpr int tiles = AtlasSize / 8;
    for (int tile = 0; tile < tiles * tiles; ++tile) {
        const int tx = tile % tiles * 8, ty = tile / tiles * 8;
        for (int i = 0; i < 64; ++i) {
            const int x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
            const int y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
            std::memcpy(linear + ((ty + y) * AtlasSize + tx + x) * 4, tiled + tile * 256 + i * 4, 4);
        }
    }
}

// --- Drawing -------------------------------------------------------------------------------

struct Rgb {
    float r, g, b;
};
Rgb Mix(Rgb a, Rgb b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
constexpr Rgb Byte(int r, int g, int b) {
    return {r / 255.0f, g / 255.0f, b / 255.0f};
}
// The games' icons: dark buttons with a beige edge and cream lettering, orange for a
// highlighted direction.
constexpr Rgb DarkTop = Byte(78, 72, 65), DarkBottom = Byte(22, 20, 18);
constexpr Rgb FaceTop = Byte(98, 91, 82), FaceBottom = Byte(46, 42, 38);
constexpr Rgb Rim = Byte(206, 188, 158), Ink = Byte(247, 241, 228), Orange = Byte(240, 138, 40);
constexpr Rgb OrangeTop = Byte(250, 170, 80), OrangeBottom = Byte(196, 98, 24);
constexpr Rgb Black = Byte(0, 0, 0);

// A cell's picture: premultiplied RGBA, one float per channel.
struct Canvas {
    int w, h;
    std::vector<float> px; // r g b a
    Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height * 4) {}
    void Over(int x, int y, Rgb c, float a) {
        float* p = &px[(size_t(y) * w + x) * 4];
        const float keep = 1.0f - a;
        p[0] = c.r * a + p[0] * keep;
        p[1] = c.g * a + p[1] * keep;
        p[2] = c.b * a + p[2] * keep;
        p[3] = a + p[3] * keep;
    }
};

// Signed distances in pixels (negative inside).
float Box(float x, float y, float cx, float cy, float hw, float hh, float r_top, float r_bottom) {
    const float r = std::min({y < cy ? r_top : r_bottom, hw, hh});
    const float qx = std::fabs(x - cx) - hw + r, qy = std::fabs(y - cy) - hh + r;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
float Box(float x, float y, float cx, float cy, float hw, float hh, float r) {
    return Box(x, y, cx, cy, hw, hh, r, r);
}
float Circle(float x, float y, float cx, float cy, float r) {
    return std::hypot(x - cx, y - cy) - r;
}
// A triangle, its corners clockwise on screen (y down).
float Triangle(float x, float y, const float (&p)[3][2]) {
    float d = -1e9f;
    for (int i = 0; i < 3; ++i) {
        const float* a = p[i];
        const float* b = p[(i + 1) % 3];
        const float ex = b[0] - a[0], ey = b[1] - a[1], len = std::hypot(ex, ey);
        d = std::max(d, ((x - a[0]) * ey - (y - a[1]) * ex) / len);
    }
    return d;
}
// A triangle pointing in direction (dx, dy) with its tip at distance `size` from the centre.
float Arrow(float x, float y, float cx, float cy, float dx, float dy, float size) {
    const float px = -dy, py = dx;
    float p[3][2] = {{cx + dx * size, cy + dy * size},
                     {cx - dx * size * 0.6f - px * size * 0.9f, cy - dy * size * 0.6f - py * size * 0.9f},
                     {cx - dx * size * 0.6f + px * size * 0.9f, cy - dy * size * 0.6f + py * size * 0.9f}};
    if ((p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]) < 0) {
        std::swap(p[1], p[2]);
    }
    return Triangle(x, y, p);
}

template <class Sdf, class Paint>
void Fill(Canvas& c, Sdf sdf, Paint paint, float alpha = 1.0f) {
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float cover = std::clamp(0.5f - sdf(fx, fy), 0.0f, 1.0f);
            if (cover > 0.0f) {
                c.Over(x, y, paint(fx, fy), cover * alpha);
            }
        }
    }
}
template <class Sdf>
void Fill(Canvas& c, Sdf sdf, Rgb color, float alpha = 1.0f) {
    Fill(c, sdf, [color](float, float) { return color; }, alpha);
}
auto Vertical(Rgb top, Rgb bottom, float y0, float y1) {
    return [=](float, float y) { return Mix(top, bottom, std::clamp((y - y0) / (y1 - y0), 0.0f, 1.0f)); };
}
// A band `width` pixels wide just inside the shape's edge.
template <class Sdf>
auto Edge(Sdf sdf, float width) {
    return [=](float x, float y) { return std::fabs(sdf(x, y) + width * 0.5f) - width * 0.5f; };
}

void Blur(std::vector<float>& m, int w, int h, int radius) {
    std::vector<float> t(m.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float s = 0;
                for (int k = -radius; k <= radius; ++k) s += m[y * w + std::clamp(x + k, 0, w - 1)];
                t[y * w + x] = s / (2 * radius + 1);
            }
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float s = 0;
                for (int k = -radius; k <= radius; ++k) s += t[std::clamp(y + k, 0, h - 1) * w + x];
                m[y * w + x] = s / (2 * radius + 1);
            }
        }
    }
}
// A soft halo around a shape (drawn first, under it).
template <class Sdf>
void Glow(Canvas& c, Sdf sdf, Rgb color, float alpha, float spread) {
    std::vector<float> m(size_t(c.w) * c.h);
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) m[y * c.w + x] = std::clamp(0.5f - sdf(x + 0.5f, y + 0.5f), 0.0f, 1.0f);
    }
    Blur(m, c.w, c.h, std::max(1, int(spread / 1.7f + 0.5f)));
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            if (m[y * c.w + x] > 0.0f) c.Over(x, y, color, std::min(1.0f, m[y * c.w + x] * alpha));
        }
    }
}

// --- Text ----------------------------------------------------------------------------------

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false, bold = false;
};
// Windows' bold sans fonts; DejaVu Sans (embedded, made bolder) otherwise.
const Font& TextFont() {
    static const Font font = [] {
        Font f;
#ifdef _WIN32
        const char* windir = std::getenv("WINDIR");
        for (const char* name : {"segoeuib.ttf", "arialbd.ttf"}) {
            std::ifstream file(std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\" + name, std::ios::binary);
            f.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (!f.data.empty() && stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
                f.ok = f.bold = true;
                return f;
            }
        }
#endif
        f.data.assign(gow3_font_ttf, gow3_font_ttf_end);
        f.ok = stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0)) != 0;
        return f;
    }();
    return font;
}

std::vector<int> Codepoints(const std::string& text) {
    std::vector<int> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        const int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        int cp = n == 1 ? c : c & (0x3f >> (n - 1));
        for (int k = 1; k < n && i + k < text.size(); ++k) cp = cp << 6 | (text[i + k] & 0x3f);
        out.push_back(cp);
        i += n;
    }
    return out;
}

// One line centred on (cx, cy) with capitals `cap` pixels tall; narrower, then smaller when it
// is wider than max_w.
void Text(Canvas& c, const std::string& text, float cx, float cy, float cap, float max_w, Rgb color) {
    const Font& font = TextFont();
    if (!font.ok || text.empty()) {
        return;
    }
    const std::vector<int> cps = Codepoints(text);
    int x0, y0, x1, y1;
    if (!stbtt_GetCodepointBox(&font.info, 'H', &x0, &y0, &x1, &y1)) {
        return;
    }
    float sy = cap / float(y1);
    float units = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&font.info, cps[i], &advance, &bearing);
        units += advance;
        if (i + 1 < cps.size()) units += stbtt_GetCodepointKernAdvance(&font.info, cps[i], cps[i + 1]);
    }
    float sx = sy;
    if (units * sx > max_w) {
        sx = std::max(sy * 0.72f, max_w / units);
        if (units * sx > max_w) {
            const float k = max_w / (units * sx);
            sx *= k;
            sy *= k;
        }
    }
    constexpr int S = 4; // supersampling
    const int W = c.w * S, H = c.h * S;
    std::vector<float> cover(size_t(W) * H);
    float pen = (cx - units * sx * 0.5f) * S;
    const float base = (cy + float(y1) * sy * 0.5f) * S;
    for (size_t i = 0; i < cps.size(); ++i) {
        const int glyph = stbtt_FindGlyphIndex(&font.info, cps[i]);
        int advance, bearing;
        stbtt_GetGlyphHMetrics(&font.info, glyph, &advance, &bearing);
        const float shift = pen - std::floor(pen);
        int bx0, by0, bx1, by1;
        stbtt_GetGlyphBitmapBoxSubpixel(&font.info, glyph, sx * S, sy * S, shift, 0, &bx0, &by0, &bx1, &by1);
        const int bw = bx1 - bx0, bh = by1 - by0;
        if (glyph && bw > 0 && bh > 0) {
            std::vector<unsigned char> bitmap(size_t(bw) * bh);
            stbtt_MakeGlyphBitmapSubpixel(&font.info, bitmap.data(), bw, bh, bw, sx * S, sy * S, shift, 0, glyph);
            const int ox = int(std::floor(pen)) + bx0, oy = int(std::floor(base)) + by0;
            for (int y = 0; y < bh; ++y) {
                for (int x = 0; x < bw; ++x) {
                    const int X = ox + x, Y = oy + y;
                    if (X >= 0 && X < W && Y >= 0 && Y < H) {
                        float& v = cover[size_t(Y) * W + X];
                        v = std::max(v, bitmap[y * bw + x] / 255.0f);
                    }
                }
            }
        }
        pen += advance * sx * S;
        if (i + 1 < cps.size()) pen += stbtt_GetCodepointKernAdvance(&font.info, cps[i], cps[i + 1]) * sx * S;
    }
    if (!font.bold) { // DejaVu Sans is a regular weight: thicken the strokes
        const int grow = std::max(1, int(cap * S / 24));
        std::vector<float> t(cover.size());
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float v = 0;
                for (int k = 0; k <= grow; ++k) v = std::max(v, cover[size_t(y) * W + std::clamp(x - k, 0, W - 1)]);
                t[size_t(y) * W + x] = v;
            }
        }
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float v = 0;
                for (int k = 0; k <= grow / 2; ++k) v = std::max(v, t[size_t(std::clamp(y - k, 0, H - 1)) * W + x]);
                cover[size_t(y) * W + x] = v;
            }
        }
    }
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            float s = 0;
            for (int j = 0; j < S; ++j) {
                for (int i = 0; i < S; ++i) s += cover[size_t(y * S + j) * W + x * S + i];
            }
            if (s > 0) c.Over(x, y, color, s / (S * S));
        }
    }
}

int Length(const std::string& text) {
    return int(Codepoints(text).size());
}

// --- Icons ---------------------------------------------------------------------------------

// A key: a dark keycap with a beige edge (orange when lit) and its name on the top face.
void Keycap(Canvas& c, float cx, float cy, float hw, float hh, const std::string& label, bool lit = false) {
    const float r = std::min(hw, hh) * 0.3f;
    const auto outer = [=](float x, float y) { return Box(x, y, cx, cy, hw, hh, r); };
    Glow(c, outer, Black, 0.6f, std::max(1.5f, hh * 0.12f));
    Fill(c, outer, Vertical(DarkTop, DarkBottom, cy - hh, cy + hh));
    const float side = std::max(1.5f, hw * 0.12f), top = std::max(1.2f, hh * 0.09f),
                bottom = std::max(2.0f, hh * 0.22f);
    const float fhw = hw - side, fhh = hh - (top + bottom) * 0.5f, fcy = cy - (bottom - top) * 0.5f;
    const auto face = [=](float x, float y) { return Box(x, y, cx, fcy, fhw, fhh, std::max(1.0f, r - side * 0.5f)); };
    Fill(c, face, lit ? Vertical(OrangeTop, OrangeBottom, fcy - fhh, fcy + fhh) : Vertical(FaceTop, FaceBottom, fcy - fhh, fcy + fhh));
    Fill(c, Edge(outer, std::max(1.1f, hh * 0.06f)), lit ? Orange : Rim);
    const int n = Length(label);
    const float cap = fhh * 2.0f * (n <= 1 ? 0.56f : n == 2 ? 0.5f : 0.42f);
    Text(c, label, cx, fcy, cap, fhw * 2.0f - std::max(2.0f, fhw * 0.2f), lit ? Byte(40, 24, 10) : Ink);
}

// The key that fills a cell, wider for longer names.
void KeyInCell(Canvas& c, const std::string& label) {
    const float w = c.w - 6.0f, h = c.h - 6.0f;
    const int n = Length(label);
    const float side = std::min(w * 0.8f, h);
    float kw = side, kh = side;
    if (n == 2) {
        kw = std::min(w, side * 1.2f);
    } else if (n > 2) {
        kw = w;
        kh = std::min(h, w * 0.62f);
    }
    Keycap(c, c.w * 0.5f, c.h * 0.5f, kw * 0.5f, kh * 0.5f, label);
}

// The mouse with one of its sources lit: 1 left, 2 middle, 3 right, 4 X1, 5 X2, 6 wheel up,
// 7 wheel down; 0 is the mouse moved (arrows on its sides).
void Mouse(Canvas& c, int source) {
    const float bh = (c.h - 6.0f) * 0.5f, bw = bh * 0.64f;
    float cx = c.w * 0.5f;
    const float cy = c.h * 0.5f;
    const bool wheel_turn = source == 6 || source == 7;
    if (wheel_turn) cx -= bw * 0.75f;
    const auto body = [=](float x, float y) { return Box(x, y, cx, cy, bw, bh, bw * 0.95f, bw * 0.9f); };
    Glow(c, body, Black, 0.6f, 2.0f);
    Fill(c, body, Vertical(DarkTop, DarkBottom, cy - bh, cy + bh));
    const float split = cy - bh * 0.12f;
    if (source == 1 || source == 3) {
        const bool left = source == 1;
        Fill(c, [=](float x, float y) { return std::max({body(x, y), left ? x - cx : cx - x, y - split}); },
             Vertical(OrangeTop, OrangeBottom, cy - bh, split));
    }
    const float line = std::max(1.0f, bh * 0.06f);
    Fill(c, [=](float x, float y) { return std::max(body(x, y), std::fabs(y - split) - line * 0.5f); }, Byte(8, 7, 6));
    Fill(c, [=](float x, float y) { return std::max({body(x, y), std::fabs(x - cx) - line * 0.5f, y - split}); }, Byte(8, 7, 6));
    const bool wheel_lit = source == 2 || wheel_turn;
    const float wy = cy - bh * 0.52f;
    Fill(c, [=](float x, float y) { return Box(x, y, cx, wy, bw * 0.2f, bh * 0.2f, bw * 0.2f); },
         wheel_lit ? Vertical(OrangeTop, OrangeBottom, wy - bh * 0.2f, wy + bh * 0.2f) : Vertical(Byte(170, 160, 146), Byte(96, 90, 82), wy - bh * 0.2f, wy + bh * 0.2f));
    if (source == 4 || source == 5) { // side buttons, X2 above X1
        const float sy = cy + (source == 4 ? 0.12f : -0.28f) * bh;
        Fill(c, [=](float x, float y) { return Box(x, y, cx - bw + 0.5f, sy, std::max(1.6f, bw * 0.16f), bh * 0.15f, 1.5f); }, Orange);
    }
    Fill(c, Edge(body, std::max(1.1f, bh * 0.06f)), Rim);
    const float a = std::max(3.0f, bh * 0.22f);
    if (wheel_turn) {
        const float dir = source == 6 ? -1.0f : 1.0f;
        Fill(c, [=](float x, float y) { return Arrow(x, y, cx + bw + a * 1.9f, cy, 0.0f, dir, a * 1.3f); }, Orange);
    } else if (source == 0) {
        for (const float dir : {-1.0f, 1.0f}) {
            Fill(c, [=](float x, float y) { return Arrow(x, y, cx + dir * (bw + a * 1.6f), cy, dir, 0.0f, a); }, Ink);
        }
    }
}

// Four keys: in a cross (the d-pad) or as W A S D. lit: 1 up, 2 down, 4 left, 8 right.
void KeyCross(Canvas& c, const std::string (&labels)[4], int lit, bool wasd) {
    const float gap = 2.0f, w = c.w - 4.0f, h = c.h - 4.0f;
    const float cx = c.w * 0.5f, cy = c.h * 0.5f;
    if (wasd) {
        const float k = std::min((w - 2 * gap) / 3, (h - gap) / 2), step = k + gap;
        Keycap(c, cx, cy - step * 0.5f, k * 0.5f, k * 0.5f, labels[0], lit & 1);
        Keycap(c, cx - step, cy + step * 0.5f, k * 0.5f, k * 0.5f, labels[2], lit & 4);
        Keycap(c, cx, cy + step * 0.5f, k * 0.5f, k * 0.5f, labels[1], lit & 2);
        Keycap(c, cx + step, cy + step * 0.5f, k * 0.5f, k * 0.5f, labels[3], lit & 8);
    } else {
        const float k = std::min(w, h) / 3 - gap, step = k + gap;
        Keycap(c, cx, cy - step, k * 0.5f, k * 0.5f, labels[0], lit & 1);
        Keycap(c, cx, cy + step, k * 0.5f, k * 0.5f, labels[1], lit & 2);
        Keycap(c, cx - step, cy, k * 0.5f, k * 0.5f, labels[2], lit & 4);
        Keycap(c, cx + step, cy, k * 0.5f, k * 0.5f, labels[3], lit & 8);
    }
}

Rgb LetterColor(char letter) {
    switch (letter) {
    case 'A': return Byte(118, 196, 64);
    case 'B': return Byte(236, 74, 60);
    case 'X': return Byte(64, 146, 236);
    case 'Y': return Byte(248, 200, 52);
    default: return Ink;
    }
}

// A face button: a dark disc with its letter and a halo of its colour, as the games draw them.
void PadFace(Canvas& c, char letter, bool coloured) {
    const float cx = c.w * 0.5f, cy = c.h * 0.5f, R = std::min(c.w, c.h) * 0.5f - 3.0f;
    const Rgb color = coloured ? LetterColor(letter) : Ink;
    Glow(c, [=](float x, float y) { return Circle(x, y, cx, cy, R * 0.8f); }, coloured ? color : Black, 0.85f, R * 0.18f);
    const auto disc = [=](float x, float y) { return Circle(x, y, cx, cy, R * 0.7f); };
    Fill(c, disc, Vertical(DarkTop, DarkBottom, cy - R * 0.7f, cy + R * 0.7f));
    Fill(c, Edge(disc, std::max(1.2f, R * 0.06f)), Mix(color, Byte(60, 56, 52), coloured ? 0.35f : 0.5f));
    Text(c, std::string(1, letter), cx, cy, R * 0.62f, R * 1.1f, color);
}

// A shoulder button (rounded on top) or a trigger (a dome), with its name.
void Shoulder(Canvas& c, const std::string& label, bool trigger) {
    const float cx = c.w * 0.5f, cy = c.h * 0.5f;
    const float hw = (c.w - 6.0f) * (trigger ? 0.36f : 0.44f), hh = (c.h - 6.0f) * (trigger ? 0.47f : 0.4f);
    const auto shape = [=](float x, float y) {
        return Box(x, y, cx, cy, hw, hh, trigger ? hw : hh * 0.9f, trigger ? hw * 0.3f : hh * 0.35f);
    };
    Glow(c, shape, Black, 0.6f, 2.0f);
    Fill(c, shape, Vertical(DarkTop, DarkBottom, cy - hh, cy + hh));
    Fill(c, Edge(shape, std::max(1.1f, hh * 0.06f)), Rim);
    Text(c, label, cx, cy + (trigger ? hh * 0.18f : hh * 0.05f), hh * (trigger ? 0.62f : 0.8f), hw * 1.7f, Ink);
}

// A stick pressed in: a stick top with its name.
void StickClick(Canvas& c, const std::string& label) {
    const float cx = c.w * 0.5f, cy = c.h * 0.5f, R = std::min(c.w, c.h) * 0.5f - 4.0f;
    const auto disc = [=](float x, float y) { return Circle(x, y, cx, cy, R); };
    Glow(c, disc, Black, 0.6f, 2.0f);
    Fill(c, disc, Vertical(DarkTop, DarkBottom, cy - R, cy + R));
    Fill(c, Edge([=](float x, float y) { return Circle(x, y, cx, cy, R * 0.8f); }, std::max(1.0f, R * 0.08f)), Byte(10, 9, 8));
    Fill(c, Edge(disc, std::max(1.1f, R * 0.07f)), Rim);
    Text(c, label, cx, cy, R * 0.62f, R * 1.4f, Ink);
}

// Menu (three lines) and View (two windows) of an Xbox pad; + and - of a Nintendo one.
void SmallButton(Canvas& c, int kind) {
    const float cx = c.w * 0.5f, cy = c.h * 0.5f, R = std::min(c.w, c.h) * 0.5f - 4.0f;
    const auto disc = [=](float x, float y) { return Circle(x, y, cx, cy, R); };
    Glow(c, disc, Black, 0.6f, 2.0f);
    Fill(c, disc, Vertical(DarkTop, DarkBottom, cy - R, cy + R));
    Fill(c, Edge(disc, std::max(1.1f, R * 0.07f)), Rim);
    const float t = std::max(1.4f, R * 0.13f);
    if (kind == 0) { // Menu
        for (const float dy : {-0.3f, 0.0f, 0.3f}) {
            Fill(c, [=](float x, float y) { return Box(x, y, cx, cy + dy * R, R * 0.42f, t * 0.5f, t * 0.5f); }, Ink);
        }
    } else if (kind == 1) { // View
        Fill(c, Edge([=](float x, float y) { return Box(x, y, cx + R * 0.1f, cy + R * 0.1f, R * 0.3f, R * 0.24f, R * 0.06f); }, t * 0.8f), Ink);
        Fill(c, [=](float x, float y) { return Box(x, y, cx - R * 0.12f, cy - R * 0.12f, R * 0.3f, R * 0.24f, R * 0.06f); }, Ink);
    } else { // plus (2) or minus (3)
        Fill(c, [=](float x, float y) { return Box(x, y, cx, cy, R * 0.42f, t * 0.5f, t * 0.5f); }, Ink);
        if (kind == 2) Fill(c, [=](float x, float y) { return Box(x, y, cx, cy, t * 0.5f, R * 0.42f, t * 0.5f); }, Ink);
    }
}

// --- The icon font -------------------------------------------------------------------------

// The glyphs of symbol3.fnt (the same in the three games): rectangle and what it shows.
enum class Art : uint8_t { Dpad, Button, StickLeft, StickRight, Keep };
struct Cell {
    uint8_t id;
    int16_t x, y, w, h;
    Art art;
    int8_t which; // Button: GOW3_GLYPH_*; Dpad: the lit arms (1 up, 2 down, 4 left, 8 right)
};
constexpr Cell cells[] = {
    {1, 0, 0, 75, 75, Art::Dpad, 0},
    {2, 0, 75, 64, 64, Art::Button, GOW3_GLYPH_CIRCLE},
    {3, 64, 75, 64, 64, Art::Button, GOW3_GLYPH_SQUARE},
    {4, 128, 75, 64, 64, Art::Button, GOW3_GLYPH_TRIANGLE},
    {5, 192, 75, 64, 64, Art::Button, GOW3_GLYPH_CROSS},
    {6, 84, 252, 77, 46, Art::Button, GOW3_GLYPH_R1},
    {7, 0, 139, 64, 60, Art::Button, GOW3_GLYPH_R2},
    {8, 161, 243, 77, 46, Art::Button, GOW3_GLYPH_L1},
    {9, 64, 139, 64, 60, Art::Button, GOW3_GLYPH_L2},
    {10, 128, 139, 74, 53, Art::StickLeft, 0},
    {11, 148, 192, 64, 51, Art::Button, GOW3_GLYPH_OPTIONS},
    {12, 75, 0, 75, 75, Art::Dpad, 4 | 8},
    {13, 150, 0, 75, 75, Art::Dpad, 1 | 2},
    {14, 0, 252, 84, 50, Art::Button, GOW3_GLYPH_TOUCHPAD}, // glyph 30 is the same picture
    {15, 0, 199, 74, 53, Art::StickLeft, 0},
    {16, 74, 199, 74, 53, Art::StickRight, 0},
    {18, 238, 216, 86, 86, Art::Dpad, 1},
    {21, 0, 355, 75, 75, Art::Dpad, 2},
    {24, 0, 302, 74, 53, Art::Button, GOW3_GLYPH_L3},
    {25, 74, 302, 74, 53, Art::Button, GOW3_GLYPH_R3},
    {29, 148, 298, 64, 51, Art::Keep, 0}, // Share: never sent to the games
    {40, 340, 113, 77, 46, Art::Button, GOW3_GLYPH_R1},
    {41, 256, 0, 64, 60, Art::Button, GOW3_GLYPH_R2},
    {42, 417, 104, 77, 46, Art::Button, GOW3_GLYPH_L1},
    {43, 320, 0, 64, 60, Art::Button, GOW3_GLYPH_L2},
    {44, 404, 53, 64, 51, Art::Button, GOW3_GLYPH_OPTIONS},
    {45, 256, 60, 74, 53, Art::StickLeft, 0},
    {46, 330, 60, 74, 53, Art::StickRight, 0},
    {47, 256, 163, 74, 53, Art::Button, GOW3_GLYPH_L3},
    {48, 330, 163, 74, 53, Art::Button, GOW3_GLYPH_R3},
    {49, 404, 159, 64, 51, Art::Keep, 0},
};

// A key's name on a keycap: short names for the wide keys, others as the keyboard layout
// names them (SDL), in capitals.
std::string KeyLabel(int code) {
    switch (code) {
    case SDL_SCANCODE_SPACE: return "SPACE";
    case SDL_SCANCODE_LCTRL: return "CTRL";
    case SDL_SCANCODE_RCTRL: return "R CTRL";
    case SDL_SCANCODE_LSHIFT: return "SHIFT";
    case SDL_SCANCODE_RSHIFT: return "R SHIFT";
    case SDL_SCANCODE_LALT: return "ALT";
    case SDL_SCANCODE_RALT: return "ALT GR";
    case SDL_SCANCODE_ESCAPE: return "ESC";
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "ENTER";
    case SDL_SCANCODE_BACKSPACE: return "BKSP";
    case SDL_SCANCODE_TAB: return "TAB";
    case SDL_SCANCODE_CAPSLOCK: return "CAPS";
    case SDL_SCANCODE_DELETE: return "DEL";
    case SDL_SCANCODE_INSERT: return "INS";
    case SDL_SCANCODE_PAGEUP: return "PG UP";
    case SDL_SCANCODE_PAGEDOWN: return "PG DN";
    case SDL_SCANCODE_UP: return "\xe2\x86\x91";
    case SDL_SCANCODE_DOWN: return "\xe2\x86\x93";
    case SDL_SCANCODE_LEFT: return "\xe2\x86\x90";
    case SDL_SCANCODE_RIGHT: return "\xe2\x86\x92";
    default: break;
    }
    const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(code), SDL_KMOD_NONE, false));
    if (!name || !*name) name = SDL_GetScancodeName(SDL_Scancode(code));
    std::string label = name ? name : "?";
    if (label.rfind("Keypad ", 0) == 0) label = "NUM " + label.substr(7);
    for (char& ch : label) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return label;
}

// What the keyboard and mouse show for a PS4 button; false: nothing presses it (the game's icon).
bool PaintKeys(Canvas& c, const Gow3GlyphState& s, int button) {
    if (s.key[button] > 0) {
        KeyInCell(c, KeyLabel(s.key[button]));
        return true;
    }
    if (s.mouse[button] > 0) {
        Mouse(c, s.mouse[button]);
        return true;
    }
    return false;
}

// What a gamepad button looks like on an Xbox (or another non-PlayStation) pad.
bool PaintPad(Canvas& c, const Gow3GlyphState& s, int pad) {
    const bool nintendo = s.face[0] == 'B' && s.face[1] == 'A';
    switch (pad) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH: {
        const char letter = s.face[pad] ? s.face[pad] : "ABXY"[pad];
        PadFace(c, letter, s.style == GOW3_GLYPHS_XBOX);
        return true;
    }
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: Shoulder(c, nintendo ? "L" : "LB", false); return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: Shoulder(c, nintendo ? "R" : "RB", false); return true;
    case GOW3_GLYPH_PAD_LEFT_TRIGGER: Shoulder(c, nintendo ? "ZL" : "LT", true); return true;
    case GOW3_GLYPH_PAD_RIGHT_TRIGGER: Shoulder(c, nintendo ? "ZR" : "RT", true); return true;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: StickClick(c, "LS"); return true;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: StickClick(c, "RS"); return true;
    case SDL_GAMEPAD_BUTTON_START: SmallButton(c, nintendo ? 2 : 0); return true;
    case SDL_GAMEPAD_BUTTON_BACK: SmallButton(c, nintendo ? 3 : 1); return true;
    default: return false; // d-pad, paddles: the game's icon
    }
}

bool Paint(Canvas& c, const Cell& cell, const Gow3GlyphState& s) {
    if (s.style == GOW3_GLYPHS_KEYBOARD) {
        switch (cell.art) {
        case Art::Button:
            return PaintKeys(c, s, cell.which);
        case Art::StickLeft: {
            const int* k = &s.key[GOW3_GLYPH_LS_UP];
            if (!k[0] && !k[1] && !k[2] && !k[3]) return false;
            const std::string labels[4] = {k[0] ? KeyLabel(k[0]) : "", k[1] ? KeyLabel(k[1]) : "",
                                           k[2] ? KeyLabel(k[2]) : "", k[3] ? KeyLabel(k[3]) : ""};
            KeyCross(c, labels, 0, true);
            return true;
        }
        case Art::StickRight:
            Mouse(c, 0);
            return true;
        case Art::Dpad: {
            const int* k = &s.key[GOW3_GLYPH_UP];
            if (!k[0] && !k[1] && !k[2] && !k[3]) return false;
            const std::string labels[4] = {k[0] ? KeyLabel(k[0]) : "", k[1] ? KeyLabel(k[1]) : "",
                                           k[2] ? KeyLabel(k[2]) : "", k[3] ? KeyLabel(k[3]) : ""};
            KeyCross(c, labels, cell.which, false);
            return true;
        }
        case Art::Keep:
            return false;
        }
        return false;
    }
    // A gamepad: its buttons; the sticks, d-pad and Share keep the games' (generic) pictures.
    return cell.art == Art::Button && s.pad[cell.which] >= 0 && PaintPad(c, s, s.pad[cell.which]);
}

// Writes a cell's picture into the atlas (straight alpha). Transparent texels take the colour
// of their nearest drawn neighbour, so filtering at the edges blends in no dark fringe.
void Store(const Canvas& c, std::vector<uint8_t>& atlas, int x0, int y0) {
    const size_t n = size_t(c.w) * c.h;
    std::vector<float> rgb(n * 3);
    std::vector<uint8_t> known(n);
    for (size_t i = 0; i < n; ++i) {
        const float a = c.px[i * 4 + 3];
        if (a > 1.0f / 255.0f) {
            for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = c.px[i * 4 + k] / a;
            known[i] = 1;
        }
    }
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<uint8_t> next = known;
        for (int y = 0; y < c.h; ++y) {
            for (int x = 0; x < c.w; ++x) {
                const size_t i = size_t(y) * c.w + x;
                if (known[i]) continue;
                float sum[3] = {};
                int count = 0;
                static constexpr int steps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& step : steps) {
                    const int nx = x + step[0], ny = y + step[1];
                    if (nx < 0 || ny < 0 || nx >= c.w || ny >= c.h) continue;
                    const size_t j = size_t(ny) * c.w + nx;
                    if (!known[j]) continue;
                    for (int k = 0; k < 3; ++k) sum[k] += rgb[j * 3 + k];
                    ++count;
                }
                if (count) {
                    for (int k = 0; k < 3; ++k) rgb[i * 3 + k] = sum[k] / count;
                    next[i] = 1;
                }
            }
        }
        known.swap(next);
    }
    for (int y = 0; y < c.h; ++y) {
        for (int x = 0; x < c.w; ++x) {
            const size_t i = size_t(y) * c.w + x;
            uint8_t* out = &atlas[(size_t(y0 + y) * AtlasSize + x0 + x) * 4];
            const Rgb fallback = DarkBottom;
            const float v[3] = {known[i] ? rgb[i * 3] : fallback.r, known[i] ? rgb[i * 3 + 1] : fallback.g,
                                known[i] ? rgb[i * 3 + 2] : fallback.b};
            for (int k = 0; k < 3; ++k) out[k] = uint8_t(std::clamp(v[k], 0.0f, 1.0f) * 255.0f + 0.5f);
            out[3] = uint8_t(std::clamp(c.px[i * 4 + 3], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

using Originals = std::array<std::vector<uint8_t>, 4>;
using Published = std::array<std::shared_ptr<const std::vector<uint8_t>>, 4>;

// The textures of each role with the icons for the state.
Published Draw(const Gow3GlyphState& s, const Originals& base) {
    std::vector<uint8_t> atlas(AtlasBytes);
    if (!base[RoleRgba].empty()) {
        atlas = base[RoleRgba];
    } else {
        for (size_t i = 0; i < AtlasBytes; i += 4) {
            std::memcpy(&atlas[i], &base[RoleColor][i], 3);
            atlas[i + 3] = base[RoleAlpha][i];
        }
    }
    for (const Cell& cell : cells) {
        Canvas canvas(cell.w, cell.h);
        if (Paint(canvas, cell, s)) {
            Store(canvas, atlas, cell.x, cell.y);
        }
    }
    Published out{};
    if (!base[RoleRgba].empty()) {
        out[RoleRgba] = std::make_shared<const std::vector<uint8_t>>(atlas);
    }
    if (!base[RoleColor].empty()) { // its alpha channel stays as the game made it
        auto color = std::make_shared<std::vector<uint8_t>>(base[RoleColor]);
        for (size_t i = 0; i < AtlasBytes; i += 4) std::memcpy(&(*color)[i], &atlas[i], 3);
        out[RoleColor] = std::move(color);
        auto alpha = std::make_shared<std::vector<uint8_t>>(base[RoleAlpha]);
        for (size_t i = 0; i < AtlasBytes; i += 4) std::memset(&(*alpha)[i], atlas[i + 3], 3);
        out[RoleAlpha] = std::move(alpha);
    }
    return out;
}

// --- Shared state and the worker -----------------------------------------------------------

std::mutex mutex;
std::condition_variable wake;
Originals originals;  // linear RGBA of each role found so far
Published published;  // what to upload, per role
Gow3GlyphState wanted{};
bool have_wanted = false, pending = false, worker_started = false;
std::atomic<uint32_t> generation{1};

const char* StyleName(int style) {
    switch (style) {
    case GOW3_GLYPHS_KEYBOARD: return "keyboard and mouse";
    case GOW3_GLYPHS_XBOX: return "Xbox";
    case GOW3_GLYPHS_OTHER_PAD: return "gamepad";
    default: return "PlayStation (the game's own)";
    }
}

void Work() {
    int reports = 0;
    for (;;) {
        Gow3GlyphState state;
        Originals base;
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, [] { return pending; });
            pending = false;
            state = wanted;
            base = originals;
        }
        const bool pair_ready = base[RoleColor].empty() == base[RoleAlpha].empty();
        const bool any = !base[RoleRgba].empty() || !base[RoleColor].empty();
        Published made{};
        if (state.style != GOW3_GLYPHS_PLAYSTATION && any && pair_ready) {
            made = Draw(state, base);
        } else if (!pair_ready) {
            continue; // Uncharted 3's colour or alpha texture is still to come
        }
        bool changed = false;
        {
            std::scoped_lock lock{mutex};
            if (pending) {
                continue; // newer input arrived meanwhile: draw that instead
            }
            for (size_t i = 0; i < made.size(); ++i) changed |= made[i] || published[i];
            published = made;
        }
        if (changed) {
            generation.fetch_add(1, std::memory_order_release);
            if (reports++ < 20) {
                std::printf("Glyphs: %s button icons\n", StyleName(state.style));
                std::fflush(stdout);
            }
        }
    }
}

// Called with the mutex held.
void Schedule() {
    if (!have_wanted) {
        return; // nothing to draw until the pad code says what the player uses
    }
    pending = true;
    if (!worker_started) {
        worker_started = true;
        std::thread(Work).detach();
    }
    wake.notify_one();
}

} // namespace

bool Candidate(uint32_t width, uint32_t height, bool rgba8, uint32_t levels, bool thin_1d) {
    return width == AtlasSize && height == AtlasSize && rgba8 && levels == 1 && thin_1d;
}

Role Identify(const uint8_t* guest, uint32_t size) {
    if (size != AtlasBytes) {
        return RoleNone;
    }
    const uint64_t hash = XXH3_64bits(guest, size);
    for (const Known& k : known) {
        if (k.hash != hash) {
            continue;
        }
        std::scoped_lock lock{mutex};
        if (originals[k.role].empty()) {
            originals[k.role].resize(AtlasBytes);
            Detile(guest, originals[k.role].data());
            if (k.role != RoleAlpha) {
                std::printf("Glyphs: %s's button icon font found\n", k.game);
                std::fflush(stdout);
            }
            Schedule();
        }
        return k.role;
    }
    return RoleNone;
}

uint32_t Generation() {
    return generation.load(std::memory_order_acquire);
}

std::shared_ptr<const std::vector<uint8_t>> Pixels(Role role) {
    std::scoped_lock lock{mutex};
    return role < published.size() ? published[role] : nullptr;
}

} // namespace Gow3Glyphs

extern "C" void gow3gpu_glyphs_set(const Gow3GlyphState* state) {
    using namespace Gow3Glyphs;
    std::scoped_lock lock{mutex};
    if (have_wanted && !std::memcmp(&wanted, state, sizeof(wanted))) {
        return;
    }
    wanted = *state;
    have_wanted = true;
    if (!originals[RoleRgba].empty() || !originals[RoleColor].empty() || !originals[RoleAlpha].empty()) {
        Schedule();
    }
}
