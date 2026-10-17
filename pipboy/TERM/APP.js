(function(params){
  // ---- PIP LINK terminal ----
  // A thin display: the host (ESP32 over USB) drives it through the global T,
  // and knob input goes back up as <<T ...>> lines. Text args are base64.
  var VER = "1.2";
  var FONT = "Monofonto16";
  var X0 = 8, TOP = 34, BOT = 298;          // grid area (header above, footer below)
  var MID = (TOP + BOT) >> 1;

  h.setFont(FONT);
  var cw = h.stringWidth("M"), rowH = h.getFontHeight() + 3;
  var COLS = Math.floor((480 - 2 * X0) / cw);
  var ROWS = Math.floor((BOT - TOP) / rowH);

  var rows = [], dirty = [];
  for (var i = 0; i < ROWS; i++) { rows.push({ t: "", a: 2 }); dirty.push(true); }
  var hdrText = "PIP LINK", ftrText = "waiting for the PIP LINK unit...";
  var hdrDirty = true, ftrDirty = true;
  var cr = 0, cc = 0;                       // scroll-mode cursor
  var cur = { r: -1, c: 0 };                // host's text cursor (TERM tab): an underline, r -1 = none
  // TERM tab canvas: its own font + grid over the grid area. [font, char w, row h, cols, rows, x0, text dy]
  // (the PIP LINK unit has the same table: VT_COLS in hub_main.cpp). 3 = BBS: 80 columns fill the whole
  // width, so with a left margin the last column or two run off the right edge
  var VT_SIZES = [["Monofonto16", 8, 22, 58, 12, 8, 1], ["Monofonto14", 7, 16, 66, 16, 9, 0], ["6x8", 6, 11, 78, 24, 6, 2],
                  ["6x8", 6, 11, 80, 24, 0, 2]];
  var vt = { on: false, z: null, x: 0, t: [], d: [] };   // x: left edge (the unit's "Text margin" setting)
  var rt = null;                            // pending render timeout
  var timers = [];

  // the Pip-Boy's USB holds back its most recent output until something else is printed, so every
  // message is followed (shortly after) by a padding line that pushes it out to the host
  var padT = 0;
  function pad() { if (!padT) padT = setTimeout(function () { padT = 0; print(" "); }, 15); }
  function up(s) { print("<<T " + s + ">>"); pad(); }
  function dec(b) { try { return atob(b || ""); } catch (e) { return ""; } }
  // drawString (left/top aligned) with a hand-drawn degree sign: the Pip-Boy fonts have no glyph for it
  function dtext(gr, s, x, y) {
    var i = s.indexOf("\xB0");
    if (i < 0) { gr.drawString(s, x, y); return; }
    var p = s.split("\xB0").join(" "), fh = gr.getFontHeight(), r = fh > 20 ? 3 : 2;
    gr.drawString(p, x, y);
    for (; i >= 0; i = s.indexOf("\xB0", i + 1)) gr.drawCircle(x + gr.stringWidth(p.substr(0, i)) + r + 1, y + r + Math.round(fh * 0.12), r);
  }

  // ---------- colour theme: PIP LINK only, the Pip-Boy's own colour comes back on exit ----------
  var themed = false;
  function cfg() { return typeof settings != "undefined" ? settings : {}; }
  function palette(r, gr, b) {               // the same maths as the firmware's colour menu
    var c0 = cfg().color || {}, sl = c0.scanline === undefined ? 128 : c0.scanline, ov = c0.overscan === undefined ? 128 : c0.overscan;
    var fr = r / 255, fg = gr / 255, fb = b / 255, d = 1 - sl / 255, e = ov / 1275;
    var pal = [new Uint16Array(16), new Uint16Array(16), new Uint16Array(16), new Uint16Array(16)];
    for (var i = 0; i < 16; i++) {
      var c = Math.max(0, (i - 12) / 30), k = i / 15;
      pal[0][i] = g.toColor(c + fr * k, c + fg * k, c + fb * k);
      pal[1][i] = g.toColor(c + d * fr * k, c + d * fg * k, c + d * fb * k);
      pal[2][i] = g.toColor(c + (e + fr) * k, c + (e + fg) * k, c + (e + fb) * k);
      pal[3][i] = g.toColor(c + (e + d * fr) * k, c + (e + d * fg) * k, c + (e + d * fb) * k);
    }
    Pip.setPalette(pal);
  }
  // firmware 1.1.x: setRGB(colour) + C.AMBER / C.GREEN, exactly what the Pip-Boy's own menus use
  function fwTheme() { return typeof setRGB == "function" && typeof C != "undefined" && C.AMBER; }
  function setTheme(n) {
    if (fwTheme()) setRGB(n == 1 ? C.AMBER : C.GREEN);
    else if (n == 1) palette(255, 112, 0); else palette(0, 255, 0);
  }
  function restoreTheme() {
    if (!themed) return;
    themed = false;
    try {
      if (fwTheme()) { setRGB(Pip.settings.theme); return; }
      var s = cfg();
      if (s.palette) Pip.setPalette(s.palette.split(",").map(function (a) { return new Uint16Array(E.toArrayBuffer(atob(a))); }));
      else palette(0, 255, 0);
    } catch (e) {}
  }

  // ---------- saved locations: PIPLINK/WPT.TXT, kept by the host (it only ever appends) ----------
  var WPTFILE = "PIPLINK/WPT.TXT", wl = null;
  function wlStop() { if (wl) { if (wl.t) clearTimeout(wl.t); try { wl.f.close(); } catch (e) {} wl = null; } }

  // ---------- rendering ----------
  // big text / bar graph spanning n rows (the rows under it are marked cov and skip drawing)
  var bigFont = null;
  function setBig(px) {
    if (bigFont === null) {
      bigFont = "";
      try { h.setFont("Monofonto28", 2); if (h.getFontHeight() >= 40) bigFont = "M"; } catch (e) {}
    }
    if (bigFont) h.setFont("Monofonto28", 2); else h.setFontVector(px);
  }
  function drawBig(y, r) {
    var H = r.n * rowH;
    h.setColor((r.a & 3) || 3);
    setBig(Math.min(H - 6, 64));
    h.setFontAlign(0, 0);
    h.drawString(r.t, 240, y + H / 2);
  }
  function drawBars(y, r) {
    var H = r.n * rowH, d = r.d, n = d.length, x0 = X0, w = 480 - 2 * X0;
    h.setColor(1); h.drawLine(x0, y + H - 1, x0 + w, y + H - 1);
    if (r.m) {                              // threshold marker (dashed)
      var my = y + H - 1 - Math.round(r.m * (H - 2) / 254);
      for (var x = x0; x < x0 + w; x += 8) h.drawLine(x, my, x + 3, my);
    }
    if (!n) return;
    var bw = w / n;
    h.setColor((r.a & 3) || 3);
    for (var i = 0; i < n; i++) {
      var v = d.charCodeAt(i) - 1;           // sent as 1..255 (no NULs) = 0..254
      if (v <= 0) continue;
      var bx = Math.floor(x0 + i * bw);
      h.fillRect(bx, y + H - 1 - Math.round(v * (H - 2) / 254), Math.max(bx, Math.floor(x0 + (i + 1) * bw) - 1), y + H - 2);
    }
  }
  function drawRow(i) {
    var y = TOP + i * rowH, r = rows[i];
    if (r.cov) return;
    h.clearRect(0, y, 479, y + (r.n || 1) * rowH - 1);
    if (r.k === 1) { drawBig(y, r); return; }
    if (r.k === 2) { drawBars(y, r); return; }
    if (i === cur.r) { h.setColor(3); h.fillRect(X0 + cur.c * cw, y + rowH - 3, X0 + (cur.c + 1) * cw - 2, y + rowH - 2); }
    if (!r.t) return;
    if (r.a & 4) {                          // inverse highlight bar
      h.setColor(1); h.fillRect(2, y, 477, y + rowH - 1);
      h.setColor(3);
    } else h.setColor((r.a & 3) || 2);
    h.setFont(FONT); h.setFontAlign(-1, -1);
    dtext(h, r.t, X0, y + 1);
    var ic = icons();                       // a map marker icon in the row's leading spaces
    if (r.ic >= 0 && ic && r.ic * 32 < ic.length)
      h.drawImage({ width: 16, height: 16, bpp: 1, transparent: 0, buffer: E.toArrayBuffer(ic.substr(r.ic * 32, 32)) }, X0 + 1, y + ((rowH - 16) >> 1));
  }
  function drawHdr() {
    h.clearRect(0, 0, 479, TOP - 2);
    h.setColor(2); h.setFont(FONT); h.setFontAlign(-1, 0);
    h.drawString(hdrText, X0 + 4, 14);
    h.setColor(1); h.drawLine(0, 28, 479, 28);
  }
  function drawFtr() {
    h.clearRect(0, BOT + 1, 479, 319);
    h.setColor(1); h.drawLine(0, BOT + 2, 479, BOT + 2);
    h.setFont("Monofonto14"); h.setFontAlign(-1, -1);
    h.setColor(3);                             // full brightness: the footer carries the live readouts
    dtext(h, ftrText, 240 - (h.stringWidth(ftrText.split("\xB0").join(" ")) >> 1), 318 - h.getFontHeight());
  }
  // ---------- map mode: tiles + markers from PIPLINK/MAP/ on the SD card (the drawing is in MAP.js) ----------
  // last: the view on screen (for panning by scrolling it), under: the pixels beneath the crosshair
  var mp = { on: false, lat: 0, lon: 0, z: 15, dyn: "", fl: 0, dirty: false, info: null, icons: null, drawnAt: 0, last: null, under: null };
  var MAPDIR = "PIPLINK/MAP/";                 // ours - the Pip-Boy's own world map lives in MAP/NV + MAP/F3
  function rd(f) { try { return require("fs").readFileSync(f); } catch (e) { return undefined; } }
  function icons() { if (mp.icons === null) mp.icons = rd(MAPDIR + "ICONS.BIN") || ""; return mp.icons; }
  // the map viewer (MAP.js) and the keyboard (KB.js) are read into this function's scope only while they're
  // in use: the Pip-Boy keeps every function's source in RAM, and all of it at once no longer fitted
  var mapm = null, kbm = null;
  function kbDone(id, result) {            // KB.js: the keyboard closed (result null = cancelled)
    kbm = null;
    if (result === null) up("KB " + id + " CANCEL");
    else up("KB " + id + " OK " + btoa(result));
    h.clear(); allDirty(); render();
  }
  function drawVt(i) {
    var z = vt.z, y = TOP + i * z[2];
    h.clearRect(0, y, 479, y + z[2] - 1);
    h.setColor(3); h.setFont(z[0]); h.setFontAlign(-1, -1);
    if (vt.t[i]) dtext(h, vt.t[i], vt.x, y + z[6]);
    if (i === cur.r) h.fillRect(vt.x + cur.c * z[1], y + z[2] - 2, vt.x + (cur.c + 1) * z[1] - 2, y + z[2] - 1);
  }
  function render() {
    rt = null;
    if (kbm) return;                        // keyboard owns the screen
    if (hdrDirty) { drawHdr(); hdrDirty = false; }
    if (vt.on) { for (var j = 0; j < vt.z[4]; j++) if (vt.d[j]) { drawVt(j); vt.d[j] = false; } }
    else if (mp.on) { if (mp.dirty && mapm) mapm.draw(); }
    else for (var i = 0; i < ROWS; i++) if (dirty[i]) { drawRow(i); dirty[i] = false; }
    if (ftrDirty) { drawFtr(); ftrDirty = false; }
    h.flip();
  }
  function schedule() { if (!rt) rt = setTimeout(render, 15); }
  function allDirty() {
    for (var i = 0; i < ROWS; i++) dirty[i] = true;
    hdrDirty = ftrDirty = true; mp.dirty = true; mp.last = null;
    for (i = 0; i < vt.d.length; i++) vt.d[i] = true;
  }

  // ---------- scroll mode ----------
  function scrollUp() {
    rows.shift(); rows.push({ t: "", a: 2 }); cr = ROWS - 1;
    for (var i = 0; i < ROWS; i++) dirty[i] = true;
  }
  function newline() { cc = 0; if (++cr >= ROWS) scrollUp(); }
  function pr(s) {
    var i = 0, n = s.length;
    while (i < n) {
      var ch = s[i];
      if (ch === "\n") { newline(); i++; }
      else if (ch === "\r") { cc = 0; i++; }
      else if (ch === "\b") { if (cc > 0) cc--; i++; }
      else if (ch === "\f") { cls(); i++; }
      else if (ch < " ") i++;
      else {
        // write a whole run of printable text into the row in one go
        var j = i;
        while (j < n && s[j] >= " " && j - i < COLS - cc) j++;
        var seg = s.substring(i, j), r = rows[cr], t = r.t;
        while (t.length < cc) t += " ";
        r.t = t.substr(0, cc) + seg + t.substr(cc + seg.length);
        dirty[cr] = true;
        cc += seg.length; i = j;
        if (cc >= COLS) newline();
      }
    }
  }
  function span(row, n, o) {
    if (row < 0 || n < 1 || row + n > ROWS) return;
    rows[row] = o; dirty[row] = true;
    for (var j = 1; j < n; j++) rows[row + j] = { t: "", a: 2, cov: true };
    schedule();
  }
  function cls() {
    for (var i = 0; i < ROWS; i++) { rows[i] = { t: "", a: 2 }; dirty[i] = true; }
    cr = cc = 0;
  }

  // ---------- input ----------
  // a map redraw blocks for ~1 s; knob turns made meanwhile arrive in a burst right after it and
  // would overshoot (e.g. MAP -> RADIATION), so drop anything that shows up within 150 ms of a draw.
  // Not while panning (flag 2): those turns are meant, and the unit sends only the newest view
  function stale() { return mp.on && !(mp.fl & 2) && getTime() - mp.drawnAt < 0.15; }
  function onK1(d) {
    if (kbm) { kbm.knob(1, d); return; }
    if (stale()) return;
    up("K1 " + (d > 0 ? "+1" : d < 0 ? "-1" : "C"));
  }
  function onK2(d) {
    if (kbm) { kbm.knob(2, d); return; }
    if (stale()) return;
    up("K2 " + (d > 0 ? "+1" : d < 0 ? "-1" : "C"));
  }

  // ---------- the STATS / ITEMS / DATA buttons and the side switch ----------
  // Normally they swap the Pip-Boy's menus (and so close PIP LINK). With ctl(1) from the unit PIP LINK
  // has them instead: a press reports <<T BTN 0|1|2>>, the switch <<T SW 0-4>>, and holding STATS goes
  // back to the Pip-Boy's own menus. remove() hands them back. (Holding ITEMS is still the torch, and
  // STATS + DATA held together still restarts the Pip-Boy - the firmware's own long presses.)
  var ctlSaved = null;
  function onBtn(m) { up("BTN " + m); }
  function onSw(x) { Pip.MENUX = x; up("SW " + x); }     // MENUX: or the firmware keeps re-reporting it
  function onLong(b) { if (b === "STATS" && !(typeof BTN_DATA != "undefined" && BTN_DATA.read())) leave(); }
  function ctlOn() {
    if (ctlSaved) return;
    ctlSaved = { mode: Pip["#onmode"], menuX: Pip["#onmenuX"] };
    Pip.removeAllListeners("mode"); Pip.removeAllListeners("menuX");
    Pip.on("mode", onBtn); Pip.on("menuX", onSw); Pip.on("longPress", onLong);
    up("SW " + Pip.MENUX);
  }
  function ctlOff() {
    if (!ctlSaved) return;
    Pip.removeAllListeners("mode"); Pip.removeAllListeners("menuX"); Pip.removeListener("longPress", onLong);
    [["mode", ctlSaved.mode], ["menuX", ctlSaved.menuX]].forEach(function (p) {
      [].concat(p[1] || []).forEach(function (f) { if (typeof f == "function") Pip.on(p[0], f); });
    });
    ctlSaved = null;
  }
  // the lights in the three buttons: "0" off, "1" on, "2" blinking (something waiting behind that button)
  var ledS = "000", ledOn = true, ledT = 0;
  function ledApply() {
    try {
      var b = Pip.settings.brightness;
      Pip.fadeTo([LED_STATS, LED_ITEMS, LED_DATA].map(function (p, i) {
        var c = ledS[i];
        return { pin: p, target: c === "1" || (c === "2" && ledOn) ? b : 0 };
      }));
    } catch (e) {}
  }
  function leds(st) {
    ledS = String(st);
    if (ledT) { clearInterval(ledT); ledT = 0; }
    ledOn = true;
    ledApply();
    if (ledS.indexOf("2") >= 0) ledT = setInterval(function () { ledOn = !ledOn; ledApply(); }, 600);
  }
  function leave() {                        // hold STATS: the firmware's STATS button (it closes PIP LINK)
    ctlOff();
    Pip.emit("mode", 0, true);
  }

  // ---------- host API (global T) ----------
  var api = {
    id: "TERM",
    fullscreen: true,                       // firmware skips its header/clock/battery overlay for fullscreen apps
    hello: function () {
      up("HI " + COLS + " " + ROWS + " " + VER);
      var c = rd("HOLO/TERM/CONFIG.TXT");      // written by the PC setup app (time zone, ...)
      if (c) up("CFG " + btoa(c));
    },
    cls: function () { cls(); schedule(); },
    // ln(row, b64, attr, icon): icon = marker type drawn in the row's first two columns
    ln: function (row, b64, attr, ic) {
      if (row < 0 || row >= ROWS) return;
      var o = rows[row];
      if (o.n > 1) for (var j = 1; j < o.n && row + j < ROWS; j++) { rows[row + j] = { t: "", a: 2 }; dirty[row + j] = true; }
      rows[row] = { t: dec(b64).substr(0, COLS), a: attr || 2, ic: ic === undefined ? -1 : ic };
      dirty[row] = true; schedule();
    },
    // big(row, n, b64text, attr): large centred text over rows row..row+n-1
    big: function (row, n, b64, attr) { span(row, n, { t: dec(b64), a: attr || 3, k: 1, n: n }); },
    // bars(row, n, b64bytes, mark, attr): bar graph, one byte per bar (1..255 = 0..254); mark = dashed level line
    bars: function (row, n, b64, mark, attr) { span(row, n, { t: "", a: attr || 3, k: 2, n: n, d: dec(b64), m: mark || 0 }); },
    // map(on): switch the grid area to the map viewer; replies <<T MAP zmin zmax lat lon>>
    map: function (on) {
      mp.on = !!on; mp.last = null; mp.under = null;
      mapm = mp.on ? eval(rd("HOLO/TERM/MAP.js")) : null;
      if (mp.on) {
        mp.info = mapm.info(); mp.icons = null; mp.dirty = true;
        up("MAP " + mp.info.zmin + " " + mp.info.zmax + " " + mp.info.lat + " " + mp.info.lon);
      } else { h.clearRect(0, TOP, 479, BOT); allDirty(); mp.dirty = false; }
      schedule();
    },
    // mv(lat, lon, zoom, b64 "lat,lon,type,heading,label;...", flags): type -1 = the player arrow,
    // -3 = NAV line from the player to there; flags 1 = centre crosshair, 2 = panning (scroll, don't redraw)
    mv: function (lat, lon, z, b64, fl) { mp.lat = lat; mp.lon = lon; mp.z = z; mp.dyn = dec(b64); mp.fl = fl | 0; mp.dirty = true; schedule(); },
    // theme(0 green | 1 amber | -1 the Pip-Boy's own colour, as picked in its settings): PIP LINK's colour;
    // the Pip-Boy's own palette is put back on exit
    theme: function (n) {
      try {
        if (n === 0 || n === 1) { setTheme(n); themed = true; }
        else restoreTheme();                // -1: the colour picked in the Pip-Boy's own settings
      } catch (e) {}
    },
    // wload(): send PIPLINK/WPT.TXT up as <<T WPF b64>> chunks, then <<T WPF END bytes>>
    wload: function () {
      wlStop();
      var f = null, n = 0;
      try { f = E.openFile(WPTFILE, "r"); } catch (e) {}
      if (!f) { up("WPF END 0"); return; }
      wl = { f: f, t: 0 };
      (function step() {
        for (var k = 0; k < 6 && wl; k++) {
          var d = wl.f.read(150);
          if (d === undefined || !d.length) { wlStop(); up("WPF END " + n); return; }
          n += d.length; up("WPF " + btoa(d));
        }
        if (wl) wl.t = setTimeout(step, 40);
      })();
    },
    // wadd(b64): append line(s) to PIPLINK/WPT.TXT; replies <<T WAD 1|0>>
    wadd: function (b64) {
      var fs = require("fs"), ok = 0;
      try { fs.mkdirSync("PIPLINK"); } catch (e) {}
      try { ok = fs.appendFileSync(WPTFILE, dec(b64)) === false ? 0 : 1; } catch (e) {}
      up("WAD " + ok);
    },
    pr: function (b64) { pr(dec(b64)); schedule(); },
    // cur(row, col): text cursor underline; row -1 hides it
    cur: function (r, c) {
      var d = vt.on ? vt.d : dirty;
      if (cur.r >= 0 && cur.r < d.length) d[cur.r] = true;
      cur.r = r; cur.c = c;
      if (r >= 0 && r < d.length) d[r] = true;
      schedule();
    },
    // vt(size, margin): TERM canvas on (0 large 58x12, 1 medium 66x16, 2 small 78x24, 3 BBS 80x24) or off (-1),
    // margin = the left edge in pixels; replies <<T VT cols rows>>
    vt: function (n, m) {
      var z = VT_SIZES[n];
      h.clearRect(0, TOP, 479, BOT);
      cur.r = -1;
      if (!z) { vt.on = false; vt.t = []; vt.d = []; allDirty(); schedule(); return; }
      vt.on = true; vt.z = z; vt.t = []; vt.d = []; vt.x = m >= 0 ? m : z[5];
      for (var i = 0; i < z[4]; i++) { vt.t.push(""); vt.d.push(true); }
      up("VT " + z[3] + " " + z[4]);
      schedule();
    },
    // vl(row, b64): one line of the TERM canvas
    vl: function (r, b64) { if (vt.on && r >= 0 && r < vt.t.length) { vt.t[r] = dec(b64); vt.d[r] = true; schedule(); } },
    hdr: function (b64) { hdrText = dec(b64); hdrDirty = true; schedule(); },
    ftr: function (b64) { ftrText = dec(b64); ftrDirty = true; schedule(); },
    // kb(id, title, text, s1, s2): s1 / s2 = wheel clicks per step for knob1 / knob2
    kb: function (id, tB64, iB64, s1, s2) {
      if (kbm) kbm.stop();
      kbm = eval(rd("HOLO/TERM/KB.js"));
      kbm.open(String(id), dec(tB64), dec(iB64), s1, s2);
    },
    kn: function (k, d) { (k == 2 ? onK2 : onK1)(d); },   // a knob turn / click, for testing from a PC
    // ctl(1): PIP LINK takes the buttons and the side switch (see above); ctl(0) gives them back
    ctl: function (on) { if (on) ctlOn(); else ctlOff(); },
    // led(0 STATS | 1 ITEMS | 2 DATA | -1 none): the light in the buttons under the screen
    led: function (n) { leds(n === 0 ? "100" : n === 1 ? "010" : n === 2 ? "001" : "000"); },
    // leds("012"): STATS, ITEMS, DATA - 0 off, 1 on, 2 blinking
    leds: leds,
    snd: function (name) { try { Pip.playSound(name || "SELECT"); } catch (e) {} },
    play: function (b64path) { try { Pip.audioStart(dec(b64path)); } catch (e) {} },   // WAV: 16-bit mono 16 kHz
    ack: function (n) { up("ACK " + n); },
    pad: pad,
    dump: function () {                     // debug: report exactly what is on screen
      up("DUMP HDR " + btoa(hdrText));
      up("DUMP FTR " + btoa(ftrText));
      for (var i = 0; i < ROWS; i++) if (rows[i].t) up("DUMP ROW " + i + " " + rows[i].a + " " + btoa(rows[i].t));
      up("DUMP END");
    },
    remove: function () {
      for (var i = 0; i < timers.length; i++) clearInterval(timers[i]);
      if (rt) clearTimeout(rt);
      if (kbm) kbm.stop();
      ctlOff();
      if (ledT) clearInterval(ledT);
      try { if (flipT && Pip.timers.flip === flipT) changeInterval(flipT, 50); } catch (e) {}
      if (Pip.blitOptions) { if (scanWas) Pip.blitOptions.noScanEffect = true; else delete Pip.blitOptions.noScanEffect; }
      wlStop();
      restoreTheme();
      Pip.removeAllListeners("knob1"); Pip.removeAllListeners("knob2");
      try { delete global.T; } catch (e) {}
      up("BYE");
      echo(1);
    }
  };

  // ---------- start ----------
  Pip.removeAllListeners("knob1"); Pip.removeAllListeners("knob2");
  Pip.on("knob1", onK1); Pip.on("knob2", onK2);
  timers.push(setInterval(function () { if (kbm) kbm.blink(); }, 500));
  // the firmware re-sends the whole screen 20 times a second (its CRT look) - 34 ms each time, so it took
  // most of the CPU and everything here waited on it (a chain of short jobs ran 13x slower). PIP LINK
  // flips after each change itself, so slow that timer down while it runs, keeping the 20/s pace only
  // while a screen-glitch animation (a tap on the case) plays. The moving scan line is timed by the clock
  // and jumped about at one flip a second, so it's off while PIP LINK is open. remove() puts both back.
  var flipT = 0, scanWas = Pip.blitOptions && Pip.blitOptions.noScanEffect;
  if (Pip.blitOptions) Pip.blitOptions.noScanEffect = true;
  function slowFlip() {
    if (!Pip.timers || !Pip.timers.flip || Pip.timers.flip === flipT) return;
    try { changeInterval(Pip.timers.flip, 1000); flipT = Pip.timers.flip; } catch (e) {}
  }
  slowFlip();
  timers.push(setInterval(function () {
    if (Pip.blitOptions && Pip.blitOptions.anim.length) h.flip();
    slowFlip();                             // started again after the Pip-Boy slept
  }, 50));
  global.T = api;
  echo(0);                                  // host commands shouldn't echo back
  h.clear(); render();
  api.hello();
  return api;
})
