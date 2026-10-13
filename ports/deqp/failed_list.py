#!/usr/bin/env python3
"""Writes a dEQP case list (one name a line, as Run-M1 LIST= takes) of the
cases whose latest result in the given logs is not Pass, for a rerun of just
those (package m7-1: copy it into the package's caselists/).

    python3 ports/deqp/failed_list.py OUT.txt RESULTS... [--keep-unsupported]

Not supported for a reason that holds on ogles2 (no EXT_draw_buffers, clip
control, debug markers or multisampled default framebuffer in these runs)
is left out unless --keep-unsupported.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import summary  # noqa: E402

SETTLED = (
    "Multiple attachments not supported",   # EXT_draw_buffers
    "No multisample buffers",                # runs without MSAA=1
    "GL_EXT_clip_control",
    "clip control",
    "debug",
)


def main(argv):
    keep = "--keep-unsupported" in argv
    args = [a for a in argv if not a.startswith("--")]
    out, paths = args[0], args[1:]
    latest = {}
    for p in summary.qpa_files(paths):
        for name, status, msg in summary.cases(p):
            latest[name] = (status, msg)
    names = []
    for name, (status, msg) in sorted(latest.items()):
        if status == "Pass":
            continue
        if status == "NotSupported" and not keep and \
                (any(s in msg for s in SETTLED) or ".clip_control." in name or ".debug_marker." in name):
            continue
        names.append(name)
    with open(out, "w", newline="\n") as f:
        f.write("".join(n + "\n" for n in names))
    print("%d case(s) -> %s" % (len(names), out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
