#!/usr/bin/env python3
"""Compares the corpus captures from the A1200 with the PC's references.

    compare.py REFDIR SHOTDIR [OUTDIR]

For each REFDIR/<name>.ppm with a SHOTDIR/<name>.ppm: the share of pixels
whose largest channel difference exceeds TOL (edges and filtering differ a
little between llvmpipe and V3D), PASS below LIMIT percent. OUTDIR gets
<name>.png: reference | capture | difference (x4), side by side.
Standard library only.
"""
import os
import struct
import sys
import zlib

TOL = 40       # per channel, of 255
LIMIT = 2.0    # percent of the pixels


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:  # P6, width, height, maxval; comments are not used here
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    pos += 1
    assert parts[0] == b"P6", path
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ref_dir, shot_dir = sys.argv[1], sys.argv[2]
    out_dir = sys.argv[3] if len(sys.argv) > 3 else None
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    failed = 0
    for name in sorted(n[:-4] for n in os.listdir(ref_dir) if n.endswith(".ppm")):
        rw, rh, ref = read_ppm(os.path.join(ref_dir, name + ".ppm"))
        shot_path = os.path.join(shot_dir, name + ".ppm")
        if not os.path.exists(shot_path):
            print("%-22s MISSING" % name)
            failed += 1
            continue
        sw, sh, shot = read_ppm(shot_path)
        if (sw, sh) != (rw, rh):
            print("%-22s FAIL size %dx%d, reference %dx%d" % (name, sw, sh, rw, rh))
            failed += 1
            continue
        bad = 0
        diff = bytearray(len(ref))
        for i in range(0, len(ref), 3):
            d = max(abs(ref[i] - shot[i]), abs(ref[i + 1] - shot[i + 1]), abs(ref[i + 2] - shot[i + 2]))
            if d > TOL:
                bad += 1
            diff[i] = diff[i + 1] = diff[i + 2] = min(255, d * 4)
        pct = 100.0 * bad / (rw * rh)
        ok = pct < LIMIT
        failed += not ok
        print("%-22s %s %.2f%% of the pixels differ by more than %d" % (name, "PASS" if ok else "FAIL", pct, TOL))
        if out_dir:
            row = []
            for y in range(rh):
                s = slice(y * rw * 3, (y + 1) * rw * 3)
                row.append(bytes(ref[s]) + bytes(shot[s]) + bytes(diff[s]))
            write_png(os.path.join(out_dir, name + ".png"), rw * 3, rh, b"".join(row))
    print("%d failed" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
