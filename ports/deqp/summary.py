#!/usr/bin/env python3
"""Summarises dEQP test logs (.qpa): totals by result and by group, and the
cases that did not pass, with dEQP's message.

    python3 ports/deqp/summary.py RESULTS_DIR_OR_QPA... [--depth N] [--all]

A case whose log ends without a result (the program stopped in it) counts
as "Crash". --depth sets how many name parts make a group (default 3, as
dEQP-GLES2.functional.fbo); --all lists passing cases' groups too.
"""
import os
import re
import sys
from collections import Counter, defaultdict

BEGIN = re.compile(r"^#beginTestCaseResult (\S+)")
TERM = re.compile(r"^#terminateTestCaseResult (\S+)")
RESULT = re.compile(r'<Result StatusCode="([^"]+)">(.*?)</Result>')


def cases(path):
    """(name, status, message) for each case in a .qpa file."""
    name = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = BEGIN.match(line)
            if m:
                if name:
                    yield name, "Crash", "no result in the log"
                name = m.group(1)
                continue
            m = TERM.match(line)
            if m and name:
                yield name, m.group(1), "terminated"
                name = None
                continue
            m = RESULT.search(line)
            if m and name:
                yield name, m.group(1), m.group(2)
                name = None
    if name:
        yield name, "Crash", "no result in the log"


def qpa_files(args):
    for a in args:
        if os.path.isdir(a):
            for root, _, files in os.walk(a):
                for f in sorted(files):
                    if f.endswith(".qpa"):
                        yield os.path.join(root, f)
        else:
            yield a


def main(argv):
    depth = 3
    show_all = "--all" in argv
    if "--depth" in argv:
        depth = int(argv[argv.index("--depth") + 1])
    paths = [a for i, a in enumerate(argv)
             if not a.startswith("--") and not (i > 0 and argv[i - 1] == "--depth")]
    results = {}  # the last result of each case wins (a rerun replaces it)
    for p in qpa_files(paths):
        for name, status, msg in cases(p):
            results[name] = (status, msg)
    totals = Counter(s for s, _ in results.values())
    groups = defaultdict(Counter)
    for name, (status, _) in results.items():
        groups[".".join(name.split(".")[:depth])][status] += 1
    print("%d cases: %s" % (len(results), ", ".join("%s %d" % kv for kv in totals.most_common())))
    print()
    for g in sorted(groups):
        c = groups[g]
        if show_all or set(c) - {"Pass"}:
            print("  %-60s %s" % (g, ", ".join("%s %d" % kv for kv in c.most_common())))
    bad = [(n, s, m) for n, (s, m) in sorted(results.items())
           if s not in ("Pass", "NotSupported", "QualityWarning", "CompatibilityWarning")]
    if bad:
        print()
        print("Not passed (%d):" % len(bad))
        for n, s, m in bad:
            print("  %-14s %s: %s" % (s, n, m))


if __name__ == "__main__":
    main(sys.argv[1:])
