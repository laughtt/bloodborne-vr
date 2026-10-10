// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// PNG input for sandbox3d without zlib: a small inflate (stored, fixed and dynamic Huffman blocks,
// decoded canonically as in zlib's puff.c) and the five PNG row filters. 8-bit gray, gray+alpha,
// RGB, RGBA and palette images, not interlaced; the result is RGBA8 or, for gray images, one
// channel (ReadPng's `channels`).

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sb {

namespace png_detail {

struct Huffman {
    uint16_t counts[16] = {};
    uint16_t symbols[320] = {};
};

class Inflater {
public:
    Inflater(const uint8_t* data, size_t size) : in(data), size(size) {}

    /// Decompresses a zlib stream (2-byte header, deflate blocks; the Adler-32 is not checked).
    bool Zlib(std::vector<uint8_t>& out) {
        if (size < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) {
            return Fail("bad zlib header");
        }
        pos = 2;
        int last = 0;
        do {
            last = Bits(1);
            const int type = Bits(2);
            bool ok = false;
            if (type == 0) {
                ok = Stored(out);
            } else if (type == 1) {
                ok = Fixed(out);
            } else if (type == 2) {
                ok = Dynamic(out);
            } else {
                return Fail("bad block type");
            }
            if (!ok || error) {
                return false;
            }
        } while (!last);
        return true;
    }

    std::string message;

private:
    const uint8_t* in;
    size_t size, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool error = false;

    bool Fail(const char* what) {
        if (!error) {
            message = what;
        }
        error = true;
        return false;
    }

    int Bits(int need) {
        uint32_t v = bitbuf;
        while (bitcnt < need) {
            if (pos >= size) {
                Fail("unexpected end of data");
                return 0;
            }
            v |= uint32_t(in[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = v >> need;
        bitcnt -= need;
        return int(v & ((1u << need) - 1));
    }

    bool Stored(std::vector<uint8_t>& out) {
        bitbuf = 0;
        bitcnt = 0;
        if (pos + 4 > size) {
            return Fail("short stored block");
        }
        const unsigned len = in[pos] | (in[pos + 1] << 8);
        const unsigned nlen = in[pos + 2] | (in[pos + 3] << 8);
        pos += 4;
        if (len != (~nlen & 0xFFFF) || pos + len > size) {
            return Fail("bad stored block");
        }
        out.insert(out.end(), in + pos, in + pos + len);
        pos += len;
        return true;
    }

    static int Build(Huffman& h, const uint8_t* lengths, int n) {
        for (auto& c : h.counts) {
            c = 0;
        }
        for (int s = 0; s < n; ++s) {
            ++h.counts[lengths[s]];
        }
        if (h.counts[0] == n) {
            return 0; // no codes: complete, but decoding will fail
        }
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.counts[len];
            if (left < 0) {
                return -1; // over-subscribed
            }
        }
        uint16_t offs[16] = {};
        for (int len = 1; len < 15; ++len) {
            offs[len + 1] = uint16_t(offs[len] + h.counts[len]);
        }
        for (int s = 0; s < n; ++s) {
            if (lengths[s]) {
                h.symbols[offs[lengths[s]]++] = uint16_t(s);
            }
        }
        return left;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= Bits(1);
            const int count = h.counts[len];
            if (code - count < first) {
                return h.symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        Fail("bad Huffman code");
        return -1;
    }

    bool Codes(std::vector<uint8_t>& out, const Huffman& lencode, const Huffman& distcode) {
        static const uint16_t lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                                           15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                           17,   25,   33,   49,   65,   97,    129,   193,
                                           257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                           4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(lencode);
            if (symbol < 0 || error) {
                return false;
            }
            if (symbol < 256) {
                out.push_back(uint8_t(symbol));
                continue;
            }
            if (symbol == 256) {
                return true;
            }
            symbol -= 257;
            if (symbol >= 29) {
                return Fail("bad length symbol");
            }
            const int len = lbase[symbol] + Bits(lext[symbol]);
            const int ds = Decode(distcode);
            if (ds < 0 || ds >= 30) {
                return Fail("bad distance symbol");
            }
            const size_t dist = size_t(dbase[ds] + Bits(dext[ds]));
            if (dist > out.size()) {
                return Fail("distance too far back");
            }
            const size_t from = out.size() - dist;
            for (int k = 0; k < len; ++k) {
                out.push_back(out[from + size_t(k)]);
            }
        }
    }

    bool Fixed(std::vector<uint8_t>& out) {
        static Huffman lencode, distcode;
        static bool built = false;
        if (!built) {
            uint8_t lengths[320];
            int s = 0;
            for (; s < 144; ++s) {
                lengths[s] = 8;
            }
            for (; s < 256; ++s) {
                lengths[s] = 9;
            }
            for (; s < 280; ++s) {
                lengths[s] = 7;
            }
            for (; s < 288; ++s) {
                lengths[s] = 8;
            }
            Build(lencode, lengths, 288);
            for (s = 0; s < 30; ++s) {
                lengths[s] = 5;
            }
            Build(distcode, lengths, 30);
            built = true;
        }
        return Codes(out, lencode, distcode);
    }

    bool Dynamic(std::vector<uint8_t>& out) {
        static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                          11, 4,  12, 3, 13, 2, 14, 1, 15};
        const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > 286 || ndist > 30) {
            return Fail("bad dynamic block counts");
        }
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) {
            lengths[order[i]] = uint8_t(Bits(3));
        }
        Huffman lencode, distcode;
        if (Build(lencode, lengths, 19) != 0) {
            return Fail("incomplete code-length code");
        }
        int index = 0;
        while (index < nlen + ndist) {
            int symbol = Decode(lencode);
            if (symbol < 0 || error) {
                return false;
            }
            if (symbol < 16) {
                lengths[index++] = uint8_t(symbol);
                continue;
            }
            uint8_t len = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (index == 0) {
                    return Fail("repeat with no first length");
                }
                len = lengths[index - 1];
                repeat = 3 + Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (index + repeat > nlen + ndist) {
                return Fail("too many lengths");
            }
            while (repeat--) {
                lengths[index++] = len;
            }
        }
        if (lengths[256] == 0) {
            return Fail("no end-of-block code");
        }
        const int lerr = Build(lencode, lengths, nlen);
        if (lerr < 0 || (lerr > 0 && nlen - lencode.counts[0] != 1)) {
            return Fail("bad literal/length code");
        }
        const int derr = Build(distcode, lengths + nlen, ndist);
        if (derr < 0 || (derr > 0 && ndist - distcode.counts[0] != 1)) {
            return Fail("bad distance code");
        }
        return Codes(out, lencode, distcode);
    }
};

inline uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline int Paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

} // namespace png_detail

/// Decodes a PNG file's bytes. Gray images give 1 channel, everything else RGBA (4 channels).
inline bool DecodePng(const std::vector<uint8_t>& file, uint32_t& width, uint32_t& height,
                      int& channels, std::vector<uint8_t>& pixels, std::string& error) {
    using namespace png_detail;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (file.size() < 8 || !std::equal(signature, signature + 8, file.begin())) {
        error = "not a PNG";
        return false;
    }
    std::vector<uint8_t> idat, palette, trns;
    int depth = 0, color = -1, interlace = 0;
    for (size_t at = 8; at + 12 <= file.size();) {
        const uint32_t len = Be32(&file[at]);
        if (at + 12 + len > file.size()) {
            error = "truncated chunk";
            return false;
        }
        const std::string kind(reinterpret_cast<const char*>(&file[at + 4]), 4);
        const uint8_t* body = &file[at + 8];
        if (kind == "IHDR" && len >= 13) {
            width = Be32(body);
            height = Be32(body + 4);
            depth = body[8];
            color = body[9];
            interlace = body[12];
        } else if (kind == "PLTE") {
            palette.assign(body, body + len);
        } else if (kind == "tRNS") {
            trns.assign(body, body + len);
        } else if (kind == "IDAT") {
            idat.insert(idat.end(), body, body + len);
        } else if (kind == "IEND") {
            break;
        }
        at += 12 + len;
    }
    static const int kChannels[7] = {1, 0, 3, 1, 2, 0, 4};
    if (depth != 8 || color < 0 || color > 6 || !kChannels[color] || interlace != 0 ||
        width == 0 || height == 0 || width > 16384 || height > 16384) {
        error = "unsupported PNG (needs 8-bit, not interlaced)";
        return false;
    }
    const int in_ch = kChannels[color];
    std::vector<uint8_t> raw;
    raw.reserve(size_t(width * in_ch + 1) * height);
    Inflater inflater(idat.data(), idat.size());
    if (!inflater.Zlib(raw)) {
        error = "inflate: " + inflater.message;
        return false;
    }
    const size_t stride = size_t(width) * size_t(in_ch);
    if (raw.size() < (stride + 1) * height) {
        error = "image data too short";
        return false;
    }
    std::vector<uint8_t> img(stride * height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* row = &img[y * stride];
        const uint8_t* up = y ? &img[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(in_ch) ? row[x - size_t(in_ch)] : 0;
            const int b = up ? up[x] : 0;
            const int c = (up && x >= size_t(in_ch)) ? up[x - size_t(in_ch)] : 0;
            int v = src[x];
            switch (filter) {
            case 0: break;
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: v += Paeth(a, b, c); break;
            default: error = "bad row filter"; return false;
            }
            row[x] = uint8_t(v);
        }
    }
    if (color == 0) {
        channels = 1;
        pixels = std::move(img);
        return true;
    }
    channels = 4;
    pixels.assign(size_t(width) * height * 4, 255);
    for (size_t i = 0, n = size_t(width) * height; i < n; ++i) {
        uint8_t* out = &pixels[i * 4];
        const uint8_t* in = &img[i * size_t(in_ch)];
        switch (color) {
        case 2: out[0] = in[0], out[1] = in[1], out[2] = in[2]; break;
        case 4: out[0] = out[1] = out[2] = in[0], out[3] = in[1]; break;
        case 6: out[0] = in[0], out[1] = in[1], out[2] = in[2], out[3] = in[3]; break;
        case 3: {
            const size_t p = in[0];
            if (p * 3 + 2 < palette.size()) {
                out[0] = palette[p * 3], out[1] = palette[p * 3 + 1], out[2] = palette[p * 3 + 2];
            }
            out[3] = p < trns.size() ? trns[p] : 255;
            break;
        }
        default: break;
        }
    }
    return true;
}

/// Reads and decodes a PNG file.
inline bool ReadPng(const std::string& path, uint32_t& width, uint32_t& height, int& channels,
                    std::vector<uint8_t>& pixels, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> file;
    uint8_t buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) {
        file.insert(file.end(), buffer, buffer + n);
    }
    std::fclose(f);
    return DecodePng(file, width, height, channels, pixels, error);
}

} // namespace sb
