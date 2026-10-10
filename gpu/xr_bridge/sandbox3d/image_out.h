// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// PNG output for sandbox3d's offline renders, without zlib: deflate "stored" blocks (no
// compression) with the CRC32 and Adler-32 checks PNG needs.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sb {

inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/// RGBA8 pixels, rows top down. Alpha is written as is.
inline bool WritePng(const std::string& path, uint32_t width, uint32_t height,
                     const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (width * 4 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba.begin() + size_t(y) * width * 4,
                   rgba.begin() + size_t(y + 1) * width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t at = 0; at < raw.size() || at == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n & 0xFF));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n & 0xFF));
        z.push_back(uint8_t((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (last) {
            break;
        }
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) {
        z.push_back(uint8_t(adler >> s));
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const auto be32 = [](std::vector<uint8_t>& out, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) {
            out.push_back(uint8_t(v >> s));
        }
    };
    const auto chunk = [&](const char* kind, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out;
        be32(out, uint32_t(data.size()));
        out.insert(out.end(), kind, kind + 4);
        out.insert(out.end(), data.begin(), data.end());
        be32(out, Crc32(out.data() + 4, data.size() + 4));
        std::fwrite(out.data(), 1, out.size(), f);
    };
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(signature, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, width);
    be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

} // namespace sb
