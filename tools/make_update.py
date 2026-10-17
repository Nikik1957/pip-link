"""Assemble a PIP LINK software package: hub firmware + Pip-Boy holotape, with a version number.

    python make_update.py 1.1                 # -> release/PIP-LINK-update-v1.1.zip  (for "Install update file...")
    python make_update.py 1.0 --dir DIR        # unpacked folder (the setup app's built-in package)

Build the firmware first (pio run -e hub). Package layout:
    piplink.json                     {"name": "PIP LINK", "version": "1.1", "built": "..."}
    firmware/bootloader.bin  partitions.bin  boot_app0.bin  firmware.bin    (flashed at 0x0 / 0x8000 / 0xe000 / 0x10000)
    pipboy-sd/HOLO/TERM/...  pipboy-sd/APPINFO/TERM.info                       (copied to the Pip-Boy SD card)
"""
import argparse
import json
import os
import shutil
import sys
import time

import rjsmin                                   # pip install rjsmin

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
BUILD = os.path.join(REPO, ".pio", "build", "hub")
BOOT_APP0 = os.path.join(os.path.expanduser("~"), ".platformio", "packages", "framework-arduinoespressif32",
                         "tools", "partitions", "boot_app0.bin")
FIRMWARE = {"bootloader.bin": os.path.join(BUILD, "bootloader.bin"), "partitions.bin": os.path.join(BUILD, "partitions.bin"),
            "boot_app0.bin": BOOT_APP0, "firmware.bin": os.path.join(BUILD, "firmware.bin")}
HOLOTAPE = os.path.join(REPO, "pipboy", "TERM")
# words that must never ship (names, callsigns, places): one per line in a private file outside the project,
# so the list itself doesn't ship either. The build machine's user name is always checked.
PRIVATE_WORDS = os.path.join(os.path.expanduser("~"), ".piplink-private-words.txt")


def personal_words():
    words = {os.path.basename(os.path.expanduser("~")).lower().encode()}
    if os.path.exists(PRIVATE_WORDS):
        with open(PRIVATE_WORDS, encoding="utf-8") as f:
            words |= {ln.strip().lower().encode() for ln in f if ln.strip() and not ln.startswith("#")}
    else:
        print(f"note: no {PRIVATE_WORDS} - only checking for the user name")
    return words


def make(version, out_dir):
    if os.path.exists(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(os.path.join(out_dir, "firmware"))
    for name, src in FIRMWARE.items():
        if not os.path.exists(src):
            sys.exit(f"missing {src} - build the firmware first (pio run -e hub)")
        shutil.copy(src, os.path.join(out_dir, "firmware", name))
    term = os.path.join(out_dir, "pipboy-sd", "HOLO", "TERM")
    os.makedirs(term)
    for f in os.listdir(HOLOTAPE):
        if f.upper().endswith(".JS"):
            # Espruino keeps every function's source text in RAM: ship it minified (about 40% smaller)
            with open(os.path.join(HOLOTAPE, f), encoding="utf-8") as fh:
                js = rjsmin.jsmin(fh.read())
            with open(os.path.join(term, f), "w", encoding="utf-8", newline="\n") as fh:
                fh.write(js)
        elif f.upper().endswith(".WAV"):
            shutil.copy(os.path.join(HOLOTAPE, f), os.path.join(term, f))
    os.makedirs(os.path.join(out_dir, "pipboy-sd", "APPINFO"))
    with open(os.path.join(REPO, "pipboy", "TERM.info"), encoding="utf-8") as f:
        info = json.load(f)
    info["name"], info["version"] = "PIP LINK", version
    with open(os.path.join(out_dir, "pipboy-sd", "APPINFO", "TERM.info"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(info, f, indent=2)
    with open(os.path.join(out_dir, "piplink.json"), "w", encoding="utf-8") as f:
        json.dump({"name": "PIP LINK", "version": version, "built": time.strftime("%Y-%m-%d")}, f, indent=2)
    words = personal_words()
    for d, _, fs in os.walk(out_dir):                           # privacy check
        for fn in fs:
            data = open(os.path.join(d, fn), "rb").read().lower()
            bad = [w.decode() for w in words if w in data]
            if bad:
                sys.exit(f"refusing to package {fn}: contains {bad}")
    return out_dir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--dir", help="write an unpacked folder here instead of a zip")
    a = ap.parse_args()
    if a.dir:
        print("package:", make(a.version, a.dir))
        return
    tmp = os.path.join(REPO, "build", f"pkg-{a.version}")
    make(a.version, tmp)
    os.makedirs(os.path.join(REPO, "release"), exist_ok=True)
    z = shutil.make_archive(os.path.join(REPO, "release", f"PIP-LINK-update-v{a.version}"), "zip", tmp)
    shutil.rmtree(tmp)
    print("update file:", z)


if __name__ == "__main__":
    main()
