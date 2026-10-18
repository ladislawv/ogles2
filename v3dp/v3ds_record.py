#!/usr/bin/env python3
"""Writes the shader records (<24 hex digits>.v3ds) that glCompileShader
looks for in a program's shaders/ drawer (ogles2/program.c, plan 8.4): with
the compiler installed, a shader without a record starts it, even when the
program ships its .v3dp binaries. A record names the shader by BLAKE3 over
its stage (4 bytes, little-endian: 0 vertex, 4 fragment), its length (4
bytes, little-endian) and its text, and holds "v3ds 1\\n" followed by the
compiler's info log: here none, as the Amiga's compiler writes for a shader
it accepts without messages. Each file must hold the shader byte for byte
as the program passes it to glShaderSource (LF line ends, nothing added).
usage: v3ds_record.py OUTDIR SHADER... (stage by extension: .vert or .vs
vertex, .frag or .fs fragment)"""
import os
import struct
import sys

# BLAKE3, hash mode, 32-byte output (the specification's reference_impl.rs),
# as ogles2/blake3.c implements it on the Amiga.
IV = (0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
      0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19)
PERM = (2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8)
CHUNK_START, CHUNK_END, PARENT, ROOT = 1, 2, 4, 8
M32 = 0xFFFFFFFF


def g(s, a, b, c, d, mx, my):
    s[a] = (s[a] + s[b] + mx) & M32
    x = s[d] ^ s[a]
    s[d] = ((x >> 16) | (x << 16)) & M32
    s[c] = (s[c] + s[d]) & M32
    x = s[b] ^ s[c]
    s[b] = ((x >> 12) | (x << 20)) & M32
    s[a] = (s[a] + s[b] + my) & M32
    x = s[d] ^ s[a]
    s[d] = ((x >> 8) | (x << 24)) & M32
    s[c] = (s[c] + s[d]) & M32
    x = s[b] ^ s[c]
    s[b] = ((x >> 7) | (x << 25)) & M32


def compress(cv, m, counter, block_len, flags):
    s = list(cv[:8]) + list(IV[:4]) + [counter & M32, (counter >> 32) & M32, block_len, flags]
    m = list(m)
    for r in range(7):
        g(s, 0, 4, 8, 12, m[0], m[1])
        g(s, 1, 5, 9, 13, m[2], m[3])
        g(s, 2, 6, 10, 14, m[4], m[5])
        g(s, 3, 7, 11, 15, m[6], m[7])
        g(s, 0, 5, 10, 15, m[8], m[9])
        g(s, 1, 6, 11, 12, m[10], m[11])
        g(s, 2, 7, 8, 13, m[12], m[13])
        g(s, 3, 4, 9, 14, m[14], m[15])
        if r < 6:
            m = [m[p] for p in PERM]
    for i in range(8):
        s[i] ^= s[i + 8]
        s[i + 8] ^= cv[i]
    return s


def blake3(data):
    chunks = [data[i:i + 1024] for i in range(0, len(data), 1024)] or [b""]
    stack = []
    for n, chunk in enumerate(chunks):
        cv = IV
        blocks = [chunk[i:i + 64] for i in range(0, len(chunk), 64)] or [b""]
        for k, block in enumerate(blocks):
            flags = (CHUNK_START if k == 0 else 0) | (CHUNK_END if k == len(blocks) - 1 else 0)
            words = struct.unpack("<16I", block.ljust(64, b"\0"))
            if n == len(chunks) - 1 and k == len(blocks) - 1:
                node = (cv, words, n, len(block), flags)  # the last block: the output node
            else:
                cv = compress(cv, words, n, len(block), flags)[:8]
        if n < len(chunks) - 1:
            # A completed chunk merges with the subtrees it completes.
            total = n + 1
            while total & 1 == 0:
                cv = compress(IV, stack.pop() + cv, 0, 64, PARENT)[:8]
                total >>= 1
            stack.append(cv)
    while stack:
        node = (IV, stack.pop() + compress(*node)[:8], 0, 64, PARENT)
    cv, words, counter, block_len, flags = node
    return struct.pack("<8I", *compress(cv, words, counter, block_len, flags | ROOT)[:8])


def record_name(stage, text):
    return blake3(struct.pack("<II", stage, len(text)) + text)[:12].hex() + ".v3ds"


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    assert blake3(b"").hex() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for path in sys.argv[2:]:
        ext = os.path.splitext(path)[1]
        if ext in (".vert", ".vs"):
            stage = 0
        elif ext in (".frag", ".fs"):
            stage = 4
        else:
            raise SystemExit("%s: neither .vert/.vs nor .frag/.fs" % path)
        text = open(path, "rb").read()
        name = record_name(stage, text)
        with open(os.path.join(out, name), "wb") as f:
            f.write(b"v3ds 1\n")
        print("%s -> %s" % (os.path.basename(path), name))


if __name__ == "__main__":
    main()
