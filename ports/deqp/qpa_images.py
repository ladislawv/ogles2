#!/usr/bin/env python3
"""Writes the images of dEQP test logs (.qpa, run with --deqp-log-images=enable;
package m7-1: Run-M1 IMAGES=1) to PNG files: <outdir>/<case>-<image name>.png.

    python3 ports/deqp/qpa_images.py LOG.qpa OUTDIR [CASE-SUBSTRING]
"""
import base64
import os
import re
import sys

IMAGE = re.compile(r'<Image Name="([^"]*)"[^>]*CompressionMode="PNG"[^>]*>(.*?)</Image>', re.S)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    log, out = argv[0], argv[1]
    want = argv[2] if len(argv) > 2 else ""
    os.makedirs(out, exist_ok=True)
    text = open(log, encoding="utf-8", errors="replace").read()
    n = 0
    for part in text.split("#beginTestCaseResult ")[1:]:
        case = part.split("\n", 1)[0].strip()
        if want not in case:
            continue
        for name, data in IMAGE.findall(part):
            path = os.path.join(out, "%s-%s.png" % (case.replace("dEQP-GLES2.functional.", ""), name))
            with open(path, "wb") as f:
                f.write(base64.b64decode(re.sub(r"\s+", "", data)))
            n += 1
    print("%d image(s) in %s" % (n, out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
