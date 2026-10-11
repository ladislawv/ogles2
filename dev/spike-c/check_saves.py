#!/usr/bin/env python3
"""Find m68k functions that write a callee-saved register (d2-d7, a2-a6) without
saving it in their prologue: the GCC 16.1 wrong-code pattern found in
builtin_functions.cpp (new_sig clobbers a2/a3).

Heuristic, per function in `objdump -d` output:
  saved   = registers pushed in the first few instructions (movel rX,sp@- or moveml ...,sp@-)
  written = registers that appear as the destination of an instruction
A function is reported when written & callee_saved - saved is non-empty.
usage: check_saves.py <objdump binary> <object or archive>...
"""
import re, subprocess, sys

CALLEE = ({f"d{i}" for i in range(2, 8)} | {f"a{i}" for i in range(2, 7)}
          | {f"fp{i}" for i in range(2, 8)})


def regs_in_list(s):
    out = set()
    for part in s.split("/"):
        m = re.match(r"(fp|[ad])(\d)-(?:fp|[ad])?(\d)", part)
        if m:
            kind = m.group(1)
            for i in range(int(m.group(2)), int(m.group(3)) + 1):
                out.add(f"{kind}{i}")
        elif re.fullmatch(r"(fp|[ad])\d", part):
            out.add(part)
    return out


def dest(ins):
    # m68k MIT syntax: "op src,dst" -> destination is the last operand
    m = re.match(r"(\S+)\s+(.*)", ins)
    if not m:
        return None, None
    op, ops = m.group(1), m.group(2)
    parts = re.split(r",(?![^(]*\))", ops)
    return op, parts[-1].strip()


def scan(objdump, path):
    # -m m68k:68040: without it objdump does not decode 68020+ and FPU instructions
    # (mulsl, bfins, tstl An, fmove...), loses sync and reports phantom writes.
    txt = subprocess.run([objdump, "-m", "m68k:68040", "-d", path], capture_output=True, text=True).stdout
    bad = []
    fn, lines = None, []

    def drop_jump_tables(rows):
        """Remove switch tables: after "jmp %pc@(T,dN...)" the 16-bit entries at T are
        offsets from T; the smallest one marks where code resumes."""
        skip = set()
        for k, (addr, words, ins) in enumerate(rows):
            m = re.match(r"jmp %pc@\(([0-9a-f]+) ", ins)
            if not m:
                continue
            t = int(m.group(1), 16)
            data = []
            for a2, w2, _ in rows[k + 1:]:
                if a2 < t:
                    continue
                data += w2
                if len(data) >= 512:
                    break
            entries = [int(w, 16) for w in data]
            entries = [e - 0x10000 if e & 0x8000 else e for e in entries]
            end = None
            for i, e in enumerate(entries):
                if e > 0 and (end is None or e < end):
                    end = e
                if end is not None and 2 * (i + 1) >= end:
                    break
            if end is None:
                continue
            for j, (a2, _, _) in enumerate(rows):
                if t <= a2 < t + end:
                    skip.add(j)
        return [r for j, r in enumerate(rows) if j not in skip]

    def check():
        nonlocal lines
        if not fn or not lines:
            return
        rows = drop_jump_tables(lines)
        ins_only = [ins for _, _, ins in rows]
        # Data placed after the function (GCC 6 puts string constants in .text)
        # decodes as instructions: only look up to the last return or jump.
        last = max((k for k, ins in enumerate(ins_only)
                    if ins.split()[0] in ("rts", "rte", "rtd") or ins.startswith(("jmp", "bra"))),
                   default=None)
        if last is None:
            return  # no return at all: data, not a function
        lines = ins_only[:last + 1]
        saved, written = set(), set()
        for k, ins in enumerate(lines):
            op, d = dest(ins)
            if op is None:
                continue
            if k < 6 and op.startswith("link"):
                # link aN,#x pushes the old aN (frame pointer)
                saved.add(ins.split()[1].split(",")[0])
            if k < 6 and d == "sp@-" and op.startswith("movel"):
                src = ins.split()[1].split(",")[0]
                saved.add(src)
            if k < 6 and op.startswith(("moveml", "fmovemx", "fmovem")) and d == "sp@-":
                saved |= regs_in_list(ins.split()[1].split(",")[0])
            if op.startswith(("moveml", "fmovem")) and not d.startswith("sp@"):
                written |= regs_in_list(d)
            elif d in CALLEE and not op.startswith(("cmp", "tst", "btst", "pea", "jsr", "jmp")):
                written.add(d)
        missing = (written & CALLEE) - saved
        if missing:
            bad.append((fn, sorted(missing)))

    for line in txt.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if m:
            check()
            fn, lines = m.group(1), []
            continue
        m = re.match(r"^\s+([0-9a-f]+):\s+((?:[0-9a-f]{4} ?)+)\s+(\S.*)$", line)
        if m and fn:
            lines.append((int(m.group(1), 16), m.group(2).split(), m.group(3)))
    check()
    return bad


def main():
    objdump = sys.argv[1]
    total = 0
    for p in sys.argv[2:]:
        for fn, regs in scan(objdump, p):
            total += 1
            print(f"{p.rsplit('/', 1)[-1]}: {fn[:110]}  clobbers {','.join(regs)}")
    print(f"suspect functions: {total}")


if __name__ == "__main__":
    main()
