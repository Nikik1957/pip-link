"""PIP LINK map library: Pip-Boy maps that grow.

A map is a folder (by default %LOCALAPPDATA%/PIP LINK/maps/<name>/, shared with PIP LINK Setup):
    map.db                  SQLite: every tile built so far, the areas, landmarks, KMZ pins and lines
    SD card/PIPLINK/MAP/    the map the way the Pip-Boy reads it (export() keeps it up to date). Copy the PIPLINK
                            folder to the SD card, or send it over USB: python map_upload.py "<map>/SD card/PIPLINK/MAP"

Adding an area only draws the tiles the map doesn't have yet: a deeper build of a small area adds detail to a
big overview map, a neighbouring area extends it, and nothing already built is lost. Big areas are downloaded
and drawn piece by piece (about 150 km pieces for zoom <= 12, 10-60 km pieces above), middle first, and a
stopped build continues where it left off.

    python map_lib.py add --map Nevada --place "Clark County, Nevada" --zooms 10-15
    python map_lib.py add --map Nevada --bbox 35.7,-115.6,36.0,-115.3 --zooms 10-16 --name "Goodsprings"
    python map_lib.py add --map Nevada --center 35.8325,-115.4319 --radius-km 3 --zooms 10-17
    python map_lib.py kmz --map Nevada pins.kmz
    python map_lib.py resume --map Nevada
    python map_lib.py info --map Nevada
    python map_lib.py export --map Nevada         (add/resume/kmz/remove export by themselves)

Card format "PBMAP 2" (PIPLINK/MAP/ on the SD card):
    INFO.TXT     "PBMAP 2", "zoom <min> <max>", "center <lat> <lon>", "bundle 8", "bucket 13", "types ..."
    ICONS.BIN    marker icons, 32 bytes each (map_icons.py)
    B/<z>/<bx>/<by>.PBB   8x8 tiles (bx = x div 8, by = y div 8): 64 x (u32 offset, u32 length) LE indexed
                 by (y mod 8) * 8 + (x mod 8), then the tiles. Offset 0 = the map doesn't have that tile;
                 length 0 = an empty tile. A tile: 4 strips of 64 rows, each u16 length + heatshrink(w8,l6)
                 of 256x64 2bpp (length 0 = blank strip).
    M/<x>_<y>.TXT         landmarks by zoom-13 tile, "lat,lon,typeIndex,name" per line
    PINS.TXT     the KMZ pins ("lat,lon,TYPE,name") - merged into the Pip-Boy's saved locations (PIPLINK/WPT.TXT)
                 when the map is sent or copied; the Pip-Boy doesn't read this file
"""
import argparse
import concurrent.futures
import json
import math
import os
import re
import shutil
import sqlite3
import struct
import sys
import threading
import time

import heatshrink2

import map_build as mb
import map_icons
import osm_extract

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "PIP LINK")
ROOT = os.path.join(DATA, "maps")
CACHE = os.path.join(DATA, "cache")
STYLES = os.path.join(HERE, "marker_styles.json")
PACK = os.path.join("SD card", "PIPLINK", "MAP")
BUNDLE = 8
BUCKET_Z = mb.BUCKET_Z
ZOOM_MIN, ZOOM_MAX = 8, 17
# rough sizes for estimates (bytes per tile with contours; town tiles are bigger, desert tiles smaller)
AVG_TILE = {8: 2500, 9: 2500, 10: 2500, 11: 2500, 12: 2600, 13: 2800, 14: 4000, 15: 4000, 16: 3500, 17: 3000}
DRAW_RATE = 40           # tiles a second (measured ~40-70)
PIECE_SECS = 12          # downloading one piece (street data + elevation)
SCHEMA = """
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
CREATE TABLE IF NOT EXISTS tiles(z INTEGER, x INTEGER, y INTEGER, gen INTEGER, n INTEGER, data BLOB,
                                 PRIMARY KEY(z, x, y));
CREATE TABLE IF NOT EXISTS areas(id INTEGER PRIMARY KEY, name TEXT, s REAL, w REAL, n REAL, e REAL,
                                 zmin INTEGER, zmax INTEGER, opts TEXT, made TEXT, done INTEGER DEFAULT 0,
                                 chunks TEXT DEFAULT '[]');
CREATE TABLE IF NOT EXISTS markers(lat REAL, lon REAL, type TEXT, name TEXT, PRIMARY KEY(lat, lon, type, name));
CREATE TABLE IF NOT EXISTS pins(lat REAL, lon REAL, type TEXT, name TEXT, src TEXT);
CREATE TABLE IF NOT EXISTS pins_gone(lat REAL, lon REAL, type TEXT, name TEXT);
CREATE TABLE IF NOT EXISTS lines(src TEXT, pts TEXT);
CREATE TABLE IF NOT EXISTS bundles(z INTEGER, x INTEGER, y INTEGER, gen INTEGER, PRIMARY KEY(z, x, y));
"""


class Cancelled(Exception):
    pass


# ------------------------------------------------------------------ tile maths
def tile_range(rect, z):
    """Tiles at zoom z touching rect (s, w, n, e): (x0, y0, x1, y1), inclusive."""
    s, w, n, e = rect
    k = 2 ** z / mb.TILE
    x0, y0 = mb.w0(n, w)
    x1, y1 = mb.w0(s, e)
    top = 2 ** z - 1
    return (max(0, int(x0 * k)), max(0, int(y0 * k)), min(top, int(x1 * k)), min(top, int(y1 * k)))


def w0_rect(wx0, wy0, wx1, wy1):
    """z0 world px box -> (s, w, n, e)."""
    n, w = mb.unw0(wx0, wy0)
    s, e = mb.unw0(wx1, wy1)
    return (s, w, n, e)


def around(lat, lon, km):
    """(s, w, n, e) of a square 2*km across centred on lat, lon."""
    dlat = km / 111.0
    dlon = km / (111.0 * math.cos(math.radians(lat)))
    return (lat - dlat, lon - dlon, lat + dlat, lon + dlon)


def rect_km(rect):
    s, w, n, e = rect
    mid = (s + n) / 2
    return mb.km_between((mid, w), (mid, e)), mb.km_between((s, w), (n, w))


def tiers(zmin, zmax):
    """How an area is fetched and drawn: [(zooms, piece zoom)]. Zoom <= 12 needs only big roads, so it comes
    in big pieces (a zoom-8 tile, ~150 km); the street zooms come in pieces 4 zooms out from the deepest one
    (16x16 tiles of it: ~60 km for zoom 13, ~20 km for zoom 15, ~10 km for zoom 16+)."""
    lo = [z for z in range(zmin, zmax + 1) if z <= 12]
    hi = [z for z in range(zmin, zmax + 1) if z > 12]
    out = []
    if lo:
        out.append((lo, min(8, lo[0])))
    if hi:
        out.append((hi, min(12, max(9, zmax - 4))))
    return out


def dem_zoom(zmax, rect):
    """Elevation tile zoom for contours drawn up to zmax: as before for town-sized maps, one coarser for big
    areas (z13 elevation is ~15 m - plenty for 20 ft contours at zoom 15, and a quarter of the downloads)."""
    w, h = rect_km(rect)
    big = w * h > 2000
    return max(9, min(14, zmax - (2 if big and zmax >= 15 else 1)))


# ------------------------------------------------------------------ the map database
class MapDB:
    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        os.makedirs(self.folder, exist_ok=True)
        self.lock = threading.RLock()
        self.db = sqlite3.connect(os.path.join(self.folder, "map.db"), check_same_thread=False, timeout=60)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")
        self.db.executescript(SCHEMA)

    @property
    def name(self):
        return os.path.basename(self.folder)

    @property
    def pack_dir(self):
        return os.path.join(self.folder, PACK)

    def q(self, sql, args=()):
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def x(self, sql, args=(), many=False):
        with self.lock:
            (self.db.executemany if many else self.db.execute)(sql, args)
            self.db.commit()

    def close(self):
        with self.lock:
            self.db.close()

    def get(self, k, default=None):
        r = self.q("SELECT v FROM meta WHERE k=?", (k,))
        return json.loads(r[0][0]) if r else default

    def set(self, k, v):
        self.x("INSERT OR REPLACE INTO meta VALUES(?,?)", (k, json.dumps(v)))

    # ---- areas
    def areas(self):
        cols = ("id", "name", "s", "w", "n", "e", "zmin", "zmax", "opts", "made", "done", "chunks")
        out = []
        for r in self.q("SELECT " + ",".join(cols) + " FROM areas ORDER BY id"):
            a = dict(zip(cols, r))
            a["rect"] = (a["s"], a["w"], a["n"], a["e"])
            a["opts"] = json.loads(a["opts"] or "{}")
            a["chunks"] = json.loads(a["chunks"] or "[]")
            out.append(a)
        return out

    def area(self, area_id):
        for a in self.areas():
            if a["id"] == area_id:
                return a
        raise KeyError(f"no area {area_id}")

    # ---- tiles
    def count(self, z, x0, y0, x1, y1):
        return self.q("SELECT COUNT(*) FROM tiles WHERE z=? AND x BETWEEN ? AND ? AND y BETWEEN ? AND ?",
                      (z, x0, x1, y0, y1))[0][0]

    def existing(self, z, x0, y0, x1, y1):
        return set(self.q("SELECT x, y FROM tiles WHERE z=? AND x BETWEEN ? AND ? AND y BETWEEN ? AND ?",
                          (z, x0, x1, y0, y1)))

    def get_tile(self, z, x, y):
        """The tile's data (b"" = an empty tile), or None when the map doesn't have it."""
        r = self.q("SELECT data FROM tiles WHERE z=? AND x=? AND y=?", (z, x, y))
        return bytes(r[0][0]) if r else None

    def zoom_range(self):
        r = self.q("SELECT MIN(z), MAX(z) FROM tiles")[0]
        return (r[0], r[1]) if r[0] is not None else None

    def stats(self):
        n, size = self.q("SELECT COUNT(*), COALESCE(SUM(n), 0) FROM tiles")[0]
        return {"tiles": n, "bytes": size + 512 * max(1, n // 40), "zooms": self.zoom_range(),
                "areas": len(self.areas())}

    def center(self):
        home = self.get("home")
        if home:
            return tuple(home)
        a = self.areas()
        if a:
            s, w, n, e = a[0]["rect"]
            return ((s + n) / 2, (w + e) / 2)
        return None

    # ---- markers + pins
    def markers(self):
        return self.q("SELECT lat, lon, type, name FROM markers")

    def pins(self):
        return self.q("SELECT lat, lon, type, name FROM pins")

    def pin_rows(self):
        """[(id, lat, lon, type, name, src)] for the pins editor."""
        return self.q("SELECT rowid, lat, lon, type, name, src FROM pins ORDER BY name COLLATE NOCASE")

    def lines(self):
        return [json.loads(p) for (p,) in self.q("SELECT pts FROM lines")]


def safe_name(name):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "", name).strip().strip(".")
    return name[:60]


def list_maps(root=ROOT):
    if not os.path.isdir(root):
        return []
    return sorted((d for d in os.listdir(root) if os.path.exists(os.path.join(root, d, "map.db"))), key=str.lower)


def open_map(name, root=ROOT):
    n = safe_name(name)
    if not n:
        raise ValueError("give the map a name")
    return MapDB(os.path.join(root, n))


def rename_map(db, new, root=ROOT):
    """Closes db; returns the renamed map, opened."""
    n = safe_name(new)
    if not n:
        raise ValueError("give the map a name")
    dest = os.path.join(root, n)
    same = os.path.normcase(os.path.abspath(dest)) == os.path.normcase(db.folder)   # only the capitals change
    if os.path.exists(dest) and not same:
        raise ValueError(f"there's already a map called {n}")
    db.close()
    os.rename(db.folder, dest)
    return MapDB(dest)


def delete_map(db):
    db.close()
    shutil.rmtree(db.folder, ignore_errors=True)


# ------------------------------------------------------------------ planning
def estimate(db, rect, zmin, zmax, redo=False):
    """What adding rect at zmin..zmax would take: new/existing tiles, bytes, seconds, pieces."""
    new = have = nbytes = 0
    for z in range(zmin, zmax + 1):
        x0, y0, x1, y1 = tile_range(rect, z)
        tot = (x1 - x0 + 1) * (y1 - y0 + 1)
        h = db.count(z, x0, y0, x1, y1) if db and not redo else 0
        new += tot - h
        have += h
        nbytes += (tot - h) * AVG_TILE.get(z, 3000)
    pieces = 0
    for zooms, cz in tiers(zmin, zmax):
        x0, y0, x1, y1 = tile_range(rect, cz)
        pieces += (x1 - x0 + 1) * (y1 - y0 + 1)
    ex = osm_extract.for_bbox(rect, 0.05)
    secs = new / DRAW_RATE + (pieces * (4 if ex else PIECE_SECS) if new else 0)
    return {"new": new, "have": have, "bytes": nbytes, "secs": secs, "pieces": pieces,
            "offline": ex.name if ex else None}


def add_area(db, name, rect, zmin, zmax, contours=True, landmarks=True, buildings=True, redo=False):
    s, w, n, e = rect
    if not (s < n and w < e):
        raise ValueError("the area is empty")
    zmin, zmax = max(ZOOM_MIN, zmin), min(ZOOM_MAX, zmax)
    if zmin > zmax:
        raise ValueError("pick a zoom range")
    opts = {"contours": contours, "landmarks": landmarks, "buildings": buildings, "redo": redo, "fresh": False}
    with db.lock:
        cur = db.db.execute("INSERT INTO areas(name, s, w, n, e, zmin, zmax, opts, made) VALUES(?,?,?,?,?,?,?,?,?)",
                            (name, s, w, n, e, zmin, zmax, json.dumps(opts), time.strftime("%Y-%m-%d")))
        db.db.commit()
        return cur.lastrowid


def update_area(db, area_id):
    """Draw the area again from fresh OpenStreetMap data (new roads etc.); run build() next."""
    a = db.area(area_id)
    opts = dict(a["opts"], redo=True, fresh=True)
    db.x("UPDATE areas SET done=0, chunks='[]', opts=? WHERE id=?", (json.dumps(opts), area_id))


def remove_area(db, area_id, log=print):
    """Delete an area and the tiles and landmarks only it covers (tiles another area covers stay)."""
    a = db.area(area_id)
    others = [o for o in db.areas() if o["id"] != area_id]
    removed = 0
    with db.lock:
        for z in range(a["zmin"], a["zmax"] + 1):
            x0, y0, x1, y1 = tile_range(a["rect"], z)
            cond, args = "z=? AND x BETWEEN ? AND ? AND y BETWEEN ? AND ?", [z, x0, x1, y0, y1]
            for o in others:
                if o["zmin"] <= z <= o["zmax"]:
                    ox0, oy0, ox1, oy1 = tile_range(o["rect"], z)
                    cond += " AND NOT (x BETWEEN ? AND ? AND y BETWEEN ? AND ?)"
                    args += [ox0, ox1, oy0, oy1]
            removed += db.db.execute("DELETE FROM tiles WHERE " + cond, args).rowcount
            db.db.execute("DELETE FROM bundles WHERE z=? AND x BETWEEN ? AND ? AND y BETWEEN ? AND ?",
                          (z, x0 // BUNDLE, x1 // BUNDLE, y0 // BUNDLE, y1 // BUNDLE))
        s, w, n, e = a["rect"]
        cond, args = "lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?", [s, n, w, e]
        for o in others:
            cond += " AND NOT (lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?)"
            args += [o["s"], o["n"], o["w"], o["e"]]
        db.db.execute("DELETE FROM markers WHERE " + cond, args)
        db.db.execute("DELETE FROM areas WHERE id=?", (area_id,))
        db.db.commit()
    log(f"removed {a['name']}: {removed} tiles")
    return removed


# ------------------------------------------------------------------ building
def _fetch(bbox, core, zooms, opts, cache, pois, dz):
    """Street data (+ landmarks) and contours for one piece. Runs on the download thread. core = the part of
    the piece inside the area: offline data that has it is used (beyond the extract there's just no data)."""
    zt = max(zooms)
    buildings = opts.get("buildings") and zt >= 16
    ex = osm_extract.for_bbox(core, 0.05)                     # offline data for this area? (osm_extract.py)
    if ex:
        data = ex.query(bbox, buildings, pois, zt)
    else:
        data = mb.overpass(bbox, os.path.join(cache, "osm"), buildings, pois, zt, opts.get("fresh", False))
    feats, places, found = mb.parse_osm(data)
    if opts.get("contours", True):
        feats = mb.make_contours(bbox, dz, os.path.join(cache, "dem_cache"), zooms) + feats
    return feats, places, found


def _todo(db, rect, zooms, cz, cx, cy, redo):
    """{zoom: {(x, y)}} - the piece's tiles inside the area that still need drawing."""
    todo = {}
    for z in zooms:
        f = 2 ** (z - cz)
        rx0, ry0, rx1, ry1 = tile_range(rect, z)
        x0, y0 = max(rx0, cx * f), max(ry0, cy * f)
        x1, y1 = min(rx1, cx * f + f - 1), min(ry1, cy * f + f - 1)
        if x0 > x1 or y0 > y1:
            continue
        want = {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}
        if not redo:
            want -= db.existing(z, x0, y0, x1, y1)
        if want:
            todo[z] = want
    return todo


def _piece_size(rect, zooms, cz, cx, cy):
    n = 0
    for z in zooms:
        f = 2 ** (z - cz)
        rx0, ry0, rx1, ry1 = tile_range(rect, z)
        w = min(rx1, cx * f + f - 1) - max(rx0, cx * f) + 1
        h = min(ry1, cy * f + f - 1) - max(ry0, cy * f) + 1
        n += max(0, w) * max(0, h)
    return n


def _data_box(todo):
    """(s, w, n, e) of the data a piece needs: its tiles plus room for lines and labels crossing the edge."""
    bx = [math.inf, math.inf, -math.inf, -math.inf]
    for z, tiles in todo.items():
        k = mb.TILE / 2 ** z
        bx[0] = min(bx[0], min(x for x, _ in tiles) * k)
        bx[1] = min(bx[1], min(y for _, y in tiles) * k)
        bx[2] = max(bx[2], (max(x for x, _ in tiles) + 1) * k)
        bx[3] = max(bx[3], (max(y for _, y in tiles) + 1) * k)
    m = (mb.PAD + 32) / 2 ** min(todo)
    return w0_rect(bx[0] - m, bx[1] - m, bx[2] + m, bx[3] + m)


def build(db, area_id, log=print, progress=None, cancel=None, cache=CACHE):
    """Draw an area's missing tiles into the map. progress(fraction, text). Raises Cancelled when cancel
    (a threading.Event) is set - run it again to continue."""
    mb.LOG = log
    a = db.area(area_id)
    rect, zmin, zmax, opts, title = a["rect"], a["zmin"], a["zmax"], a["opts"], a["name"]
    redo = opts.get("redo", False)
    gen = (db.get("gen", 0) or 0) + 1
    db.set("gen", gen)
    lines = [ln for ln in db.lines() if len(ln) >= 2]
    done = set(a["chunks"])
    dz = dem_zoom(zmax, rect)
    plan = []                                                 # (landmarks here?, zooms, cz, cx, cy)
    for ti, (zooms, cz) in enumerate(tiers(zmin, zmax)):
        x0, y0, x1, y1 = tile_range(rect, cz)
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        for cx, cy in sorted(((x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)),
                             key=lambda c: (c[0] - mx) ** 2 + (c[1] - my) ** 2):   # middle first
            if f"{cz}/{cx}/{cy}" not in done:
                plan.append((ti == 0 and opts.get("landmarks", True), zooms, cz, cx, cy))
    sizes = [_piece_size(rect, p[1], p[2], p[3], p[4]) for p in plan]
    total = max(1, sum(sizes))
    w, h = rect_km(rect)
    log(f"{title}: {w:.0f} x {h:.0f} km, zoom {zmin}-{zmax}, {len(plan)} pieces to do"
        + (" (redrawing everything)" if redo else ""))
    state = {"done": 0, "drawn": 0, "t0": time.time()}

    def report(text):
        if not progress:
            return
        frac = state["done"] / total
        el = time.time() - state["t0"]
        eta = ""
        if frac > 0.05 and el > 30:
            left = el / frac * (1 - frac)
            eta = f"  -  about {fmt_secs(left)} left"
        progress(frac, text + eta)

    def prepare(i):
        if i >= len(plan):
            return None
        pois, zooms, cz, cx, cy = plan[i]
        todo = _todo(db, rect, zooms, cz, cx, cy, redo)
        if not todo:
            return todo, None
        box = _data_box(todo)
        core = (max(box[0], rect[0]), max(box[1], rect[1]), min(box[2], rect[2]), min(box[3], rect[3]))
        return todo, pool.submit(_fetch, box, core, sorted(todo), opts, cache, pois, dz)

    pool = concurrent.futures.ThreadPoolExecutor(1)           # downloads the next piece while this one draws
    try:
        nxt = prepare(0)
        for i, (pois, zooms, cz, cx, cy) in enumerate(plan):
            todo, fut = nxt
            nxt = prepare(i + 1)
            where = f"piece {i + 1} of {len(plan)}"
            if todo:
                report(f"{title}: downloading map data ({where})")
                while True:
                    if cancel and cancel.is_set():
                        raise Cancelled()
                    try:
                        feats, places, found = fut.result(timeout=0.5)
                        break
                    except concurrent.futures.TimeoutError:
                        continue
                if pois:
                    cs, cw, cn, ce = w0_rect(cx * mb.TILE / 2 ** cz, cy * mb.TILE / 2 ** cz,
                                             (cx + 1) * mb.TILE / 2 ** cz, (cy + 1) * mb.TILE / 2 ** cz)
                    s, w_, n, e = rect
                    keep = [(round(la, 6), round(lo, 6), t, nm) for la, lo, t, nm in found
                            if max(s, cs) <= la <= min(n, cn) and max(w_, cw) <= lo <= min(e, ce)]
                    db.x("INSERT OR IGNORE INTO markers VALUES(?,?,?,?)", keep, many=True)
                for z in sorted(todo):
                    def drew(n, z=z):
                        state["done"] += n
                        state["drawn"] += n
                        report(f"{title}: drawing zoom {z} ({where})")
                    _draw(db, z, todo[z], feats, places, lines, gen, cancel, drew)
                state["done"] += sizes[i] - sum(len(t) for t in todo.values())
            else:
                state["done"] += sizes[i]
            with db.lock:                                     # this piece is finished: a resume skips it
                row = db.db.execute("SELECT chunks FROM areas WHERE id=?", (area_id,)).fetchone()
                ch = json.loads(row[0] or "[]") + [f"{cz}/{cx}/{cy}"]
                db.db.execute("UPDATE areas SET chunks=? WHERE id=?", (json.dumps(ch), area_id))
                db.db.commit()
            report(f"{title}: {where} done")
    finally:
        pool.shutdown(wait=False, cancel_futures=True)
    o = dict(opts, redo=False, fresh=False)
    db.x("UPDATE areas SET done=1, chunks='[]', opts=? WHERE id=?", (json.dumps(o), area_id))
    if not db.get("home"):
        s, w_, n, e = rect
        db.set("home", [(s + n) / 2, (w_ + e) / 2])
    el = time.time() - state["t0"]
    log(f"{title}: done - {state['drawn']} tiles drawn in {fmt_secs(el)}")
    return state["drawn"]


def _draw(db, z, tiles, feats, places, lines, gen, cancel, step):
    """Draw tiles {(x, y)} at zoom z into the map."""
    rows = []

    def save():                                               # in batches: a commit costs ~0.2 s
        if rows:
            db.x("INSERT OR REPLACE INTO tiles VALUES(?,?,?,?,?,?)", rows, many=True)
            step(len(rows))
            rows.clear()

    def out(tx, ty, blob):
        rows.append((z, tx, ty, gen, len(blob), blob))
        if len(rows) >= 64:
            save()
    try:
        _render(z, tiles, feats, places, lines, cancel, out)
    finally:
        save()                                                # keep what's drawn, even when stopped


def preview(db, rect, zmax, z, tiles, opts, cache=CACHE, log=print):
    """Draw tiles {(x, y)} at zoom z the way adding rect (deepest zoom zmax) with opts would - KMZ lines, landmarks,
    buildings and contours included - without saving anything. Returns ({(x, y): data}, [landmarks])."""
    mb.LOG = log
    box = _data_box({z: tiles})
    feats, places, found = _fetch(box, box, [z], opts, cache, opts.get("landmarks", True), dem_zoom(zmax, rect))
    out = {}
    _render(z, tiles, feats, places, [ln for ln in db.lines() if len(ln) >= 2], None,
            lambda tx, ty, blob: out.__setitem__((tx, ty), blob))
    s, w, n, e = w0_rect(*[v * mb.TILE / 2 ** z for v in (min(x for x, _ in tiles), min(y for _, y in tiles),
                                                          max(x for x, _ in tiles) + 1, max(y for _, y in tiles) + 1)])
    return out, [p for p in found if s <= p[0] <= n and w <= p[1] <= e]


def _render(z, tiles, feats, places, lines, cancel, out):
    """Draw tiles {(x, y)} at zoom z: out(x, y, data) for each."""
    k = mb.TILE / 2 ** z
    area = (min(x for x, _ in tiles) * k, min(y for _, y in tiles) * k,
            (max(x for x, _ in tiles) + 1) * k, (max(y for _, y in tiles) + 1) * k)

    def allow(box):                                           # labels stay inside the tiles drawn now
        for x in range(int(box[0] // mb.TILE), int(box[2] // mb.TILE) + 1):
            for y in range(int(box[1] // mb.TILE), int(box[3] // mb.TILE) + 1):
                if (x, y) not in tiles:
                    return False
        return True

    labels = mb.place_labels(z, feats, places, allow, area)
    metas = {}
    for tx, ty in tiles:
        metas.setdefault((tx // mb.META, ty // mb.META), []).append((tx, ty))
    for (mx, my), tl in sorted(metas.items()):
        if cancel and cancel.is_set():
            raise Cancelled()
        img, x0, y0 = mb.render_meta(z, mx, my, feats, lines, labels)
        for tx, ty in tl:
            ox, oy = tx * mb.TILE - x0, ty * mb.TILE - y0
            out(tx, ty, mb.encode_tile(img.crop((ox, oy, ox + mb.TILE, oy + mb.TILE))) or b"")


def fmt_secs(s):
    if s < 90:
        return f"{max(1, round(s))} s"
    if s < 5400:
        return f"{round(s / 60)} min"
    return f"{s / 3600:.1f} h"


# ------------------------------------------------------------------ KMZ pins + lines
def add_kmz(db, paths, styles=STYLES, log=print):
    """Add Google Earth files: placemarks become pins (the Pip-Boy's GO TO list), lines/polygons are drawn into
    tiles built from now on. Adding a file again replaces what it added before. Returns (pins, lines)."""
    cfg = {}
    if styles and os.path.exists(styles):
        with open(styles, encoding="utf-8") as f:
            cfg = json.load(f)
    npins = nlines = 0
    for p in paths:
        pts, lines, report = mb.parse_kml(p, cfg)
        src = os.path.basename(p)
        keep = {(round(la, 6), round(lo, 6), t, nm) for la, lo, t, nm in pts}
        _forget(db, [i for i, la, lo, t, nm in db.q("SELECT rowid, lat, lon, type, name FROM pins WHERE src=?", (src,))
                     if (round(la, 6), round(lo, 6), t, nm) not in keep])   # gone from the new version of the file
        with db.lock:
            db.db.execute("DELETE FROM pins WHERE src=?", (src,))
            db.db.execute("DELETE FROM lines WHERE src=?", (src,))
            db.db.executemany("INSERT INTO pins VALUES(?,?,?,?,?)",
                              [(round(la, 6), round(lo, 6), t, nm, src) for la, lo, t, nm in pts])
            db.db.executemany("INSERT INTO lines VALUES(?,?)", [(src, json.dumps(ln)) for ln in lines])
            db.db.commit()
        log(f"kml: {src}: {len(pts)} pins, {len(lines)} lines/areas")
        for (key, typ, why), n in sorted(report.items(), key=lambda kv: -kv[1]):
            log(f"  {key[:42]:42} -> {typ:11} ({why:7}) {n}")
        npins += len(pts)
        nlines += len(lines)
    return npins, nlines


def clear_pins(db):
    _forget(db, db.q("SELECT rowid FROM pins"))
    db.x("DELETE FROM pins")
    db.x("DELETE FROM lines")


# Editing pins. A pin that was changed or deleted may already be on the Pip-Boy: its old version goes into
# pins_gone, and sending the map writes a "-" (deleted) line for it into the Pip-Boy's WPT.TXT when an entry
# exactly like it is there - so a corrected icon or a removed duplicate is corrected there too.
def _forget(db, ids):
    ids = [i[0] if isinstance(i, (tuple, list)) else i for i in ids]
    rows = [r for i in ids for r in db.q("SELECT lat, lon, type, name FROM pins WHERE rowid=?", (i,))]
    if rows:
        db.x("INSERT INTO pins_gone VALUES(?,?,?,?)", rows, many=True)


def clean_name(name):
    return mb.ascii_name(name)


def add_pin(db, lat, lon, typ, name, src="added"):
    with db.lock:
        cur = db.db.execute("INSERT INTO pins VALUES(?,?,?,?,?)", (round(lat, 6), round(lon, 6), typ, clean_name(name), src))
        db.db.commit()
        return cur.lastrowid


def update_pin(db, pid, lat, lon, typ, name):
    old = db.q("SELECT lat, lon, type, name FROM pins WHERE rowid=?", (pid,))
    new = (round(lat, 6), round(lon, 6), typ, clean_name(name))
    if not old or tuple(old[0]) == new:
        return
    _forget(db, [pid])
    db.x("UPDATE pins SET lat=?, lon=?, type=?, name=? WHERE rowid=?", new + (pid,))


def delete_pins(db, ids):
    _forget(db, ids)
    db.x("DELETE FROM pins WHERE rowid=?", [(i,) for i in ids], many=True)


def duplicate_pins(db, metres=40):
    """Pins that repeat another one: [(keep id, [duplicate ids])] - same name (any case) within `metres`, or any
    two pins within 5 m. The first one added is kept."""
    rows = db.q("SELECT rowid, lat, lon, name FROM pins ORDER BY rowid")
    out, used = [], set()
    for i, (a, la, lo, nm) in enumerate(rows):
        if a in used:
            continue
        dups = []
        for b, lb, lob, nb in rows[i + 1:]:
            if b in used:
                continue
            d = mb.km_between((la, lo), (lb, lob)) * 1000
            if (d <= metres and nm.lower() == nb.lower()) or d <= 5:
                dups.append(b)
                used.add(b)
        if dups:
            out.append((a, dups))
    return out


# ------------------------------------------------------------------ export (the SD card copy of the map)
def bundle_bytes(tiles):
    """tiles: {(x mod 8, y mod 8): data} -> a .PBB file."""
    head = bytearray(BUNDLE * BUNDLE * 8)
    body = bytearray()
    for (bx, by), data in sorted(tiles.items()):
        struct.pack_into("<II", head, (by * BUNDLE + bx) * 8, len(head) + len(body), len(data))
        body += data
    return bytes(head + body)


def export(db, log=print, progress=None):
    """Bring "<map>/SD card/PIPLINK/MAP" up to date: rewrites only the bundles with new tiles."""
    out = db.pack_dir
    zr = db.zoom_range()
    if not zr:
        raise RuntimeError("the map is empty - add an area first")
    os.makedirs(out, exist_ok=True)
    center = db.center() or (0.0, 0.0)
    _write_if_changed(os.path.join(out, "INFO.TXT"),
                      (f"PBMAP 2\nzoom {zr[0]} {zr[1]}\ncenter {center[0]:.6f} {center[1]:.6f}\nbundle {BUNDLE}\n"
                       f"bucket {BUCKET_Z}\ntypes " + ",".join(map_icons.TYPES) + "\n"
                       f"name {mb.ascii_name(db.name, 60)}\n").encode())   # so sending can tell maps apart
    _write_if_changed(os.path.join(out, "ICONS.BIN"), map_icons.icons_bin())
    with open(os.path.join(db.folder, "SD card", "READ ME.TXT"), "w", newline="\r\n") as f:
        f.write(f"PIP LINK map: {db.name}\n\nCopy the PIPLINK folder to the top of the Pip-Boy's SD card (merge it with\n"
                "the PIPLINK folder that's there). PIP LINK Setup's COPY TO SD CARD does this for you and also\n"
                "adds the map's KMZ pins to the Pip-Boy's saved locations.\n")

    # landmarks, bucketed by zoom-13 tile
    mdir = os.path.join(out, "M")
    os.makedirs(mdir, exist_ok=True)
    buckets = {}
    for lat, lon, typ, name in db.markers():
        x, y = mb.w0(lat, lon)
        key = (int(x * 2 ** BUCKET_Z // mb.TILE), int(y * 2 ** BUCKET_Z // mb.TILE))
        buckets.setdefault(key, []).append(f"{lat:.6f},{lon:.6f},{map_icons.type_index(typ)},{name}")
    names = set()
    for (bx, by), lines in buckets.items():
        names.add(f"{bx}_{by}.TXT")
        _write_if_changed(os.path.join(mdir, f"{bx}_{by}.TXT"), ("\n".join(sorted(lines)) + "\n").encode())
    for fn in os.listdir(mdir):
        if fn not in names:
            os.remove(os.path.join(mdir, fn))
    export_pins(db)

    # tile bundles: rewrite those holding newer tiles than last time, drop those that are empty now
    want = {(z, bx, by): g for z, bx, by, g in db.q(
        f"SELECT z, x / {BUNDLE}, y / {BUNDLE}, MAX(gen) FROM tiles GROUP BY z, x / {BUNDLE}, y / {BUNDLE}")}
    have = {(z, x, y): g for z, x, y, g in db.q("SELECT z, x, y, gen FROM bundles")}
    todo = [k for k, g in want.items() if have.get(k) != g or not os.path.exists(_bundle_path(out, *k))]
    written = []
    for i, (z, bx, by) in enumerate(sorted(todo)):
        rows = db.q("SELECT x, y, data FROM tiles WHERE z=? AND x BETWEEN ? AND ? AND y BETWEEN ? AND ?",
                    (z, bx * BUNDLE, bx * BUNDLE + BUNDLE - 1, by * BUNDLE, by * BUNDLE + BUNDLE - 1))
        path = _bundle_path(out, z, bx, by)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(bundle_bytes({(x % BUNDLE, y % BUNDLE): bytes(d) for x, y, d in rows}))
        os.replace(tmp, path)
        written.append((z, bx, by, want[(z, bx, by)]))
        if len(written) >= 200 or i == len(todo) - 1:
            db.x("INSERT OR REPLACE INTO bundles VALUES(?,?,?,?)", written, many=True)
            written = []
        if progress and (i % 20 == 0 or i == len(todo) - 1):
            progress((i + 1) / len(todo), f"preparing the SD card files {i + 1}/{len(todo)}")
    gone = 0
    bdir = os.path.join(out, "B")
    for d, _, fs in os.walk(bdir, topdown=False):
        for fn in fs:
            rel = os.path.relpath(os.path.join(d, fn), bdir).replace("\\", "/").split("/")
            try:
                key = (int(rel[0]), int(rel[1]), int(rel[2].split(".")[0]))
            except (ValueError, IndexError):
                key = None
            if key not in want or not fn.endswith(".PBB"):
                os.remove(os.path.join(d, fn))
                gone += 1
        if d != bdir and not os.listdir(d):
            os.rmdir(d)
    stale = [k for k in have if k not in want]
    if stale:
        db.x("DELETE FROM bundles WHERE z=? AND x=? AND y=?", stale, many=True)
    if todo or gone:
        log(f"SD card files: {len(todo)} updated" + (f", {gone} removed" if gone else ""))
    return pack_size(out)


def export_pins(db):
    """Just the pin files (PINS.TXT, and PINS.DEL: old versions of pins changed or removed - see _forget)."""
    out = db.pack_dir
    os.makedirs(out, exist_ok=True)
    for fn, rows in (("PINS.TXT", db.pins()), ("PINS.DEL", db.q("SELECT lat, lon, type, name FROM pins_gone"))):
        path = os.path.join(out, fn)
        if rows:
            _write_if_changed(path, "".join(f"{la:.6f},{lo:.6f},{t},{nm}\n" for la, lo, t, nm in rows).encode())
        elif os.path.exists(path):
            os.remove(path)


def _bundle_path(out, z, bx, by):
    return os.path.join(out, "B", str(z), str(bx), f"{by}.PBB")


def _write_if_changed(path, data):
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return
    except OSError:
        pass
    with open(path, "wb") as f:
        f.write(data)


def pack_size(path):
    n = size = 0
    for d, _, fs in os.walk(path):
        for f in fs:
            if not f.startswith("."):
                n += 1
                size += os.path.getsize(os.path.join(d, f))
    return {"files": n, "bytes": size}


# ------------------------------------------------------------------ reading tiles back (previews)
def tile_image(data):
    """A tile's data -> 256x256 'P' image with pixel values 0..3 (no palette set)."""
    import numpy as np
    from PIL import Image
    px = np.zeros((mb.TILE, mb.TILE), np.uint8)
    p = 0
    for s in range(mb.TILE // mb.STRIP):
        if p + 2 > len(data):
            break
        n = data[p] | (data[p + 1] << 8)
        p += 2
        if n:
            raw = np.frombuffer(heatshrink2.decompress(data[p:p + n], window_sz2=8, lookahead_sz2=6), np.uint8)
            q = np.stack([raw >> 6, (raw >> 4) & 3, (raw >> 2) & 3, raw & 3], axis=1).reshape(-1)
            px[s * mb.STRIP:(s + 1) * mb.STRIP] = q[:mb.STRIP * mb.TILE].reshape(mb.STRIP, mb.TILE)
        p += n
    return Image.frombytes("P", (mb.TILE, mb.TILE), px.tobytes())


class PackSource:
    """A map folder as the Pip-Boy sees it (PIPLINK/MAP): bundled (PBMAP 2) or one file per tile (PBMAP 1)."""
    def __init__(self, path):
        self.path = path
        self.info = {}
        with open(os.path.join(path, "INFO.TXT")) as f:
            for line in f:
                p = line.split()
                if p:
                    self.info[p[0]] = p[1:]
        self.bs = int(self.info["bundle"][0]) if "bundle" in self.info else 0

    def zoom_range(self):
        return int(self.info["zoom"][0]), int(self.info["zoom"][1])

    def center(self):
        return float(self.info["center"][0]), float(self.info["center"][1])

    def get_tile(self, z, x, y):
        if not self.bs:
            p = os.path.join(self.path, "T", str(z), str(x), f"{y}.PBT")
            return open(p, "rb").read() if os.path.exists(p) else None
        p = _bundle_path(self.path, z, x // self.bs, y // self.bs)
        if not os.path.exists(p):
            return None
        with open(p, "rb") as f:
            f.seek(((y % self.bs) * self.bs + x % self.bs) * 8)
            off, n = struct.unpack("<II", f.read(8))
            if not off:
                return None
            f.seek(off)
            return f.read(n)

    def markers(self):
        out = []
        mdir = os.path.join(self.path, "M")
        files = [os.path.join(mdir, f) for f in os.listdir(mdir)] if os.path.isdir(mdir) else []
        for fn in files:
            with open(fn) as f:
                for line in f:
                    p = line.strip().split(",", 3)
                    if len(p) == 4:
                        t = map_icons.TYPES[int(p[2])] if p[2].isdigit() and int(p[2]) < len(map_icons.TYPES) else p[2]
                        out.append((float(p[0]), float(p[1]), t, p[3]))
        return out


# ------------------------------------------------------------------ command line
def describe(db):
    st = db.stats()
    print(f"map {db.name}: {st['tiles']} tiles, {st['bytes'] / 1048576:.1f} MB, zoom "
          f"{st['zooms'][0]}-{st['zooms'][1]}" if st["zooms"] else f"map {db.name}: empty")
    for a in db.areas():
        w, h = rect_km(a["rect"])
        print(f"  #{a['id']:<3} {a['name'][:40]:40} {w:5.0f} x {h:<5.0f} km  zoom {a['zmin']}-{a['zmax']}  "
              + ("done" if a["done"] else f"UNFINISHED ({len(a['chunks'])} pieces done)"))
    print(f"  {len(db.markers())} landmarks, {len(db.pins())} KMZ pins, {len(db.lines())} KMZ lines")
    print(f"  SD card folder: {os.path.join(db.folder, 'SD card')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["list", "info", "add", "resume", "update", "remove", "kmz", "export"])
    ap.add_argument("files", nargs="*", help="kmz: Google Earth .kmz/.kml files")
    ap.add_argument("--root", default=ROOT, help="the map library folder")
    ap.add_argument("--map", default="My Map")
    ap.add_argument("--name", help="add: what to call the area")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--place", help="add: a place name (its whole extent, e.g. a county or state)")
    g.add_argument("--bbox", help="add: south,west,north,east")
    g.add_argument("--center", help="add: lat,lon (with --radius-km)")
    ap.add_argument("--radius-km", type=float, default=5.0)
    ap.add_argument("--zooms", default="10-15", help="add: e.g. 10-15 (8-17)")
    ap.add_argument("--no-contours", action="store_true")
    ap.add_argument("--no-pois", action="store_true", help="no automatic landmarks")
    ap.add_argument("--no-buildings", action="store_true")
    ap.add_argument("--redo", action="store_true", help="add: redraw tiles the map already has")
    ap.add_argument("--area", type=int, help="resume/update/remove: area number (see info)")
    a = ap.parse_args()
    if a.cmd == "list":
        for n in list_maps(a.root):
            describe(MapDB(os.path.join(a.root, n)))
        return
    db = open_map(a.map, a.root)
    try:
        if a.cmd == "add":
            if a.place:
                hits = mb.search_places(a.place, 1)
                if not hits:
                    sys.exit(f"can't find {a.place}")
                name, lat, lon, rect = hits[0]
                print(f"place: {name}")
                title = a.name or a.place
                if not rect or min(rect_km(rect)) < 1:        # an address or a point: --radius-km around it
                    rect = around(lat, lon, a.radius_km)
            elif a.bbox:
                rect = tuple(float(v) for v in a.bbox.split(","))
                title = a.name or "area"
            elif a.center:
                lat, lon = (float(v) for v in a.center.split(","))
                rect = around(lat, lon, a.radius_km)
                title = a.name or f"{a.radius_km:g} km around {lat:.4f},{lon:.4f}"
            else:
                sys.exit("add needs --place, --bbox or --center")
            zmin, zmax = (int(v) for v in a.zooms.split("-"))
            est = estimate(db, rect, zmin, zmax, a.redo)
            print(f"{est['new']} tiles to draw ({est['have']} already in the map), ~{est['bytes'] / 1048576:.0f} MB, "
                  f"roughly {fmt_secs(est['secs'])}")
            aid = add_area(db, title, rect, zmin, zmax, not a.no_contours, not a.no_pois, not a.no_buildings, a.redo)
            build(db, aid, progress=_cli_progress)
        elif a.cmd in ("resume", "update"):
            ids = [a.area] if a.area else [x["id"] for x in db.areas() if not x["done"]]
            if a.cmd == "update":
                if not a.area:
                    sys.exit("update needs --area")
                update_area(db, a.area)
            for aid in ids:
                build(db, aid, progress=_cli_progress)
        elif a.cmd == "remove":
            if not a.area:
                sys.exit("remove needs --area")
            remove_area(db, a.area)
        elif a.cmd == "kmz":
            add_kmz(db, a.files)
        if a.cmd != "info":
            print()
            st = export(db)
            print(f"SD card folder: {st['files']} files, {st['bytes'] / 1048576:.1f} MB")
        describe(db)
    except Cancelled:
        print("\nstopped")
    finally:
        db.close()


def _cli_progress(frac, text):
    print(f"\r  {frac * 100:5.1f}%  {text[:100]:100}", end="", flush=True)


if __name__ == "__main__":
    main()
