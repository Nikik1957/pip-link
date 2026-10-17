"""Copy files to the Pip-Boy SD card over USB serial (size-verified, retries).

    python pb_put.py ../pipboy/TERM/APP.js HOLO/TERM/APP.js [more src dest pairs...] [--port COM6]
"""
import argparse
import base64
import sys

from map_upload import CHUNK, Repl, js_str


def put(r, src, dest):
    with open(src, "rb") as f:
        data = f.read()
    b64 = base64.b64encode(data).decode()
    d = js_str(dest)
    for attempt in range(4):
        try:                                   # a reply can go missing while the Pip-Boy is busy: start the file over
            for i in range(0, max(1, len(b64)), CHUNK):
                fn = "writeFileSync" if i == 0 else "appendFileSync"
                r.call(f'require("fs").{fn}({d},atob("{b64[i:i + CHUNK]}"));print("@@>>")')
            got = r.call(f'print("@@"+require("fs").statSync({d}).size+">>")')
        except TimeoutError:
            got = "(no reply)"
            r.sp.write(b"\necho(0)\n")
        if got.strip() == str(len(data)):
            print(f"OK    {dest}  {len(data)} bytes")
            return
        print(f"RETRY {dest}: device size {got} != {len(data)}")
    sys.exit(f"FAILED {dest}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pairs", nargs="+", help="src dest [src dest ...]")
    ap.add_argument("--port", default="auto")
    a = ap.parse_args()
    if len(a.pairs) % 2:
        sys.exit("give src dest pairs")
    r = Repl(a.port)
    try:
        for i in range(0, len(a.pairs), 2):
            put(r, a.pairs[i], a.pairs[i + 1])
    finally:
        r.close()


if __name__ == "__main__":
    main()
