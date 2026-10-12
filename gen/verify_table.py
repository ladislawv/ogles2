#!/usr/bin/env python3
"""Check the dispatch table in the LINKED library (plan section 6.6), not just
the C header's layout: the header words, and that every slot is relocated to
the function of the same name (ogles2_<entry>), in abi_order.txt's order.

Usage: verify_table.py ogles2.library.debug [abi_order.txt]
Reads the AmigaOS hunk file directly (HUNK_SYMBOL and HUNK_RELOC32).
"""
import os, struct, sys

HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END = 0x3EC, 0x3F0, 0x3F1, 0x3F2
HUNK_HEADER, HUNK_RELOC32SHORT, HUNK_DREL32 = 0x3F3, 0x3FC, 0x3F7
ABI_VERSION, BACKEND_FLAGS = 1, 1


def read_hunks(path):
    d = open(path, "rb").read()
    o = 0

    def u32():
        nonlocal o
        v = struct.unpack_from(">I", d, o)[0]
        o += 4
        return v

    if u32() != HUNK_HEADER:
        sys.exit("not a hunk file")
    while u32():  # resident library names
        pass
    count, first, last = u32(), u32(), u32()
    for _ in range(last - first + 1):
        u32()
    hunks = []
    cur = None
    while o < len(d):
        t = u32() & 0x3FFFFFFF
        if t in (HUNK_CODE, HUNK_DATA):
            n = u32() * 4
            cur = {"data": d[o:o + n], "relocs": {}, "symbols": {}}
            o += n
        elif t == HUNK_BSS:
            u32()
            cur = {"data": b"", "relocs": {}, "symbols": {}}
        elif t == HUNK_RELOC32:
            while True:
                n = u32()
                if not n:
                    break
                target = u32()
                for _ in range(n):
                    cur["relocs"][u32()] = target
        elif t == HUNK_RELOC32SHORT or t == HUNK_DREL32:
            while True:
                n = struct.unpack_from(">H", d, o)[0]; o += 2
                if not n:
                    break
                target = struct.unpack_from(">H", d, o)[0]; o += 2
                for _ in range(n):
                    cur["relocs"][struct.unpack_from(">H", d, o)[0]] = target; o += 2
            if o % 4:
                o += 2
        elif t == HUNK_SYMBOL:
            while True:
                n = u32()
                if not n:
                    break
                name = d[o:o + n * 4].rstrip(b"\0").decode()
                o += n * 4
                cur["symbols"][name] = u32()
        elif t == HUNK_DEBUG:
            o += u32() * 4
        elif t == HUNK_END:
            hunks.append(cur)
            cur = None
        else:
            sys.exit("unhandled hunk type 0x%x at %d" % (t, o - 4))
    return hunks


def main():
    lib = sys.argv[1]
    order_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "abi_order.txt")
    order = [l.split("#", 1)[0].strip() for l in open(order_path)]
    order = [l for l in order if l]
    hunks = read_hunks(lib)
    where = [(i, h["symbols"]["_ogles2_dispatch_table"]) for i, h in enumerate(hunks)
             if "_ogles2_dispatch_table" in h["symbols"]]
    if not where:
        sys.exit("FAIL: no _ogles2_dispatch_table symbol (link with symbols, not stripped)")
    hi, table = where[0]
    h = hunks[hi]
    # symbol value -> name, per hunk
    names = {}
    for j, hk in enumerate(hunks):
        for n, v in hk["symbols"].items():
            names[(j, v)] = n
    abi, size, flags, reserved = struct.unpack_from(">4I", h["data"], table)
    errors = []
    if (abi, size, flags, reserved) != (ABI_VERSION, 16 + 4 * len(order), BACKEND_FLAGS, 0):
        errors.append("header %s, expected %s" % ((abi, size, flags, reserved),
                                                  (ABI_VERSION, 16 + 4 * len(order), 1, 0)))
    for i, entry in enumerate(order):
        slot = table + 16 + 4 * i
        if entry.startswith("-"):
            continue
        if slot not in h["relocs"]:
            errors.append("slot %d (%s): no relocation, NULL or absolute pointer" % (i, entry))
            continue
        target = h["relocs"][slot]
        value = struct.unpack_from(">I", h["data"], slot)[0]
        got = names.get((target, value), "?")
        # the function itself, or its share-group wrapper (ogles2/share.c)
        if got not in ("_ogles2_" + entry, "_ogles2_lk_" + entry):
            errors.append("slot %d: %s, expected _ogles2_%s" % (i, got, entry))
    if errors:
        print("FAIL: %d problem(s)" % len(errors))
        for e in errors[:20]:
            print("  " + e)
        sys.exit(1)
    print("PASS: linked table: ABI %d, %d bytes, all %d slots point to the matching ogles2_* "
          "function or its ogles2_lk_* wrapper" % (abi, size, len(order)))


if __name__ == "__main__":
    main()
