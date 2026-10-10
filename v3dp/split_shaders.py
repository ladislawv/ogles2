#!/usr/bin/env python3
"""Splits shader-source records (ogles2's T:ogles2_missing_shaders.txt, or
the corpus harness's corpus_shaders.txt) into one .vert/.frag pair per
distinct program, for v3dglslc.

    split_shaders.py RECORDS OUTDIR

A record is
    --- program <name or hash>
    --- vertex shader
    <source, exactly>
    --- fragment shader
    <source, exactly>
    --- end
The sources are kept byte for byte: the runtime finds a program's binary by
the BLAKE3 hash of exactly these bytes (v3dp/FORMAT.md). Pairs are named
p00.vert/p00.frag, ... in order of first appearance; duplicates are dropped.
"""
import os
import sys


def records(data):
    vs_tag, fs_tag, end_tag = b"\n--- vertex shader\n", b"\n--- fragment shader\n", b"\n--- end\n"
    pos = 0
    while True:
        start = data.find(b"--- program ", pos)
        if start < 0:
            return
        a = data.index(vs_tag, start)
        b = data.index(fs_tag, a)
        c = data.index(end_tag, b)
        yield data[start + 12:a].decode("ascii", "replace"), data[a + len(vs_tag):b], data[b + len(fs_tag):c]
        pos = c + len(end_tag)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    seen = {}
    with open(src, "rb") as f:
        data = f.read()
    for name, vs, fs in records(data):
        if (vs, fs) in seen:
            continue
        stem = "p%02d" % len(seen)
        seen[(vs, fs)] = stem
        with open(os.path.join(out, stem + ".vert"), "wb") as f:
            f.write(vs)
        with open(os.path.join(out, stem + ".frag"), "wb") as f:
            f.write(fs)
        print("%s  %s" % (stem, name))


if __name__ == "__main__":
    main()
