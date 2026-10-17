#!/usr/bin/env python3
"""
term_sim.py - PC stand-in for the ESP32: drives the RobCo Terminal holotape
over the Pip-Boy's USB serial using the same protocol the ESP will use.

  python term_sim.py hello            # launch terminal, print HI
  python term_sim.py demo [secs]      # interactive menu: knobs + keyboard test
  python term_sim.py burst            # flow-control stress test (~5 KB)
  python term_sim.py map [secs]       # map viewer test: knob1 zoom, click = marker test, knob2 = pan east
Options: --port COM6
"""
import argparse, base64, queue, re, sys, threading, time

import serial

WINDOW = 1400          # max unacknowledged bytes (Espruino RX buffer is 2048)
EV = re.compile(r"<<T (.+?)>>")


def b64(s):
    return base64.b64encode(s.encode("ascii", "replace")).decode("ascii")


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.05)
        self.events = queue.Queue()
        self.errors = []
        self.inflight = 0
        self.seq = 0
        self.capture = None              # when a list, every raw line is collected
        threading.Thread(target=self._reader, daemon=True).start()
        self.raw("")                     # prime: first line after open can be swallowed
        time.sleep(0.3)
        self.raw("echo(0)")
        time.sleep(0.2)

    def _reader(self):
        buf = b""
        while True:
            data = self.ser.read(512)
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("ascii", "replace").rstrip("\r")
                if self.capture is not None:
                    self.capture.append(s)
                m = EV.search(s)
                if m:
                    self.events.put(m.group(1))
                elif "Uncaught" in s or "Error" in s:
                    self.errors.append(s)
                    print("  !! device:", s)

    def raw(self, js):
        # on Windows the Pip-Boy's last output of a REPL line can sit unread until something prints
        # later; a short delayed print pushes it through (the ESP host doesn't need this)
        self.ser.write((js + ';setTimeout(function(){print(" ")},30)\n').encode("ascii", "replace"))

    def wait(self, prefix, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                ev = self.events.get(timeout=0.1)
            except queue.Empty:
                continue
            if ev.startswith(prefix):
                return ev
            self.events.put(ev) if prefix == "__never__" else None
        return None

    def send(self, js):
        """Queue one command, honouring the ack window."""
        if self.inflight + len(js) + 1 > WINDOW:
            self.flush()
        self.raw(js)
        self.inflight += len(js) + 1

    def flush(self, timeout=5.0):
        self.seq += 1
        self.raw("T.ack(%d)" % self.seq)
        want = "ACK %d" % self.seq
        end = time.time() + timeout
        while time.time() < end:
            try:
                ev = self.events.get(timeout=0.1)
            except queue.Empty:
                continue
            if ev == want:
                self.inflight = 0
                return True
        print("  !! ack %d timed out" % self.seq)
        self.inflight = 0
        return False

    # ---- terminal API (mirrors what pip_term.cpp will expose) ----
    def launch(self):
        self.raw('print("<<T ID "+(Pip.CURRENT&&Pip.CURRENT.id)+">>")')
        ev = self.wait("ID ", 3)
        if ev != "ID TERM":
            self.raw('Pip.loadHolotape("HOLO/TERM/APP.js")')
        else:
            self.raw("T.hello()")
        hi = self.wait("HI ", 8)
        if not hi:
            sys.exit("terminal did not answer HI")
        _, cols, rows, ver = hi.split()
        return int(cols), int(rows), ver

    def hdr(self, s): self.send('T.hdr("%s")' % b64(s))
    def ftr(self, s): self.send('T.ftr("%s")' % b64(s))
    def ln(self, row, s, attr=2): self.send('T.ln(%d,"%s",%d)' % (row, b64(s), attr))
    def pr(self, s): self.send('T.pr("%s")' % b64(s))
    def cls(self): self.send("T.cls()")
    def kb(self, kid, title, init=""): self.send('T.kb("%s","%s","%s")' % (kid, b64(title), b64(init)))
    def snd(self, name="SELECT"): self.send('T.snd("%s")' % name)


def cmd_hello(link):
    cols, rows, ver = link.launch()
    print("terminal v%s  grid %dx%d" % (ver, cols, rows))


def cmd_demo(link, secs):
    cols, rows, ver = link.launch()
    items = ["MESHCORE", "RADIACODE", "TERMINAL", "SETTINGS", "KEYBOARD TEST"]
    sel = 0
    log = []

    def paint():
        link.hdr("ROBCO TERMLINK :: PC SIM  (v%s %dx%d)" % (ver, cols, rows))
        link.ftr("knob1: move  click: select   knob2: clear log")
        for i, it in enumerate(items):
            link.ln(i, ("> " if i == sel else "  ") + it, 4 if i == sel else 2)
        link.ln(len(items), "-" * cols, 1)
        for i in range(rows - len(items) - 1):
            r = len(items) + 1 + i
            link.ln(r, log[i] if i < len(log) else "", 3 if i == 0 else 1)
        link.flush()

    def note(s):
        log.insert(0, s)
        del log[rows:]
        print("  ", s)

    link.cls()
    paint()
    print("demo running for %ds - turn/click the Pip-Boy knobs..." % secs)
    end = time.time() + secs
    while time.time() < end:
        try:
            ev = link.events.get(timeout=0.2)
        except queue.Empty:
            continue
        if ev == "BYE":
            print("terminal closed (BYE)")
            return
        parts = ev.split()
        if parts[0] == "K1" and parts[1] in ("+1", "-1"):
            sel = (sel + (1 if parts[1] == "+1" else -1)) % len(items)
            link.snd("SCROLL")
            note("K1 %s -> %s" % (parts[1], items[sel]))
        elif parts[0] == "K1" and parts[1] == "C":
            if items[sel] == "KEYBOARD TEST":
                note("opening keyboard...")
                paint()
                link.kb("1", "TYPE SOMETHING, THEN SEND", "")
                continue
            note("selected %s" % items[sel])
        elif parts[0] == "K2":
            log.clear()
            note("K2 %s (log cleared)" % parts[1])
        elif parts[0] == "KB":
            if parts[2] == "OK":
                txt = base64.b64decode(parts[3]).decode("ascii", "replace") if len(parts) > 3 else ""
                note('keyboard: "%s"' % txt)
            else:
                note("keyboard: cancelled")
        paint()


def cmd_burst(link):
    cols, rows, ver = link.launch()
    link.hdr("BURST TEST :: scroll mode")
    link.ftr("flow-control stress test")
    link.cls()
    link.flush()
    t0 = time.time()
    sent = 0
    for n in range(120):
        s = "line %03d  the quick brown fox jumps over the lazy dog\n" % n
        link.pr(s)
        sent += len(s)
    ok = link.flush(timeout=10)
    dt = time.time() - t0
    print("burst: %d chars of text, %d acks, %.2fs, final ack %s, device errors: %d"
          % (sent, link.seq, dt, "OK" if ok else "LOST", len(link.errors)))


def cmd_inspect(link):
    """Read-only: show what's on the terminal grid and the ESP's SD log. Never relaunches."""
    link.raw('print("<<T ID "+(Pip.CURRENT&&Pip.CURRENT.id)+">>")')
    ev = link.wait("ID ", 3)
    print("running app:", ev[3:] if ev else "?")
    if ev == "ID TERM":
        link.raw("if(T.dump)T.dump();else print('<<T DUMP END>>')")
        grid = {}
        end = time.time() + 5
        while time.time() < end:
            try:
                e = link.events.get(timeout=0.2)
            except queue.Empty:
                continue
            p = e.split(" ")
            if p[0] != "DUMP":
                continue
            if p[1] == "END":
                break
            if p[1] in ("HDR", "FTR"):
                grid[p[1]] = base64.b64decode(p[2]).decode("ascii", "replace") if len(p) > 2 else ""
            elif p[1] == "ROW":
                grid[int(p[2])] = (int(p[3]), base64.b64decode(p[4]).decode("ascii", "replace"))
        print("+-- header: %s" % grid.get("HDR", ""))
        for k in sorted(x for x in grid if isinstance(x, int)):
            attr, text = grid[k]
            print("| %2d [%s] %s" % (k, "INV" if attr & 4 else "c%d" % (attr & 3), text))
        print("+-- footer: %s" % grid.get("FTR", ""))
    link.capture = []
    # stream only the last ~12 KB in pieces: reading the whole log at once runs the Pip-Boy out of memory
    link.raw('try{var F="HOLO/TERM/ESP.LOG",n=require("fs").statSync(F).size,f=E.openFile(F,"r");'
             'f.skip(Math.max(0,n-12000));print("<<T LOGB>>");var d,c="";while((d=f.read(512))&&d.length){c+=d;'
             'var i=c.lastIndexOf("\\n");if(i>=0){print(c.substr(0,i));c=c.substr(i+1)}}if(c)print(c);'
             'f.close();f=d=c=undefined;print("<<T LOGE>>")}catch(e){print("<<T NOLOG>>")}')
    end = time.time() + 6
    while time.time() < end and not any("<<T LOGE>>" in l or "<<T NOLOG>>" in l for l in link.capture):
        time.sleep(0.1)
    lines = link.capture
    link.capture = None
    if any("<<T NOLOG>>" in l for l in lines):
        print("(no ESP.LOG on the SD card)")
        return
    try:
        a = next(i for i, l in enumerate(lines) if "<<T LOGB>>" in l)
        b = next(i for i, l in enumerate(lines) if "<<T LOGE>>" in l)
        print("---- ESP.LOG ----")
        for l in lines[a + 1:b]:
            print(l)
    except StopIteration:
        print("(log read timed out)")


def cmd_map(link, secs):
    cols, rows, ver = link.launch()
    print("terminal v%s" % ver)
    link.send("T.map(1)")
    info = link.wait("MAP ", 5)
    print("map info:", info)
    if not info:
        return
    _, zmin, zmax, lat, lon = info.split()
    zmin, zmax, lat, lon = int(zmin), int(zmax), float(lat), float(lon)
    z, hd, east = min(16, zmax), 45, 0.0

    def show():
        dyn = "%.5f,%.5f,7,-1,TEST RPTR;%.5f,%.5f,22,-1,FRIEND;%.6f,%.6f,-1,%d,;" % (
            lat + 0.003, lon + east + 0.004, lat - 0.002, lon + east - 0.003, lat, lon + east, hd)
        link.hdr("MAP TEST  z%d  %.5f,%.5f" % (z, lat, lon + east))
        link.ftr("knob1: zoom   click: turn   knob2: pan")
        t = time.time()
        link.send('T.mv(%.6f,%.6f,%d,"%s")' % (lat, lon + east, z, b64(dyn)))
        link.flush()
        link.flush(10)                                    # 2nd ack only after the redraw ran
        print("z%d drawn in ~%.2fs" % (z, time.time() - t))

    show()
    end = time.time() + secs
    while time.time() < end:
        try:
            ev = link.events.get(timeout=0.2)
        except queue.Empty:
            continue
        if ev.startswith("K1 "):
            d = ev[3:]
            if d == "C":
                hd = (hd + 45) % 360
            else:
                z = max(zmin, min(zmax, z + (1 if d == "+1" else -1)))
            show()
        elif ev.startswith("K2 ") and ev[3:] != "C":
            east += (0.002 if ev[3:] == "+1" else -0.002) * 2 ** (16 - z)
            show()
        elif ev == "BYE":
            break
    if secs > 0:                                          # map 0 = leave it on screen (for pb_screen.py)
        link.send("T.map(0)")
        link.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["hello", "demo", "burst", "inspect", "map"])
    ap.add_argument("secs", nargs="?", type=int, default=60)
    ap.add_argument("--port", default="COM6")
    a = ap.parse_args()
    link = Link(a.port)
    {"hello": lambda: cmd_hello(link),
     "demo": lambda: cmd_demo(link, a.secs),
     "burst": lambda: cmd_burst(link),
     "inspect": lambda: cmd_inspect(link),
     "map": lambda: cmd_map(link, a.secs)}[a.cmd]()


if __name__ == "__main__":
    main()
