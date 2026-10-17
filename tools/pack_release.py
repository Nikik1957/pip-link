"""Freeze a finished version into release/v<version>/: the setup .exe, READ ME FIRST.txt, THIRD-PARTY.txt and a
source zip (pipboy-hub-v<version>-source.zip). Build the .exe first (app/build_exe.py <version>).

    python pack_release.py 1.1

The source zip leaves out build output, caches and logs, and goes through the same privacy check as
make_update.py (the user name + ~/.piplink-private-words.txt).
"""
import os
import shutil
import sys
import zipfile

from make_update import personal_words

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SOURCE = ["app", "docs", "include", "lib", "pipboy", "src", "test", "tools", "platformio.ini"]
SKIP_DIRS = {"__pycache__", ".pio", "build", "release"}
SKIP_EXT = {".log", ".pyc"}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    ver = sys.argv[1]
    out = os.path.join(REPO, "release", f"v{ver}")
    if os.path.exists(out):
        sys.exit(f"{out} already exists - versions are frozen once packed")
    words = personal_words()
    files = []
    for top in SOURCE:
        p = os.path.join(REPO, top)
        if os.path.isfile(p):
            files.append(p)
            continue
        for d, dirs, fs in os.walk(p):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            files += [os.path.join(d, f) for f in fs if os.path.splitext(f)[1].lower() not in SKIP_EXT]
    bad = []
    for f in files:
        data = open(f, "rb").read().lower()
        hit = [w.decode() for w in words if w in data]
        if hit:
            bad.append(f"{os.path.relpath(f, REPO)}: {hit}")
    if bad:
        sys.exit("refusing to pack - personal words found:\n  " + "\n  ".join(bad))
    os.makedirs(out)
    for name in ("PIP LINK Setup.exe", "READ ME FIRST.txt", "THIRD-PARTY.txt"):
        shutil.copy2(os.path.join(REPO, "release", name), os.path.join(out, name))
    zpath = os.path.join(out, f"pipboy-hub-v{ver}-source.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(f, os.path.relpath(f, REPO).replace("\\", "/"))
    print(f"packed v{ver}: {len(files)} source files -> {out}")
    for f in sorted(os.listdir(out)):
        print(f"  {f}  {os.path.getsize(os.path.join(out, f)) / 1048576:.1f} MB")


if __name__ == "__main__":
    main()
