"""TPF texture archives and the texture formats in them, read-only, stdlib only.

    TPF     FromSoftware's texture pack: named textures with a format byte, size, mip count and
            (console TPFs) a raw, headerless payload. PC TPFs hold whole DDS files instead.
    PS4     payloads are tiled: block-compressed images are stored in 8x8-block tiles, the tiles in
            row order, the blocks inside a tile in Morton (Z) order, the image padded to whole
            tiles. ps4_untile() returns the linear block rows.
    Formats BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC4, BC5, BC7 and 8-bit RGBA/BGRA decode to RGBA8.
            BC6H (HDR) is detected and reported (TextureError), not decoded.

Bloodborne's menu textures (menu/*.tpf.dcx) are PS4 TPFs of BC7 (DXGI 98) and BC1 (DXGI 71):
menu_knowledge_<iconId:05d>.tpf.dcx holds one 400x400 item picture, the iconId of EquipParamGoods,
EquipParamWeapon (1000+), EquipParamProtector (2000+) and EquipParamAccessory (4000+) rows.

Also: to_dds() wraps a texture's linear data in a DDS header (DX10 extension) for other tools,
write_png() writes RGBA8 or 8-bit gray PNGs. Nothing here writes into the game folder.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

import souls


class TextureError(Exception):
    """A texture this module cannot decode (format or layout not supported)."""


# ---- TPF ----------------------------------------------------------------------------------------

PLATFORM_PC, PLATFORM_X360, PLATFORM_PS3, PLATFORM_PS4, PLATFORM_XBOX1 = 0, 1, 2, 4, 5


@dataclass
class Texture:
    name: str
    format: int        # FromSoftware's format byte (TPF.Texture.Format)
    type: int          # 0 texture, 1 cubemap, 2 volume
    mipmaps: int
    flags1: int
    width: int         # 0 for PC TPFs (read from the DDS header)
    height: int
    dxgi: int          # DXGI_FORMAT of PS4/Xbox One TPFs, else 0
    platform: int
    data: bytes


def tpf_read(data: bytes) -> list[Texture]:
    """The textures of a TPF (DCX-decompressed first when wrapped)."""
    data = souls.dcx_decompress(data)
    if data[:4] != b'TPF\0':
        raise ValueError(f'not TPF: {data[:4]!r}')
    platform = data[0x0c]
    e = '>' if platform in (PLATFORM_X360, PLATFORM_PS3) else '<'
    (count,) = struct.unpack_from(e + 'i', data, 0x08)
    flag2, encoding = data[0x0d], data[0x0e]
    at = 0x10
    out = []
    for _ in range(count):
        offset, size = struct.unpack_from(e + 'Ii', data, at)
        fmt, kind, mips, flags1 = data[at + 8:at + 12]
        at += 12
        width = height = 0
        if platform != PLATFORM_PC:
            width, height = struct.unpack_from(e + 'hh', data, at)
            at += 4
            if platform == PLATFORM_X360:
                at += 4
            elif platform == PLATFORM_PS3:
                at += 4 + (4 if flag2 else 0)
            elif platform in (PLATFORM_PS4, PLATFORM_XBOX1):
                at += 8  # texture count (1 or 6), 0xD
        name_offset, has_floats = struct.unpack_from(e + 'Ii', data, at)
        at += 8
        dxgi = 0
        if platform in (PLATFORM_PS4, PLATFORM_XBOX1):
            (dxgi,) = struct.unpack_from(e + 'i', data, at)
            at += 4
        if has_floats:
            _unk, length = struct.unpack_from(e + 'ii', data, at)
            at += 8 + length
        payload = data[offset:offset + size]
        if flags1 in (2, 3):
            payload = souls.dcx_decompress(payload)
        if encoding == 1:
            end = name_offset
            while data[end:end + 2] != b'\0\0':
                end += 2
            name = data[name_offset:end].decode('utf-16-be' if e == '>' else 'utf-16-le')
        else:
            end = data.index(b'\0', name_offset)
            name = data[name_offset:end].decode('shift_jis', 'replace')
        out.append(Texture(name, fmt, kind, mips, flags1, width, height, dxgi, platform, payload))
    return out


# ---- Formats --------------------------------------------------------------------------------------

# DXGI format -> (decoder name, bytes per 4x4 block or per pixel, block compressed)
DXGI = {
    70: ('bc1', 8, True), 71: ('bc1', 8, True), 72: ('bc1', 8, True),
    73: ('bc2', 16, True), 74: ('bc2', 16, True), 75: ('bc2', 16, True),
    76: ('bc3', 16, True), 77: ('bc3', 16, True), 78: ('bc3', 16, True),
    79: ('bc4', 8, True), 80: ('bc4', 8, True), 81: ('bc4s', 8, True),
    82: ('bc5', 16, True), 83: ('bc5', 16, True), 84: ('bc5s', 16, True),
    94: ('bc6h', 16, True), 95: ('bc6h', 16, True), 96: ('bc6h', 16, True),
    97: ('bc7', 16, True), 98: ('bc7', 16, True), 99: ('bc7', 16, True),
    27: ('rgba8', 4, False), 28: ('rgba8', 4, False), 29: ('rgba8', 4, False),
    87: ('bgra8', 4, False), 88: ('bgrx8', 4, False), 90: ('bgra8', 4, False),
    91: ('bgra8', 4, False), 61: ('r8', 1, False), 65: ('a8', 1, False),
}
# FromSoftware format byte -> DXGI, for TPFs that carry no DXGI (PS3, PC without DX10 headers).
FORMAT_DXGI = {0: 71, 1: 71, 3: 74, 5: 77, 9: 87, 10: 87, 16: 65, 23: 77, 24: 71, 25: 71, 33: 77,
               100: 95, 102: 98, 103: 80, 104: 83, 105: 28, 106: 98, 107: 98, 108: 71, 109: 71,
               110: 77, 112: 99, 113: 95}
SRGB_DXGI = {72, 75, 78, 99, 29, 91}


def texture_dxgi(tex: Texture) -> int:
    return tex.dxgi or FORMAT_DXGI.get(tex.format, 0)


def ps4_untile(data: bytes, width_units: int, height_units: int, unit_bytes: int) -> bytes:
    """Linear rows of `width_units` x `height_units` units (BC blocks, or pixels) from a PS4 tiled
    surface: 8x8-unit tiles in row order, Morton order inside a tile, padded to whole tiles."""
    tiles_x = (width_units + 7) // 8
    tiles_y = (height_units + 7) // 8
    need = tiles_x * tiles_y * 64 * unit_bytes
    if len(data) < need:
        raise TextureError(f'PS4 surface too small: {len(data)} < {need} bytes')
    out = bytearray(width_units * height_units * unit_bytes)
    row = width_units * unit_bytes
    src = 0
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            for t in _MORTON_8X8:
                x = tx * 8 + t[0]
                y = ty * 8 + t[1]
                if x < width_units and y < height_units:
                    dst = y * row + x * unit_bytes
                    out[dst:dst + unit_bytes] = data[src:src + unit_bytes]
                src += unit_bytes
    return bytes(out)


_MORTON_8X8 = [((t & 1) | ((t >> 1) & 2) | ((t >> 2) & 4),
                ((t >> 1) & 1) | ((t >> 2) & 2) | ((t >> 3) & 4)) for t in range(64)]


def linear_data(tex: Texture) -> tuple[int, int, int, bytes]:
    """(dxgi, width, height, top mip in linear order) of a texture: the DDS payload of PC TPFs, the
    untiled surface of PS4 ones."""
    if tex.data[:4] == b'DDS ':
        return dds_read(tex.data)
    dxgi = texture_dxgi(tex)
    if dxgi not in DXGI:
        raise TextureError(f'{tex.name}: unknown format {tex.format} (DXGI {dxgi})')
    if tex.type != 0:
        raise TextureError(f'{tex.name}: texture type {tex.type} (cubemap/volume) not supported')
    _kind, unit, block = DXGI[dxgi]
    w, h = tex.width, tex.height
    uw, uh = ((w + 3) // 4, (h + 3) // 4) if block else (w, h)
    if tex.platform == PLATFORM_PS4:
        return dxgi, w, h, ps4_untile(tex.data, uw, uh, unit)
    if tex.platform in (PLATFORM_X360, PLATFORM_PS3, PLATFORM_XBOX1):
        raise TextureError(f'{tex.name}: platform {tex.platform} layout not supported')
    return dxgi, w, h, tex.data[:uw * uh * unit]


def decode(dxgi: int, width: int, height: int, data: bytes) -> bytes:
    """RGBA8 pixels (rows top down) of a linear image in the given DXGI format."""
    if dxgi not in DXGI:
        raise TextureError(f'DXGI format {dxgi} not supported')
    kind, unit, block = DXGI[dxgi]
    if kind == 'bc6h':
        raise TextureError('BC6H (HDR) is not decoded')
    if not block:
        return _decode_plain(kind, width, height, data)
    bw, bh = (width + 3) // 4, (height + 3) // 4
    if len(data) < bw * bh * unit:
        raise TextureError(f'{kind}: {len(data)} bytes for {width}x{height}')
    block_fn = _BLOCK_DECODERS[kind]
    out = bytearray(width * height * 4)
    stride = width * 4
    at = 0
    for by in range(bh):
        for bx in range(bw):
            pixels = block_fn(data, at)
            at += unit
            x0, y0 = bx * 4, by * 4
            for py in range(4):
                y = y0 + py
                if y >= height:
                    break
                base = y * stride
                for px in range(4):
                    x = x0 + px
                    if x < width:
                        o = base + x * 4
                        out[o:o + 4] = pixels[py * 4 + px]
    return bytes(out)


def texture_rgba(tex: Texture) -> tuple[int, int, bytes]:
    """(width, height, RGBA8 rows top down) of a texture's top mip."""
    dxgi, w, h, data = linear_data(tex)
    return w, h, decode(dxgi, w, h, data)


def _decode_plain(kind: str, width: int, height: int, data: bytes) -> bytes:
    n = width * height
    if kind == 'rgba8':
        return bytes(data[:n * 4])
    out = bytearray(n * 4)
    if kind in ('bgra8', 'bgrx8'):
        out[0::4] = data[2:n * 4:4]
        out[1::4] = data[1:n * 4:4]
        out[2::4] = data[0:n * 4:4]
        out[3::4] = data[3:n * 4:4] if kind == 'bgra8' else b'\xff' * n
    elif kind == 'r8':
        out[0::4] = out[1::4] = out[2::4] = data[:n]
        out[3::4] = b'\xff' * n
    elif kind == 'a8':
        out[0::4] = out[1::4] = out[2::4] = b'\xff' * n
        out[3::4] = data[:n]
    return bytes(out)


# ---- BC1-BC5 --------------------------------------------------------------------------------------

def _rgb565(c: int) -> tuple:
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def _color_block(data: bytes, at: int, four_colors: bool) -> list:
    c0, c1, bits = struct.unpack_from('<HHI', data, at)
    r0, g0, b0 = _rgb565(c0)
    r1, g1, b1 = _rgb565(c1)
    if four_colors or c0 > c1:
        palette = [(r0, g0, b0, 255), (r1, g1, b1, 255),
                   ((2 * r0 + r1 + 1) // 3, (2 * g0 + g1 + 1) // 3, (2 * b0 + b1 + 1) // 3, 255),
                   ((r0 + 2 * r1 + 1) // 3, (g0 + 2 * g1 + 1) // 3, (b0 + 2 * b1 + 1) // 3, 255)]
    else:
        palette = [(r0, g0, b0, 255), (r1, g1, b1, 255),
                   ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255), (0, 0, 0, 0)]
    return [palette[(bits >> (2 * i)) & 3] for i in range(16)]


def _alpha_block(data: bytes, at: int, signed: bool = False) -> list:
    a0, a1 = data[at], data[at + 1]
    if signed:
        a0, a1 = (a0 - 256 if a0 > 127 else a0), (a1 - 256 if a1 > 127 else a1)
    bits = int.from_bytes(data[at + 2:at + 8], 'little')
    if a0 > a1:
        palette = [a0, a1] + [((7 - k) * a0 + k * a1 + 3) // 7 for k in range(1, 7)]
    else:
        lo, hi = (-127, 127) if signed else (0, 255)
        palette = [a0, a1] + [((5 - k) * a0 + k * a1 + 2) // 5 for k in range(1, 5)] + [lo, hi]
    values = [palette[(bits >> (3 * i)) & 7] for i in range(16)]
    if signed:
        values = [max(0, min(255, (v + 127) * 255 // 254)) for v in values]
    return values


def _bc1(data, at):
    return [bytes(p) for p in _color_block(data, at, False)]


def _bc2(data, at):
    alpha = int.from_bytes(data[at:at + 8], 'little')
    colors = _color_block(data, at + 8, True)
    return [bytes((r, g, b, ((alpha >> (4 * i)) & 15) * 17)) for i, (r, g, b, _a) in
            enumerate(colors)]


def _bc3(data, at):
    alpha = _alpha_block(data, at)
    colors = _color_block(data, at + 8, True)
    return [bytes((r, g, b, alpha[i])) for i, (r, g, b, _a) in enumerate(colors)]


def _bc4(data, at, signed=False):
    return [bytes((v, v, v, 255)) for v in _alpha_block(data, at, signed)]


def _bc5(data, at, signed=False):
    red = _alpha_block(data, at, signed)
    green = _alpha_block(data, at + 8, signed)
    return [bytes((red[i], green[i], 0, 255)) for i in range(16)]


# ---- BC7 ------------------------------------------------------------------------------------------
# Mode table: subsets, partition bits, rotation bits, index selection bits, color bits, alpha bits,
# endpoint p-bits, shared p-bits, index bits, secondary index bits (D3D11 functional spec).
_BC7_MODES = (
    (3, 4, 0, 0, 4, 0, 1, 0, 3, 0),
    (2, 6, 0, 0, 6, 0, 0, 1, 3, 0),
    (3, 6, 0, 0, 5, 0, 0, 0, 2, 0),
    (2, 6, 0, 0, 7, 0, 1, 0, 2, 0),
    (1, 0, 2, 1, 5, 6, 0, 0, 2, 3),
    (1, 0, 2, 0, 7, 8, 0, 0, 2, 2),
    (1, 0, 0, 0, 7, 7, 1, 0, 4, 0),
    (2, 6, 0, 0, 5, 5, 1, 0, 2, 0),
)
_BC7_WEIGHTS = {2: (0, 21, 43, 64), 3: (0, 9, 18, 27, 37, 46, 55, 64),
                4: (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)}


def _rows(text: str) -> list:
    return [tuple(int(c) for c in row) for row in text.split()]


# Subset of each pixel per partition (rows of 16 digits, pixel 0 first).
BC7_PARTITIONS_2 = _rows('''
0011001100110011 0001000100010001 0111011101110111 0001001100110111 0000000100010011
0011011101111111 0001001101111111 0000000100110111 0000000000010011 0011011111111111
0000000101111111 0000000000010111 0001011111111111 0000000011111111 0000111111111111
0000000000001111 0000100011101111 0111000100000000 0000000010001110 0111001100010000
0011000100000000 0000100011001110 0000000010001100 0111001100110001 0011000100010000
0000100010001100 0110011001100110 0011011001101100 0001011111101000 0000111111110000
0111000110001110 0011100110011100 0101010101010101 0000111100001111 0101101001011010
0011001111001100 0011110000111100 0101010110101010 0110100101101001 0101101010100101
0111001111001110 0001001111001000 0011001001001100 0011101111011100 0110100110010110
0011110011000011 0110011010011001 0000011001100000 0100111001000000 0010011100100000
0000001001110010 0000010011100100 0110110010010011 0011011011001001 0110001110011100
0011100111000110 0110110011001001 0110001100111001 0111111010000001 0001100011100111
0000111100110011 0011001111110000 0010001011101110 0100010001110111
''')
BC7_PARTITIONS_3 = _rows('''
0011001102212222 0001001122112221 0000200122112211 0222002200110111 0000000011221122
0011001100220022 0022002211111111 0011001122112211 0000000011112222 0000111111112222
0000111122222222 0012001200120012 0112011201120112 0122012201220122 0011011211221222
0011200122002220 0001001101121122 0111001120012200 0000112211221122 0022002200221111
0111011102220222 0001000122212221 0000001101220122 0000110022102210 0122012200110000
0012001211222222 0110122112210110 0000011012211221 0022110211020022 0110011020022222
0011012201220011 0000200022112221 0000000211221222 0222002200120011 0011001200220222
0120012001200120 0000111122220000 0120120120120120 0120201212010120 0011220011220011
0011112222000011 0101010122222222 0000000021212121 0022112200221122 0022001100220011
0220122102201221 0101222222220101 0000212121212121 0101010101012222 0222011102220111
0002111200021112 0000211221122112 0222011101110222 0002111211120002 0110011001102222
0000000021122112 0110011022222222 0022001100110022 0022112211220022 0000000000002112
0002000100020001 0222122202221222 0101222222222222 0111201122012220
''')
BC7_ANCHOR_2 = (15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
                15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
                6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15)
BC7_ANCHOR_3A = (3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
                 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
                 8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
                 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3)
BC7_ANCHOR_3B = (15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
                 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
                 15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
                 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8)


def _expand(value: int, bits: int) -> int:
    value <<= 8 - bits
    return value | (value >> bits)


def _bc7(data, at):
    v = int.from_bytes(data[at:at + 16], 'little')
    if not v & 0xFF:
        return [b'\0\0\0\0'] * 16  # reserved mode: transparent black
    mode = (v & -v).bit_length() - 1
    ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2 = _BC7_MODES[mode]
    pos = mode + 1
    part = (v >> pos) & ((1 << pb) - 1)
    pos += pb
    rotation = (v >> pos) & ((1 << rb) - 1)
    pos += rb
    selector = (v >> pos) & ((1 << isb) - 1)
    pos += isb
    count = ns * 2
    ends = [[0, 0, 0, 255] for _ in range(count)]
    mask = (1 << cb) - 1
    for c in range(3):
        for e in range(count):
            ends[e][c] = (v >> pos) & mask
            pos += cb
    if ab:
        mask = (1 << ab) - 1
        for e in range(count):
            ends[e][3] = (v >> pos) & mask
            pos += ab
    cbits, abits = cb, ab
    if epb or spb:
        pbits = []
        for _ in range(count if epb else ns):
            pbits.append((v >> pos) & 1)
            pos += 1
        for e in range(count):
            p = pbits[e if epb else e >> 1]
            for c in range(3):
                ends[e][c] = (ends[e][c] << 1) | p
            if ab:
                ends[e][3] = (ends[e][3] << 1) | p
        cbits += 1
        abits += 1 if ab else 0
    for e in ends:
        e[0], e[1], e[2] = _expand(e[0], cbits), _expand(e[1], cbits), _expand(e[2], cbits)
        if ab:
            e[3] = _expand(e[3], abits)
    if ns == 1:
        subsets = (0,) * 16
        anchors = (0,)
    elif ns == 2:
        subsets = BC7_PARTITIONS_2[part]
        anchors = (0, BC7_ANCHOR_2[part])
    else:
        subsets = BC7_PARTITIONS_3[part]
        anchors = (0, BC7_ANCHOR_3A[part], BC7_ANCHOR_3B[part])
    index = []
    for i in range(16):
        bits = ib - 1 if i in anchors else ib
        index.append((v >> pos) & ((1 << bits) - 1))
        pos += bits
    if ib2:
        index2 = []
        for i in range(16):
            bits = ib2 - 1 if i == 0 else ib2
            index2.append((v >> pos) & ((1 << bits) - 1))
            pos += bits
        color_index, alpha_index = (index2, index) if selector else (index, index2)
        color_w = _BC7_WEIGHTS[ib2 if selector else ib]
        alpha_w = _BC7_WEIGHTS[ib if selector else ib2]
        e0, e1 = ends
        out = []
        for i in range(16):
            w = color_w[color_index[i]]
            wa = alpha_w[alpha_index[i]]
            px = [((64 - w) * e0[c] + w * e1[c] + 32) >> 6 for c in range(3)]
            px.append(((64 - wa) * e0[3] + wa * e1[3] + 32) >> 6)
            if rotation:
                px[rotation - 1], px[3] = px[3], px[rotation - 1]
            out.append(bytes(px))
        return out
    weights = _BC7_WEIGHTS[ib]
    palettes = []
    for s in range(ns):
        e0, e1 = ends[2 * s], ends[2 * s + 1]
        palettes.append([bytes(((64 - w) * e0[c] + w * e1[c] + 32) >> 6 for c in range(4))
                         for w in weights])
    return [palettes[subsets[i]][index[i]] for i in range(16)]


_BLOCK_DECODERS = {
    'bc1': _bc1, 'bc2': _bc2, 'bc3': _bc3, 'bc4': _bc4, 'bc5': _bc5, 'bc7': _bc7,
    'bc4s': lambda d, a: _bc4(d, a, True), 'bc5s': lambda d, a: _bc5(d, a, True),
}


def bc7_decode_block(block: bytes) -> list:
    """The 16 RGBA pixels (bytes of 4, rows of 4) of one BC7 block."""
    return _bc7(block, 0)


# ---- DDS ------------------------------------------------------------------------------------------

def dds_read(data: bytes) -> tuple[int, int, int, bytes]:
    """(dxgi, width, height, top mip data) of a DDS file (DX10 header or the classic FourCCs)."""
    if data[:4] != b'DDS ':
        raise TextureError('not DDS')
    height, width = struct.unpack_from('<II', data, 12)
    fourcc = data[84:88]
    at = 128
    if fourcc == b'DX10':
        (dxgi,) = struct.unpack_from('<I', data, 128)
        at = 148
    else:
        dxgi = {b'DXT1': 71, b'DXT3': 74, b'DXT5': 77, b'ATI1': 80, b'BC4U': 80, b'ATI2': 83,
                b'BC5U': 83}.get(fourcc)
        if dxgi is None:
            flags, bits = struct.unpack_from('<II', data, 80)[0], data[88]
            if flags & 0x40 and bits == 32:
                (rmask,) = struct.unpack_from('<I', data, 92)
                dxgi = 28 if rmask == 0xFF else 87
            else:
                raise TextureError(f'DDS format {fourcc!r} not supported')
    _kind, unit, block = DXGI[dxgi]
    size = ((width + 3) // 4) * ((height + 3) // 4) * unit if block else width * height * unit
    return dxgi, width, height, data[at:at + size]


def to_dds(tex: Texture) -> bytes:
    """A DDS file (DX10 header) of the texture's top mip in linear order."""
    dxgi, w, h, data = linear_data(tex)
    _kind, unit, block = DXGI[dxgi]
    pitch = ((w + 3) // 4) * unit if block else w * unit
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x80000 if block else 0x8)
    header = bytearray(124)
    struct.pack_into('<IIIIIII', header, 0, 124, flags, h, w, len(data) if block else pitch, 0, 1)
    struct.pack_into('<II4s', header, 72, 32, 0x4, b'DX10')
    struct.pack_into('<I', header, 104, 0x1000)
    dx10 = struct.pack('<IIIII', dxgi, 3, 0, 1, 0)
    return b'DDS ' + bytes(header) + dx10 + data


# ---- PNG ------------------------------------------------------------------------------------------

def png_bytes(width: int, height: int, pixels: bytes, channels: int = 4, level: int = 6) -> bytes:
    """A PNG of 8-bit pixels: 4 channels RGBA, 1 channel gray (rows top down)."""
    color_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    stride = width * channels
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack('>I', len(payload)) + kind + payload +
                struct.pack('>I', zlib.crc32(kind + payload) & 0xFFFFFFFF))

    ihdr = struct.pack('>IIBBBBB', width, height, 8, color_type, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(bytes(raw), level)) + chunk(b'IEND', b''))


def write_png(path, width: int, height: int, pixels: bytes, channels: int = 4) -> None:
    with open(path, 'wb') as f:
        f.write(png_bytes(width, height, pixels, channels))
