"""Screenshot the Pip-Boy screen (the 'h' layer) over USB serial into a PNG.

    python pb_screen.py [out.png] [--port COM6]

Reads pixels with h.getPixel row by row (~10-20 s). Run it while the thing you want is on screen.
"""
import argparse
import base64
import re
import time

import serial
from PIL import Image

PAL = [(0, 0, 0), (0, 70, 20), (0, 160, 50), (60, 255, 110)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="pipboy_screen.png")
    ap.add_argument("--port", default="COM6")
    a = ap.parse_args()
    sp = serial.Serial(a.port, 115200, timeout=0.1)
    time.sleep(0.2)
    sp.reset_input_buffer()
    sp.write(b"\n")                     # the first line after opening can be swallowed
    time.sleep(0.4)
    # one row = 480 px = 120 bytes of 2bpp; printed as base64 lines <<S y data>>
    js = ('(function(){var y=0;function row(){var b=new Uint8Array(120);for(var x=0;x<480;x+=4)'
          'b[x>>2]=(h.getPixel(x,y)&3)<<6|(h.getPixel(x+1,y)&3)<<4|(h.getPixel(x+2,y)&3)<<2|(h.getPixel(x+3,y)&3);'
          'print("<<S "+y+" "+btoa(E.toString(b))+">>");if(++y<320)setTimeout(row,1);'
          'else setTimeout(function(){print("<<S END>>");setTimeout(function(){print(" ")},30)},5)}row()})()')
    sp.write((js + "\n").encode())
    rows, buf, t0 = {}, b"", time.time()
    pat = re.compile(rb"<<S (\d+) ([A-Za-z0-9+/=]+)>>")
    last, lastAt = 0, time.time()
    while time.time() - t0 < 120:
        buf += sp.read(8192)
        if len(buf) != last:
            last, lastAt = len(buf), time.time()
        elif time.time() - lastAt > 0.4:             # output held back: printing something pushes it out
            sp.write(b'print(" ")\n')
            lastAt = time.time()
        for m in pat.finditer(buf):
            rows[int(m.group(1))] = base64.b64decode(m.group(2))
        if b"<<S END>>" in buf:
            break
        print(f"\r{len(rows)}/320 rows", end="", flush=True)
    sp.close()
    img = Image.new("P", (480, 320), 0)
    img.putpalette([c for rgb in PAL for c in rgb])
    px = img.load()
    for y, data in rows.items():
        for i, byte in enumerate(data):
            for k in range(4):
                px[i * 4 + k, y] = (byte >> (6 - 2 * k)) & 3
    img.resize((960, 640), Image.NEAREST).save(a.out)
    print(f"\nwrote {a.out} ({len(rows)} rows)")


if __name__ == "__main__":
    main()
