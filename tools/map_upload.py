"""Upload a map pack (build/MAP from map_build.py) to the Pip-Boy SD card as PIPLINK/MAP/ over USB serial.

    python map_upload.py ../build/MAP [--port COMx]

The card's PIPLINK/MAP/ folder ends up mirroring the pack (nothing outside it is ever touched - the Pip-Boy's
own world map lives in MAP/NV and MAP/F3): changed files are sent (sha1 bookkeeping, so a rebuilt
tile of the same size still goes), files and folders the pack doesn't have are deleted (e.g. an old map
area). Resumable - just run it again after an interruption. Plug the Pip-Boy into the PC (not the hub).
The pack's PINS.TXT (its pins) isn't copied: its pins are merged into the saved-locations list PIPLINK/WPT.TXT
(the GO TO list) - pins already there, or deleted on the Pip-Boy, are left alone.
Also the shared serial layer (Repl) for the other Pip-Boy tools.
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import time

import serial

REMOTE_ROOT = "PIPLINK/MAP"
WPT_FILE = "PIPLINK/WPT.TXT"   # saved locations: "lat,lon,ICON,name" lines, "-..." = deleted (see hub_main.cpp)
PINS = "PINS.TXT"
PINS_DEL = "PINS.DEL"    # pins edited/removed in PIP LINK Setup: their old versions
CHUNK = 768           # base64 chars per REPL line (the Pip-Boy's RX buffer is 2048 bytes)


class PipError(Exception):
    pass


def find_pipboy(port="auto"):
    """The Pip-Boy's serial port: given explicitly, or found by its USB ID (VID 0483, PID A4F1)."""
    if port and port.lower() != "auto":
        return port
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0xA4F1:
            return p.device
    raise PipError("Pip-Boy not found - plug it into this computer with a USB data cable and switch it on")


class Repl:
    def __init__(self, port="auto", log=print):
        port = find_pipboy(port)
        log(f"Pip-Boy on {port}")
        try:
            self.sp = serial.Serial(port, 115200, timeout=0.05)
        except serial.SerialException as ex:
            raise PipError(f"can't open {port} - another program is using the Pip-Boy "
                           "(close it, or unplug and replug the Pip-Boy)") from ex
        self.n = 0
        time.sleep(0.3)
        self.sp.reset_input_buffer()
        self.sp.write(b"\n")                 # the first line after opening can be swallowed
        time.sleep(0.4)
        self.sp.write(b"echo(0)\n")
        time.sleep(0.4)
        self.sp.reset_input_buffer()
        for _ in range(5):                   # the first command after opening sometimes vanishes: sync up
            try:
                self.call('print("@@sync>>")', timeout=1.5)
                break
            except TimeoutError:
                self.sp.write(b"\n")
        else:
            self.sp.close()
            raise PipError("the Pip-Boy isn't answering - switch it on (or restart it) and try again")

    def call(self, js, timeout=10.0):
        """Run js, which must print exactly one <<R ...>> line; returns its text."""
        self.n += 1
        tag = f"<<R{self.n} "
        # the Pip-Boy's USB holds back the output of a REPL line until something prints later on,
        # so follow every reply with a short delayed print that pushes it out.
        # The tag is split in the source ("<<R"+"7 ") so an echoed command can never look like a reply
        # (echo comes back on if the Pip-Boy sleeps and closes the terminal); kick the idle timer so it doesn't
        js = js.replace('"@@', '"<<R"+"' + tag[3:]).replace("@@", tag)
        line = ('Pip.kickIdleTimer&&Pip.kickIdleTimer();' + js +
                ';setTimeout(function(){print(" ")},20)\n').encode()
        for i in range(0, len(line), 128):                  # pace it: a burst can overrun the REPL
            self.sp.write(line[i:i + 128])
            self.sp.flush()
            time.sleep(0.002)
        buf, t0, nudge = b"", time.time(), time.time() + 0.3
        while time.time() - t0 < timeout:
            buf += self.sp.read(4096)
            i = buf.find(tag.encode())
            j = buf.find(b">>", i) if i >= 0 else -1
            if j >= 0:
                return buf[i + len(tag):j].decode(errors="replace")
            if time.time() > nudge:
                # still held back: another printing command pushes the previous output through
                self.sp.write(b'print(" ")\n')
                nudge = time.time() + 0.3
        raise TimeoutError(f"no reply to: {js[:80]}  (got {buf[-200:]!r})")

    def close(self):
        try:
            self.sp.write(b"echo(1)\n")
        finally:
            self.sp.close()


def js_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def list_entries(r, path, page=16):
    """[(name, is_dir, size)] in a folder on the card, a page at a time (long replies stall the USB)."""
    out, start = [], 0
    while True:
        part = r.call('(function(d,s,n){var fs=require("fs"),o=[],l=[];try{l=fs.readdirSync(d)}catch(e){}'
                      'l=l.filter(function(f){return f[0]!="."}).slice(s,s+n);'
                      'l.forEach(function(f){var st=fs.statSync(d+"/"+f);o.push(f+":"+(st.dir?"D":st.size))});'
                      'print("@@"+l.length+"#"+o.join("|")+">>")})(' + js_str(path) + f',{start},{page})', timeout=20)
        count, _, items = part.partition("#")
        for item in filter(None, items.split("|")):
            name, _, size = item.rpartition(":")
            out.append((name, size == "D", int(size) if size.isdigit() else -1))
        if int(count) < page:
            return out
        start += page


def mkdirs(r, path, made):
    parts = path.split("/")
    for i in range(1, len(parts) + 1):
        p = "/".join(parts[:i])
        if p not in made:
            r.call(f'try{{require("fs").mkdirSync({js_str(p)})}}catch(e){{}}print("@@ok>>")')
            made.add(p)


def remove_tree(r, path, log, cancel=None):
    """Delete a folder on the card with everything in it. Returns the number of files removed."""
    n = 0
    for name, is_dir, _ in list_entries(r, path):
        if cancel and cancel.is_set():
            return n
        sub = path + "/" + name
        if is_dir:
            n += remove_tree(r, sub, log, cancel)
        else:
            r.call(f'try{{require("fs").unlinkSync({js_str(sub)})}}catch(e){{}}print("@@>>")')
            n += 1
    r.call(f'try{{require("fs").unlinkSync({js_str(path)})}}catch(e){{}}print("@@>>")')
    return n


def upload(pack, port="auto", remote_root=REMOTE_ROOT, log=print, progress=None, cancel=None):
    """Mirror the pack folder to remote_root on the card. progress(done_bytes, total_bytes, eta_s)."""
    try:
        _upload(pack, port, remote_root, log, progress, cancel)
    except (serial.SerialException, OSError) as ex:
        raise PipError("lost the connection to the Pip-Boy (unplugged or asleep?) - reconnect it and "
                       "send again; it continues where it stopped") from ex


def cleanup_legacy(r, log):
    """Early PIP LINK builds put the map straight into MAP/ (shared with the Pip-Boy's own world map).
    Remove only those PIP LINK files - never MAP/NV or MAP/F3."""
    head = r.call('try{var f=E.openFile("MAP/INFO.TXT","r"),d=f.read(5);f.close();print("@@"+d+">>")}'
                  'catch(e){print("@@->>")}')
    if not head.startswith("PBMAP"):
        return
    log("removing the old PIP LINK map from MAP/ (the Pip-Boy's own NV/F3 maps are left alone)")
    n = 0
    for name in ("T", "M"):
        n += remove_tree(r, "MAP/" + name, log)
    for name in ("INFO.TXT", "ICONS.BIN"):
        r.call(f'try{{require("fs").unlinkSync("MAP/{name}")}}catch(e){{}}print("@@>>")')
        n += 1
    log(f"removed {n} old files")


def read_file(r, path):
    """A file from the card as bytes (b'' if it isn't there), read in small pieces."""
    size = int(r.call(f'var s=-1;try{{s=require("fs").statSync({js_str(path)}).size}}catch(e){{}}print("@@"+s+">>")'))
    if size <= 0:
        return b""
    out = b""
    while len(out) < size:
        part = r.call(f'var f=E.openFile({js_str(path)},"r");f.seek({len(out)});var d=f.read(384);f.close();'
                      'print("@@"+btoa(d||"")+">>")')
        chunk = base64.b64decode(part)
        if not chunk:
            break
        out += chunk
    return out


def parse_places(text):
    """The saved locations in a WPT.TXT: ([(lat, lon, type, name)] still there, [... deleted])."""
    live, gone = [], []
    for line in text.splitlines():
        line = line.strip()
        dead = line.startswith("-")
        p = (line[1:] if dead else line).split(",", 3)
        if len(p) < 4:
            continue
        try:
            item = (float(p[0]), float(p[1]), p[2].strip(), p[3].strip())
        except ValueError:
            continue
        same = [i for i, x in enumerate(live) if same_place(x, item)]
        if dead:
            gone.append(item)
            if same:
                del live[same[0]]
        elif not same:
            live.append(item)
    return live, gone


def same_place(a, b):
    """Same name (any case) within ~30 m - the rule the PIP LINK unit uses."""
    import math
    dy = (a[0] - b[0]) * 111.2
    dx = (a[1] - b[1]) * 111.2 * math.cos(math.radians(a[0]))
    return a[3].lower() == b[3].lower() and dx * dx + dy * dy < 0.03 ** 2


def _exactly(a, b):
    """The same saved location: name, icon and position (within ~2 m)."""
    return a[2] == b[2] and a[3].lower() == b[3].lower() and abs(a[0] - b[0]) < 2e-5 and abs(a[1] - b[1]) < 3e-5


def pins_to_add(pins_path, have, log, del_path=None):
    """(lines to append to WPT.TXT as bytes, how many) - the map's pins that aren't there (or deleted there by
    hand), and a "-" line for each pin edited or removed in PIP LINK Setup (PINS.DEL) that is still there."""
    def read(p):                                       # every line (parse_places would merge the old versions)
        out = []
        if p and os.path.exists(p):
            with open(p, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    f = line.strip().split(",", 3)
                    try:
                        out.append((float(f[0]), float(f[1]), f[2].strip(), f[3].strip()))
                    except (ValueError, IndexError):
                        pass
        return out
    pins, dels = read(pins_path), read(del_path)
    live, gone = parse_places(have)
    lines = []
    for d in dels:                                     # old versions of edited / removed pins
        if any(_exactly(d, p) for p in pins):
            continue
        for x in live:
            if _exactly(d, x):
                lines.append(f"-{x[0]:.6f},{x[1]:.6f},{x[2]},{x[3]}\n")
                live.remove(x)
                break
    gone = [g for g in gone if not any(_exactly(g, d) for d in dels)]   # our own removals don't block re-adding
    new = []
    for p in pins:
        if not any(same_place(p, x) for x in live + gone + new):
            new.append(p)
    lines += [f"{lat:.6f},{lon:.6f},{typ},{name}\n" for lat, lon, typ, name in new]
    if not lines:
        log(f"locations: all {len(pins)} map pins are already on the Pip-Boy")
        return b"", 0
    text = "".join(lines).encode()
    if have and not have.endswith("\n"):
        text = b"\n" + text
    removed = len(lines) - len(new)
    log(f"locations: adding {len(new)} map pins to the GO TO list" + (f", removing {removed} old versions" if removed else "")
        + f" ({len(pins) - len(new)} were already there)")
    return text, len(lines)


def merge_pins(r, pins_path, log, del_path=None):
    """Add the pack's KMZ pins to the saved locations on the card (not ones already there or deleted there)."""
    have = read_file(r, WPT_FILE).decode("utf-8", errors="replace")
    text, n = pins_to_add(pins_path, have, log, del_path)
    if not n:
        return 0
    mkdirs(r, "PIPLINK", set())
    b64 = base64.b64encode(text).decode()
    for i in range(0, len(b64), CHUNK):
        r.call(f'require("fs").appendFileSync({js_str(WPT_FILE)},atob("{b64[i:i + CHUNK]}"));print("@@>>")')
    got = int(r.call(f'print("@@"+require("fs").statSync({js_str(WPT_FILE)}).size+">>")'))
    if got != len(have.encode()) + len(text):
        raise PipError("couldn't add the pins to the saved locations - send the map again")
    return n


def download_places(port="auto", log=print):
    """The Pip-Boy's saved locations: [(lat, lon, type, name)]."""
    r = Repl(port, log)
    try:
        live, _ = parse_places(read_file(r, WPT_FILE).decode("utf-8", errors="replace"))
    finally:
        r.close()
    return live


def places_kml(places, title="PIP LINK locations"):
    """A Google Earth KML of the saved locations (the marker type goes in the description)."""
    from xml.sax.saxutils import escape
    marks = "".join(f"  <Placemark><name>{escape(name)}</name><description>{escape(typ)}</description>"
                    f"<Point><coordinates>{lon:.6f},{lat:.6f},0</coordinates></Point></Placemark>\n"
                    for lat, lon, typ, name in places)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<kml xmlns="http://www.opengis.net/kml/2.2"><Document>\n'
            f"  <name>{escape(title)}</name>\n{marks}</Document></kml>\n")


def map_info(text):
    """INFO.TXT -> {"name", "center", "zoom"} (name "" for maps made before maps had names)."""
    info = {"name": "", "center": "", "zoom": ""}
    for line in (text or "").splitlines():
        key, _, val = line.partition(" ")
        if key in info:
            info[key] = val.strip()
    return info


def other_map(pack, have_text):
    """A description of the map already there when it isn't the same map as pack (sending replaces it), else None."""
    if not have_text or not have_text.startswith("PBMAP"):
        return None
    with open(os.path.join(pack, "INFO.TXT"), encoding="utf-8", errors="replace") as fh:
        mine = map_info(fh.read())
    there = map_info(have_text)
    if there["name"] == mine["name"] and (there["name"] or there["center"] == mine["center"]):
        return None
    if there["name"]:
        return f'the map "{there["name"]}"'
    c = there["center"].split()
    return "an older map" + (f" (around {float(c[0]):.3f}, {float(c[1]):.3f})" if len(c) == 2 else "")


def remote_info(port="auto", log=print):
    """INFO.TXT of the map on the Pip-Boy ('' if none)."""
    r = Repl(port, log)
    try:
        return read_file(r, REMOTE_ROOT + "/INFO.TXT").decode("utf-8", errors="replace")
    finally:
        r.close()


def card_info(root):
    try:
        with open(os.path.join(root, *REMOTE_ROOT.split("/"), "INFO.TXT"), encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


# ------------------------------------------------------------------ straight to the SD card (card reader)
CARD_DIRS = ("HOLO", "MAP", "PIPLINK", "LOGS", "USER", "RADIO")   # folders a Pip-Boy card has


def removable_drives():
    """[(root, label, free bytes, total bytes)]: SD card readers / USB sticks with a card in them (Windows)."""
    import ctypes
    import shutil
    import string
    k32 = ctypes.windll.kernel32
    out = []
    mask = k32.GetLogicalDrives()
    for i, letter in enumerate(string.ascii_uppercase):
        root = f"{letter}:\\"
        if not (mask >> i) & 1 or k32.GetDriveTypeW(root) != 2:          # 2 = DRIVE_REMOVABLE
            continue
        label = ctypes.create_unicode_buffer(64)
        if not k32.GetVolumeInformationW(root, label, 64, None, None, None, None, 0):
            continue                                                       # empty card reader
        try:
            du = shutil.disk_usage(root)
        except OSError:
            continue
        out.append((root, label.value, du.free, du.total))
    return out


def looks_like_pipboy_card(root):
    return any(os.path.isdir(os.path.join(root, d)) for d in CARD_DIRS)


def copy_to_card(pack, root, log=print, progress=None, cancel=None):
    """Copy a map (the PIPLINK/MAP folder from map_lib) to an SD card in a card reader at root (e.g. "E:\\"):
    <root>/PIPLINK/MAP ends up the same as the pack (only changed files are copied; files the map no longer
    has are deleted), and the map's KMZ pins are added to the Pip-Boy's saved locations. progress(done, total)
    in bytes."""
    import shutil
    src = os.path.abspath(pack)
    dest = os.path.join(root, *REMOTE_ROOT.split("/"))
    files = []
    for d, _, fs in os.walk(src):
        for f in fs:
            if f.startswith(".") or (f in (PINS, PINS_DEL) and d == src):
                continue
            full = os.path.join(d, f)
            files.append((os.path.relpath(full, src).replace("\\", "/"), full, os.path.getsize(full)))
    if not files:
        raise PipError("the map is empty - build a map first")
    state_path = os.path.join(dest, ".sync.json")              # what was copied last time (sha1 per file)
    try:
        with open(state_path) as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        state = {}
    log("checking what's already on the card...")
    digest, todo = {}, []
    for rel, full, size in files:
        with open(full, "rb") as fh:
            digest[rel] = hashlib.sha1(fh.read()).hexdigest()
        there = os.path.join(dest, rel)
        if state.get(rel) != digest[rel] or not os.path.exists(there) or os.path.getsize(there) != size:
            todo.append((rel, full, size))
    total = sum(f[2] for f in todo)
    free = shutil.disk_usage(root).free
    if total > free - (8 << 20):
        raise PipError(f"the SD card is too full: the map needs {total / 1048576:.0f} MB, the card has "
                       f"{free / 1048576:.0f} MB free")
    log(f"{len(files) - len(todo)} files already on the card, copying {len(todo)} ({total / 1048576:.1f} MB)")
    done = 0
    try:
        for k, (rel, full, size) in enumerate(todo):
            if cancel and cancel.is_set():
                raise PipError("cancelled - copy again to continue where it stopped")
            there = os.path.join(dest, *rel.split("/"))
            os.makedirs(os.path.dirname(there), exist_ok=True)
            shutil.copyfile(full, there)
            state[rel] = digest[rel]
            done += size
            if progress:
                progress(done, total)
            if k % 200 == 199:
                with open(state_path, "w") as fh:
                    json.dump(state, fh)
    finally:
        os.makedirs(dest, exist_ok=True)
        with open(state_path, "w") as fh:
            json.dump(state, fh)
    removed = 0                                                 # files the map doesn't have any more
    for d, _, fs in os.walk(dest, topdown=False):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), dest).replace("\\", "/")
            if rel != ".sync.json" and rel not in digest:
                os.remove(os.path.join(d, f))
                state.pop(rel, None)
                removed += 1
        if d != dest and not os.listdir(d):
            os.rmdir(d)
    if removed:
        log(f"removed {removed} old files from the card")
        with open(state_path, "w") as fh:
            json.dump(state, fh)
    old = os.path.join(root, "MAP", "INFO.TXT")                 # very early PIP LINK maps lived in MAP/
    try:
        with open(old, "rb") as fh:
            legacy = fh.read(5) == b"PBMAP"
    except OSError:
        legacy = False
    if legacy:
        log("removing the old PIP LINK map from MAP/ (the Pip-Boy's own NV/F3 maps are left alone)")
        for name in ("T", "M"):
            shutil.rmtree(os.path.join(root, "MAP", name), ignore_errors=True)
        for name in ("INFO.TXT", "ICONS.BIN"):
            try:
                os.remove(os.path.join(root, "MAP", name))
            except OSError:
                pass
    pins_path = os.path.join(src, PINS)
    if os.path.exists(pins_path) or os.path.exists(os.path.join(src, PINS_DEL)):
        wpt = os.path.join(root, *WPT_FILE.split("/"))
        try:
            with open(wpt, "rb") as fh:
                have = fh.read().decode("utf-8", errors="replace")
        except OSError:
            have = ""
        text, n = pins_to_add(pins_path, have, log, os.path.join(src, PINS_DEL))
        if n:
            with open(wpt, "ab") as fh:
                fh.write(text)
    log("copy complete")


def _upload(pack, port, remote_root, log, progress, cancel):
    if not remote_root.startswith("PIPLINK/"):          # this mirrors (deletes) - keep it inside our own folder
        raise PipError("refusing to upload outside PIPLINK/")
    root = os.path.abspath(pack)
    files = []
    for d, _, fs in os.walk(root):
        for f in fs:
            if f.startswith(".") or (f in (PINS, PINS_DEL) and d == root):   # bookkeeping / pins (merged, below)
                continue
            full = os.path.join(d, f)
            rel = os.path.relpath(full, root).replace("\\", "/")
            files.append((rel, full, os.path.getsize(full)))
    if not files:
        raise PipError("the map pack is empty - build a map first")
    pins_path = os.path.join(root, PINS)
    files.sort(key=lambda x: (x[0].count("/"), x[0]))       # INFO/ICONS first, then markers, then tiles
    local_files = {f[0] for f in files}
    local_dirs = {"/".join(f[0].split("/")[:i]) for f in files for i in range(1, f[0].count("/") + 1)}
    log(f"map pack: {len(files)} files, {sum(f[2] for f in files) / 1024:.0f} KB")

    r = Repl(port, log)
    try:
        # walk what's on the card: delete what the pack doesn't have, note sizes of the rest
        made, remote, removed = set(), {}, 0
        cleanup_legacy(r, log)
        mkdirs(r, remote_root, made)
        todo_dirs = [""]
        log("checking what's already on the card...")
        while todo_dirs:
            d = todo_dirs.pop()
            path = remote_root + ("/" + d if d else "")
            made.add(path)
            for name, is_dir, size in list_entries(r, path):
                if cancel and cancel.is_set():
                    raise PipError("cancelled")
                rel = (d + "/" + name).lstrip("/")
                if is_dir:
                    if rel in local_dirs:
                        todo_dirs.append(rel)
                    else:
                        removed += remove_tree(r, remote_root + "/" + rel, log, cancel)
                elif rel in local_files:
                    remote[rel] = size
                else:
                    r.call(f'try{{require("fs").unlinkSync({js_str(remote_root + "/" + rel)})}}catch(e){{}}print("@@>>")')
                    removed += 1
        if removed:
            log(f"removed {removed} old files from the card")

        state_path = os.path.join(root, ".uploaded.json")
        try:
            with open(state_path) as fh:
                state = json.load(fh)
        except (OSError, ValueError):
            state = {}
        digest = {f[0]: hashlib.sha1(open(f[1], "rb").read()).hexdigest() for f in files}
        todo = [f for f in files if remote.get(f[0]) != f[2] or state.get(f[0]) != digest[f[0]]]
        total = sum(f[2] for f in todo)
        log(f"{len(files) - len(todo)} files already on the card, sending {len(todo)} ({total / 1024:.0f} KB)")
        sent, t0 = 0, time.time()
        if progress:
            progress(0, total, None)
        for k, (rel, full, size) in enumerate(todo):
            if cancel and cancel.is_set():
                raise PipError("cancelled - run it again to continue where it stopped")
            mkdirs(r, os.path.dirname(remote_root + "/" + rel).replace("\\", "/"), made)
            with open(full, "rb") as fh:
                b64 = base64.b64encode(fh.read()).decode()
            dest = js_str(remote_root + "/" + rel)
            for attempt in range(3):
                for i in range(0, max(1, len(b64)), CHUNK):
                    fn = "writeFileSync" if i == 0 else "appendFileSync"
                    r.call(f'require("fs").{fn}({dest},atob("{b64[i:i + CHUNK]}"));print("@@>>")')
                got = r.call(f'print("@@"+require("fs").statSync({dest}).size+">>")')
                if got.strip() == str(size):
                    break
                log(f"{rel}: size {got} != {size}, retrying")
                r.sp.write(b"echo(0)\n")                 # in case the Pip-Boy turned echo back on
            else:
                raise PipError(f"failed to upload {rel}")
            sent += size
            state[rel] = digest[rel]
            if k % 20 == 19 or k == len(todo) - 1:
                with open(state_path, "w") as fh:
                    json.dump(state, fh)
            el = time.time() - t0
            rate = sent / el if el > 0 else 0
            if progress:
                progress(sent, total, (total - sent) / rate if rate else None)
        if os.path.exists(pins_path) or os.path.exists(os.path.join(root, PINS_DEL)):
            merge_pins(r, pins_path, log, os.path.join(root, PINS_DEL))
        log("upload complete")
    finally:
        r.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack", nargs="?", default=os.path.join(os.path.dirname(__file__), "..", "build", "MAP"))
    ap.add_argument("--port", default="auto", help="serial port (default: find the Pip-Boy by USB ID)")
    ap.add_argument("--remote", default=REMOTE_ROOT)
    a = ap.parse_args()

    def bar(done, total, eta):
        print(f"\r  {done / 1024:.0f}/{total / 1024:.0f} KB" + (f"  ~{eta:.0f}s left   " if eta else "   "),
              end="", flush=True)
    try:
        upload(a.pack, a.port, a.remote, progress=bar)
        print()
    except PipError as ex:
        sys.exit(str(ex))


if __name__ == "__main__":
    main()
