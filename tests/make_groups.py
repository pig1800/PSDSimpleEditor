#!/usr/bin/env python3
"""Writes a small 8-bit RGB PSD with nested groups and the image resources that list layers by
position (1024 target layer, 1026 layer group IDs, 1072 layer-group-enabled IDs), for testing
moves. Every layer has distinct raw pixel data.

usage: python -I make_groups.py <out.psd>

Records, bottom first (as psdcheck dump lists them):
  0 bg   1 </end G>   2 a   3 </end H>   4 h1   5 H (group, closed)   6 b   7 G (group, open)   8 top
Tree, top first:  top / G { b, H { h1 }, a } / bg
"""
import random
import struct
import sys

W, H = 64, 48
rnd = random.Random(1)


def record(name, rect, kind=None, alpha=True):
    top, left, bottom, right = rect
    w, h = right - left, bottom - top
    ids = ([-1] if alpha else []) + [0, 1, 2]
    data = {cid: struct.pack(">H", 0) + bytes(rnd.randrange(256) for _ in range(w * h)) for cid in ids}
    rec = struct.pack(">4iH", top, left, bottom, right, len(ids))
    for cid in ids:
        rec += struct.pack(">hI", cid, len(data[cid]))
    rec += b"8BIMnorm" + bytes([255, 0, 0 if kind != 3 else 2, 0])
    legacy = name.encode("utf-8")
    pascal = bytes([len(legacy)]) + legacy
    pascal += bytes((-len(pascal)) % 4)
    units = name.encode("utf-16-be")
    luni = struct.pack(">I", len(units) // 2) + units
    luni += bytes((-len(luni)) % 4)
    extra = struct.pack(">II", 0, 0) + pascal + b"8BIMluni" + struct.pack(">I", len(luni)) + luni
    if kind is not None:
        extra += b"8BIMlsct" + struct.pack(">II", 4, kind)
    return rec + struct.pack(">I", len(extra)) + extra, b"".join(data[c] for c in ids)


def main():
    layers = [
        record("bg", (0, 0, 48, 64), alpha=False),
        record("</Layer group>", (0, 0, 0, 0), kind=3),
        record("a", (2, 3, 12, 20)),
        record("</Layer group>", (0, 0, 0, 0), kind=3),
        record("h1", (10, 10, 30, 22)),
        record("H", (0, 0, 0, 0), kind=2),
        record("b", (5, 30, 25, 41)),
        record("G", (0, 0, 0, 0), kind=1),
        record("top", (30, 40, 47, 63)),
    ]
    n = len(layers)
    info = struct.pack(">h", n) + b"".join(r for r, _ in layers) + b"".join(d for _, d in layers)
    info += bytes(len(info) % 2)
    lm = struct.pack(">I", len(info)) + info + struct.pack(">I", 0)

    def resource(rid, data):
        return b"8BIM" + struct.pack(">H", rid) + b"\0\0" + struct.pack(">I", len(data)) + data + bytes(len(data) & 1)
    resources = (resource(1024, struct.pack(">H", 6))                       # target: "b"
                 + resource(1026, b"".join(struct.pack(">H", 100 + i) for i in range(n)))
                 + resource(1072, bytes(range(10, 10 + n))))
    with open(sys.argv[1], "wb") as f:
        f.write(b"8BPS" + struct.pack(">H6xHIIHH", 1, 3, H, W, 8, 3))
        f.write(struct.pack(">I", 0) + struct.pack(">I", len(resources)) + resources)
        f.write(struct.pack(">I", len(lm)) + lm)
        f.write(struct.pack(">H", 0) + bytes(rnd.randrange(256) for _ in range(W * H * 3)))


if __name__ == "__main__":
    main()
