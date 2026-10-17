"""Preview a map on the PC the way the Pip-Boy draws it (green 4-shade palette, markers).

    python map_preview.py "<map folder>" --zoom 16 [--center lat,lon] [--out preview.png]

The folder can be a map from map_lib.py (has map.db) or a card folder (PIPLINK/MAP: INFO.TXT + B/ or T/).
"""
import argparse
import os

from PIL import Image

import map_icons
import map_lib
from map_build import w0, TILE

PAL = [(0, 0, 0), (0, 70, 20), (0, 160, 50), (60, 255, 110)]   # roughly the Pip-Boy's green levels
W, H = 480, 264


def open_source(path):
    """A map library folder (MapDB) or a card folder (PackSource)."""
    if os.path.exists(os.path.join(path, "map.db")):
        return map_lib.MapDB(path)
    return map_lib.PackSource(path)


def render(src, zoom, center=None, player=True, size=(W, H)):
    """The Pip-Boy's view of a map at zoom, centred on (lat, lon) or the map's centre: a 'P' image.
    Like the Pip-Boy, a tile missing at this zoom is filled in from 1-2 zooms out, enlarged."""
    w_, h_ = size
    lat, lon = center if center else src.center()
    z = zoom
    cx, cy = (v * 2 ** z for v in w0(lat, lon))
    ox, oy = cx - w_ / 2, cy - h_ / 2
    out = Image.new("P", (w_, h_), 0)
    out.putpalette([c for rgb in PAL for c in rgb])
    zr = src.zoom_range() or (z, z)
    for tx in range(int(ox // TILE), int((ox + w_) // TILE) + 1):
        for ty in range(int(oy // TILE), int((oy + h_) // TILE) + 1):
            pos = (int(round(tx * TILE - ox)), int(round(ty * TILE - oy)))
            for k in range(0, 3):
                if z - k < zr[0]:
                    break
                data = src.get_tile(z - k, tx >> k, ty >> k)
                if data is None:
                    continue
                if data:
                    t = map_lib.tile_image(data)
                    if k:
                        sub = TILE >> k
                        t = t.crop(((tx % (1 << k)) * sub, (ty % (1 << k)) * sub,
                                    (tx % (1 << k) + 1) * sub, (ty % (1 << k) + 1) * sub)).resize((TILE, TILE), Image.NEAREST)
                    out.paste(t, pos)
                break
    icons = map_icons.icons_bin()
    markers = list(src.markers())
    if hasattr(src, "pins"):
        markers += src.pins()
    for mlat, mlon, typ, _name in markers:
        x, y = (v * 2 ** z for v in w0(mlat, mlon))
        t = map_icons.TYPES.index(typ) if typ in map_icons.TYPES else 0
        x, y = int(x - ox), int(y - oy)
        if not (-8 <= x < w_ + 8 and -8 <= y < h_ + 8):
            continue
        for r in range(16):
            bits = (icons[t * 32 + r * 2] << 8) | icons[t * 32 + r * 2 + 1]
            for c in range(16):
                if bits & (0x8000 >> c) and 0 <= x - 8 + c < w_ and 0 <= y - 8 + r < h_:
                    out.putpixel((x - 8 + c, y - 8 + r), 3)
    if player:                                                  # player arrow at the centre
        for dy in range(-10, 9):
            half = max(0, (dy + 10) // 2)
            for dx in range(-half, half + 1):
                if 0 <= w_ // 2 + dx < w_ and 0 <= h_ // 2 + dy < h_:
                    out.putpixel((w_ // 2 + dx, h_ // 2 + dy), 3)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--zoom", type=int, default=16)
    ap.add_argument("--center")
    ap.add_argument("--out", default="map_preview.png")
    a = ap.parse_args()
    center = tuple(float(v) for v in a.center.split(",")) if a.center else None
    render(open_source(a.map), a.zoom, center).resize((W * 2, H * 2), Image.NEAREST).save(a.out)
    print(f"wrote {a.out} (z{a.zoom})")


if __name__ == "__main__":
    main()
