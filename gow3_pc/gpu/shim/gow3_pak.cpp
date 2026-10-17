// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: HUD button pictures in a Naughty Dog pak (gow3_pak.h).

#include "gow3_pak.h"

#include <xxhash.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string_view>

namespace Gow3Pak {

namespace {

constexpr uint32_t Magic = 0xA79;
constexpr uint32_t ItemMarker = 0x12345;
constexpr std::string_view RecordType{"VRAM_DESC\0", 10};
constexpr std::string_view Folder = "/hud/buttons/";

uint32_t Dword(const std::vector<char>& data, size_t at) {
    uint32_t value;
    std::memcpy(&value, data.data() + at, 4);
    return value;
}

bool Fail(std::string* error, std::string why) {
    if (error) {
        *error = std::move(why);
    }
    return false;
}

} // namespace

uint32_t Level0Bytes(uint32_t width, uint32_t height) {
    const uint32_t blocks_w = (width + 3) / 4, blocks_h = (height + 3) / 4;
    return (blocks_w + 7) / 8 * ((blocks_h + 7) / 8) * 64 * 8;
}

std::vector<Texture> ReadButtons(const std::string& path, std::string* error) {
    std::vector<Texture> out;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail(error, "cannot open " + path);
        return out;
    }
    file.seekg(0, std::ios::end);
    const uint64_t file_size = uint64_t(file.tellg());
    std::vector<char> head(0x40);
    file.seekg(0);
    if (file_size < head.size() || !file.read(head.data(), head.size()) || Dword(head, 0) != Magic) {
        Fail(error, path + " is not a Naughty Dog pak");
        return out;
    }
    const uint32_t gpu_size = Dword(head, 0x20);
    if (!gpu_size || gpu_size >= file_size) {
        Fail(error, path + ": unexpected GPU data size");
        return out;
    }
    const uint64_t gpu_base = file_size - gpu_size;
    std::vector<char> records(gpu_base);
    file.seekg(0);
    if (!file.read(records.data(), records.size())) {
        Fail(error, "cannot read " + path);
        return out;
    }
    const std::string_view text(records.data(), records.size());
    for (size_t at = text.find(RecordType); at != std::string_view::npos; at = text.find(RecordType, at + 1)) {
        // The cooked picture's path ends right before the record's type name.
        if (at < 2 || text[at - 1] != '\0') {
            continue;
        }
        const size_t end = at - 1, start = text.rfind('\0', end - 1) + 1;
        if (end <= start) {
            continue;
        }
        const std::string_view cooked = text.substr(start, end - start);
        const size_t folder = cooked.find(Folder);
        if (folder == std::string_view::npos) {
            continue;
        }
        // The item marker before the path, then the record's dwords.
        size_t marker = std::string_view::npos;
        for (size_t m = start >= 4 ? start - 4 : 0; m + 4 <= start && start - m < 512; --m) {
            if (Dword(records, m) == ItemMarker) {
                marker = m;
                break;
            }
            if (m == 0) {
                break;
            }
        }
        if (marker == std::string_view::npos || marker + 4 + 14 * 4 > start) {
            continue;
        }
        const size_t fields = marker + 4;
        Texture t;
        t.offset = gpu_base + Dword(records, fields + 4 * 4);
        t.size = Dword(records, fields + 6 * 4);
        t.dxgi = Dword(records, fields + 9 * 4);
        t.levels = Dword(records, fields + 11 * 4);
        t.width = Dword(records, fields + 12 * 4);
        t.height = Dword(records, fields + 13 * 4);
        const bool known_format = t.dxgi == 71 || t.dxgi == 72 || t.dxgi == 80;
        if (!known_format || t.width < 4 || t.width > 4096 || t.height < 4 || t.height > 4096 || !t.levels ||
            t.levels > 13 || t.size < Level0Bytes(t.width, t.height) || t.offset + t.size > file_size) {
            continue;
        }
        const std::string_view file_name = cooked.substr(folder + Folder.size());
        t.name.assign(file_name.substr(0, std::min(file_name.find('/'), file_name.size())));
        for (char& ch : t.name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (t.name.size() > 4 && t.name.compare(t.name.size() - 4, 4, ".tga") == 0) {
            t.name.resize(t.name.size() - 4);
        }
        if (std::any_of(out.begin(), out.end(), [&](const Texture& o) { return o.offset == t.offset; })) {
            continue; // the same texture listed under another spelling of its name
        }
        std::vector<uint8_t> level0;
        if (!ReadLevel0(path, t, level0)) {
            continue;
        }
        t.hash = XXH3_64bits(level0.data(), level0.size());
        out.push_back(std::move(t));
    }
    if (out.empty()) {
        Fail(error, path + " has no HUD button pictures");
    }
    return out;
}

bool ReadLevel0(const std::string& path, const Texture& texture, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    out.resize(Level0Bytes(texture.width, texture.height));
    file.seekg(std::streamoff(texture.offset));
    return file && file.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
}

} // namespace Gow3Pak
