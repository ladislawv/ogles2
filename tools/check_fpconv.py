#!/usr/bin/env python3
"""Find float-to-integer conversions that GCC 6 (bebbo amiga6) compiled
without fintrz, in GCC assembly output (-S).

GCC 6 at -O1 and above drops the fintrz of a float->integer conversion and
emits only "fmove.l fpN,dM" (or .w/.b), which rounds by the FPCR rounding
mode -- round to nearest under libnix and clib2 -- instead of truncating as C
requires: (int)255.5f is 256. GCC 16.2 and GCC 6 at -O0 are correct. Found on
the A1200 in M1 run 1 (plan section 13).

A move from an FP register to an integer destination is accepted when the
last instruction that wrote that FP register (in the same function, looking
back) is an fintrz. Everything else is reported.
  usage: check_fpconv.py file.s [...]     exit status 1 if anything is found
"""
import re, sys

MOVE_OUT = re.compile(r"^\s*fmove\.([bwl])\s+(%?fp[0-7]),\s*(.+)$")
WRITES_FP = re.compile(r"^\s*f[a-z]+(\.[a-z]+)?\s+.*,\s*(%?fp[0-7])\s*$")
LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):")


def scan(path):
    found = []
    func = "?"
    last_writer = {}
    for n, line in enumerate(open(path, errors="replace"), 1):
        line = line.split("|")[0].rstrip()
        m = LABEL.match(line)
        if m and not m.group(1).startswith("."):
            func = m.group(1)
            last_writer = {}
            continue
        m = MOVE_OUT.match(line)
        if m and not re.match(r"%?fp[0-7]$", m.group(3).strip()) and "fpcr" not in m.group(3):
            reg = m.group(2).lstrip("%")
            if last_writer.get(reg, "") != "fintrz":
                found.append((n, func, line.strip()))
            continue
        m = WRITES_FP.match(line)
        if m:
            last_writer[m.group(2).lstrip("%")] = line.split()[0].split(".")[0]
    return found


def main():
    total = 0
    for path in sys.argv[1:]:
        for n, func, text in scan(path):
            print("%s:%d: %s: %s" % (path, n, func, text))
            total += 1
    print("%d float->integer conversion(s) without fintrz" % total)
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
