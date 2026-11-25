#!/usr/bin/env python3
"""Tile the runtime's BMP screenshots (160x144, 32-bit) into one PNG, x2.
No dependency besides the standard library.

    python pc/tools/shots.py out.png a.bmp b.bmp ...
"""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    stride = ((w * bpp // 8) + 3) & ~3
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        r = d[off + sy * stride: off + sy * stride + w * bpp // 8]
        px = []
        for x in range(w):
            b, g, rr = r[x * bpp // 8], r[x * bpp // 8 + 1], r[x * bpp // 8 + 2]
            px.append((rr, g, b))
        rows.append(px)
    return rows


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for p in r for c in p) for r in rows)

    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    out, files = sys.argv[1], sys.argv[2:]
    imgs = [read_bmp(f) for f in files]
    cols = min(4, len(imgs))
    nrow = (len(imgs) + cols - 1) // cols
    W, H, S = 160, 144, 2
    canvas = [[(40, 40, 40)] * (cols * (W * S + 4)) for _ in range(nrow * (H * S + 4))]
    for i, im in enumerate(imgs):
        ox, oy = (i % cols) * (W * S + 4), (i // cols) * (H * S + 4)
        for y in range(H * S):
            row = im[y // S]
            for x in range(W * S):
                canvas[oy + y][ox + x] = row[x // S]
    write_png(out, canvas)


if __name__ == "__main__":
    main()
