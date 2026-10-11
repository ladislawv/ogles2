#!/usr/bin/env python3
"""Callee-saved-register check on GCC assembly output (-S), m68k Motorola syntax.

Works on the compiler's own .s files rather than disassembly: jump tables and
strings are data directives there, and every function, static ones included,
has a label, so the check has no decoding artifacts.

A function is reported when an instruction writes a callee-saved register
(d2-d7, a2-a6, fp2-fp7) that the prologue did not save. The prologue is the
run of instructions before the first one that is not a save, frame setup or
stack adjustment.
usage: check_saves_asm.py file.s ...
"""
import re, sys

CALLEE = ({f"d{i}" for i in range(2, 8)} | {f"a{i}" for i in range(2, 7)}
          | {f"fp{i}" for i in range(2, 8)})
REG = r"(?:%?)(d[0-7]|a[0-7]|fp[0-7]|sp)"


NAMES = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]


def regs_in_list(s, predec=False):
    """Register list "d2-d3/a2" or numeric mask "#12320" (GCC 16.2 emits these).
    For movem with -(An) the mask bits are reversed (bit 15 = d0)."""
    s = s.strip()
    if s.startswith("#"):
        v = int(s[1:], 0)
        return {NAMES[15 - b] if predec else NAMES[b] for b in range(16) if v & (1 << b)}
    out = set()
    for part in s.replace("%", "").split("/"):
        m = re.fullmatch(r"(fp|[ad])(\d)-(?:fp|[ad])?(\d)", part)
        if m:
            out |= {f"{m.group(1)}{i}" for i in range(int(m.group(2)), int(m.group(3)) + 1)}
        elif re.fullmatch(r"(fp|[ad])\d", part):
            out.add(part)
    return out


def split_ops(s):
    return [p.strip() for p in re.split(r",(?![^(]*\))", s)]


def scan(path):
    bad, fn, ins = [], None, []

    def check():
        if not fn or not ins:
            return
        saved, written = set(), set()
        prologue = True
        for op, ops in ins:
            parts = split_ops(ops) if ops else []
            dst = parts[-1].replace("%", "") if parts else ""
            is_save = False
            if op.startswith("link"):
                saved.add(parts[0].replace("%", "")); is_save = True
            elif op.startswith("fmovem") and dst in ("-(sp)", "-(a7)") and parts[0].startswith("#"):
                # FMOVEM numeric mask, predecrement mode: bit n = fpn.
                v = int(parts[0][1:], 0)
                saved |= {f"fp{n}" for n in range(8) if v & (1 << n)}; is_save = True
            elif op.startswith(("movem", "fmovem")) and dst in ("-(sp)", "-(a7)"):
                saved |= regs_in_list(parts[0], predec=op.startswith("movem")); is_save = True
            elif op.startswith("move.l") and dst in ("-(sp)", "-(a7)") and re.fullmatch(r"%?[ad][2-7]", parts[0]):
                saved.add(parts[0].replace("%", "")); is_save = True
            elif op.startswith(("fmove.x", "fmovem.x")) and dst in ("-(sp)", "-(a7)"):
                saved |= regs_in_list(parts[0]); is_save = True
            elif op.startswith(("subq", "sub.", "lea")) and dst in ("sp", "a7") and prologue:
                is_save = True
            if prologue and not is_save:
                prologue = False
            if is_save and prologue:
                continue
            if op.startswith(("movem", "fmovem")):
                # Loads into registers; restores from (sp)+ are the epilogue.
                if not parts[0].startswith("(sp)+") and not dst.startswith(("-(sp)", "(sp)", "#")):
                    written |= regs_in_list(dst)
                continue
            if op.startswith(("cmp", "tst", "btst", "pea", "jsr", "jmp", "bra", "b", "db", "rts", "chk", "trap")) \
                    and not op.startswith(("bfins", "bfext", "bfset", "bfclr", "bfchg", "bset", "bclr", "bchg")):
                continue
            # 68020 two-register forms (divsl/divul/mulsl/mulul Dr:Dq) write both.
            m = re.fullmatch(r"(d\d):(d\d)", dst)
            if m:
                written |= {m.group(1), m.group(2)}
            elif dst in CALLEE:
                written.add(dst)
            elif op.startswith(("exg",)) and len(parts) == 2:
                written |= {p.replace("%", "") for p in parts}
        missing = (written & CALLEE) - saved
        if missing:
            bad.append((fn, sorted(missing)))

    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        m = re.match(r"^([A-Za-z_.$][\w.$]*):", line)
        if m and not m.group(1).startswith((".L", "L")):
            check()
            fn, ins = m.group(1), []
            continue
        m = re.match(r"^\s+([a-z][a-z0-9.]*)(?:\s+(.*))?$", line)
        if m and fn and not m.group(1).startswith("."):
            ins.append((m.group(1), (m.group(2) or "").split("|")[0].strip()))
    check()
    return bad


def main():
    total = 0
    for p in sys.argv[1:]:
        for fn, regs in scan(p):
            total += 1
            print(f"{p.rsplit('/', 1)[-1]}: {fn[:110]}  clobbers {','.join(regs)}")
    print(f"suspect functions: {total}")


if __name__ == "__main__":
    main()
