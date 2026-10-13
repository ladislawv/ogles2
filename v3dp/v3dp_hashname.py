#!/usr/bin/env python3
"""Renames .v3dp files to the name the runtime looks for (FORMAT.md, PROGRAM):
the program hash's first 24 hex digits + ".v3dp", 29 characters (FFS file
names are at most 30); a .v3dw file (the same program's variants for
32-bit samplers) keeps its extension. usage: v3dp_hashname.py FILE..."""
import os
import struct
import sys


def program_hash(path):
    data = open(path, "rb").read()
    if data[:4] != b"V3DP":
        raise SystemExit("%s: not a .v3dp file" % path)
    sec_type, sec_len = struct.unpack_from("<II", data, 32)
    if sec_type != 1 or sec_len < 32:
        raise SystemExit("%s: the first section is not PROGRAM" % path)
    return data[40:72].hex()[:24]


for path in sys.argv[1:]:
    ext = ".v3dw" if path.endswith(".v3dw") else ".v3dp"
    new = os.path.join(os.path.dirname(path), program_hash(path) + ext)
    if os.path.abspath(new) != os.path.abspath(path):
        os.replace(path, new)
    print("%s -> %s" % (os.path.basename(path), os.path.basename(new)))
