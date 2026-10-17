"""PIP LINK Setup - one Windows program to flash the PIP LINK unit (XIAO ESP32-S3), install the PIP LINK
holotape on the Pip-Boy, and build + send offline maps.

Run from source:   python piplink_setup.py            (needs ../tools on the path - done below)
Build the .exe:    python build_exe.py
"""
import contextlib
import json
import os
import queue
import re
import shutil
import sys
import threading
import time
import tkinter as tk
import traceback
import zipfile
from tkinter import filedialog, messagebox, simpledialog, ttk

from PIL import Image, ImageTk

FROZEN = getattr(sys, "frozen", False)
BASE = sys._MEIPASS if FROZEN else os.path.dirname(os.path.abspath(__file__))
if not FROZEN:
    sys.path.insert(0, os.path.join(BASE, "..", "tools"))
ASSETS = os.path.join(BASE, "assets")
DATA = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "PIP LINK")
os.makedirs(DATA, exist_ok=True)

import map_build      # noqa: E402  (tools/)
import map_lib        # noqa: E402
import map_upload     # noqa: E402
import osm_extract    # noqa: E402
import map_icons      # noqa: E402
from map_picker import MapPicker, icon_image   # noqa: E402
from serial.tools import list_ports   # noqa: E402

APP_NAME = "PIP LINK Setup"
BG, PANEL, FG, DIM, DARK, WARN = "#050b06", "#0b160d", "#3cff6e", "#1f9a45", "#0f3a1c", "#ffcf3c"
FONT = ("Consolas", 11)
FONT_B = ("Consolas", 11, "bold")
FONT_H = ("Consolas", 18, "bold")
FONT_T = ("Consolas", 26, "bold")

TIMEZONES = [   # label, POSIX TZ (what the PIP LINK unit uses for message times); the first is the default
    ("US Pacific (Las Vegas, LA)", "PST8PDT,M3.2.0,M11.1.0"),
    ("US Mountain (Denver, Salt Lake)", "MST7MDT,M3.2.0,M11.1.0"),
    ("US Arizona (no DST)", "MST7"),
    ("US Central", "CST6CDT,M3.2.0,M11.1.0"),
    ("US Eastern", "EST5EDT,M3.2.0,M11.1.0"),
    ("US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"),
    ("US Hawaii", "HST10"),
    ("UK / Ireland", "GMT0BST,M3.5.0/1,M10.5.0"),
    ("Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("UTC", "UTC0"),
]
MAP_ROOT = os.path.join(DATA, "maps")      # the map library (tools/map_lib.py)
ZOOM_DESC = {12: "highways, main roads, towns", 13: "+ main streets", 14: "+ every street",
             15: "+ more street names (good for a big area)", 16: "+ all street names, paths, buildings",
             17: "extra close-up (small areas)"}
UPLOAD_BPS = 5500       # measured: ~5.5 KB/s over the Pip-Boy's USB serial


# ------------------------------------------------------------------ settings + software package
def load_settings():
    try:
        with open(os.path.join(DATA, "settings.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_settings(s):
    with open(os.path.join(DATA, "settings.json"), "w") as f:
        json.dump(s, f, indent=2)


class Package:
    """A PIP LINK software release: firmware/*.bin + pipboy-sd/ + piplink.json (see tools/make_update.py)."""
    def __init__(self, root, builtin):
        self.root, self.builtin = root, builtin
        with open(os.path.join(root, "piplink.json")) as f:
            self.meta = json.load(f)
        self.version = self.meta.get("version", "?")

    def label(self):
        return f"PIP LINK v{self.version}" + ("  (built in)" if self.builtin else "  (update file)")

    def fw(self, name):
        return os.path.join(self.root, "firmware", name)


def builtin_package():
    return Package(os.path.join(ASSETS, "package"), True)


def active_package(settings):
    p = settings.get("update_dir")
    if p and os.path.exists(os.path.join(p, "piplink.json")):
        try:
            return Package(p, False)
        except (OSError, ValueError):
            pass
    return builtin_package()


# ------------------------------------------------------------------ devices
def find_devices():
    pip = esp = None
    for p in list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0xA4F1:
            pip = p.device
        elif p.vid == 0x303A:                                 # Espressif: XIAO in bootloader (setup) mode
            esp = p.device
    return pip, esp


class StreamToLog:
    """File-like object for esptool's console output -> log lines + progress."""
    def __init__(self, on_line):
        self.on_line, self.buf = on_line, ""

    def write(self, s):
        self.buf += s
        while True:
            m = re.search(r"[\r\n]", self.buf)
            if not m:
                break
            line, self.buf = self.buf[:m.start()], self.buf[m.end():]
            if line.strip():
                self.on_line(line.strip())
        return len(s)

    def flush(self):
        pass


# ------------------------------------------------------------------ the window
class App:
    def __init__(self, root):
        self.root = root
        self.settings = load_settings()
        self.pkg = active_package(self.settings)
        self.q = queue.Queue()
        self.busy = False
        self.cancel = threading.Event()
        self.pip_port = self.esp_port = None
        self.places, self.areas, self.extracts, self.pin_rows = [], [], [], []
        self.mapdb = None                                     # the open map (map_lib.MapDB)
        self.sel = self.sel_name = None                       # the area picked to add: (s, w, n, e), name
        self.est = None
        # --auto holo|send|flash: run one step unattended, then close (testing / support)
        self.auto = sys.argv[sys.argv.index("--auto") + 1] if "--auto" in sys.argv[:-1] else None
        self.logfile = open(os.path.join(DATA, "setup.log"), "a", encoding="utf-8")
        self.logfile.write(f"\n==== {time.strftime('%Y-%m-%d %H:%M:%S')}  {APP_NAME}  {sys.argv[1:]}  "
                           f"{os.path.abspath(sys.executable if FROZEN else __file__)}\n")
        self.logfile.flush()
        # a mistake in a button handler shouldn't vanish without a trace (the .exe has no console)
        root.report_callback_exception = lambda *exc: self.log(
            "ERROR (window): " + " | ".join(traceback.format_exception(*exc)[-3:]).replace("\n", " "))

        root.title(APP_NAME)
        root.configure(bg=BG)
        root.geometry("1240x780+10+0")
        root.minsize(1100, 720)
        try:
            root.iconbitmap(os.path.join(ASSETS, "icon.ico"))
        except tk.TclError:
            pass
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("G.Horizontal.TProgressbar", troughcolor=DARK, background=FG, bordercolor=DIM,
                        lightcolor=FG, darkcolor=FG)
        style.configure("G.TCombobox", fieldbackground=PANEL, background=DARK, foreground=FG, arrowcolor=FG)
        style.map("G.TCombobox", fieldbackground=[("readonly", PANEL)], foreground=[("readonly", FG)],
                  selectbackground=[("readonly", PANEL)], selectforeground=[("readonly", FG)])
        root.option_add("*TCombobox*Listbox.background", PANEL)
        root.option_add("*TCombobox*Listbox.foreground", FG)

        top = tk.Frame(root, bg=BG)
        top.pack(fill="x", padx=18, pady=(14, 4))
        tk.Label(top, text="PIP LINK", font=FONT_T, fg=FG, bg=BG).pack(side="left")
        tk.Label(top, text="  SETUP", font=FONT_H, fg=DIM, bg=BG).pack(side="left", pady=(8, 0))
        self.pkg_label = tk.Label(top, text="", font=FONT, fg=DIM, bg=BG)
        self.pkg_label.pack(side="right", pady=(10, 0))
        tk.Frame(root, bg=DIM, height=2).pack(fill="x", padx=18)

        body = tk.Frame(root, bg=BG)
        body.pack(fill="both", expand=True, padx=18, pady=10)
        nav = tk.Frame(body, bg=BG)
        nav.pack(side="left", fill="y", padx=(0, 14))
        self.pages, self.nav_btns = {}, {}
        content = tk.Frame(body, bg=PANEL, highlightbackground=DIM, highlightthickness=1)
        content.pack(side="left", fill="both", expand=True)
        for key, title in (("unit", "1  PIP LINK UNIT"), ("holo", "2  PIP-BOY"), ("maps", "3  MAPS"), ("upd", "   UPDATES")):
            b = tk.Button(nav, text=title, font=FONT_B, width=18, anchor="w", relief="flat", bd=0, padx=10, pady=10,
                          bg=BG, fg=FG, activebackground=DARK, activeforeground=FG, command=lambda k=key: self.show(k))
            b.pack(fill="x", pady=2)
            self.nav_btns[key] = b
            f = tk.Frame(content, bg=PANEL)
            self.pages[key] = f
        tk.Label(nav, text="", bg=BG).pack(expand=True)
        tk.Label(nav, text="Map data (c) OpenStreetMap\ncontributors. Elevation:\nAWS Terrain Tiles.",
                 font=("Consolas", 8), fg=DIM, bg=BG, justify="left").pack(side="bottom", anchor="w")

        status = tk.Frame(root, bg=BG)
        status.pack(fill="x", padx=18, pady=(0, 4))
        self.status_pip = tk.Label(status, text="", font=FONT, fg=DIM, bg=BG)
        self.status_pip.pack(side="left")
        self.status_esp = tk.Label(status, text="", font=FONT, fg=DIM, bg=BG)
        self.status_esp.pack(side="left", padx=30)
        self.logbox = tk.Text(root, height=5, bg=BG, fg=DIM, font=("Consolas", 9), relief="flat",
                              highlightthickness=1, highlightbackground=DARK, state="disabled")
        self.logbox.pack(fill="x", padx=18, pady=(0, 12))

        self.build_unit_page()
        self.build_holo_page()
        self.build_maps_page()
        self.build_updates_page()
        self.refresh_pkg()
        self.show("unit")
        self.poll_devices()
        self.root.after(100, self.pump)
        if self.auto:
            self.root.after(1500, self.autorun)

    # ---------------------------------------------------------------- helpers
    def show(self, key):
        for k, f in self.pages.items():
            f.pack_forget()
            self.nav_btns[k].configure(bg=DARK if k == key else BG)
        self.pages[key].pack(fill="both", expand=True, padx=20, pady=16)

    def h(self, parent, text):
        tk.Label(parent, text=text, font=FONT_H, fg=FG, bg=PANEL, anchor="w").pack(fill="x", pady=(0, 8))

    def p(self, parent, text, fg=None, wrap=880, **kw):
        lab = tk.Label(parent, text=text, font=FONT, fg=fg or FG, bg=PANEL, justify="left", anchor="w",
                       wraplength=wrap, **kw)
        lab.pack(fill="x", pady=2)
        return lab

    def button(self, parent, text, cmd, big=True):
        return tk.Button(parent, text=text, command=cmd, font=FONT_B if big else FONT, bg=DARK, fg=FG,
                         activebackground=FG, activeforeground=BG, disabledforeground="#2b5a37", relief="flat",
                         padx=16 if big else 8, pady=8 if big else 4, cursor="hand2")

    def progress_row(self, parent):
        bar = ttk.Progressbar(parent, style="G.Horizontal.TProgressbar", maximum=1000)
        bar.pack(fill="x", pady=(12, 2))
        lab = tk.Label(parent, text="", font=FONT, fg=DIM, bg=PANEL, anchor="w")
        lab.pack(fill="x")
        return bar, lab

    def notify(self, text):
        self.log(text.replace("\n\n", " ").replace("\n", " "))
        if self.auto:
            self.root.after(800, self.root.destroy)
        else:
            messagebox.showinfo(APP_NAME, text)

    def autorun(self):
        act = {"holo": (self.pip_port, self.do_holo, "holo"), "send": (self.pip_port, self.do_send, "maps"),
               "flash": (self.esp_port, self.do_flash, "unit")}.get(self.auto)
        if not act:
            return
        ready, fn, page = act
        if not ready:                                     # wait for the device
            self.root.after(1000, self.autorun)
            return
        self.show(page)
        fn()

    def log(self, msg):
        self.q.put(("log", msg))

    def ui(self, fn, *a):
        self.q.put(("call", lambda: fn(*a)))

    def pump(self):
        try:
            while True:
                kind, v = self.q.get_nowait()
                if kind == "log":
                    self.logfile.write(v + "\n")
                    self.logfile.flush()
                    self.logbox.configure(state="normal")
                    self.logbox.insert("end", v + "\n")
                    self.logbox.see("end")
                    self.logbox.configure(state="disabled")
                else:
                    v()
        except queue.Empty:
            pass
        self.root.after(80, self.pump)

    def run(self, work, done=None):
        """Run work() on a worker thread; done(ok, result_or_error) back on the UI thread."""
        if self.busy:
            self.log("busy - please wait")
            return
        self.busy = True
        self.cancel.clear()

        def worker():
            try:
                res, ok = work(), True
            except Exception as ex:                       # noqa: BLE001 - shown to the user
                res, ok = ex, False
                self.log("ERROR: " + str(ex))
                if not isinstance(ex, (map_upload.PipError, RuntimeError)):
                    self.log(" | ".join(traceback.format_exc().strip().splitlines()[-5:]))
            self.q.put(("call", lambda: self.finish(done, ok, res)))
        threading.Thread(target=worker, daemon=True).start()

    def finish(self, done, ok, res):
        self.busy = False
        if done:
            done(ok, res)
        if self.auto and not ok:
            self.root.after(800, self.root.destroy)
        self.update_buttons()

    def poll_devices(self):
        try:
            self.pip_port, self.esp_port = find_devices()
        except Exception:                                 # noqa: BLE001
            self.pip_port = self.esp_port = None
        self.status_pip.configure(text=f"PIP-BOY: connected ({self.pip_port})" if self.pip_port else "PIP-BOY: not connected",
                                  fg=FG if self.pip_port else DIM)
        self.status_esp.configure(text=f"PIP LINK UNIT: ready for firmware ({self.esp_port})" if self.esp_port
                                  else "PIP LINK UNIT: not in setup mode", fg=FG if self.esp_port else DIM)
        self.update_buttons()
        self.root.after(1500, self.poll_devices)

    def update_buttons(self):
        st = lambda ok: "normal" if ok and not self.busy else "disabled"   # noqa: E731
        self.btn_flash.configure(state=st(self.esp_port))
        self.btn_holo.configure(state=st(self.pip_port))
        self.btn_add.configure(state=st(self.sel and self.est and (self.est["new"] or self.redo.get())))
        self.btn_send.configure(state=st(self.pip_port and self.pack_ready()))
        self.btn_copy.configure(state=st(self.pack_ready()))
        self.btn_getdata.configure(state=st(True))
        self.btn_preview.configure(state=st(self.mapdb))
        self.btn_cancel.configure(state="normal" if self.busy else "disabled")
        self.esp_hint.configure(text="Unit detected - click INSTALL FIRMWARE." if self.esp_port
                                else "Waiting for the unit in setup mode (steps 1-2)...", fg=FG if self.esp_port else WARN)
        self.pip_hint.configure(text="Pip-Boy detected - click INSTALL ON PIP-BOY." if self.pip_port
                                else "Waiting for the Pip-Boy (step 1)...", fg=FG if self.pip_port else WARN)

    def refresh_pkg(self):
        self.pkg_label.configure(text="software: " + self.pkg.label())
        self.upd_current.configure(text="Ready to install:  " + self.pkg.label())

    # ---------------------------------------------------------------- 1: flash the unit
    def build_unit_page(self):
        f = self.pages["unit"]
        self.h(f, "SET UP THE PIP LINK UNIT")
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x")
        img = Image.open(os.path.join(ASSETS, "xiao_boot.png"))
        self.xiao_img = ImageTk.PhotoImage(img.resize((280, 200)))
        tk.Label(row, image=self.xiao_img, bg=PANEL).pack(side="right", padx=(10, 0), anchor="n")
        left = tk.Frame(row, bg=PANEL)
        left.pack(side="left", fill="both", expand=True)
        self.p(left, "1. Unplug the PIP LINK unit from the Pip-Boy and plug its USB-C into this computer.", wrap=380)
        self.p(left, "2. Put it in setup mode: HOLD the B (BOOT) button, TAP the R (RESET) button, then let go of B.", wrap=380)
        self.p(left, "3. Click INSTALL FIRMWARE when the unit is detected.", wrap=380)
        self.p(left, "4. When it's done, tap R (RESET) once, unplug it, and connect it to the Pip-Boy.", wrap=380)
        self.esp_hint = self.p(left, "", fg=WARN, wrap=380)
        self.erase = tk.BooleanVar(value=False)
        tk.Checkbutton(left, text="Also clear saved settings (radio, PIN, pairings)", variable=self.erase,
                       font=FONT, fg=DIM, bg=PANEL, selectcolor=BG, activebackground=PANEL, activeforeground=FG
                       ).pack(anchor="w", pady=(8, 0))
        self.btn_flash = self.button(left, "INSTALL FIRMWARE", self.do_flash)
        self.btn_flash.pack(anchor="w", pady=10)
        self.flash_bar, self.flash_lab = self.progress_row(f)

    def do_flash(self):
        port, pkg, erase = self.esp_port, self.pkg, self.erase.get()
        files = [("0x0", "bootloader.bin"), ("0x8000", "partitions.bin"), ("0xe000", "boot_app0.bin"), ("0x10000", "firmware.bin")]
        sizes = [os.path.getsize(pkg.fw(n)) for _, n in files]
        state = {"seg": -1, "last": 101}

        def on_line(line):
            m = re.search(r"Writing at 0x[0-9a-f]+\.*\s*\((\d+)\s*%\)", line)
            if m:
                pct = int(m.group(1))
                if pct < state["last"]:
                    state["seg"] = min(state["seg"] + 1, len(files) - 1)
                state["last"] = pct
                done = sum(sizes[:state["seg"]]) + sizes[state["seg"]] * pct / 100
                self.ui(self.set_progress, self.flash_bar, self.flash_lab, done / sum(sizes), f"writing... {pct}%")
            elif not line.startswith("Writing at"):
                self.log(line)

        def work():
            import esptool
            base = ["--chip", "esp32s3", "--port", port, "--baud", "921600", "--before", "default_reset", "--after", "hard_reset"]
            self.log(f"installing {pkg.label()} on {port}")
            out = StreamToLog(on_line)
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                try:
                    if erase:
                        self.log("clearing saved settings")
                        esptool.main(base[:-1] + ["no_reset", "erase_region", "0x9000", "0x5000"])
                    args = base + ["write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "8MB"]
                    for addr, name in files:
                        args += [addr, pkg.fw(name)]
                    esptool.main(args)
                except SystemExit as ex:
                    if ex.code not in (0, None):
                        raise RuntimeError("the firmware install failed - put the unit in setup mode again and retry")
                except esptool.FatalError as ex:
                    raise RuntimeError(f"firmware install failed: {ex}")

        def done(ok, res):
            if ok:
                self.set_progress(self.flash_bar, self.flash_lab, 1, "DONE - tap R (RESET) on the unit, then connect it to the Pip-Boy.")
                self.notify("Firmware installed!\n\nTap the R (RESET) button on the PIP LINK unit, "
                                              "then connect it to the Pip-Boy.")
            else:
                self.set_progress(self.flash_bar, self.flash_lab, 0, "Failed - see the log below.")
        self.set_progress(self.flash_bar, self.flash_lab, 0, "starting...")
        self.run(work, done)

    def set_progress(self, bar, lab, frac, text):
        bar["value"] = int(max(0, min(1, frac)) * 1000)
        lab.configure(text=text)

    # ---------------------------------------------------------------- 2: holotape
    def build_holo_page(self):
        f = self.pages["holo"]
        self.h(f, "INSTALL PIP LINK ON THE PIP-BOY")
        self.p(f, "1. Plug the Pip-Boy into this computer with a USB data cable and switch it on.")
        self.p(f, "   (Close any Pip-Boy web page or Espruino tool that might be connected to it.)", fg=DIM)
        self.p(f, "2. Pick your time zone (used for message times) and click INSTALL ON PIP-BOY.")
        self.p(f, "3. Restart the Pip-Boy once. After that, just connect the PIP LINK unit - it starts on its own.")
        self.pip_hint = self.p(f, "", fg=WARN)
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x", pady=8)
        tk.Label(row, text="Time zone:", font=FONT, fg=FG, bg=PANEL).pack(side="left")
        labels = [t[0] for t in TIMEZONES]
        saved = self.settings.get("tz_label", "")
        self.tz = tk.StringVar(value=saved if saved in labels else                 # (labels gained city names)
                               next((lb for lb in labels if saved and lb.startswith(saved)), labels[0]))
        ttk.Combobox(row, textvariable=self.tz, values=labels, state="readonly", width=34, style="G.TCombobox",
                     font=FONT).pack(side="left", padx=10)
        self.btn_holo = self.button(f, "INSTALL ON PIP-BOY", self.do_holo)
        self.btn_holo.pack(anchor="w", pady=10)
        self.holo_bar, self.holo_lab = self.progress_row(f)

    def do_holo(self):
        pkg = self.pkg
        tz_label = self.tz.get()
        tz = dict(TIMEZONES).get(tz_label, TIMEZONES[0][1])
        self.settings["tz_label"] = tz_label
        save_settings(self.settings)
        src = os.path.join(pkg.root, "pipboy-sd")
        files = []
        for d, _, fs in os.walk(src):
            for fn in fs:
                full = os.path.join(d, fn)
                files.append((full, os.path.relpath(full, src).replace("\\", "/")))
        files.sort(key=lambda x: x[1].endswith(".info"))          # the menu entry last
        cfg = f"tz={tz}\n".encode()
        total = sum(os.path.getsize(f[0]) for f in files) + len(cfg)

        def work():
            try:
                r = map_upload.Repl(self.pip_port or "auto", self.log)
            except OSError as ex:
                raise map_upload.PipError("can't reach the Pip-Boy - check the cable and that it's switched on") from ex
            try:
                made, sent = set(), 0
                for _, dest in files + [(None, "HOLO/TERM/CONFIG.TXT")]:
                    map_upload.mkdirs(r, "/".join(dest.split("/")[:-1]), made)
                for full, dest in files + [(None, "HOLO/TERM/CONFIG.TXT")]:
                    data = cfg if full is None else open(full, "rb").read()
                    self.put(r, data, dest)
                    sent += len(data)
                    self.ui(self.set_progress, self.holo_bar, self.holo_lab, sent / total, f"copied {dest}")
            finally:
                r.close()

        def done(ok, res):
            if ok:
                self.set_progress(self.holo_bar, self.holo_lab, 1, "DONE - restart the Pip-Boy once.")
                self.notify(f"{pkg.label()} is installed on the Pip-Boy.\n\nRestart the Pip-Boy once. "
                                              "Then connect the PIP LINK unit - it opens by itself.")
            else:
                self.set_progress(self.holo_bar, self.holo_lab, 0, "Failed - see the log below.")
        self.set_progress(self.holo_bar, self.holo_lab, 0, "connecting to the Pip-Boy...")
        self.run(work, done)

    def put(self, r, data, dest):
        import base64
        b64 = base64.b64encode(data).decode()
        d = map_upload.js_str(dest)
        for attempt in range(4):
            try:                               # a reply can go missing while the Pip-Boy is busy: start the file over
                for i in range(0, max(1, len(b64)), map_upload.CHUNK):
                    fn = "writeFileSync" if i == 0 else "appendFileSync"
                    r.call(f'require("fs").{fn}({d},atob("{b64[i:i + map_upload.CHUNK]}"));print("@@>>")')
                if r.call(f'print("@@"+require("fs").statSync({d}).size+">>")').strip() == str(len(data)):
                    self.log(f"OK  {dest}  ({len(data)} bytes)")
                    return
            except TimeoutError:
                self.log(f"{dest}: the Pip-Boy didn't answer - trying again")
            r.sp.write(b"\necho(0)\n")
        raise map_upload.PipError(f"couldn't write {dest} - try again")

    # ---------------------------------------------------------------- 3: maps
    def build_maps_page(self):
        f = self.pages["maps"]
        head = tk.Frame(f, bg=PANEL)
        head.pack(fill="x")
        tk.Label(head, text="MAPS", font=FONT_H, fg=FG, bg=PANEL).pack(side="left")
        tk.Label(head, text="    map:", font=FONT, fg=FG, bg=PANEL).pack(side="left")
        self.map_var = tk.StringVar()
        self.map_combo = ttk.Combobox(head, textvariable=self.map_var, state="readonly", width=20, style="G.TCombobox",
                                      font=FONT)
        self.map_combo.pack(side="left", padx=6)
        self.map_combo.bind("<<ComboboxSelected>>", lambda e: self.open_map(self.map_var.get()))
        for text, cmd in (("NEW", self.new_map), ("RENAME", self.rename_map), ("DELETE", self.delete_map),
                          ("FOLDER", self.open_map_folder)):
            self.button(head, text, cmd, big=False).pack(side="left", padx=2)
        self.map_stats = tk.Label(f, text="", font=("Consolas", 9), fg=DIM, bg=PANEL, anchor="w")
        self.map_stats.pack(fill="x", pady=(2, 0))

        mid = tk.Frame(f, bg=PANEL)
        mid.pack(fill="both", expand=True, pady=(8, 0))
        left = tk.Frame(mid, bg=PANEL)
        left.pack(side="left", fill="both", expand=True)
        self.picker = MapPicker(left, os.path.join(DATA, "cache"), on_select=self.on_pick, on_hover=self.on_hover,
                                on_click=self.on_map_click, width=540, height=360)
        self.picker.pack(fill="both", expand=True)
        hint = tk.Frame(left, bg=PANEL)
        hint.pack(fill="x")
        tk.Label(hint, text="drag: move   wheel: zoom   RIGHT-drag: pick an area", font=("Consolas", 9), fg=DIM,
                 bg=PANEL).pack(side="left")
        self.show_mine = tk.BooleanVar(value=True)
        tk.Checkbutton(hint, text="show the Pip-Boy map", variable=self.show_mine, command=self.toggle_mine,
                       font=("Consolas", 9), fg=DIM, bg=PANEL, selectcolor=BG, activebackground=PANEL,
                       activeforeground=FG).pack(side="right")
        self.coord_lab = tk.Label(hint, text="", font=("Consolas", 9), fg=DIM, bg=PANEL)
        self.coord_lab.pack(side="right", padx=8)

        right = tk.Frame(mid, bg=PANEL, width=380)
        right.pack(side="left", fill="y", padx=(12, 0))
        right.pack_propagate(False)
        tabs = tk.Frame(right, bg=PANEL)
        tabs.pack(fill="x")
        body = tk.Frame(right, bg=PANEL)
        body.pack(fill="both", expand=True, pady=(6, 0))
        self.map_tabs, self.map_tab_btns = {}, {}
        for key, title in (("add", "ADD AREA"), ("areas", "AREAS"), ("pins", "PINS"), ("data", "OFFLINE DATA")):
            b = tk.Button(tabs, text=title, font=("Consolas", 9, "bold"), relief="flat", bd=0, padx=6, pady=4, bg=BG,
                          fg=FG, activebackground=DARK, activeforeground=FG, command=lambda k=key: self.map_tab(k))
            b.pack(side="left", padx=(0, 2))
            self.map_tab_btns[key] = b
            self.map_tabs[key] = tk.Frame(body, bg=PANEL)
        self.build_add_tab(self.map_tabs["add"])
        self.build_areas_tab(self.map_tabs["areas"])
        self.build_pins_tab(self.map_tabs["pins"])
        self.build_data_tab(self.map_tabs["data"])
        self.map_tab("add")

        self.map_bar, self.map_lab = self.progress_row(f)
        brow = tk.Frame(f, bg=PANEL)
        brow.pack(fill="x", pady=(4, 0))
        self.btn_send = self.button(brow, "SEND TO PIP-BOY (USB)", self.do_send)
        self.btn_send.pack(side="left")
        self.btn_copy = self.button(brow, "COPY TO SD CARD...", self.do_copy)
        self.btn_copy.pack(side="left", padx=8)
        self.btn_cancel = self.button(brow, "STOP", lambda: self.cancel.set(), big=False)
        self.btn_cancel.pack(side="left")
        self.send_info = tk.Label(brow, text="", font=("Consolas", 9), fg=DIM, bg=PANEL, justify="left", anchor="w")
        self.send_info.pack(side="left", padx=12)
        self.root.after(200, self.refresh_maps)

    def small(self, parent, text, fg=None, wrap=370):
        lab = tk.Label(parent, text=text, font=("Consolas", 9), fg=fg or DIM, bg=PANEL, justify="left", anchor="w",
                       wraplength=wrap)
        lab.pack(fill="x")
        return lab

    def map_tab(self, key):
        for k, fr in self.map_tabs.items():
            fr.pack_forget()
            self.map_tab_btns[k].configure(bg=DARK if k == key else BG)
        self.map_tabs[key].pack(fill="both", expand=True)
        if key == "data":
            self.refresh_extracts()

    def build_add_tab(self, f):
        srow = tk.Frame(f, bg=PANEL)
        srow.pack(fill="x")
        self.q_entry = tk.Entry(srow, font=FONT, bg=BG, fg=FG, insertbackground=FG, relief="flat",
                                highlightthickness=1, highlightbackground=DIM)
        self.q_entry.pack(side="left", fill="x", expand=True, ipady=3)
        self.q_entry.bind("<Return>", lambda e: self.do_search())
        self.button(srow, "FIND", self.do_search, big=False).pack(side="left", padx=(4, 0))
        self.small(f, "town, county, state, address or lat,lon")
        self.results = tk.Listbox(f, height=3, font=("Consolas", 9), bg=BG, fg=FG, selectbackground=DARK,
                                  selectforeground=FG, relief="flat", highlightthickness=1, highlightbackground=DARK)
        self.results.pack(fill="x", pady=(2, 4))
        self.results.bind("<<ListboxSelect>>", self.pick_result)
        self.sel_lab = tk.Label(f, text="", font=FONT_B, fg=WARN, bg=PANEL, anchor="w", justify="left", wraplength=330)
        self.sel_lab.pack(fill="x")
        zrow = tk.Frame(f, bg=PANEL)
        zrow.pack(fill="x", pady=(2, 0))
        tk.Label(zrow, text="zoom from", font=FONT, fg=FG, bg=PANEL).pack(side="left")
        self.zfrom = tk.StringVar(value=str(self.settings.get("zoom_from", 10)))
        self.zto = tk.StringVar(value=str(self.settings.get("zoom_to", 15)))
        for var, vals in ((self.zfrom, [str(z) for z in range(8, 14)]), (self.zto, [str(z) for z in range(12, 18)])):
            cb = ttk.Combobox(zrow, textvariable=var, values=vals, state="readonly", width=3, style="G.TCombobox", font=FONT)
            cb.pack(side="left", padx=4)
            cb.bind("<<ComboboxSelected>>", lambda e: self.update_estimate())
            if var is self.zfrom:
                tk.Label(zrow, text="to", font=FONT, fg=FG, bg=PANEL).pack(side="left")
        self.zdesc = self.small(f, "")
        self.contours = tk.BooleanVar(value=True)
        self.landmarks = tk.BooleanVar(value=True)
        self.buildings = tk.BooleanVar(value=True)
        self.redo = tk.BooleanVar(value=False)
        opts = tk.Frame(f, bg=PANEL)
        opts.pack(fill="x", pady=(2, 0))
        for i, (text, var, cmd) in enumerate((("contours", self.contours, None), ("landmarks", self.landmarks, None),
                                              ("buildings (z16+)", self.buildings, self.update_estimate),
                                              ("redraw what's there", self.redo, self.update_estimate))):
            tk.Checkbutton(opts, text=text, variable=var, command=cmd, font=("Consolas", 9), fg=FG, bg=PANEL,
                           selectcolor=BG, activebackground=PANEL, activeforeground=FG, anchor="w"
                           ).grid(row=i // 2, column=i % 2, sticky="w")
        self.est_lab = self.small(f, "", fg=FG)
        arow = tk.Frame(f, bg=PANEL)
        arow.pack(fill="x", pady=(4, 0))
        self.btn_add = self.button(arow, "ADD TO MAP", self.do_add)
        self.btn_add.pack(side="left")
        self.btn_preview = self.button(arow, "PREVIEW", self.do_preview)
        self.btn_preview.pack(side="left", padx=6)

    def build_areas_tab(self, f):
        self.small(f, "Everything added to this map. Tiles stay until you remove the area they came from.")
        self.area_list = tk.Listbox(f, height=9, font=("Consolas", 9), bg=BG, fg=FG, selectbackground=DARK,
                                    selectforeground=FG, relief="flat", highlightthickness=1, highlightbackground=DARK,
                                    exportselection=False)
        self.area_list.pack(fill="x", pady=4)
        self.area_list.bind("<<ListboxSelect>>", lambda e: self.area_info())
        self.area_list.bind("<Double-Button-1>", lambda e: self.area_show())
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x")
        self.area_btns = []
        for text, cmd in (("SHOW", self.area_show), ("RESUME", self.area_resume), ("UPDATE", self.area_update),
                          ("REMOVE", self.area_remove)):
            b = self.button(row, text, cmd, big=False)
            b.pack(side="left", padx=(0, 4))
            self.area_btns.append(b)
        self.area_lab = self.small(f, "")
        self.small(f, "\nRESUME finishes a stopped area. UPDATE draws it again with new map data (new roads etc.).")

    def build_pins_tab(self, f):
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x")
        self.button(row, "ADD KMZ/KML...", self.add_kmz, big=False).pack(side="left")
        self.btn_newpin = self.button(row, "NEW PIN", self.new_pin, big=False)
        self.btn_newpin.pack(side="left", padx=3)
        self.button(row, "DUPLICATES", self.find_duplicates, big=False).pack(side="left")
        self.pin_list = tk.Listbox(f, height=7, font=("Consolas", 9), bg=BG, fg=FG, selectbackground=DARK,
                                   selectforeground=WARN, relief="flat", highlightthickness=1, highlightbackground=DARK,
                                   exportselection=False, selectmode="extended")
        self.pin_list.pack(fill="x", pady=(4, 2))
        self.pin_list.bind("<<ListboxSelect>>", lambda e: self.pin_picked())
        self.pin_name = tk.Entry(f, font=FONT, bg=BG, fg=FG, insertbackground=FG, relief="flat",
                                 highlightthickness=1, highlightbackground=DIM)
        self.pin_name.pack(fill="x", ipady=2)
        self.pin_name.bind("<Return>", lambda e: self.save_pin())
        erow = tk.Frame(f, bg=PANEL)
        erow.pack(fill="x", pady=(3, 0))
        self.pin_type = tk.StringVar(value="LANDMARK")
        cb = ttk.Combobox(erow, textvariable=self.pin_type, values=map_icons.TYPES, state="readonly", width=9,
                          style="G.TCombobox", font=("Consolas", 10), height=26)
        cb.pack(side="left")
        cb.bind("<<ComboboxSelected>>", lambda e: self.show_pin_icon())
        self.pin_icon = tk.Label(erow, bg=BG, width=36, height=36)
        self.pin_icon.pack(side="left", padx=4)
        self.pin_pos = tk.Entry(erow, font=("Consolas", 9), bg=BG, fg=FG, insertbackground=FG, relief="flat",
                                highlightthickness=1, highlightbackground=DIM)
        self.pin_pos.pack(side="left", fill="x", expand=True, ipady=3)
        self.pin_pos.bind("<Return>", lambda e: self.save_pin())
        brow = tk.Frame(f, bg=PANEL)
        brow.pack(fill="x", pady=(3, 0))
        self.button(brow, "SAVE", self.save_pin, big=False).pack(side="left")
        self.button(brow, "DELETE", self.delete_pin, big=False).pack(side="left", padx=3)
        self.button(brow, "SAVE AS KML...", self.export_pins_kml, big=False).pack(side="left")
        self.pins_lab = self.small(f, "", fg=FG)
        self.small(f, "Click a pin on the map to pick it. Changes reach the Pip-Boy's GO TO list when the map is "
                      "sent or copied.")
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x", pady=(2, 0))
        self.button(row, "PIP-BOY'S OWN -> KML...", self.save_places, big=False).pack(side="left")
        self.button(row, "CLEAR ALL", self.clear_kmz, big=False).pack(side="left", padx=3)
        self.show_pin_icon()

    def build_data_tab(self, f):
        self.small(f, "For big maps (a county, a state): download a whole state's street data once. Areas inside it "
                      "are then built offline - faster, and without waiting on the busy public map servers. "
                      "Nevada is a 120 MB download and takes several minutes to unpack.")
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x", pady=6)
        self.region = tk.Entry(row, font=FONT, bg=BG, fg=FG, insertbackground=FG, relief="flat", width=16,
                               highlightthickness=1, highlightbackground=DIM)
        self.region.insert(0, "Nevada")
        self.region.pack(side="left", ipady=3)
        self.btn_getdata = self.button(row, "GET", self.get_extract, big=False)
        self.btn_getdata.pack(side="left", padx=4)
        self.small(f, "a US state, country or region as Geofabrik names it (e.g. Nevada, Utah, California)")
        tk.Label(f, text="Installed:", font=FONT_B, fg=FG, bg=PANEL, anchor="w").pack(fill="x", pady=(8, 0))
        self.ex_list = tk.Listbox(f, height=4, font=("Consolas", 9), bg=BG, fg=FG, selectbackground=DARK,
                                  selectforeground=FG, relief="flat", highlightthickness=1, highlightbackground=DARK,
                                  exportselection=False)
        self.ex_list.pack(fill="x", pady=4)
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x")
        self.button(row, "UPDATE", lambda: self.get_extract(update=True), big=False).pack(side="left")
        self.button(row, "REMOVE", self.remove_extract, big=False).pack(side="left", padx=4)
        self.small(f, "\nStreet data (c) OpenStreetMap contributors, from Geofabrik.")

    # ---- the map library
    def refresh_maps(self, select=None):
        names = map_lib.list_maps(MAP_ROOT)
        if not names:
            map_lib.open_map("My Map", MAP_ROOT).close()
            names = ["My Map"]
        self.map_combo.configure(values=names)
        want = select or self.settings.get("map")
        self.open_map(want if want in names else names[0])

    def open_map(self, name):
        if self.busy:
            self.map_var.set(self.mapdb.name if self.mapdb else "")
            return
        if self.mapdb:
            self.mapdb.close()
        self.mapdb = map_lib.open_map(name, MAP_ROOT)
        self.map_var.set(self.mapdb.name)
        self.settings["map"] = self.mapdb.name
        save_settings(self.settings)
        self.picker.clear_preview()
        self.picker.set_map(self.mapdb)
        c = self.mapdb.center()
        if c:
            self.picker.set_view(c[0], c[1], 10)
        self.refresh_map_info()

    def refresh_map_info(self):
        db = self.mapdb
        st = db.stats()
        if st["zooms"]:
            self.map_stats.configure(text=f"{db.name}:  {st['areas']} area{'s' if st['areas'] != 1 else ''}  ·  zoom "
                                          f"{st['zooms'][0]}-{st['zooms'][1]}  ·  {st['tiles']:,} tiles  ·  "
                                          f"{st['bytes'] / 1048576:.1f} MB")
        else:
            self.map_stats.configure(text="empty - pick an area and ADD TO MAP")
        self.area_list.delete(0, "end")
        self.areas = db.areas()
        for a in self.areas:
            w, h = map_lib.rect_km(a["rect"])
            self.area_list.insert("end", ("  " if a["done"] else "! ") + f"{a['name'][:22]:22} z{a['zmin']}-{a['zmax']}"
                                  + ("" if a["done"] else "  STOPPED"))
        self.refresh_pins()
        pack = db.pack_dir
        if os.path.exists(os.path.join(pack, "INFO.TXT")):
            ps = map_lib.pack_size(pack)
            usb = ps["bytes"] / UPLOAD_BPS
            self.send_info.configure(text=f"{ps['bytes'] / 1048576:.1f} MB, {ps['files']:,} files\nUSB: about "
                                          f"{map_lib.fmt_secs(usb)} the first time, then only changes" +
                                          ("  -  use the SD card" if usb > 1800 else ""))
        else:
            self.send_info.configure(text="")
        self.update_estimate()
        self.update_buttons()

    def new_map(self):
        if self.busy:
            return
        name = simpledialog.askstring(APP_NAME, "Name for the new map (e.g. Nevada, Vegas trip):", parent=self.root)
        if not name or not map_lib.safe_name(name):
            return
        if map_lib.safe_name(name) in map_lib.list_maps(MAP_ROOT):
            messagebox.showinfo(APP_NAME, f"There's already a map called {name}.")
            return
        map_lib.open_map(name, MAP_ROOT).close()
        self.refresh_maps(map_lib.safe_name(name))

    def rename_map(self):
        if self.busy or not self.mapdb:
            return
        name = simpledialog.askstring(APP_NAME, "New name:", initialvalue=self.mapdb.name, parent=self.root)
        if not name or name == self.mapdb.name:
            return
        try:
            self.mapdb = map_lib.rename_map(self.mapdb, name, MAP_ROOT)
        except ValueError as ex:                              # bad or taken name: nothing changed
            messagebox.showerror(APP_NAME, str(ex))
            return
        except OSError as ex:                                 # the rename itself failed: the map was closed
            messagebox.showerror(APP_NAME, f"Couldn't rename the map: {ex}")
            self.mapdb = map_lib.open_map(self.map_var.get(), MAP_ROOT)
        self.refresh_maps(self.mapdb.name)

    def delete_map(self):
        if self.busy or not self.mapdb:
            return
        st = self.mapdb.stats()
        if not messagebox.askyesno(APP_NAME, f"Delete the map {self.mapdb.name} ({st['tiles']:,} tiles) from this "
                                             "computer?\n\nThe copy on the Pip-Boy stays until you send another map."):
            return
        map_lib.delete_map(self.mapdb)
        self.mapdb = None
        self.settings.pop("map", None)
        self.refresh_maps()

    def open_map_folder(self):
        if self.mapdb:
            os.startfile(self.mapdb.folder)

    def toggle_mine(self):
        self.picker.show_mine = self.show_mine.get()
        self.picker.redraw()

    def on_hover(self, lat, lon):
        self.coord_lab.configure(text=f"{lat:.5f}, {lon:.5f}")

    # ---- picking an area
    def on_pick(self, rect):
        self.picker.clear_preview()
        self.sel = rect
        self.sel_name = "Area" if rect else None
        self.update_estimate()

    def do_search(self):
        q = self.q_entry.get().strip()
        if not q:
            return
        m = re.match(r"^\s*(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)\s*$", q)
        if m:
            lat, lon = float(m.group(1)), float(m.group(2))
            self.set_selection(map_lib.around(lat, lon, 3), f"{lat:.4f},{lon:.4f}")
            return

        def done(ok, res):
            self.results.delete(0, "end")
            self.places = res if ok else []
            for name, *_ in self.places:
                self.results.insert("end", name)
            if ok and not self.places:
                self.log(f"nothing found for '{q}'")
            if len(self.places) == 1:
                self.results.selection_set(0)
                self.pick_result()
        self.run(lambda: map_build.search_places(q), done)

    def pick_result(self, _e=None):
        sel = self.results.curselection()
        if not sel or sel[0] >= len(self.places):
            return
        name, lat, lon, rect = self.places[sel[0]]
        if not rect or min(map_lib.rect_km(rect)) < 2:          # an address or a small place: 3 km around it
            rect = map_lib.around(lat, lon, 3)
        self.set_selection(rect, name.split(",")[0])

    def set_selection(self, rect, name):
        self.picker.clear_preview()
        self.sel, self.sel_name = rect, name
        self.picker.set_selection(rect, fit=True)
        self.update_estimate()

    def zooms(self):
        zmin, zmax = int(self.zfrom.get()), int(self.zto.get())
        return min(zmin, zmax), max(zmin, zmax)

    def update_estimate(self):
        if not hasattr(self, "est_lab"):
            return
        zmin, zmax = self.zooms()
        self.zdesc.configure(text=f"zoom {zmax}: {ZOOM_DESC.get(zmax, '')}")
        self.settings["zoom_from"], self.settings["zoom_to"] = zmin, zmax
        if not self.sel or not self.mapdb:
            self.sel_lab.configure(text="No area picked yet")
            self.est_lab.configure(text="")
            self.update_buttons()
            return
        w, h = map_lib.rect_km(self.sel)
        self.sel_lab.configure(text=f"{(self.sel_name or 'Area')[:30]}:  {w:.1f} x {h:.1f} km")
        est = map_lib.estimate(self.mapdb, self.sel, zmin, zmax, self.redo.get())
        if not est["new"]:
            txt = "The map already has all of this - nothing to add.\n(tick 'redraw' to draw it again)"
        else:
            txt = (f"{est['new']:,} new tiles" + (f" ({est['have']:,} already in the map)" if est["have"] else "") +
                   f"\nabout {est['bytes'] / 1048576:.0f} MB, roughly {map_lib.fmt_secs(est['secs'])} to build")
            if est["offline"]:
                txt += f"\nusing the offline {est['offline']} data"
            elif est["new"] > 30000:
                txt += "\nbig area: get its OFFLINE DATA first - much faster"
            if zmax >= 16 and w * h > 400 and self.buildings.get():
                txt += "\nbuildings make big areas slow - untick them?"
        self.est_lab.configure(text=txt)
        self.est = est
        self.update_buttons()

    def do_add(self):
        if not self.sel:
            return
        zmin, zmax = self.zooms()
        save_settings(self.settings)
        name = simpledialog.askstring(APP_NAME, "Name for this area:", initialvalue=self.sel_name or "Area",
                                      parent=self.root)
        if not name:
            return
        aid = map_lib.add_area(self.mapdb, name.strip()[:40], self.sel, zmin, zmax, self.contours.get(),
                               self.landmarks.get(), self.buildings.get(), self.redo.get())
        self.redo.set(False)
        self.run_build(aid)

    def run_build(self, aid):
        db = self.mapdb
        last = [0.0]

        def prog(frac, text):
            self.ui(self.set_progress, self.map_bar, self.map_lab, frac, text)
            if time.time() - last[0] > 5:                     # show the new tiles on the map as they come
                last[0] = time.time()
                self.ui(self.picker.refresh_mine)

        def work():
            try:
                n = map_lib.build(db, aid, self.log, prog, self.cancel)
            except map_lib.Cancelled:
                n = None
            map_lib.export(db, self.log, lambda f, t: self.ui(self.set_progress, self.map_bar, self.map_lab, f, t))
            return n

        def done(ok, res):
            self.picker.clear_preview()
            self.refresh_map_info()
            self.picker.refresh_mine()
            if ok and res is not None:
                self.set_progress(self.map_bar, self.map_lab, 1, f"Done - {res:,} tiles added. Send it to the "
                                                                 "Pip-Boy with USB or the SD card.")
            elif ok:
                self.set_progress(self.map_bar, self.map_lab, 0, "Stopped - AREAS > RESUME continues where it left off.")
            else:
                self.set_progress(self.map_bar, self.map_lab, 0, "Stopped - see the log below. AREAS > RESUME "
                                                                 "continues where it left off.")
        self.set_progress(self.map_bar, self.map_lab, 0, "starting...")
        self.run(work, done)

    # ---- areas
    def picked_area(self):
        sel = self.area_list.curselection()
        return self.areas[sel[0]] if sel and sel[0] < len(self.areas) else None

    def area_info(self):
        a = self.picked_area()
        if not a:
            self.area_lab.configure(text="")
            return
        w, h = map_lib.rect_km(a["rect"])
        o = a["opts"]
        self.area_lab.configure(text=f"{a['name']}\n{w:.1f} x {h:.1f} km, zoom {a['zmin']}-{a['zmax']}, added {a['made']}"
                                     + ("" if a["done"] else "\nnot finished - RESUME it") +
                                     ("\nno contours" if not o.get("contours", True) else ""))

    def area_show(self):
        a = self.picked_area()
        if a:
            self.picker.set_selection(None)
            self.picker.fit(a["rect"])

    def area_resume(self):
        a = self.picked_area()
        if a and not a["done"]:
            self.run_build(a["id"])
        elif a:
            messagebox.showinfo(APP_NAME, "That area is finished. UPDATE draws it again with new map data.")

    def area_update(self):
        a = self.picked_area()
        if not a or not messagebox.askyesno(APP_NAME, f"Draw {a['name']} again with the newest map data?\n\n"
                                                      "Its tiles are replaced as the new ones are drawn."):
            return
        map_lib.update_area(self.mapdb, a["id"])
        self.run_build(a["id"])

    def area_remove(self):
        a = self.picked_area()
        if not a or self.busy:
            return
        if not messagebox.askyesno(APP_NAME, f"Remove {a['name']} from the map?\n\nIts tiles are deleted, except where "
                                             "another area covers the same spot."):
            return
        db = self.mapdb

        def work():
            map_lib.remove_area(db, a["id"], self.log)
            if db.zoom_range():
                map_lib.export(db, self.log)
            elif os.path.isdir(db.pack_dir):
                shutil.rmtree(db.pack_dir, ignore_errors=True)

        def done(ok, res):
            self.refresh_map_info()
            self.picker.refresh_mine()
        self.run(work, done)

    # ---- pins
    def add_kmz(self):
        paths = filedialog.askopenfilenames(title="Google Earth pin files", filetypes=[("Google Earth", "*.kmz *.kml")])
        if not paths:
            return
        db = self.mapdb

        def work():
            res = map_lib.add_kmz(db, paths, os.path.join(DATA, "marker_styles.json"), self.log)
            map_lib.export_pins(db)
            return res

        def done(ok, res):
            self.refresh_map_info()
            if ok:
                pins, lines = res
                msg = (f"Added {pins} pins" + (f" and {lines} lines/shapes" if lines else "") + " to this map.\n\n"
                       "Check them on the map and in the list: click one to fix its icon or name, DUPLICATES finds "
                       "repeats.")
                if lines:
                    msg += ("\n\nLines are drawn into tiles built from now on - PREVIEW (under ADD AREA) shows how "
                            "they'll look. To put them on tiles the map already has, UPDATE the area under AREAS.")
                self.notify(msg)
        self.run(work, done)

    def clear_kmz(self):
        if self.mapdb and messagebox.askyesno(APP_NAME, "Remove all pins and KMZ lines from this map?\n\n"
                                                        "The pins are also taken off the Pip-Boy's GO TO list the next "
                                                        "time the map is sent (locations made on the Pip-Boy stay)."):
            map_lib.clear_pins(self.mapdb)
            map_lib.export_pins(self.mapdb)
            self.refresh_map_info()

    # ---- editing pins
    def refresh_pins(self, select=None):
        db = self.mapdb
        self.pin_rows = db.pin_rows()
        self.pin_list.delete(0, "end")
        for pid, lat, lon, typ, name, src in self.pin_rows:
            self.pin_list.insert("end", f"{typ[:9]:9} {name}")
        n = len(self.pin_rows)
        self.pins_lab.configure(text=f"{n} pin{'s' if n != 1 else ''}" +
                                (f", {len(db.lines())} KMZ lines/shapes" if db.lines() else ""))
        ids = [r[0] for r in self.pin_rows]
        if select in ids:
            i = ids.index(select)
            self.pin_list.selection_set(i)
            self.pin_list.see(i)
        self.pin_picked(move=False)

    def picked_pins(self):
        return [self.pin_rows[i] for i in self.pin_list.curselection() if i < len(self.pin_rows)]

    def pin_picked(self, move=True):
        sel = self.picked_pins()
        p = sel[0] if len(sel) == 1 else None
        self.pin_name.delete(0, "end")
        self.pin_pos.delete(0, "end")
        if p:
            self.pin_name.insert(0, p[4])
            self.pin_pos.insert(0, f"{p[1]:.6f},{p[2]:.6f}")
            self.pin_type.set(p[3])
            self.show_pin_icon()
            x, y = self.picker.to_screen(p[1], p[2])
            if move and not (20 < x < self.picker.winfo_width() - 20 and 20 < y < self.picker.winfo_height() - 20):
                self.picker.set_view(p[1], p[2], max(self.picker.z, 13))
        self.picker.set_pins([r[:5] for r in self.pin_rows], self.mapdb.markers(), p[0] if p else None)

    def show_pin_icon(self):
        big = icon_image(self.pin_type.get(), (140, 255, 170, 255)).resize((36, 36), Image.NEAREST)
        self.pin_icon_img = ImageTk.PhotoImage(big)
        self.pin_icon.configure(image=self.pin_icon_img)

    def pins_changed(self, select=None):
        map_lib.export_pins(self.mapdb)
        self.refresh_pins(select)

    def save_pin(self):
        sel = self.picked_pins()
        if len(sel) != 1:
            return
        m = re.match(r"^\s*(-?\d+(?:\.\d+)?)\s*[, ]\s*(-?\d+(?:\.\d+)?)\s*$", self.pin_pos.get())
        name = self.pin_name.get().strip()
        if not m or not name:
            messagebox.showinfo(APP_NAME, "A pin needs a name and a position as lat,lon (e.g. 35.8325,-115.4319).")
            return
        map_lib.update_pin(self.mapdb, sel[0][0], float(m.group(1)), float(m.group(2)), self.pin_type.get(), name)
        self.pins_changed(sel[0][0])

    def delete_pin(self):
        sel = self.picked_pins()
        if not sel:
            return
        what = sel[0][4] if len(sel) == 1 else f"these {len(sel)} pins"
        if messagebox.askyesno(APP_NAME, f"Delete {what}?\n\nIt also goes from the Pip-Boy's GO TO list the next time "
                                         "the map is sent."):
            map_lib.delete_pins(self.mapdb, [p[0] for p in sel])
            self.pins_changed()

    def new_pin(self):
        self.adding_pin = not getattr(self, "adding_pin", False)
        self.btn_newpin.configure(bg=WARN if self.adding_pin else DARK, fg=BG if self.adding_pin else FG)
        self.picker.configure(cursor="crosshair" if self.adding_pin else "fleur")
        if self.adding_pin:
            self.map_lab.configure(text="Click the map where the new pin goes (NEW PIN again to cancel).")

    def on_map_click(self, lat, lon, px, py):
        if getattr(self, "adding_pin", False):
            self.new_pin()                                    # back to normal
            name = simpledialog.askstring(APP_NAME, f"Name for the pin at {lat:.5f}, {lon:.5f}:", parent=self.root)
            if name and name.strip():
                pid = map_lib.add_pin(self.mapdb, lat, lon, self.pin_type.get(), name.strip())
                self.map_tab("pins")
                self.pins_changed(pid)
                self.map_lab.configure(text="Pin added - pick its icon and click SAVE if it needs a different one.")
            return
        p = self.picker.pin_at(px, py)
        if p:
            self.map_tab("pins")
            ids = [r[0] for r in self.pin_rows]
            self.pin_list.selection_clear(0, "end")
            if p[0] in ids:
                self.pin_list.selection_set(ids.index(p[0]))
                self.pin_list.see(ids.index(p[0]))
            self.pin_picked(move=False)

    def find_duplicates(self):
        groups = map_lib.duplicate_pins(self.mapdb)
        if not groups:
            messagebox.showinfo(APP_NAME, "No duplicate pins - nothing has the same name within 40 m, and no two "
                                          "pins sit on the same spot.")
            return
        names = {r[0]: r[4] for r in self.pin_rows}
        extra = [d for _, ds in groups for d in ds]
        lines = "\n".join(f"  {names.get(k, '?')}  (+{len(ds)})" for k, ds in groups[:12])
        if messagebox.askyesno(APP_NAME, f"{len(extra)} pins repeat another one:\n\n{lines}" +
                                         ("\n  ..." if len(groups) > 12 else "") +
                                         "\n\nDelete the repeats (the first of each is kept)?\nNo selects them in the "
                                         "list so you can look first."):
            map_lib.delete_pins(self.mapdb, extra)
            self.pins_changed()
        else:
            ids = [r[0] for r in self.pin_rows]
            self.pin_list.selection_clear(0, "end")
            for d in extra:
                if d in ids:
                    self.pin_list.selection_set(ids.index(d))
            self.pin_picked(move=False)

    def export_pins_kml(self):
        path = filedialog.asksaveasfilename(title="Save this map's pins", defaultextension=".kml",
                                            initialfile=f"{self.mapdb.name} pins.kml", filetypes=[("Google Earth", "*.kml")])
        if path:
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(map_upload.places_kml(self.mapdb.pins(), f"{self.mapdb.name} pins"))
            self.log(f"saved {len(self.mapdb.pins())} pins to {path}")

    # ---- preview: what the options look like, drawn but not saved
    def do_preview(self):
        zmin, zmax = self.zooms()
        z = max(zmin, min(zmax, self.picker.z))
        if z != self.picker.z:
            self.picker.set_view(*self.picker.center(), z)
        tiles = self.picker.visible_tiles(z)
        c = self.picker.center()
        rect = self.sel or map_lib.around(c[0], c[1], 3)
        opts = {"contours": self.contours.get(), "landmarks": self.landmarks.get(), "buildings": self.buildings.get()}
        db = self.mapdb

        def work():
            out, marks = map_lib.preview(db, rect, zmax, z, tiles, opts, log=self.log)
            return {k: map_lib.tile_image(v) if v else Image.new("P", (256, 256), 0) for k, v in out.items()}, marks

        def done(ok, res):
            if ok:
                imgs, marks = res
                self.picker.set_preview(z, imgs, marks if opts["landmarks"] else [])
                self.set_progress(self.map_bar, self.map_lab, 1, f"Preview at zoom {z} - nothing saved. Change the "
                                                                 "options and PREVIEW again, or zoom and PREVIEW.")
        self.set_progress(self.map_bar, self.map_lab, 0, f"drawing a preview at zoom {z}...")
        self.run(work, done)

    def save_places(self):
        path = filedialog.asksaveasfilename(title="Save the Pip-Boy's saved locations", defaultextension=".kml",
                                            initialfile="PIP LINK locations.kml", filetypes=[("Google Earth", "*.kml")])
        if not path:
            return

        def work():
            places = map_upload.download_places(self.pip_port or "auto", self.log)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(map_upload.places_kml(places))
            return len(places)

        def done(ok, res):
            if ok:
                self.log(f"saved {res} locations to {path}")
                self.notify(f"Saved {res} locations from the Pip-Boy to\n{path}\n\nOpen it in Google Earth, or add it "
                            "to a map with ADD KMZ / KML.")
        self.log("reading the saved locations from the Pip-Boy...")
        self.run(work, done)

    # ---- offline street data
    def refresh_extracts(self):
        osm_extract.forget()
        self.extracts = osm_extract.installed()
        self.ex_list.delete(0, "end")
        for ex in self.extracts:
            self.ex_list.insert("end", f"{ex.name:16} data from {ex.date}")
        if not self.extracts:
            self.ex_list.insert("end", "(none - maps use the internet)")
        self.update_estimate()

    def get_extract(self, update=False):
        if update:
            sel = self.ex_list.curselection()
            if not sel or sel[0] >= len(self.extracts):
                return
            region = os.path.basename(self.extracts[sel[0]].path)[:-3].replace("_", "/")
        else:
            region = self.region.get().strip()
        if not region:
            return

        def work():
            r = osm_extract.find_region(region, self.log)
            if not r:
                raise RuntimeError(f"Geofabrik has nothing called '{region}' - try the full name, e.g. 'Nevada'")
            osm_extract.forget()
            prog = lambda f, t: self.ui(self.set_progress, self.map_bar, self.map_lab, f if f is not None else 0.5, t)  # noqa: E731
            ex = osm_extract.get(r[0], self.log, prog, self.cancel)
            return ex.name

        def done(ok, res):
            self.refresh_extracts()
            if ok:
                self.set_progress(self.map_bar, self.map_lab, 1, f"{res} street data ready - areas inside it are "
                                                                 "built offline now.")
            else:
                self.set_progress(self.map_bar, self.map_lab, 0, "Didn't finish - see the log below.")
        self.set_progress(self.map_bar, self.map_lab, 0, "getting the street data...")
        self.run(work, done)

    def remove_extract(self):
        sel = self.ex_list.curselection()
        if self.busy or not sel or sel[0] >= len(self.extracts):
            return
        ex = self.extracts[sel[0]]
        if messagebox.askyesno(APP_NAME, f"Remove the offline {ex.name} street data?\n\nMaps you've built keep "
                                         "their tiles; new areas there will use the internet again."):
            osm_extract.remove(ex)
            self.refresh_extracts()

    # ---- getting the map onto the Pip-Boy
    def pack_ready(self):
        return bool(self.mapdb) and os.path.exists(os.path.join(self.mapdb.pack_dir, "INFO.TXT"))

    def do_send(self):
        pack = self.mapdb.pack_dir
        size = map_lib.pack_size(pack)["bytes"]
        if size / UPLOAD_BPS > 1800 and not self.auto:
            if not messagebox.askyesno(APP_NAME, f"This map is {size / 1048576:.0f} MB - about "
                                                 f"{map_lib.fmt_secs(size / UPLOAD_BPS)} over USB the first time "
                                                 "(later sends only send changes).\n\nCOPY TO SD CARD is much faster "
                                                 "for big maps. Send over USB anyway?"):
                return

        def checked(ok, have):                                # what's on the Pip-Boy now?
            if ok and self.confirm_replace(pack, have):
                self.set_progress(self.map_bar, self.map_lab, 0, "connecting to the Pip-Boy...")
                self.run(work, done)

        def work():
            def prog(done, total, eta):
                txt = f"sending... {done / 1024:.0f} / {total / 1024:.0f} KB"
                if eta:
                    txt += f"   about {map_lib.fmt_secs(eta)} left"
                self.ui(self.set_progress, self.map_bar, self.map_lab, done / total if total else 1, txt)
            map_upload.upload(pack, self.pip_port or "auto", map_upload.REMOTE_ROOT, self.log, prog, self.cancel)

        def done(ok, res):
            if ok:
                self.set_progress(self.map_bar, self.map_lab, 1, "DONE - the map is on the Pip-Boy.")
                self.notify("The map is on the Pip-Boy.\n\nConnect the PIP LINK unit and open the MAP tab.")
            else:
                self.map_lab.configure(text="Stopped - click SEND TO PIP-BOY again to continue where it left off.")
        self.set_progress(self.map_bar, self.map_lab, 0, "checking the map on the Pip-Boy...")
        self.run(lambda: map_upload.remote_info(self.pip_port or "auto", self.log), checked)

    def confirm_replace(self, pack, have):
        """The Pip-Boy holds one map: ask before a different map replaces the one there."""
        other = map_upload.other_map(pack, have)
        if not other or self.auto:
            return True
        return messagebox.askyesno(APP_NAME, f"The Pip-Boy has {other} on it.\n\nIt holds one map at a time, so sending "
                                             f"{self.mapdb.name} replaces that map.\n\nTo have both, put all the areas "
                                             "in one map: pick that map at the top, add the other areas to it (ADD "
                                             "AREA), then send it.\n\nReplace the map on the Pip-Boy?")

    def do_copy(self):
        drives = map_upload.removable_drives()
        root = None
        if len(drives) == 1:
            r, label, free, total = drives[0]
            if messagebox.askyesno(APP_NAME, f"Copy the map {self.mapdb.name} to the SD card in {r} "
                                             f"({label or 'no name'}, {free / 1073741824:.1f} GB free)?"):
                root = r
            else:
                return
        elif len(drives) > 1:
            root = self.choose_drive(drives)
        else:
            if not messagebox.askyesno(APP_NAME, "No SD card found.\n\nTake the SD card out of the Pip-Boy (switch it "
                                                 "off first), put it in a card reader on this computer, then click "
                                                 "Yes.\n\nOr click No to pick the drive yourself."):
                root = filedialog.askdirectory(title="The Pip-Boy's SD card (the top of the drive)")
            else:
                drives = map_upload.removable_drives()
                root = drives[0][0] if len(drives) == 1 else (self.choose_drive(drives) if drives else None)
                if not root:
                    messagebox.showinfo(APP_NAME, "Still no SD card - check the card reader.")
                    return
        if not root:
            return
        root = os.path.splitdrive(os.path.abspath(root))[0] + "\\" if len(root) <= 3 else root
        if not map_upload.looks_like_pipboy_card(root) and not messagebox.askyesno(
                APP_NAME, f"{root} doesn't look like the Pip-Boy's SD card (no HOLO or MAP folder).\n\nCopy anyway?"):
            return
        pack = self.mapdb.pack_dir
        if not self.confirm_replace(pack, map_upload.card_info(root)):
            return

        def work():
            def prog(done, total):
                self.ui(self.set_progress, self.map_bar, self.map_lab, done / total if total else 1,
                        f"copying... {done / 1048576:.1f} / {total / 1048576:.1f} MB")
            map_upload.copy_to_card(pack, root, self.log, prog, self.cancel)

        def done(ok, res):
            if ok:
                self.set_progress(self.map_bar, self.map_lab, 1, f"DONE - the map is on the SD card in {root}")
                self.notify(f"The map is on the SD card in {root}\n\nEject the card (right-click the drive in "
                            "File Explorer > Eject), put it back in the Pip-Boy, and open the MAP tab.")
            else:
                self.map_lab.configure(text="Stopped - COPY TO SD CARD again continues where it left off.")
        self.set_progress(self.map_bar, self.map_lab, 0, "copying to the SD card...")
        self.run(work, done)

    def choose_drive(self, drives):
        win = tk.Toplevel(self.root)
        win.title("Which SD card?")
        win.configure(bg=PANEL)
        win.transient(self.root)
        win.grab_set()
        tk.Label(win, text="Which drive is the Pip-Boy's SD card?", font=FONT_B, fg=FG, bg=PANEL).pack(padx=16, pady=8)
        lb = tk.Listbox(win, height=len(drives), font=FONT, bg=BG, fg=FG, selectbackground=DARK, width=44)
        for r, label, free, total in drives:
            lb.insert("end", f"{r}  {label or '(no name)':16} {total / 1073741824:5.1f} GB" +
                      ("  Pip-Boy card" if map_upload.looks_like_pipboy_card(r) else ""))
        lb.pack(padx=16)
        lb.selection_set(0)
        out = []
        self.button(win, "COPY HERE", lambda: (out.append(drives[lb.curselection()[0]][0]) if lb.curselection()
                                               else None, win.destroy())).pack(pady=10)
        self.root.wait_window(win)
        return out[0] if out else None

    # ---------------------------------------------------------------- updates
    def build_updates_page(self):
        f = self.pages["upd"]
        self.h(f, "UPDATES")
        self.upd_current = self.p(f, "")
        builtin = builtin_package().version
        self.p(f, f"This program has PIP LINK v{builtin} built in. When a newer version comes out you'll get an update "
                  "file (PIP-LINK-update-v....zip). Load it here, then run steps 1 and 2 again to install it on the "
                  "PIP LINK unit and the Pip-Boy. Your saved radio, PIN and maps are kept.", fg=DIM, wrap=880)
        row = tk.Frame(f, bg=PANEL)
        row.pack(fill="x", pady=12)
        self.button(row, "LOAD UPDATE FILE...", self.load_update).pack(side="left")
        self.button(row, f"USE BUILT-IN v{builtin}", self.use_builtin, big=False).pack(side="left", padx=10)

    def load_update(self):
        path = filedialog.askopenfilename(title="PIP LINK update file", filetypes=[("PIP LINK update", "*.zip")])
        if not path:
            return
        try:
            with zipfile.ZipFile(path) as z:
                meta = json.loads(z.read("piplink.json"))
                need = {f"firmware/{n}" for n in ("bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin")}
                need.add("pipboy-sd/HOLO/TERM/APP.js")
                missing = need - set(z.namelist())
                if missing:
                    raise ValueError("missing " + ", ".join(sorted(missing)))
                dest = os.path.join(DATA, "updates", "v" + re.sub(r"[^0-9A-Za-z.]", "", str(meta.get("version", "x"))))
                if os.path.exists(dest):
                    shutil.rmtree(dest)
                z.extractall(dest)
            self.pkg = Package(dest, False)
            self.settings["update_dir"] = dest
            save_settings(self.settings)
            self.refresh_pkg()
            self.log(f"loaded {self.pkg.label()}")
            messagebox.showinfo(APP_NAME, f"Loaded {self.pkg.label()}.\n\nNow do step 1 (PIP LINK UNIT) and step 2 "
                                          "(PIP-BOY) again to install it.")
        except (OSError, KeyError, ValueError, zipfile.BadZipFile) as ex:
            messagebox.showerror(APP_NAME, f"That isn't a PIP LINK update file:\n{ex}")

    def use_builtin(self):
        self.pkg = builtin_package()
        self.settings.pop("update_dir", None)
        save_settings(self.settings)
        self.refresh_pkg()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
