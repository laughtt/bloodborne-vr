#!/usr/bin/env python3
"""Turn BB_FRAME_DUMP_TRIGGER raw dumps (NAME_INDEX_WxH.raw, 32-bit pixels) into BMP files
for the VR sandbox (bbport_xr_sandbox --image).

    python scripts/vr_frames.py <dump_dir> [--out DIR] [--rgba]

Raw pixels are B,G,R,A (the window/swapchain order) unless --rgba; BMP stores B,G,R,A rows
bottom-up, with alpha forced to 255 (the game leaves it at 0).
"""
import argparse
import re
import struct
from pathlib import Path

NAME = re.compile(r'(?P<name>.+)_(?P<index>\d+)_(?P<w>\d+)x(?P<h>\d+)\.raw$')


def to_bmp(raw, width, height, rgba):
    pixels = bytearray(raw[:width * height * 4])
    if rgba:
        pixels[0::4], pixels[2::4] = pixels[2::4], pixels[0::4]
    pixels[3::4] = b'\xff' * (width * height)
    stride = width * 4
    rows = b''.join(pixels[y * stride:(y + 1) * stride] for y in range(height - 1, -1, -1))
    header = struct.pack('<2sIHHI', b'BM', 54 + len(rows), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 32, 0, len(rows), 2835, 2835, 0, 0)
    return header + info + rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('dump_dir', type=Path)
    parser.add_argument('--out', type=Path, help='output folder (default: <dump_dir>/bmp)')
    parser.add_argument('--rgba', action='store_true', help='raw pixels are R,G,B,A')
    args = parser.parse_args()
    out = args.out or args.dump_dir / 'bmp'
    out.mkdir(parents=True, exist_ok=True)
    for raw in sorted(args.dump_dir.glob('*.raw')):
        match = NAME.match(raw.name)
        if not match:
            continue
        width, height = int(match['w']), int(match['h'])
        data = raw.read_bytes()
        if len(data) < width * height * 4:
            print(f'skip {raw.name}: {len(data)} bytes, expected {width * height * 4}')
            continue
        target = out / (raw.stem + '.bmp')
        target.write_bytes(to_bmp(data, width, height, args.rgba))
        print(target)


if __name__ == '__main__':
    main()
