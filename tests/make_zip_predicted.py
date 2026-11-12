#!/usr/bin/env python3
"""Writes a PSD whose layer channels use ZIP with prediction (compression 3, what Photoshop uses
for 16-bit layers), plus the premultiplied RGBA8 the editor must decode it to.

usage: python -I make_zip_predicted.py <out.psd> <expected.pam> <depth 8|16> [lr16]

lr16: put the layer info in an 'Lr16' tagged block (as Photoshop does for 16-bit documents).
"""
import random
import struct
import sys
import zlib


def predicted(plane, w, h, depth):
    out = bytearray()
    mask = 0xFF if depth == 8 else 0xFFFF
    for y in range(h):
        row = plane[y * w:(y + 1) * w]
        prev = 0
        for x, v in enumerate(row):
            d = v if x == 0 else (v - prev) & mask
            out += struct.pack(">B" if depth == 8 else ">H", d)
            prev = v
    return zlib.compress(bytes(out), 9)


def main():
    out_psd, out_pam, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
    rnd = random.Random(42)
    W, H = 64, 48                      # document
    top, left, bottom, right = 5, -3, 28, 34   # layer: 37 x 23, partly outside the canvas
    w, h = right - left, bottom - top
    maxv = 255 if depth == 8 else 65535
    # Smooth-ish random planes (so prediction matters), plus a transparency gradient.
    planes = {}
    for cid in (0, 1, 2):
        base = rnd.randrange(maxv)
        planes[cid] = [(base + x * rnd.randrange(maxv // 64 + 1) + y * 977 * (cid + 1)) % (maxv + 1) for y in range(h) for x in range(w)]
    planes[-1] = [min(maxv, (x * maxv) // (w - 1)) if y % 5 else maxv // 3 for y in range(h) for x in range(w)]

    name = "预测 ZIP レイヤー"
    channel_data = {cid: struct.pack(">H", 3) + predicted(planes[cid], w, h, depth) for cid in (-1, 0, 1, 2)}
    rec = struct.pack(">4i", top, left, bottom, right) + struct.pack(">H", 4)
    for cid in (-1, 0, 1, 2):
        rec += struct.pack(">hI", cid, len(channel_data[cid]))
    rec += b"8BIMnorm" + bytes([255, 0, 0, 0])
    legacy = name.encode("utf-8")
    pascal = bytes([len(legacy)]) + legacy
    pascal += b"\0" * ((-len(pascal)) % 4)
    units = name.encode("utf-16-be")
    luni_data = struct.pack(">I", len(units) // 2) + units
    luni_data += b"\0" * ((-len(luni_data)) % 4)
    luni = b"8BIMluni" + struct.pack(">I", len(luni_data)) + luni_data
    extra = struct.pack(">I", 0) + struct.pack(">I", 0) + pascal + luni
    rec += struct.pack(">I", len(extra)) + extra
    layer_info = struct.pack(">h", 1) + rec + b"".join(channel_data[c] for c in (-1, 0, 1, 2))
    layer_info += b"\0" * (len(layer_info) % 2)
    if len(sys.argv) > 4 and sys.argv[4] == "lr16":
        block = layer_info + bytes((-len(layer_info)) % 4)
        lm = struct.pack(">I", 0) + struct.pack(">I", 0) + b"8BIMLr16" + struct.pack(">I", len(block)) + block
    else:
        lm = struct.pack(">I", len(layer_info)) + layer_info + struct.pack(">I", 0)

    bps = depth // 8
    composite = struct.pack(">H", 0) + bytes(W * H * 3 * bps)  # raw, black
    with open(out_psd, "wb") as f:
        f.write(b"8BPS" + struct.pack(">H6xHIIHH", 1, 3, H, W, depth, 3))
        f.write(struct.pack(">I", 0) + struct.pack(">I", 0))
        f.write(struct.pack(">I", len(lm)) + lm)
        f.write(composite)

    hi = (lambda v: v) if depth == 8 else (lambda v: v >> 8)  # the editor keeps the high byte
    px = bytearray()
    for i in range(w * h):
        a = hi(planes[-1][i])
        px += bytes([(hi(planes[c][i]) * a + 127) // 255 for c in (0, 1, 2)] + [a])
    with open(out_pam, "wb") as f:
        f.write(b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (w, h) + px)


if __name__ == "__main__":
    main()
