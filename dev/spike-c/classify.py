#!/usr/bin/env python3
"""Group the replay's compile errors by normalised message (first error per file)."""
import collections, json, os, re, sys

out = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/spike-c")
res = json.load(open(os.path.join(out, "results.json")))
first = collections.Counter()
example = {}
allmsg = collections.Counter()
for r in res:
    if r["ok"]:
        continue
    msgs = []
    for e in r["errors"]:
        m = re.sub(r"^.*?(fatal error|error): ", r"\1: ", e)
        m = re.sub(r"'[^']*'", "'X'", m) if "No such file" not in m else m
        msgs.append(m)
    if not msgs:
        msgs = ["(no error line captured)"]
    first[msgs[0]] += 1
    example.setdefault(msgs[0], (r["file"], r["errors"][:2]))
    for m in set(msgs):
        allmsg[m] += 1
print("== first error per failing file")
for m, n in first.most_common(25):
    f, ex = example[m]
    print(f"{n:4d}  {m}\n        e.g. {f}\n        {ex[0] if ex else ''}")
print("\n== any error, files affected")
for m, n in allmsg.most_common(25):
    print(f"{n:4d}  {m}")
