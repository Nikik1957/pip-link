"""Install the PIP LINK holotape (and its sounds) on a Pip-Boy over USB.

    python install_holotape.py [--port COMx]

Copies everything under ../pipboy-sd/ (HOLO/TERM/*, APPINFO/TERM.info) to the same paths on the
Pip-Boy's SD card. Afterwards "PIP LINK" shows up under ITEMS > HOLOTAPES after a restart; you
don't need to open it yourself - the hub launches it when it connects.
"""
import argparse
import os
import sys

from map_upload import Repl, js_str
from pb_put import put

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="auto")
    ap.add_argument("--src", default=None, help="folder that mirrors the SD card (default ../pipboy-sd)")
    a = ap.parse_args()
    src = a.src or next((p for p in (os.path.join(HERE, "..", "pipboy-sd"),) if os.path.isdir(p)), None)
    if not src:
        sys.exit("can't find the pipboy-sd folder")
    files = []
    for d, _, fs in os.walk(src):
        for f in fs:
            full = os.path.join(d, f)
            files.append((full, os.path.relpath(full, src).replace("\\", "/")))
    r = Repl(a.port)
    try:
        made = set()
        for _, dest in files:
            parts = dest.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                p = "/".join(parts[:i])
                if p not in made:
                    r.call(f'try{{require("fs").mkdirSync({js_str(p)})}}catch(e){{}}print("@@ok>>")')
                    made.add(p)
        for full, dest in sorted(files, key=lambda f: f[1].endswith(".info")):   # the .info last
            put(r, full, dest)
        print("\nInstalled. Restart the Pip-Boy once so PIP LINK appears in its holotape list.")
    finally:
        r.close()


if __name__ == "__main__":
    main()
