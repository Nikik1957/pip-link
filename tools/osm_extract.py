"""Offline OpenStreetMap data for big maps: a Geofabrik extract (e.g. all of Nevada, ~120 MB) downloaded once and
imported into a local database, so map_lib.py can draw a whole state without asking the busy public Overpass
servers for every piece. Areas inside an installed extract use it automatically; anything else still uses
Overpass.

    python osm_extract.py list [nevada]          # regions Geofabrik offers
    python osm_extract.py get us/nevada          # download + import (a few minutes)
    python osm_extract.py installed

Each extract is cache/extracts/<id>.db: features stored as Overpass-style JSON (zlib) with an R-tree on their
bounding boxes and the smallest zoom that draws them. Data (c) OpenStreetMap contributors, ODbL.
"""
import argparse
import json
import os
import sqlite3
import sys
import time
import zlib

import requests

import map_build as mb

DATA = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "PIP LINK")
DIR = os.path.join(DATA, "cache", "extracts")
INDEX_URL = "https://download.geofabrik.de/index-v1-nogeom.json"
KEYS = ("highway", "railway", "waterway", "aeroway", "building", "natural", "landuse", "place", "amenity",
        "office", "military", "leisure", "tourism", "historic")
RAIL = {"rail", "light_rail", "subway", "tram"}


class Cancelled(Exception):
    pass


# ------------------------------------------------------------------ what Geofabrik offers
def regions(log=print):
    """[(id, name, parent, pbf url)] - cached for a week."""
    os.makedirs(DIR, exist_ok=True)
    path = os.path.join(DIR, "index.json")
    if not os.path.exists(path) or time.time() - os.path.getmtime(path) > 7 * 86400:
        r = requests.get(INDEX_URL, headers=mb.UA, timeout=60)
        r.raise_for_status()
        with open(path, "wb") as f:
            f.write(r.content)
    with open(path, encoding="utf-8") as f:
        idx = json.load(f)
    out = []
    for feat in idx.get("features", []):
        p = feat.get("properties", {})
        if p.get("urls", {}).get("pbf"):
            name = p.get("name", p["id"])
            if "/" in name:                                   # US states are named "us/nevada"
                name = name.rsplit("/", 1)[-1].replace("-", " ").title()
            out.append((p["id"], name, p.get("parent", ""), p["urls"]["pbf"]))
    return out


def find_region(text, log=print):
    """The region whose id or name matches text best (e.g. 'nevada', 'us/nevada')."""
    t = text.lower().strip()
    rs = regions(log)
    for r in rs:
        if r[0].lower() == t or r[3].lower().endswith("/" + t + "-latest.osm.pbf"):
            return r
    for r in rs:
        if r[1].lower() == t:
            return r
    hits = [r for r in rs if t in r[1].lower() or t in r[0].lower()]
    return min(hits, key=lambda r: len(r[1])) if hits else None


# ------------------------------------------------------------------ download + import
def download(url, dest, log=print, progress=None, cancel=None):
    tmp = dest + ".part"
    have = os.path.getsize(tmp) if os.path.exists(tmp) else 0
    hdr = dict(mb.UA)
    if have:
        hdr["Range"] = f"bytes={have}-"                    # continue a stopped download
    with requests.get(url, headers=hdr, stream=True, timeout=60) as r:
        if r.status_code == 200:
            have = 0
        elif r.status_code != 206:
            r.raise_for_status()
        total = have + int(r.headers.get("content-length", 0))
        log(f"downloading {url.rsplit('/', 1)[-1]} ({total / 1048576:.0f} MB)")
        with open(tmp, "ab" if have else "wb") as f:
            for chunk in r.iter_content(1 << 20):
                if cancel and cancel.is_set():
                    raise Cancelled()
                f.write(chunk)
                have += len(chunk)
                if progress and total:
                    progress(have / total, f"downloading the map data: {have / 1048576:.0f} of {total / 1048576:.0f} MB")
    os.replace(tmp, dest)


def _minz(kind, tags):
    if kind == "road":
        return mb.ROAD[tags["highway"]][0]
    if kind == "place":
        return mb.PLACE[tags["place"]][0]
    if kind == "waterway":
        return 14 if tags.get("waterway") == "stream" else 0
    if kind in ("rail", "runway"):
        return 12
    if kind == "building":
        return 16
    return 0


def _keep(tags, names):
    return {k: tags[k] for k in names if k in tags}


def import_pbf(pbf, dbpath, log=print, progress=None, cancel=None):
    """Read an .osm.pbf into dbpath (replaces it). Returns the number of features."""
    import osmium
    tmp = dbpath + ".tmp"
    if os.path.exists(tmp):
        os.remove(tmp)
    db = sqlite3.connect(tmp)
    db.executescript("""
        CREATE TABLE meta(k TEXT PRIMARY KEY, v TEXT);
        CREATE TABLE feats(id INTEGER PRIMARY KEY, kind TEXT, minz INTEGER, el BLOB);
        CREATE VIRTUAL TABLE idx USING rtree(id, s, n, w, e);
    """)
    box = osmium.io.Reader(pbf, osmium.osm.osm_entity_bits.NOTHING).header().box()
    bounds = (box.bottom_left.lat, box.bottom_left.lon, box.top_right.lat, box.top_right.lon) if box.valid() else None
    size = os.path.getsize(pbf)
    rows, n, t0, last = [], 0, time.time(), 0.0
    POI_KEYS = ("amenity", "office", "military", "leisure", "aeroway", "tourism", "natural", "historic", "name",
                "operator", "sport")

    def add(kind, el, s, w, nn, e, minz):
        nonlocal n
        n += 1
        rows.append((n, kind, minz, zlib.compress(json.dumps(el, separators=(",", ":")).encode(), 6), s, nn, w, e))
        if len(rows) >= 5000:
            flush()

    def flush():
        db.executemany("INSERT INTO feats VALUES(?,?,?,?)", [r[:4] for r in rows])
        db.executemany("INSERT INTO idx VALUES(?,?,?,?,?)", [(r[0],) + r[4:] for r in rows])
        rows.clear()

    def geom(nodes):
        g = []
        for nd in nodes:
            if nd.location.valid():
                g.append({"lat": round(nd.location.lat, 6), "lon": round(nd.location.lon, 6)})
        return g

    def bb(g):
        la = [p["lat"] for p in g]
        lo = [p["lon"] for p in g]
        return min(la), min(lo), max(la), max(lo)

    fp = (osmium.FileProcessor(pbf).with_locations().with_areas()
          .with_filter(osmium.filter.KeyFilter(*KEYS)))
    for o in fp:
        if cancel and cancel.is_set():
            db.close()
            os.remove(tmp)
            raise Cancelled()
        tags = {t.k: t.v for t in o.tags}
        if o.is_node():
            loc = o.location
            if not loc.valid():
                continue
            la, lo = round(loc.lat, 6), round(loc.lon, 6)
            if tags.get("place") in mb.PLACE and tags.get("name"):
                add("place", {"type": "node", "id": o.id, "lat": la, "lon": lo, "tags": _keep(tags, ("place", "name"))},
                    la, lo, la, lo, _minz("place", tags))
            elif mb.poi_type(tags):
                add("poi", {"type": "node", "id": o.id, "lat": la, "lon": lo, "tags": _keep(tags, POI_KEYS)},
                    la, lo, la, lo, 0)
        elif o.is_way():
            hw, rw, ww = tags.get("highway"), tags.get("railway"), tags.get("waterway")
            if hw in mb.ROAD:
                kind, keep = "road", ("highway", "name")
            elif rw in RAIL:
                kind, keep = "rail", ("railway",)
            elif ww in ("river", "stream", "canal"):
                kind, keep = "waterway", ("waterway", "name")
            elif tags.get("aeroway") == "runway":
                kind, keep = "runway", ("aeroway",)
            elif "building" in tags and o.is_closed():
                kind, keep = "building", ("building",)
            else:
                continue
            g = geom(o.nodes)
            if len(g) < 2:
                continue
            add(kind, {"type": "way", "id": o.id, "tags": _keep(tags, keep), "geometry": g}, *bb(g), _minz(kind, tags))
        elif o.is_area():
            water = tags.get("natural") == "water" or tags.get("landuse") == "reservoir"
            poi = mb.poi_type(tags)
            if not (water or poi):
                continue
            members = []
            for outer in o.outer_rings():
                members.append({"role": "outer", "geometry": geom(outer)})
                for inner in o.inner_rings(outer):
                    members.append({"role": "inner", "geometry": geom(inner)})
            members = [m for m in members if len(m["geometry"]) >= 4]
            if not members:
                continue
            g = [p for m in members if m["role"] == "outer" for p in m["geometry"]]
            s, w, nn, e = bb(g)
            if water:
                add("water", {"type": "relation", "id": o.id, "tags": _keep(tags, ("natural", "landuse", "name")),
                              "members": members}, s, w, nn, e, 0)
            if poi:
                la, lo = round(sum(p["lat"] for p in g) / len(g), 6), round(sum(p["lon"] for p in g) / len(g), 6)
                add("poi", {"type": "way", "id": o.id, "center": {"lat": la, "lon": lo}, "tags": _keep(tags, POI_KEYS)},
                    la, lo, la, lo, 0)
        if progress and time.time() - last > 1:
            last = time.time()
            progress(None, f"reading the map data: {n:,} features  ({fmt(time.time() - t0)})")
    flush()
    db.execute("INSERT INTO meta VALUES('bounds', ?)", (json.dumps(bounds),))
    db.execute("INSERT INTO meta VALUES('source', ?)", (os.path.basename(pbf),))
    db.execute("INSERT INTO meta VALUES('date', ?)", (time.strftime("%Y-%m-%d", time.localtime(os.path.getmtime(pbf))),))
    db.execute("INSERT INTO meta VALUES('features', ?)", (str(n),))
    db.commit()
    db.close()
    os.replace(tmp, dbpath)
    log(f"imported {n:,} features from {os.path.basename(pbf)} ({size / 1048576:.0f} MB) in {fmt(time.time() - t0)}")
    return n


def get(region_id, log=print, progress=None, cancel=None, keep_pbf=False):
    """Download and import a Geofabrik region (id like 'us/nevada'). Returns the Extract."""
    r = find_region(region_id, log)
    if not r:
        raise RuntimeError(f"Geofabrik has no region called {region_id}")
    rid, name, _, url = r
    os.makedirs(DIR, exist_ok=True)
    base = os.path.join(DIR, rid.replace("/", "_"))
    pbf = base + ".osm.pbf"
    if not os.path.exists(pbf) or time.time() - os.path.getmtime(pbf) > 86400:
        download(url, pbf, log, progress, cancel)
    import_pbf(pbf, base + ".db", log, progress, cancel)
    with sqlite3.connect(base + ".db") as db:
        db.execute("INSERT OR REPLACE INTO meta VALUES('name', ?)", (name,))
        db.execute("INSERT OR REPLACE INTO meta VALUES('region', ?)", (rid,))
    if not keep_pbf:
        os.remove(pbf)
    return Extract(base + ".db")


def fmt(s):
    return f"{s:.0f} s" if s < 90 else f"{s / 60:.0f} min"


# ------------------------------------------------------------------ using an extract
class Extract:
    def __init__(self, path):
        self.path = path
        self.db = sqlite3.connect(path, check_same_thread=False)
        meta = dict(self.db.execute("SELECT k, v FROM meta"))
        self.bounds = tuple(json.loads(meta.get("bounds") or "null") or ())
        self.name = meta.get("name", os.path.basename(path))
        self.date = meta.get("date", "?")
        self.features = int(meta.get("features", 0))

    def covers(self, bbox, slack=0.0):
        """bbox lies inside the extract (give or take slack degrees - extracts end at the state line, and the
        tiles along it reach a little past)."""
        if not self.bounds:
            return False
        s, w, n, e = bbox
        bs, bw, bn, be = self.bounds
        return bs - slack <= s and bw - slack <= w and n <= bn + slack and e <= be + slack

    def overlaps(self, bbox):
        if not self.bounds:
            return False
        s, w, n, e = bbox
        bs, bw, bn, be = self.bounds
        return s < bn and n > bs and w < be and e > bw

    def query(self, bbox, buildings, pois, zmax=17):
        """The same {"elements": [...]} map_build.overpass() returns, from the local data."""
        s, w, n, e = bbox
        kinds = ["road", "rail", "waterway", "runway", "water", "place"]
        if buildings:
            kinds.append("building")
        if pois:
            kinds.append("poi")
        sql = ("SELECT f.el FROM idx JOIN feats f ON f.id = idx.id WHERE idx.s <= ? AND idx.n >= ? AND idx.w <= ? "
               "AND idx.e >= ? AND f.minz <= ? AND f.kind IN (" + ",".join("?" * len(kinds)) + ")")
        out = [json.loads(zlib.decompress(b)) for (b,) in self.db.execute(sql, [n, s, e, w, zmax] + kinds)]
        mb.say(f"osm: {len(out)} elements from the offline {self.name} data")
        return {"elements": out}

    def close(self):
        self.db.close()


def installed():
    if not os.path.isdir(DIR):
        return []
    out = []
    for fn in sorted(os.listdir(DIR)):
        if fn.endswith(".db"):
            try:
                out.append(Extract(os.path.join(DIR, fn)))
            except sqlite3.Error:
                pass
    return out


def remove(ex):
    forget()
    ex.close()
    os.remove(ex.path)


_open = None


def for_bbox(bbox, slack=0.0):
    """The installed extract that has all of bbox, or None (then Overpass it is)."""
    global _open
    if _open is None:
        _open = installed()
    return next((ex for ex in _open if ex.covers(bbox, slack)), None)


def forget():
    """Extracts were added or removed: look again next time."""
    global _open
    for ex in _open or ():
        ex.close()
    _open = None


# ------------------------------------------------------------------ command line
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["list", "get", "installed"])
    ap.add_argument("region", nargs="?", default="")
    a = ap.parse_args()
    if a.cmd == "list":
        for rid, name, parent, _ in regions():
            if not a.region or a.region.lower() in (rid + name).lower():
                print(f"{rid:40} {name}")
    elif a.cmd == "get":
        if not a.region:
            sys.exit("which region? e.g. python osm_extract.py get us/nevada")
        get(a.region, progress=lambda f, t: print(f"\r  {t[:90]:90}", end="", flush=True))
        print()
    for ex in installed():
        print(f"{ex.name}: {ex.features:,} features, data from {ex.date}, bounds {ex.bounds}")


if __name__ == "__main__":
    main()
