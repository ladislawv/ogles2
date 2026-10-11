#!/usr/bin/env python3
"""Find exec memory a program leaves allocated at exit, from a vamos library
trace (vamos -l lib:info ...). Tracks AllocMem/FreeMem, AllocVec/FreeVec and
CreatePool/DeletePool by address, and reports what is still allocated, grouped
by the calling PC.
usage: memleak.py <vamos log>
"""
import re, sys, collections

call_re = re.compile(r"\{ CALL: \(exec\.library\)\s+\d+ (\w+)\((.*)\) from PC=([0-9a-f]+)")
ret_re = re.compile(r"\} CALL: -> d0=([0-9a-f]+)")
arg_re = lambda name: re.compile(name + r"\[\w+\]=([0-9a-f]+)")

live = {}          # addr -> (kind, size, pc)
pending = None
for line in open(sys.argv[1], errors="replace"):
    m = call_re.search(line)
    if m:
        fn, args, pc = m.group(1), m.group(2), m.group(3)
        if fn in ("AllocMem", "AllocVec"):
            size = int(arg_re("byteSize").search(args).group(1), 16)
            pending = (fn, size, pc)
        elif fn == "CreatePool":
            pending = ("Pool", 0, pc)
        elif fn == "FreeMem":
            live.pop(int(arg_re("memoryBlock").search(args).group(1), 16), None)
            pending = None
        elif fn == "FreeVec":
            live.pop(int(arg_re("memoryBlock").search(args).group(1), 16), None)
            pending = None
        elif fn == "DeletePool":
            live.pop(int(arg_re("poolHeader").search(args).group(1), 16), None)
            pending = None
        else:
            pending = None
        continue
    m = ret_re.search(line)
    if m and pending:
        addr = int(m.group(1), 16)
        if addr:
            live[addr] = pending
        pending = None

by = collections.defaultdict(lambda: [0, 0])
for addr, (kind, size, pc) in live.items():
    key = (kind, pc)
    by[key][0] += 1
    by[key][1] += size
total = sum(v[1] for v in by.values())
print(f"still allocated at exit: {len(live)} blocks, {total} bytes (pools counted as blocks)")
for (kind, pc), (n, size) in sorted(by.items(), key=lambda kv: -kv[1][1])[:15]:
    print(f"  {kind:9s} from PC={pc}: {n:5d} blocks, {size:9d} bytes")
