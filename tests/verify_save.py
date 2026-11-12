#!/usr/bin/env python3
"""Independent check of an edited PSD/PSB against its source (no shared code with the C++ writer).

usage: python -I verify_save.py <original> <saved> [--order i,j,...] [<index>=<expected name> ...]

--order: the expected record order of the saved file (bottom first) as original record indices.
Indices in <index>=<name> are original record indices.

Checks that every byte is identical except, in the renamed records, the Pascal name field, the
'luni' block and the extra-data length, plus the enclosing section length fields; that records
and their channel data appear in the expected order, each block byte-identical; that the image
resources are identical except 1024/1026/1072, which must follow the new order.
"""
import hashlib
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8")
WIDE = {b"LMsk", b"Lr16", b"Lr32", b"Layr", b"Mt16", b"Mt32", b"Mtrn", b"Alph", b"FMsk", b"lnk2", b"FEid", b"FXid", b"PxSD"}


class Psd:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        self.f.seek(0)
        assert self.rd(4) == b"8BPS"
        self.psb = self.u16() == 2
        self.f.seek(26)
        self.f.seek(self.u32(), 1)                   # colour mode data
        ir_len = self.u32()
        self.ir_start = self.f.tell()
        self.resources = []                          # (id, data offset, size)
        while self.f.tell() + 12 <= self.ir_start + ir_len:
            self.rd(4)
            rid = self.u16()
            nl = self.u8()
            self.f.seek(nl + ((1 + nl) & 1), 1)
            n = self.u32()
            self.resources.append((rid, self.f.tell(), n))
            self.f.seek(n + (n & 1), 1)
        self.f.seek(self.ir_start + ir_len)
        self.lm_len_off = self.f.tell()
        self.lm_len = self.ln()
        li_len_off = self.f.tell()
        li_len = self.ln()
        if li_len:
            self.cont_len = li_len
            self.records(self.f.tell())
        else:
            self.f.seek(self.u32(), 1)               # global layer mask info
            while True:
                sig, key = self.rd(4), self.rd(4)
                assert sig in (b"8BIM", b"8B64"), "no layer records found"
                n = self.ln(key in WIDE)
                if key in (b"Lr16", b"Lr32"):
                    self.cont_len = n
                    self.records(self.f.tell())
                    break
                self.f.seek(n, 1)

    def rd(self, n): return self.f.read(n)
    def u8(self): return self.rd(1)[0]
    def u16(self): return struct.unpack(">H", self.rd(2))[0]
    def u32(self): return struct.unpack(">I", self.rd(4))[0]
    def ln(self, wide=True): return struct.unpack(">Q", self.rd(8))[0] if (self.psb and wide) else self.u32()

    def at(self, a, b):
        self.f.seek(a)
        return self.f.read(b - a)

    def records(self, start):
        self.f.seek(start)
        self.count = struct.unpack(">h", self.rd(2))[0]
        self.recs = []
        for _ in range(abs(self.count)):
            r = {"start": self.f.tell()}
            self.f.seek(16, 1)
            nch = self.u16()
            r["chans"] = [(struct.unpack(">h", self.rd(2))[0], self.ln()) for _ in range(nch)]
            self.f.seek(12, 1)
            r["xlen_off"] = self.f.tell()
            xlen = self.u32()
            xend = self.f.tell() + xlen
            self.f.seek(self.u32(), 1)                # mask
            self.f.seek(self.u32(), 1)                # blending ranges
            r["name_start"] = self.f.tell()
            nl = self.u8()
            r["pascal"] = self.rd(nl)
            r["name_end"] = r["name_start"] + ((1 + nl + 3) & ~3)
            self.f.seek(r["name_end"])
            r["blocks"] = []
            while self.f.tell() + 12 <= xend:
                bstart = self.f.tell()
                sig, key = self.rd(4), self.rd(4)
                if sig not in (b"8BIM", b"8B64"):
                    break
                n = self.ln(key in WIDE)
                data = self.f.tell()
                if key == b"luni":
                    cnt = self.u32()
                    r["luni"] = self.rd(2 * cnt).decode("utf-16-be").rstrip("\0")
                self.f.seek(data + n)
                r["blocks"].append((key, bstart, data + n))
            r["end"] = xend
            self.f.seek(xend)
            self.recs.append(r)
        pos = self.f.tell()
        for r in self.recs:                           # channel data: records' order, contiguous
            r["data"] = (pos, pos + sum(n for _, n in r["chans"]))
            pos = r["data"][1]
        self.data_end = pos


def sha(f, a, b):
    h = hashlib.sha256()
    f.seek(a)
    left = b - a
    while left:
        chunk = f.read(min(left, 8 << 20))
        h.update(chunk)
        left -= len(chunk)
    return h.hexdigest()


def main():
    a, b = Psd(sys.argv[1]), Psd(sys.argv[2])
    expected, order = {}, None
    args = iter(sys.argv[3:])
    for arg in args:
        if arg == "--order":
            order = [int(x) for x in next(args).split(",")]
        else:
            i, name = arg.split("=", 1)
            expected[int(i)] = name
    n = len(a.recs)
    order = order or list(range(n))
    assert sorted(order) == list(range(n)), "--order must be a permutation of all records"
    delta = b.size - a.size
    problems = []
    check = lambda cond, msg: cond or problems.append(msg)

    # Header and colour mode data; resources: same list, same data except the order-dependent ones.
    check(a.at(0, a.ir_start) == b.at(0, b.ir_start), "header / colour mode data differ")
    check([(r[0], r[2]) for r in a.resources] == [(r[0], r[2]) for r in b.resources], "resource list differs")
    new_index = {orig: j for j, orig in enumerate(order)}
    section = bytearray(a.at(a.ir_start, a.lm_len_off))  # expected resource section of the saved file
    for rid, off, size in a.resources:
        data = a.at(off, off + size)
        if rid == 1024 and size == 2 and struct.unpack(">H", data)[0] < n:
            data = struct.pack(">H", new_index[struct.unpack(">H", data)[0]])
        elif rid in (1026, 1072) and size == n * (2 if rid == 1026 else 1):
            e = 2 if rid == 1026 else 1
            data = b"".join(data[i * e:(i + 1) * e] for i in order)
        section[off - a.ir_start:off - a.ir_start + size] = data
    check(bytes(section) == b.at(b.ir_start, b.lm_len_off), "image resources are not as expected")
    check(b.lm_len == a.lm_len + delta, "layer & mask length not shifted by delta")
    check(b.cont_len == a.cont_len + delta, "layer container length not shifted by delta")
    check(a.count == b.count, "layer count differs")

    renamed = set()
    for j, rb in enumerate(b.recs):
        i = order[j]
        ra = a.recs[i]
        bytes_a, bytes_b = a.at(ra["start"], ra["end"]), b.at(rb["start"], rb["end"])
        check(sha(a.f, *ra["data"]) == sha(b.f, *rb["data"]), f"record {i} (now {j}): channel data differs")
        if bytes_a == bytes_b:
            check(i not in expected, f"record {i}: expected a rename, bytes unchanged")
            continue
        renamed.add(i)
        check(i in expected, f"record {i}: changed but not in the expected renames")
        check(a.at(ra["start"], ra["xlen_off"]) == b.at(rb["start"], rb["xlen_off"]), f"record {i}: fixed part differs")
        check(a.at(ra["xlen_off"] + 4, ra["name_start"]) == b.at(rb["xlen_off"] + 4, rb["name_start"]), f"record {i}: mask/blend ranges differ")
        blocks_a = [a.at(s, e) for k, s, e in ra["blocks"] if k != b"luni"]
        blocks_b = [b.at(s, e) for k, s, e in rb["blocks"] if k != b"luni"]
        check(blocks_a == blocks_b, f"record {i}: non-luni tagged blocks differ")
        check(rb.get("luni") == expected.get(i), f"record {i}: luni {rb.get('luni')!r} != {expected.get(i)!r}")
        check((rb["end"] - rb["start"] - (ra["end"] - ra["start"])) % 4 == 0, f"record {i}: size change not a multiple of 4")
    check(sha(a.f, a.data_end, a.size) == sha(b.f, b.data_end, b.size), "global layer info / composite differ")
    check(renamed == set(expected), f"renamed {sorted(renamed)} != expected {sorted(expected)}")
    for p in problems:
        print("FAIL:", p)
    if not problems:
        moved = sum(1 for j, i in enumerate(order) if i != j)
        print(f"OK: {len(renamed)} renamed, {moved} records at new positions, size delta {delta}, everything else identical")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
