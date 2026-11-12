#!/usr/bin/env python3
"""Compare the editor's GPU decoder (decodetest.exe) with ImageMagick on every layer and the composite.

usage: python -I compare_decode.py <decodetest.exe> <file.psd|psb> [--tolerance N]

The decoder outputs premultiplied RGBA8; ImageMagick's straight RGBA8 is premultiplied the same
way ((c * a + 127) // 255) before comparing. 16-bit files need --tolerance 2: the editor keeps the
high byte of each sample, ImageMagick rounds.
"""
import os
import subprocess
import sys
import tempfile


def read_pam(path):
    with open(path, "rb") as f:
        data = f.read()
    end = data.index(b"ENDHDR\n") + 7
    header = dict(line.split(b" ", 1) for line in data[:end].split(b"\n")[1:-2])
    return int(header[b"WIDTH"]), int(header[b"HEIGHT"]), data[end:]


def main():
    tool, psd = sys.argv[1], sys.argv[2]
    tolerance = int(sys.argv[sys.argv.index("--tolerance") + 1]) if "--tolerance" in sys.argv else 0
    dump = subprocess.run([tool.replace("decodetest", "psdcheck"), "dump", psd], capture_output=True, text=True, encoding="utf-8")
    layers = int(dump.stdout.split("layers=")[1].split()[0])
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for record in list(range(layers)) + [-1]:
            pam = os.path.join(tmp, "out.pam")
            r = subprocess.run([tool, psd, str(record), pam], capture_output=True, text=True)
            label = "composite" if record < 0 else f"layer {record}"
            if r.returncode == 3:
                print(f"{label}: empty")
                continue
            if r.returncode != 0:
                print(f"{label}: decodetest failed: {r.stderr.strip()}")
                failures += 1
                continue
            w, h, ours = read_pam(pam)
            index = 0 if record < 0 else record + 1
            im = subprocess.run(["magick", f"{psd}[{index}]", "-depth", "8", "rgba:-"], capture_output=True)
            ref = im.stdout
            if b"CompressionNotSupported" in im.stderr:
                # ImageMagick can't read this composite (e.g. its own ZIP one) and blends the
                # layers instead: no reference to compare with.
                print(f"{label}: {w}x{h} SKIPPED (ImageMagick can't read it: {im.stderr.decode(errors='replace').strip()})")
                continue
            if len(ref) != w * h * 4:
                print(f"{label}: size mismatch: ours {w}x{h}, ImageMagick {len(ref)} bytes")
                failures += 1
                continue
            worst, bad = 0, 0
            for i in range(0, len(ref), 4):
                a = ref[i + 3]
                expect = [(ref[i + k] * a + 127) // 255 for k in range(3)] + [a]
                diff = max(abs(expect[k] - ours[i + k]) for k in range(4))
                worst = max(worst, diff)
                bad += diff > tolerance
            status = "OK" if bad == 0 else "FAIL"
            failures += bad > 0
            print(f"{label}: {w}x{h} {status} (max diff {worst}, {bad} pixels over tolerance {tolerance}) {r.stderr.strip()}")
    print("ALL OK" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
