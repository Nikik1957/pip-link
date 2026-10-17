"""Drawing a Pip-Boy map: OpenStreetMap data + terrain contours -> Fallout-style 2bpp tiles.

The pieces used by map_lib.py (the map library, which decides what to draw and stores it - run that for the
command line): fetching street data, contours, KMZ/KML pins and lines, label placement, rendering metatiles
and encoding tiles.

A tile is 256x256 2bpp in 4 strips of 64 rows, each u16 len + heatshrink(w8,l6) (len 0 = empty strip).

Street data: OpenStreetMap via the Overpass API (cached). (c) OpenStreetMap contributors, ODbL.
Terrain contours (feet): AWS Terrain Tiles (Terrarium; SRTM/NED/etc.), cached.
KMZ/KML: Google Earth placemarks become markers (icon style -> Fallout marker type, see marker_styles.json);
lines and polygons are drawn into the tiles as dashed overlays.
"""
import gzip
import hashlib
import json
import math
import os
import re
import threading
import time
import zipfile
import xml.etree.ElementTree as ET

import heatshrink2
import requests
from PIL import Image, ImageDraw, ImageFont

import map_icons

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = print            # map_lib / the setup app swap this for their own log


def say(*a):
    LOG(" ".join(str(x) for x in a))


TILE = 256
SS = 3                 # supersampling (gives 4-level anti-aliasing after the box downsample)
META = 4               # tiles per metatile side
PAD = 96               # metatile margin (px) so lines/labels cross tile edges cleanly
STRIP = 64
BUCKET_Z = 13
UA = {"User-Agent": "PIP-LINK-mapbuilder/1.0 (personal offline map for a Pip-Boy)"}
FONTS = [os.path.join(HERE, "DejaVuSansMono-Bold.ttf"), r"C:\Windows\Fonts\DejaVuSansMono-Bold.ttf", r"C:\Windows\Fonts\LiberationMono-Bold.ttf",
         r"C:\Windows\Fonts\consolab.ttf", "DejaVuSansMono-Bold.ttf"]

# ------------------------------------------------------------------ projection (z0 world px 0..256)
def w0(lat, lon):
    s = math.sin(math.radians(max(-85.0, min(85.0, lat))))
    return ((lon + 180.0) / 360.0 * TILE, (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * TILE)


def unw0(x, y):
    lon = x / TILE * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / TILE))))
    return lat, lon


def km_between(a, b):
    la = math.radians((a[0] + b[0]) / 2)
    dx = math.radians(b[1] - a[1]) * math.cos(la) * 6371.0
    dy = math.radians(b[0] - a[0]) * 6371.0
    return math.hypot(dx, dy)


# ------------------------------------------------------------------ styles
# class: (min zoom, width px at z16, level 1..3, dashed)
ROAD = {
    "motorway": (10, 6.5, 2, False), "trunk": (10, 6.0, 2, False),
    "motorway_link": (13, 3.0, 2, False), "trunk_link": (13, 3.0, 2, False),
    "primary": (11, 5.5, 2, False), "primary_link": (13, 3.0, 2, False),
    "secondary": (12, 5.0, 2, False), "secondary_link": (14, 3.0, 2, False),
    "tertiary": (13, 4.0, 2, False), "tertiary_link": (14, 3.0, 2, False),
    "residential": (14, 3.0, 2, False), "unclassified": (14, 3.0, 2, False), "road": (14, 3.0, 2, False),
    "living_street": (15, 2.5, 2, False), "service": (15, 2.0, 2, False),
    "track": (13, 1.5, 1, True), "path": (15, 1.5, 1, True), "footway": (16, 1.5, 1, True),
    "cycleway": (15, 1.5, 1, True), "bridleway": (15, 1.5, 1, True), "steps": (16, 1.5, 1, True),
    "pedestrian": (15, 2.5, 2, False),
}
ROAD_RANK = ["motorway", "trunk", "primary", "secondary", "tertiary", "residential", "unclassified",
             "road", "living_street", "pedestrian", "service", "track"]
PLACE = {  # min z, max z, font px
    "city": (9, 14, 17), "town": (10, 15, 15), "village": (12, 16, 13), "suburb": (12, 16, 13),
    "quarter": (14, 17, 12), "neighbourhood": (15, 17, 11), "hamlet": (14, 17, 11),
}
OSM_POIS = [  # prominent landmarks only (no fuel, shops, cell towers...)
    ('nwr["amenity"="hospital"]["name"]', "MEDICAL"),
    ('nwr["amenity"~"^(townhall|courthouse)$"]', "CAPITOL"), ('nwr["office"="government"]["name"]', "CAPITOL"),
    ('nwr["amenity"~"^(police|fire_station)$"]', "MILITARY"), ('nwr["military"]["name"]', "MILITARY"),
    ('nwr["leisure"="stadium"]', "STADIUM"), ('nwr["leisure"="pitch"]["sport"="baseball"]["name"~"[Pp]ark|[Ss]tadium"]', "STADIUM"),
    ('nwr["amenity"~"^(university|college)$"]["name"]', "LANDMARK"), ('nwr["aeroway"="aerodrome"]["name"]', "AIRPORT"),
    ('nwr["tourism"~"^(camp_site|wilderness_hut)$"]', "CAMP"), ('nwr["natural"="cave_entrance"]', "CAVE"),
    ('nwr["natural"="peak"]["name"]', "NATURE"),
    ('nwr["historic"~"^(monument|castle|fort|ruins|archaeological_site)$"]["name"]', "LANDMARK"),
]


# contour interval (feet) per zoom; every 5th line is an index contour (thicker, labelled)
CONTOUR_FT = {10: 400, 11: 200, 12: 200, 13: 100, 14: 40, 15: 20, 16: 20, 17: 20}
OVERPASS = ["https://overpass-api.de/api/interpreter", "https://overpass.private.coffee/api/interpreter",
            "https://maps.mail.ru/osm/tools/overpass/api/interpreter", "https://overpass.kumi.systems/api/interpreter"]
DEM_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"


def width_at(w16, z):
    return max(1.0, w16 * 1.6 ** (z - 16))


# ------------------------------------------------------------------ OSM via Overpass
class QueryError(RuntimeError):
    """The server refused the query itself (HTTP 400) - asking again won't help."""


def overpass(bbox, cache_dir, buildings, pois, zmax=17, fresh=False):
    """OSM elements for bbox (s, w, n, e), only what a map up to zoom zmax draws. A lake or reservoir the
    whole box sits inside (no shoreline in it) is fetched too, so the middle of Great Salt Lake isn't blank.
    fresh: ask the server even when there's a cached answer (an UPDATE of an area)."""
    s, w, n, e = bbox
    b = f"{s:.5f},{w:.5f},{n:.5f},{e:.5f}"
    roads = "|".join(sorted(c for c, v in ROAD.items() if v[0] <= zmax))
    places = "|".join(k for k, v in PLACE.items() if v[0] <= zmax)
    lines = [f'way["highway"~"^({roads})$"]({b});',
             f'way["waterway"~"^(river|canal{"|stream" if zmax >= 14 else ""})$"]({b});',
             f'way["natural"="water"]({b});', f'relation["natural"="water"]({b});',
             f'way["landuse"="reservoir"]({b});', f'relation["landuse"="reservoir"]({b});',
             f'node["place"~"^({places})$"]({b});']
    if zmax >= 12:
        lines += [f'way["railway"~"^(rail|light_rail|subway|tram)$"]({b});', f'way["aeroway"="runway"]({b});']
    inside = [f'is_in({(s + n) / 2:.5f},{(w + e) / 2:.5f})->.a;',
              '(way(pivot.a)["natural"="water"]; relation(pivot.a)["natural"="water"];',
              ' way(pivot.a)["landuse"="reservoir"]; relation(pivot.a)["landuse"="reservoir"];)->.lake;']
    body = "\n".join(inside) + "\n(\n " + "\n ".join(lines) + "\n .lake;\n);\nout geom qt;\n"
    queries = [("streets/water/places", body)]
    if buildings:
        queries.append(("buildings", f'way["building"]({b});\nout geom qt;\n'))
    if pois:
        queries.append(("landmarks", "(\n " + "\n ".join(f"{f}({b});" for f, _ in OSM_POIS) + "\n);\nout tags center qt;\n"))
    elements = []
    for what, q in queries:
        try:
            try:
                elements += overpass_one(what, "[out:json][timeout:180];\n" + q, cache_dir, fresh)
            except QueryError:
                if q is not body:
                    raise
                say(f"osm: {what}: the server can't do lake lookups - asking without")
                q = "(\n " + "\n ".join(lines) + "\n);\nout geom qt;\n"
                elements += overpass_one(what, "[out:json][timeout:180];\n" + q, cache_dir, fresh)
        except RuntimeError:
            if what == "streets/water/places":                 # the only part a map can't do without
                raise
            say(f"osm: {what}: skipped - the servers are busy (update the area later to add them)")
    return {"elements": elements}


def overpass_one(what, q, cache_dir, fresh=False):
    os.makedirs(cache_dir, exist_ok=True)
    key = "osm_" + hashlib.sha1(q.encode()).hexdigest()[:16]
    path = os.path.join(cache_dir, key + ".json.gz")
    old = os.path.join(cache_dir, key + ".json")                # earlier versions cached plain JSON
    if not fresh:
        if os.path.exists(path):
            say(f"osm: {what}: cached")
            with gzip.open(path, "rt", encoding="utf-8") as f:
                return json.load(f).get("elements", [])
        if os.path.exists(old):
            say(f"osm: {what}: cached")
            with open(old, encoding="utf-8") as f:
                return json.load(f).get("elements", [])
    for attempt in range(8):                               # rotate through public mirrors when one is busy
        url = OVERPASS[attempt % len(OVERPASS)]
        host = url.split("/")[2]
        say(f"osm: {what}: asking {host}" + (f" (try {attempt + 1})" if attempt else "") + "...")
        try:
            r = _session().post(url, data={"data": q}, headers=UA, timeout=240)
            if r.status_code == 400:
                raise QueryError(f"{host}: {r.text[:200]}")
            if r.status_code == 200 and r.text.lstrip().startswith("{"):
                data = r.json()
                remark = data.get("remark", "")
                if "error" not in remark.lower():          # a timed-out query still says 200, with half the data
                    with gzip.open(path, "wt", encoding="utf-8", compresslevel=5) as f:
                        json.dump(data, f)
                    if os.path.exists(old):
                        os.remove(old)
                    say(f"osm: {what}: {len(data.get('elements', []))} elements")
                    return data.get("elements", [])
                say(f"osm: {what}: {host}: {remark[:100]}")
            else:
                say(f"osm: {what}: {host} busy (HTTP {r.status_code})")
        except (requests.RequestException, ValueError) as ex:
            say(f"osm: {what}: {host}: {type(ex).__name__}")
        time.sleep(5 if attempt < len(OVERPASS) else 20)
    raise RuntimeError("the OpenStreetMap map servers are busy right now - try again in a few minutes "
                       "(it continues where it stopped)")


def search_places(query, limit=6):
    """[(display name, lat, lon, (s, w, n, e) or None)] for a free-text place search (OpenStreetMap Nominatim)."""
    r = requests.get("https://nominatim.openstreetmap.org/search",
                     params={"q": query, "format": "json", "limit": limit}, headers=UA, timeout=30)
    r.raise_for_status()
    out = []
    for j in r.json():
        bb = j.get("boundingbox")
        rect = (float(bb[0]), float(bb[2]), float(bb[1]), float(bb[3])) if bb and len(bb) == 4 else None
        out.append((j["display_name"], float(j["lat"]), float(j["lon"]), rect))
    return out


def geocode(place):
    r = requests.get("https://nominatim.openstreetmap.org/search", params={"q": place, "format": "json", "limit": 1},
                     headers=UA, timeout=30)
    r.raise_for_status()
    j = r.json()
    if not j:
        raise RuntimeError(f"could not find '{place}'")
    say(f"place: {j[0]['display_name']}")
    return float(j[0]["lat"]), float(j[0]["lon"])


def ascii_name(s, n=24):
    s = (s or "").replace(",", " ").replace("\n", " ").replace("\r", " ")
    s = "".join(c if 32 <= ord(c) < 127 else "?" for c in s).strip()
    return s[:n]


class Feature:
    __slots__ = ("kind", "cls", "pts", "closed", "name", "bb")

    def __init__(self, kind, cls, pts, closed=False, name=""):
        self.kind, self.cls, self.pts, self.closed, self.name = kind, cls, pts, closed, name
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        self.bb = (min(xs), min(ys), max(xs), max(ys))


def rings(ways):
    """Join a multipolygon's member ways end to end: (closed rings, pieces that don't close). Points are
    (lat, lon) tuples, compared exactly (shared nodes have identical coordinates)."""
    done, todo = [], []
    for w in ways:
        (done if len(w) >= 4 and w[0] == w[-1] else todo).append(list(w))
    loose = []
    while todo:
        cur = todo.pop()
        grown = True
        while grown and cur[0] != cur[-1]:
            grown = False
            for i, w in enumerate(todo):
                if w[0] == cur[-1]:
                    cur = cur + w[1:]
                elif w[-1] == cur[-1]:
                    cur = cur + w[-2::-1]
                elif w[-1] == cur[0]:
                    cur = w[:-1] + cur
                elif w[0] == cur[0]:
                    cur = w[:0:-1] + cur
                else:
                    continue
                del todo[i]
                grown = True
                break
        (done if len(cur) >= 4 and cur[0] == cur[-1] else loose).append(cur)
    return done, loose


def parse_osm(data):
    feats, places, pois = [], [], []
    seen = set()                                              # the lake lookup can return a way twice
    for el in data.get("elements", []):
        t = el.get("tags", {})
        if (el["type"], el.get("id")) in seen:
            continue
        seen.add((el["type"], el.get("id")))
        if el["type"] == "node" and "place" in t and "lat" in el:
            if t["place"] in PLACE and t.get("name"):
                places.append((w0(el["lat"], el["lon"]), t["place"], ascii_name(t["name"], 28)))
            continue
        if "center" in el or (el["type"] == "node" and "lat" in el and "place" not in t):
            c = el.get("center") or el
            typ = poi_type(t)
            if typ:
                pois.append((c["lat"], c["lon"], typ, ascii_name(t.get("name") or t.get("operator") or "")))
            continue
        if el["type"] == "relation":                           # lakes made of many ways, with islands
            parts = {"outer": [], "inner": []}
            for m in el.get("members", []):
                if m.get("geometry"):
                    parts["inner" if m.get("role") == "inner" else "outer"].append(
                        [(p["lat"], p["lon"]) for p in m["geometry"] if p])
            for role, ways in parts.items():
                closed, loose = rings(ways)
                for ring in closed:
                    feats.append(Feature("water", role, [w0(a, b) for a, b in ring], True))
                for piece in loose:                            # not all of it came back: draw the shore
                    if len(piece) >= 2:
                        feats.append(Feature("water", "shore", [w0(a, b) for a, b in piece]))
            continue
        geoms = []
        if el["type"] == "way" and el.get("geometry"):
            geoms.append(el["geometry"])
        for g in geoms:
            pts = [w0(p["lat"], p["lon"]) for p in g if p]
            if len(pts) < 2:
                continue
            closed = pts[0] == pts[-1]
            name = ascii_name(t.get("name", ""), 32)
            if "highway" in t and t["highway"] in ROAD:
                feats.append(Feature("road", t["highway"], pts, False, name))
            elif "railway" in t:
                feats.append(Feature("rail", "rail", pts))
            elif t.get("waterway") in ("river", "canal"):
                feats.append(Feature("river", "river", pts, False, name))
            elif t.get("waterway") == "stream":
                feats.append(Feature("river", "stream", pts))
            elif t.get("natural") == "water" or t.get("landuse") == "reservoir":
                feats.append(Feature("water", "water", pts, closed))
            elif t.get("aeroway") == "runway":
                feats.append(Feature("runway", "runway", pts))
            elif "building" in t and closed:
                feats.append(Feature("building", "building", pts, True))
    return feats, places, pois


def poi_type(t):
    named = bool(t.get("name"))
    if t.get("amenity") == "hospital" and named: return "MEDICAL"
    if t.get("amenity") in ("townhall", "courthouse") or (t.get("office") == "government" and named): return "CAPITOL"
    if t.get("amenity") in ("police", "fire_station") or ("military" in t and named): return "MILITARY"
    if t.get("leisure") == "stadium": return "STADIUM"
    if (t.get("leisure") == "pitch" and t.get("sport") == "baseball"           # ballparks, not every sports field
            and re.search(r"[Pp]ark|[Ss]tadium", t.get("name", ""))): return "STADIUM"
    if t.get("amenity") in ("university", "college") and named: return "LANDMARK"
    if t.get("aeroway") == "aerodrome" and named: return "AIRPORT"
    if t.get("tourism") in ("camp_site", "wilderness_hut"): return "CAMP"
    if t.get("natural") == "cave_entrance": return "CAVE"
    if t.get("natural") == "peak" and named: return "NATURE"
    if t.get("historic") in ("monument", "castle", "fort", "ruins", "archaeological_site") and named: return "LANDMARK"
    return None


# ------------------------------------------------------------------ terrain contours
def fetch_dem(bbox, dz, cache_dir):
    """Elevation (metres) mosaic covering bbox from Terrarium tiles at zoom dz; returns (array, tx0, ty0)."""
    import numpy as np
    s_, w_, n_, e_ = bbox
    (x0, y0), (x1, y1) = w0(n_, w_), w0(s_, e_)
    k = 2 ** dz / TILE
    edge = 8 / TILE                                            # a few DEM pixels more, so the smoothing at the
    tx0, tx1 = int(x0 * k - edge), int(x1 * k + edge)          # mosaic's edge stays outside the map (no seams
    ty0, ty1 = int(y0 * k - edge), int(y1 * k + edge)          # between areas drawn separately)
    os.makedirs(cache_dir, exist_ok=True)
    dem = np.zeros(((ty1 - ty0 + 1) * TILE, (tx1 - tx0 + 1) * TILE), dtype=np.float32)
    tiles = [(tx, ty, os.path.join(cache_dir, f"t{dz}_{tx}_{ty}.png"))
             for tx in range(tx0, tx1 + 1) for ty in range(ty0, ty1 + 1)]
    need = [t for t in tiles if not os.path.exists(t[2])]
    if need:                                                   # 8 at a time over kept-open connections
        from concurrent.futures import ThreadPoolExecutor

        def get(t):
            tx, ty, path = t
            for attempt in range(4):
                try:
                    r = _session().get(DEM_URL.format(z=dz, x=tx, y=ty), headers=UA, timeout=60)
                    r.raise_for_status()
                    break
                except requests.RequestException:
                    if attempt == 3:
                        raise
                    time.sleep(2 + 3 * attempt)
            with open(path + ".tmp", "wb") as f:
                f.write(r.content)
            os.replace(path + ".tmp", path)
        with ThreadPoolExecutor(8) as ex:
            for i, _ in enumerate(ex.map(get, need)):
                if DEM_PROGRESS:
                    DEM_PROGRESS(i + 1, len(need))
    for tx, ty, path in tiles:
        rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)
        elev = rgb[:, :, 0] * 256 + rgb[:, :, 1] + rgb[:, :, 2] / 256 - 32768
        dem[(ty - ty0) * TILE:(ty - ty0 + 1) * TILE, (tx - tx0) * TILE:(tx - tx0 + 1) * TILE] = elev
    return dem, tx0, ty0


_sessions = threading.local()


def _session():
    """One requests.Session per thread: connections stay open between tiles (a new TLS handshake costs ~1 s)."""
    s = getattr(_sessions, "s", None)
    if s is None:
        s = _sessions.s = requests.Session()
        s.mount("https://", requests.adapters.HTTPAdapter(pool_maxsize=8))
    return s


DEM_PROGRESS = None    # callable(done, total) while elevation tiles download


def box_blur(a, r):
    """(2r+1)^2 box blur, edges clamped - takes the stair-steps out of the DEM before contouring."""
    import numpy as np
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r + 1, r)
        c = np.cumsum(np.pad(a, pad, mode="edge"), axis=axis, dtype=np.float64)
        n = a.shape[axis]
        hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
        lo = np.take(c, np.arange(0, n), axis=axis)
        a = ((hi - lo) / (2 * r + 1)).astype(np.float32)
    return a


def make_contours(bbox, dz, cache_dir, zooms):
    import contourpy
    import numpy as np
    step = 0                                                # every interval used must be a multiple of the step
    for z in zooms:
        if z in CONTOUR_FT:
            step = math.gcd(step, CONTOUR_FT[z])
    if not step:
        return []
    dem, tx0, ty0 = fetch_dem(bbox, dz, cache_dir)
    ft = box_blur(dem, 2) * 3.28084
    lo, hi = int(math.ceil(ft.min() / step) * step), int(math.floor(ft.max() / step) * step)
    gen = contourpy.contour_generator(z=ft, line_type="Separate")
    k = 2 ** dz
    feats = []
    for lev in range(lo, hi + 1, step):
        for line in gen.lines(lev):
            if len(line) < 6:                               # specks from DEM noise
                continue
            if len(line) > 12:
                line = np.vstack([line[:-1:2], line[-1:]])  # half the vertices: plenty at map scale
            gx = (tx0 * TILE + line[:, 0] + 0.5) / k
            gy = (ty0 * TILE + line[:, 1] + 0.5) / k
            feats.append(Feature("contour", lev, list(zip(gx.tolist(), gy.tolist()))))
    say(f"  contours: {len(feats)} lines, {lo}-{hi} ft every {step} ft")
    return feats


# ------------------------------------------------------------------ KML / KMZ
ICON_RULES = [  # (regex on the icon href, type) - first match wins
    (r"airport|heliport", "AIRPORT"), (r"camp|picnic|tent", "CAMP"),
    (r"caution|earthquake|volcano|hazard|danger|warning|skull|forbidden", "DANGER"),
    (r"hospital|firstaid|first_aid|medical|pharmacy", "MEDICAL"), (r"police|firedept|fire|military", "MILITARY"),
    (r"park|hiker|trail|mountain|tree|forest|ranger|hiking", "NATURE"),
    (r"homegardenbusiness|home|house|lodging", "HOME"),
    (r"shopping|grocery|convenience|dollar|euro|yen|pound|store", "STORE"),
    (r"dining|coffee|bars|snack|cafe|restaurant|food", "DINER"), (r"gas_station|gas|fuel", "GAS"),
    (r"cabs|bus|rail|subway|tram|ferry|truck|car|motorcycle|parking", "VEHICLE"),
    (r"info|question", "INFO"), (r"flag", "FLAG"), (r"star", "QUEST"), (r"target|crosshair", "TARGET"),
    (r"water|swimming|marina|sailing|fishing", "WATER"), (r"/man\.|woman|person", "PERSON"),
    (r"mechanic|factory|industr", "FACTORY"), (r"electronic|radio|tower|antenna|wifi", "RADIO"),
    (r"church|religious|monument|landmark|museum|historic|ski|golf|sports", "LANDMARK"),
]
NAME_RULES = [
    (r"\bvault\b", "VAULT"), (r"repeater|\brptr\b|\btower\b|antenna|\bradio\b|\baprs\b|\bdigi\b|meshcore|\bnode\b", "RADIO"),
    (r"camp", "CAMP"), (r"\bcave\b|\bmine\b|tunnel", "CAVE"),
    (r"\bbase\b|armory|\barmy\b|guard|police|fire station|\bfort\b", "MILITARY"),
    (r"hospital|clinic|medical|\ber\b", "MEDICAL"), (r"spring|\blake\b|reservoir|\bwater\b|\bwell\b", "WATER"),
    (r"\bhome\b|\bhouse\b|\bqth\b|cabin", "HOME"), (r"\btown\b|\bcity\b|village|settlement", "TOWN"),
    (r"\bgas\b|\bfuel\b|red rocket", "GAS"), (r"\bstore\b|\bshop\b|market", "STORE"),
    (r"diner|cafe|restaurant|\bfood\b", "DINER"), (r"airport|airfield|heliport|\bstrip\b", "AIRPORT"),
    (r"danger|hazard|warning|caution|\brad\b", "DANGER"), (r"peak|summit|trailhead|\bpark\b|forest", "NATURE"),
    (r"factory|\bplant\b|\bmill\b|works", "FACTORY"),
]


def tag(e):
    return e.tag.rsplit("}", 1)[-1]


def child(e, name):
    for c in e:
        if tag(c) == name:
            return c
    return None


def text_of(e, name):
    c = child(e, name)
    return (c.text or "").strip() if c is not None else ""


def coords(s):
    out = []
    for tok in (s or "").split():
        p = tok.split(",")
        if len(p) >= 2:
            try:
                out.append((float(p[1]), float(p[0])))
            except ValueError:
                pass
    return out


def load_kml(path):
    if path.lower().endswith(".kmz"):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.lower().endswith(".kml")]
            if not names:
                raise RuntimeError(f"{path}: no .kml inside")
            names.sort(key=lambda n: (n.lower() != "doc.kml", len(n)))
            return z.read(names[0])
    with open(path, "rb") as f:
        return f.read()


def parse_kml(path, styles_cfg):
    root = ET.fromstring(load_kml(path))
    style_href, stylemap = {}, {}
    for e in root.iter():
        t = tag(e)
        if t == "Style" and e.get("id"):
            for ic in e.iter():
                if tag(ic) == "href":
                    style_href[e.get("id")] = (ic.text or "").strip()
                    break
        elif t == "StyleMap" and e.get("id"):
            for pair in e:
                if tag(pair) == "Pair" and text_of(pair, "key") == "normal":
                    stylemap[e.get("id")] = text_of(pair, "styleUrl").lstrip("#")

    def href_for(pm):
        inline = child(pm, "Style")
        if inline is not None:
            for ic in inline.iter():
                if tag(ic) == "href":
                    return (ic.text or "").strip()
        sid = text_of(pm, "styleUrl").split("#")[-1]
        sid = stylemap.get(sid, sid)
        return style_href.get(sid, "")

    points, lines, report = [], [], {}

    def classify(name, href, folders):
        for k, v in styles_cfg.get("icons", {}).items():
            if k.lower() in href.lower():
                return v, "config"
        for k, v in styles_cfg.get("names", {}).items():
            if k.lower() in name.lower():
                return v, "config"
        hay = name + " " + " ".join(folders)
        key = "/".join(href.rsplit("/", 2)[-2:])           # e.g. "shapes/campground.png", "paddle/red-stars.png"
        for rx, typ in ICON_RULES:                          # a meaningful icon wins; plain pins fall through
            if re.search(rx, key, re.I):
                return typ, "icon"
        for rx, typ in NAME_RULES:
            if re.search(rx, hay, re.I):
                return typ, "name"
        return styles_cfg.get("default", "LANDMARK"), "default"

    def geoms(e):
        for c in e.iter():
            t = tag(c)
            if t == "Point":
                yield "pt", coords(text_of(c, "coordinates"))
            elif t == "LineString":
                yield "ln", coords(text_of(c, "coordinates"))
            elif t == "LinearRing":
                yield "ln", coords(text_of(c, "coordinates"))

    def walk(e, folders):
        for c in e:
            t = tag(c)
            if t in ("Folder", "Document"):
                walk(c, folders + [text_of(c, "name")])
            elif t == "Placemark":
                name = text_of(c, "name")
                href = href_for(c)
                for kind, pts in geoms(c):
                    if not pts:
                        continue
                    if kind == "pt":
                        desc = text_of(c, "description").strip().upper()
                        # a KML saved by PIP LINK Setup keeps the marker type in the description
                        typ, why = (desc, "kml") if desc in map_icons.TYPES else classify(name, href, folders)
                        points.append((pts[0][0], pts[0][1], typ, ascii_name(name)))
                        key = (href.rsplit("/", 2)[-2:] and "/".join(href.rsplit("/", 2)[-2:])) or "(no icon)"
                        r = report.setdefault((key, typ, why), 0)
                        report[(key, typ, why)] = r + 1
                    elif len(pts) >= 2:
                        lines.append([w0(a, b) for a, b in pts])

    walk(root, [])
    return points, lines, report


# ------------------------------------------------------------------ drawing
def load_font(px):
    for p in FONTS:
        try:
            return ImageFont.truetype(p, px)
        except OSError:
            continue
    return ImageFont.load_default()


_fonts = {}


def font(px):
    if px not in _fonts:
        _fonts[px] = load_font(px * SS)
    return _fonts[px]


def dashed(draw, pts, width, fill, on, off, clip=None):
    """Dashed polyline; the dash phase runs continuously along the whole line."""
    period = on + off
    dist = 0.0                                                # distance along the line at this segment's start
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        if seg <= 0:
            continue
        if clip and (max(x0, x1) < clip[0] or min(x0, x1) > clip[2] or max(y0, y1) < clip[1] or min(y0, y1) > clip[3]):
            dist += seg
            continue
        k = math.floor(dist / period)
        while True:
            a = k * period - dist                             # dash start, relative to this segment
            if a >= seg:
                break
            lo, hi = max(a, 0.0), min(a + on, seg)
            if hi > lo:
                draw.line([(x0 + (x1 - x0) * lo / seg, y0 + (y1 - y0) * lo / seg),
                           (x0 + (x1 - x0) * hi / seg, y0 + (y1 - y0) * hi / seg)], fill=fill, width=width)
            k += 1
        dist += seg


class Label:
    __slots__ = ("x", "y", "box", "halo", "txt", "w", "h", "lvl")


def label_box(text, px, angle, cx, cy):
    """Where make_label's label would go (world px), without drawing it - most candidates don't fit."""
    l, t, r, b = font(px).getbbox(text, stroke_width=SS)
    w, h = r - l + 4, b - t + 4
    a = math.radians(angle)
    W = (abs(w * math.cos(a)) + abs(h * math.sin(a)) + 1) / SS
    H = (abs(w * math.sin(a)) + abs(h * math.cos(a)) + 1) / SS
    return (cx - W / 2, cy - H / 2, cx + W / 2, cy + H / 2)


def make_label(text, px, angle, cx, cy, lvl=255):
    f = font(px)
    l, t, r, b = f.getbbox(text, stroke_width=SS)
    w, h = r - l + 4, b - t + 4
    halo = Image.new("L", (w, h), 0)
    ImageDraw.Draw(halo).text((2 - l, 2 - t), text, font=f, fill=255, stroke_width=SS, stroke_fill=255)
    txt = Image.new("L", (w, h), 0)
    ImageDraw.Draw(txt).text((2 - l, 2 - t), text, font=f, fill=255)
    if angle:
        halo = halo.rotate(angle, resample=Image.BICUBIC, expand=True)
        txt = txt.rotate(angle, resample=Image.BICUBIC, expand=True)
    L = Label()
    L.halo, L.txt = halo, txt
    L.w, L.h = halo.size[0] / SS, halo.size[1] / SS          # in final px
    L.x, L.y = cx - L.w / 2, cy - L.h / 2                     # world px at this zoom
    L.box = (L.x, L.y, L.x + L.w, L.y + L.h)
    L.lvl = lvl
    return L


ABBREV = [(r"\bStreet\b", "St"), (r"\bAvenue\b", "Ave"), (r"\bBoulevard\b", "Blvd"), (r"\bRoad\b", "Rd"),
          (r"\bDrive\b", "Dr"), (r"\bLane\b", "Ln"), (r"\bHighway\b", "Hwy"), (r"\bParkway\b", "Pkwy"),
          (r"\bCircle\b", "Cir"), (r"\bCourt\b", "Ct"), (r"\bPlace\b", "Pl"), (r"\bTerrace\b", "Ter"),
          (r"\bNorth\b", "N"), (r"\bSouth\b", "S"), (r"\bEast\b", "E"), (r"\bWest\b", "W"),
          (r"\bMountain\b", "Mtn"), (r"\bCanyon\b", "Cyn")]


def abbreviate(name):
    for rx, short in ABBREV:
        name = re.sub(rx, short, name)
    return name


def straight_run(pts, scale, max_turn=12.0):
    """Longest run of nearly collinear segments: (length, x0, y0, x1, y1) in world px."""
    best, run = None, None
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        ax, ay, bx, by = ax * scale, ay * scale, bx * scale, by * scale
        if ax == bx and ay == by:
            continue
        ang = math.degrees(math.atan2(by - ay, bx - ax))
        if run and abs((ang - run[5] + 180) % 360 - 180) <= max_turn:
            run = (run[0] + math.hypot(bx - ax, by - ay), run[1], run[2], bx, by, run[5])
        else:
            run = (math.hypot(bx - ax, by - ay), ax, ay, bx, by, ang)
        if not best or run[0] > best[0]:
            best = run
    return best[:5] if best else None


def place_labels(z, feats, places, allow=None, area=None):
    """Labels for zoom z, not overlapping each other. allow(box) -> False drops a label (e.g. one that would
    run into a tile drawn some other time - half a label would be left there); area = (x0, y0, x1, y1) in
    z0 world px skips features nowhere near the tiles being drawn."""
    scale = 2 ** z
    placed, grid = [], {}

    def free(box):
        if allow and not allow(box):
            return False
        x0, y0, x1, y1 = box[0] - 6, box[1] - 6, box[2] + 6, box[3] + 6
        cells = [(i, j) for i in range(int(x0 // 64), int(x1 // 64) + 1) for j in range(int(y0 // 64), int(y1 // 64) + 1)]
        for c in cells:
            for b in grid.get(c, ()):
                if x0 < b[2] and b[0] < x1 and y0 < b[3] and b[1] < y1:
                    return False
        for c in cells:
            grid.setdefault(c, []).append(box)
        return True

    def near(bb):
        return not area or (bb[0] <= area[2] and bb[2] >= area[0] and bb[1] <= area[3] and bb[3] >= area[1])

    order = sorted(places, key=lambda p: list(PLACE).index(p[1]))
    for (wx, wy), kind, name in order:
        zmin, zmax, px = PLACE[kind]
        if not (zmin <= z <= zmax) or not near((wx, wy, wx, wy)):
            continue
        if free(label_box(name.upper(), px, 0, wx * scale, wy * scale)):
            placed.append(make_label(name.upper(), px, 0, wx * scale, wy * scale))

    min_rank = {14: 3, 15: 5}.get(z, 99 if z >= 16 else 2)
    named = [f for f in feats if f.kind == "road" and f.name and f.cls in ROAD_RANK
             and ROAD_RANK.index(f.cls) < min_rank and z >= ROAD[f.cls][0] and near(f.bb)]
    named.sort(key=lambda f: ROAD_RANK.index(f.cls))
    seen = {}
    for f in named:
        best = straight_run(f.pts, scale)
        if not best:
            continue
        d, x0, y0, x1, y1 = best
        name = abbreviate(f.name)
        px = 11 if ROAD_RANK.index(f.cls) < 4 else 10
        if d < len(name) * px * 0.62 + 10:
            continue
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        if any(math.hypot(cx - a, cy - b) < 260 for a, b in seen.get(name, ())):
            continue
        ang = -math.degrees(math.atan2(y1 - y0, x1 - x0))
        if ang > 90: ang -= 180
        if ang < -90: ang += 180
        if free(label_box(name, px, ang, cx, cy)):
            placed.append(make_label(name, px, ang, cx, cy, 170))   # street names: mid shade
            seen.setdefault(name, []).append((cx, cy))

    iv = CONTOUR_FT.get(z)
    if iv and z >= 14:                                        # elevations on index contours
        cseen = {}
        for f in feats:
            if f.kind != "contour" or f.cls % (iv * 5) or not near(f.bb):
                continue
            best = straight_run(f.pts, scale, max_turn=25)
            if not best or best[0] < 46:
                continue
            d, x0, y0, x1, y1 = best
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            if any(math.hypot(cx - a, cy - b) < 360 for a, b in cseen.get(f.cls, ())):
                continue
            ang = -math.degrees(math.atan2(y1 - y0, x1 - x0))
            if ang > 90: ang -= 180
            if ang < -90: ang += 180
            if free(label_box(str(f.cls), 9, ang, cx, cy)):
                placed.append(make_label(str(f.cls), 9, ang, cx, cy, 170))
                cseen.setdefault(f.cls, []).append((cx, cy))
    return placed


def render_meta(z, mx, my, feats, overlays, labels):
    scale = 2 ** z
    x0, y0 = mx * META * TILE - PAD, my * META * TILE - PAD
    size = META * TILE + 2 * PAD
    img = Image.new("L", (size * SS, size * SS), 0)
    d = ImageDraw.Draw(img)
    clip = (-40, -40, size * SS + 40, size * SS + 40)
    bx0, by0, bx1, by1 = x0 / scale, y0 / scale, (x0 + size) / scale, (y0 + size) / scale

    def vis(f):
        return f.bb[0] <= bx1 and f.bb[2] >= bx0 and f.bb[1] <= by1 and f.bb[3] >= by0

    def P(pts):
        return [((x * scale - x0) * SS, (y * scale - y0) * SS) for x, y in pts]

    fs = [f for f in feats if vis(f)]
    iv = CONTOUR_FT.get(z)
    if iv:                                                    # terrain contours, underneath everything
        for f in fs:
            if f.kind == "contour" and f.cls % iv == 0:
                index = f.cls % (iv * 5) == 0
                d.line(P(f.pts), fill=85, width=int((2.0 if index else 1.0) * SS))
    lakes = [f for f in fs if f.kind == "water" and f.closed and len(f.pts) >= 3]
    if lakes:                                                 # water, with islands cut back out (contours stay)
        mask = Image.new("L", img.size, 0)
        md = ImageDraw.Draw(mask)
        for f in lakes:
            if f.cls != "inner":
                md.polygon(P(f.pts), fill=255)
        for f in lakes:
            if f.cls == "inner":
                md.polygon(P(f.pts), fill=0)
        img.paste(85, (0, 0) + img.size, mask)
    if z >= 16:
        for f in fs:
            if f.kind == "building":
                d.polygon(P(f.pts), fill=85)
    for f in fs:                                              # waterways
        if f.kind == "river":
            w = width_at(4.0 if f.cls == "river" else 1.5, z) if (f.cls == "river" or z >= 14) else 0
            if w:
                d.line(P(f.pts), fill=85, width=max(SS, int(w * SS)))
        elif f.kind == "water" and not f.closed:
            d.line(P(f.pts), fill=85, width=SS)
        elif f.kind == "runway" and z >= 12:
            d.line(P(f.pts), fill=170, width=max(SS, int(width_at(10, z) * SS)))
    for f in fs:
        if f.kind == "rail" and z >= 12:
            dashed(d, P(f.pts), max(SS, int(1.5 * SS)), 170, 10 * SS, 6 * SS, clip)
    roads = sorted((f for f in fs if f.kind == "road"), key=lambda f: -ROAD[f.cls][1])
    roads.reverse()                                           # minor first, major on top
    for f in roads:
        zmin, w16, lvl, dash = ROAD[f.cls]
        if z < zmin:
            continue
        w = max(SS, int(width_at(w16, z) * SS))
        pts = P(f.pts)
        if dash:
            dashed(d, pts, w, lvl * 85, 6 * SS, 4 * SS, clip)
        else:
            d.line(pts, fill=lvl * 85, width=w, joint="curve")
    for line in overlays:                                     # KML lines / polygon outlines
        xs = [p[0] for p in line]
        ys = [p[1] for p in line]
        if min(xs) <= bx1 and max(xs) >= bx0 and min(ys) <= by1 and max(ys) >= by0:
            dashed(d, P(line), max(SS, int(2.2 * SS)), 255, 8 * SS, 5 * SS, clip)
    for L in labels:                                          # labels: dark halo, bright text
        if L.box[2] < x0 or L.box[0] > x0 + size or L.box[3] < y0 or L.box[1] > y0 + size:
            continue
        pos = (int(round((L.x - x0) * SS)), int(round((L.y - y0) * SS)))
        img.paste(0, pos, L.halo)
        img.paste(L.lvl, pos, L.txt)
    small = img.reduce(SS)
    return small.point([min(3, int(v / 85 + 0.5)) for v in range(256)]), x0, y0


def encode_tile(im):
    data = im.tobytes()
    if not any(data):
        return None
    out = bytearray()
    for s in range(TILE // STRIP):
        rows = data[s * STRIP * TILE:(s + 1) * STRIP * TILE]
        if not any(rows):
            out += b"\0\0"
            continue
        packed = bytes((a << 6) | (b << 4) | (c << 2) | e
                       for a, b, c, e in zip(rows[0::4], rows[1::4], rows[2::4], rows[3::4]))
        comp = heatshrink2.compress(packed, window_sz2=8, lookahead_sz2=6)
        out += len(comp).to_bytes(2, "little") + comp
    return bytes(out)


if __name__ == "__main__":
    import map_lib                                          # the command line lives there now
    map_lib.main()
