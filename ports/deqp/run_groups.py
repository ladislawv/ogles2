#!/usr/bin/env python3
"""Runs dEQP-GLES2 groups on the A1200, one package run (amictl runpkg of
tests/a1200/out/ogles2-m7-1) per case list, and prints where each one's
results went.

    python ports/deqp/run_groups.py [GROUP...] [--shaders] [--all]

GROUPs name files of the package's caselists/ (without .txt). Without any:
every group except the shader ones; --shaders: the shader ones; --all: all.
Run from the repository's root on the PC (amictl's station file).
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PKG = os.path.join(REPO, "tests", "a1200", "out", "ogles2-m7-1")
AMICTL = os.path.join(REPO, "tools", "amictl", "amictl.py")


def lists():
    d = os.path.join(PKG, "caselists")
    sizes = {}
    for f in os.listdir(d):
        if f.endswith(".txt"):
            with open(os.path.join(d, f)) as fh:
                sizes[f[:-4]] = sum(1 for _ in fh)
    return sizes


def main(argv):
    sizes = lists()
    named = [a for a in argv if not a.startswith("--")]
    if named:
        groups = named
    elif "--all" in argv:
        groups = sorted(sizes, key=lambda g: (g.startswith("shaders-"), sizes[g]))
    elif "--shaders" in argv:
        groups = sorted((g for g in sizes if g.startswith("shaders-")), key=sizes.get)
    else:
        groups = sorted((g for g in sizes if not g.startswith("shaders-")), key=sizes.get)
    for g in groups:
        if g not in sizes:
            print("no list %s" % g)
            return 1
        # About 0.2 s a case, more for the first compile of each program.
        timeout = 600 + sizes[g] * 3
        t0 = time.time()
        out = subprocess.run([sys.executable, AMICTL, "runpkg", PKG, "-a", "LIST=%s" % g,
                              "-t", str(timeout)], capture_output=True, text=True, cwd=REPO).stdout
        m = re.search(r"results: \d+ files -> (.+)", out)
        print("%-34s %5d cases %6.0f s  %s" % (g, sizes[g], time.time() - t0,
                                               m.group(1) if m else "NO RESULTS:\n" + out[-400:]),
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
