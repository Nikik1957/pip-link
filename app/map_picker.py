"""The MAPS page's map: a topo base map (USGS The National Map - public domain, US only) to find and pick areas
on, with the Pip-Boy map drawn over the parts it covers. Drag to move, mouse wheel to zoom, right-drag (or
Shift+drag) to pick an area. (OpenStreetMap's and CARTO's tile servers don't serve desktop apps without a key.)"""
import math
import os
import queue
import threading
import tkinter as tk

import requests
from PIL import Image, ImageOps, ImageTk

import map_build as mb
import map_icons
import map_lib

BASE_URL = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}"
BASE_MAXZ = 16
UA = {"User-Agent": "PIP-LINK-Setup/1.2 (personal offline map builder; area picker)"}
PIP_PAL = [0, 0, 0, 0, 70, 20, 0, 160, 50, 60, 255, 110]
BG, FG, DIM, DARK, WARN = "#050b06", "#3cff6e", "#1f9a45", "#0f3a1c", "#ffcf3c"
ZMIN, ZMAX = 3, 17


def tint(im):
    """A base map tile in the setup app's green: land dark, roads, rivers and labels bright."""
    g = ImageOps.autocontrast(ImageOps.invert(im.convert("L")), cutoff=0.5)
    return ImageOps.colorize(g, black="#020a04", white="#8dffb0", mid="#2a8a48")


def icon_image(typ, col):
    """A Pip-Boy marker icon (map_icons.py) as an 18x18 RGBA image in colour col, with a dark outline."""
    data = map_icons.icons_bin()
    t = map_icons.TYPES.index(typ) if typ in map_icons.TYPES else 0
    mask = Image.new("L", (16, 16), 0)
    for r in range(16):
        bits = (data[t * 32 + r * 2] << 8) | data[t * 32 + r * 2 + 1]
        for c in range(16):
            if bits & (0x8000 >> c):
                mask.putpixel((c, r), 255)
    im = Image.new("RGBA", (18, 18), (0, 0, 0, 0))
    for dx, dy in ((0, 1), (2, 1), (1, 0), (1, 2)):
        im.paste((0, 0, 0, 255), (dx, dy), mask)
    im.paste(col, (1, 1), mask)
    return im


class MapPicker(tk.Canvas):
    def __init__(self, parent, cache_dir, on_select=None, on_hover=None, on_click=None, **kw):
        super().__init__(parent, bg=BG, highlightthickness=1, highlightbackground=DARK, cursor="fleur", **kw)
        self.cache_dir = os.path.join(cache_dir, "basemap")
        self.on_select, self.on_hover, self.on_click = on_select, on_hover, on_click
        self.pins = []                                        # [(id, lat, lon, type, name)] - the map's pins
        self.landmarks = []                                   # [(lat, lon, type, name)] - the map's landmarks
        self.sel_pin = None                                   # id of the highlighted pin
        self.preview = {}                                     # (z, x, y) -> 'P' image: drawn, not saved
        self.preview_marks = []                               # landmarks the preview found
        self.icons = {}
        self.z = 9
        self.wx, self.wy = mb.w0(35.8325, -115.4319)               # the Pioneer Saloon, Goodsprings NV
        self.db = None
        self.show_mine = True
        self.sel = None                                       # (s, w, n, e)
        self.streets = {}                                     # (z, x, y) -> tinted PIL image
        self.photos = {}                                      # key -> PhotoImage (newest last)
        self.missing = set()                                  # OSM tiles that failed (offline)
        self.wanted = set()
        self.q_in, self.q_out = queue.Queue(), queue.Queue()
        self.drag = None
        self.pending = False
        for _ in range(2):
            threading.Thread(target=self._fetcher, daemon=True).start()
        self.bind("<Configure>", lambda e: self.redraw())
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<B1-Motion>", self._motion)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<Shift-ButtonPress-1>", lambda e: self._press(e, True))
        self.bind("<ButtonPress-3>", lambda e: self._press(e, True))
        self.bind("<B3-Motion>", self._motion)
        self.bind("<ButtonRelease-3>", self._release)
        self.bind("<MouseWheel>", lambda e: self.zoom_at(1 if e.delta > 0 else -1, e.x, e.y))
        self.bind("<Double-Button-1>", lambda e: self.zoom_at(1, e.x, e.y))
        self.bind("<Motion>", self._hover)
        self.after(100, self._poll)

    # ---------------------------------------------------------------- coordinates
    def _scale(self):
        return 2 ** self.z

    def to_screen(self, lat, lon):
        x, y = mb.w0(lat, lon)
        s = self._scale()
        return (x - self.wx) * s + self.winfo_width() / 2, (y - self.wy) * s + self.winfo_height() / 2

    def to_latlon(self, px, py):
        s = self._scale()
        return mb.unw0(self.wx + (px - self.winfo_width() / 2) / s, self.wy + (py - self.winfo_height() / 2) / s)

    def center(self):
        return mb.unw0(self.wx, self.wy)

    def set_view(self, lat, lon, z=None):
        self.wx, self.wy = mb.w0(lat, lon)
        if z is not None:
            self.z = max(ZMIN, min(ZMAX, z))
        self.redraw()

    def fit(self, rect, margin=0.15):
        s, w, n, e = rect
        x0, y0 = mb.w0(n, w)
        x1, y1 = mb.w0(s, e)
        W, H = max(100, self.winfo_width()), max(100, self.winfo_height())
        z = ZMAX
        while z > ZMIN and ((x1 - x0) * 2 ** z * (1 + margin) > W or (y1 - y0) * 2 ** z * (1 + margin) > H):
            z -= 1
        self.wx, self.wy, self.z = (x0 + x1) / 2, (y0 + y1) / 2, z
        self.redraw()

    def zoom_at(self, d, px, py):
        z = max(ZMIN, min(ZMAX, self.z + d))
        if z == self.z:
            return
        lat, lon = self.to_latlon(px, py)                     # keep the point under the mouse still
        self.z = z
        x, y = mb.w0(lat, lon)
        s = self._scale()
        self.wx = x - (px - self.winfo_width() / 2) / s
        self.wy = y - (py - self.winfo_height() / 2) / s
        self.redraw()

    def set_selection(self, rect, fit=False):
        self.sel = rect
        if fit and rect:
            self.fit(rect)
        else:
            self.redraw()

    def set_map(self, db):
        self.db = db
        for k in [k for k in self.photos if k[0] == "pip"]:
            del self.photos[k]
        self.redraw()

    def refresh_mine(self):
        """The map changed (a build drew tiles): forget the cached Pip-Boy tiles."""
        self.set_map(self.db)

    # ---------------------------------------------------------------- mouse
    def _press(self, e, select=False):
        self.drag = (e.x, e.y, self.wx, self.wy, select)

    def _motion(self, e):
        if not self.drag:
            return
        x0, y0, wx, wy, select = self.drag
        if select:
            a = self.to_latlon(x0, y0)
            b = self.to_latlon(e.x, e.y)
            self.sel = (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1]))
            self._overlay()
        else:
            s = self._scale()
            self.wx, self.wy = wx - (e.x - x0) / s, wy - (e.y - y0) / s
            self.redraw_soon()
        self._hover(e)

    def _release(self, e):
        if self.drag and not self.drag[4] and abs(e.x - self.drag[0]) < 4 and abs(e.y - self.drag[1]) < 4:
            self.drag = None                                  # a click, not a drag
            if self.on_click:
                self.on_click(*self.to_latlon(e.x, e.y), e.x, e.y)
            return
        if self.drag and self.drag[4]:
            x0, y0 = self.drag[:2]
            if abs(e.x - x0) > 6 and abs(e.y - y0) > 6 and self.on_select:
                self.on_select(self.sel)
            elif self.on_select:                              # a click, not a drag: no area
                self.sel = None
                self._overlay()
                self.on_select(None)
        self.drag = None
        self.redraw()

    def _hover(self, e):
        if self.on_hover:
            self.on_hover(*self.to_latlon(e.x, e.y))

    # ---------------------------------------------------------------- drawing
    def redraw_soon(self):
        if not self.pending:
            self.pending = True
            self.after(30, self.redraw)

    def redraw(self):
        self.pending = False
        W, H = self.winfo_width(), self.winfo_height()
        if W < 10:
            return
        self.delete("all")
        s = self._scale()
        ox, oy = self.wx * s - W / 2, self.wy * s - H / 2
        n = 2 ** self.z
        zr = self.db.zoom_range() if (self.db and self.show_mine) else None
        want = set()
        for tx in range(int(ox // 256), int((ox + W) // 256) + 1):
            for ty in range(int(oy // 256), int((oy + H) // 256) + 1):
                if not (0 <= ty < n):
                    continue
                x, y = tx * 256 - ox, ty * 256 - oy
                img = None
                if (self.z, tx % n, ty) in self.preview:
                    img = self._photo(("pv", self.z, tx % n, ty), lambda k=(self.z, tx % n, ty): self._pv(k))
                elif zr and zr[0] <= self.z <= zr[1]:
                    img = self._pip(self.z, tx % n, ty)
                if img is None:
                    f = 2 ** max(0, self.z - BASE_MAXZ)           # past the base map's last zoom: enlarged
                    want.add((self.z - (f.bit_length() - 1), (tx % n) // f, ty // f))
                    img = self._street(self.z, tx % n, ty)
                if img is not None:
                    self.create_image(x, y, image=img, anchor="nw")
        self.wanted = want
        for k in want:
            if k not in self.streets and k not in self.missing:
                self.q_in.put(k)
        self._overlay()

    def _photo(self, key, make):
        ph = self.photos.pop(key, None)
        if ph is None:
            im = make()
            if im is None:
                return None
            ph = ImageTk.PhotoImage(im)
        self.photos[key] = ph
        while len(self.photos) > 300:
            del self.photos[next(iter(self.photos))]
        return ph

    def _pip(self, z, x, y):
        if ("pipnone", z, x, y) in self.photos:
            return None

        def make():
            data = self.db.get_tile(z, x, y)
            if data is None:
                return None
            im = map_lib.tile_image(data) if data else Image.new("P", (256, 256), 0)
            im.putpalette(PIP_PAL)
            return im.convert("RGB")
        ph = self._photo(("pip", z, x, y), make)
        if ph is None:
            self.photos[("pipnone", z, x, y)] = True
        return ph

    def _pv(self, key):
        im = self.preview[key].copy()
        im.putpalette(PIP_PAL)
        return im.convert("RGB")

    def set_preview(self, z, tiles, marks):
        """tiles {(x, y): 'P' image} drawn at zoom z (not saved); marks [(lat, lon, type, name)]."""
        for k in [k for k in self.photos if k[0] == "pv"]:
            del self.photos[k]
        self.preview = {(z, x, y): im for (x, y), im in tiles.items()}
        self.preview_marks = marks
        self.redraw()

    def clear_preview(self):
        if self.preview or self.preview_marks:
            self.set_preview(0, {}, [])

    def visible_tiles(self, z):
        """The tiles at zoom z covering the view."""
        W, H = self.winfo_width(), self.winfo_height()
        s = 2 ** z
        ox, oy = self.wx * s - W / 2, self.wy * s - H / 2
        return {(tx % s, ty) for tx in range(int(ox // 256), int((ox + W) // 256) + 1)
                for ty in range(int(oy // 256), int((oy + H) // 256) + 1) if 0 <= ty < s}

    def set_pins(self, pins, landmarks, sel=None):
        self.pins, self.landmarks, self.sel_pin = pins, landmarks, sel
        self._overlay()

    def pin_at(self, px, py, radius=11):
        """The pin under (px, py) on the canvas, or None."""
        best, bd = None, radius * radius
        for p in self.pins:
            x, y = self.to_screen(p[1], p[2])
            d = (x - px) ** 2 + (y - py) ** 2
            if d <= bd:
                best, bd = p, d
        return best

    def _icon(self, typ, col):
        key = (typ, col)
        if key not in self.icons:
            self.icons[key] = ImageTk.PhotoImage(icon_image(typ, col))
        return self.icons[key]

    def _marks(self):
        W, H = self.winfo_width(), self.winfo_height()
        show = []
        if self.show_mine and self.z >= 11:                     # landmarks show from zoom 11, like the Pip-Boy
            show += [(m, (60, 255, 110, 255), False) for m in self.landmarks + self.preview_marks]
        show += [(p[1:], (255, 207, 60, 255) if p[0] == self.sel_pin else (140, 255, 170, 255), True) for p in self.pins]
        for (lat, lon, typ, name), col, pin in show:
            x, y = self.to_screen(lat, lon)
            if -10 < x < W + 10 and -10 < y < H + 10:
                if col[0] == 255:
                    self.create_oval(x - 13, y - 13, x + 13, y + 13, outline=WARN, width=2, tags="ov")
                self.create_image(x, y, image=self._icon(typ, col), tags="ov")
                if name and (self.z >= 15 or (pin and self.z >= 13)):
                    t = self.create_text(x + 11, y, anchor="w", text=name, fill="#%02x%02x%02x" % col[:3],
                                         font=("Consolas", 8), tags="ov")
                    bb = self.bbox(t)
                    self.create_rectangle(bb[0] - 1, bb[1], bb[2] + 1, bb[3], fill=BG, outline="", tags="ov")
                    self.tag_raise(t)

    def _street(self, z, x, y):
        if (z, x, y) in self.streets:
            return self._photo(("osm", z, x, y), lambda: self.streets[(z, x, y)])
        for k in range(1, 5):                                 # meanwhile: a zoomed-out tile, enlarged
            p = (z - k, x >> k, y >> k)
            if p in self.streets:
                def make(p=p, k=k):
                    sub = 256 >> k
                    ix, iy = (x % (1 << k)) * sub, (y % (1 << k)) * sub
                    return self.streets[p].crop((ix, iy, ix + sub, iy + sub)).resize((256, 256), Image.BILINEAR)
                return self._photo(("osmz", z, x, y, k), make)
        return None

    def _overlay(self):
        self.delete("ov")
        W, H = self.winfo_width(), self.winfo_height()
        if self.db:
            for a in self.db.areas():
                s, w, n, e = a["rect"]
                x0, y0 = self.to_screen(n, w)
                x1, y1 = self.to_screen(s, e)
                if x1 < 0 or y1 < 0 or x0 > W or y0 > H:
                    continue
                col = FG if a["done"] else WARN
                self.create_rectangle(x0, y0, x1, y1, outline=col, width=1, dash=() if a["done"] else (4, 3), tags="ov")
                if x1 - x0 > 60:
                    self.create_text(x0 + 4, y0 + 3, anchor="nw", fill=col, font=("Consolas", 8), tags="ov",
                                     text=f"{a['name'][:28]}  z{a['zmin']}-{a['zmax']}" + ("" if a["done"] else "  (unfinished)"))
        self._marks()
        if self.preview and any(k[0] == self.z for k in self.preview):
            self.create_text(W / 2, 8, anchor="n", fill=WARN, font=("Consolas", 9, "bold"), tags="ov",
                             text="PREVIEW - not saved")
        if self.sel:
            s, w, n, e = self.sel
            x0, y0 = self.to_screen(n, w)
            x1, y1 = self.to_screen(s, e)
            self.create_rectangle(x0, y0, x1, y1, outline="#000000", width=4, tags="ov")
            self.create_rectangle(x0, y0, x1, y1, outline=WARN, width=2, tags="ov")
            kw, kh = map_lib.rect_km(self.sel)
            self.create_text(x0 + 3, y1 - 3, anchor="sw", fill=WARN, font=("Consolas", 9, "bold"), tags="ov",
                             text=f"{kw:.1f} x {kh:.1f} km")
        # zoom, scale bar, attribution
        lat = self.center()[0]
        m_per_px = 40075016 * math.cos(math.radians(lat)) / (256 * 2 ** self.z)
        bar = next((m for m in (50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000)
                    if m / m_per_px >= 60), 1000000)
        L = bar / m_per_px
        self.create_rectangle(6, H - 24, 16 + L, H - 6, fill=BG, outline="", tags="ov")
        self.create_line(10, H - 10, 10 + L, H - 10, fill=FG, width=2, tags="ov")
        self.create_text(12 + L / 2, H - 13, anchor="s", fill=FG, font=("Consolas", 8), tags="ov",
                         text=f"{bar // 1000} km" if bar >= 1000 else f"{bar} m")
        self.create_text(8, 6, anchor="nw", fill=FG, font=("Consolas", 9, "bold"), tags="ov", text=f"zoom {self.z}")
        t = self.create_text(W - 4, H - 4, anchor="se", fill=DIM, font=("Consolas", 8), tags="ov",
                             text="base map: USGS The National Map")
        bb = self.bbox(t)
        if bb:
            self.create_rectangle(bb[0] - 3, bb[1] - 1, bb[2] + 2, bb[3] + 1, fill=BG, outline="", tags="ov")
            self.tag_raise(t)

    # ---------------------------------------------------------------- street tiles (background threads)
    def _fetcher(self):
        sess = requests.Session()
        while True:
            k = self.q_in.get()
            if k not in self.wanted or k in self.streets:
                continue
            z, x, y = k
            path = os.path.join(self.cache_dir, str(z), str(x), f"{y}.jpg")
            try:
                if not os.path.exists(path):
                    r = sess.get(BASE_URL.format(z=z, x=x, y=y), headers=UA, timeout=20)
                    r.raise_for_status()
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, "wb") as f:
                        f.write(r.content)
                self.q_out.put((k, tint(Image.open(path).convert("RGB"))))
            except Exception:                                 # noqa: BLE001 - offline: just leave it dark
                self.q_out.put((k, None))

    def _poll(self):
        got = False
        try:
            while True:
                k, im = self.q_out.get_nowait()
                if im is None:
                    self.missing.add(k)
                else:
                    self.streets[k] = im
                    got = True
        except queue.Empty:
            pass
        if len(self.streets) > 600:
            for k in list(self.streets)[:200]:
                del self.streets[k]
        if got and not self.drag:
            self.redraw_soon()
        self.after(100, self._poll)
