// PIP LINK holotape: the map viewer. APP.js evals this inside its own function when the map opens (so it
// sees APP.js's variables: h, mp, MAPDIR, rd, icons, TOP, BOT, MID ...) and drops it when the map closes -
// its code only takes up the Pip-Boy's memory while the map is on screen.
(function () {
  var hs = require("heatshrink");
  var PAL4 = new Uint16Array([0, 1, 2, 3]);    // without it drawImage squashes 2bpp levels to 0,0,1,1
  function mapInfo() {
    var info = { zmin: 0, zmax: -1, bucket: 13, lat: 0, lon: 0, bs: 0 }, s = rd(MAPDIR + "INFO.TXT");
    if (s) s.split("\n").forEach(function (l) {
      var p = l.split(" ");
      if (p[0] == "zoom") { info.zmin = +p[1]; info.zmax = +p[2]; }
      else if (p[0] == "center") { info.lat = +p[1]; info.lon = +p[2]; }
      else if (p[0] == "bucket") info.bucket = +p[1];
      else if (p[0] == "bundle") info.bs = +p[1];
    });
    return info;
  }
  // where a tile is: { f: the open file, o: offset, n: length (0 = an empty tile) }, or undefined when the map
  // doesn't have it - the caller closes t.f. PBMAP 2 keeps 8x8 tiles per file (B/z/bx/by.PBB: 64 x u32 offset +
  // u32 length, then the tiles); PBMAP 1 is one file per tile.
  function tileAt(z, x, y) {
    var B = mp.info.bs, f, d, o;
    try { f = E.openFile(B ? MAPDIR + "B/" + z + "/" + Math.floor(x / B) + "/" + Math.floor(y / B) + ".PBB" : MAPDIR + "T/" + z + "/" + x + "/" + y + ".PBT", "r"); } catch (e) { return undefined; }
    if (!B) return { f: f, o: 0, n: 65536 };
    f.seek(((y % B) * B + x % B) * 8);
    d = f.read(8);
    o = d && d.length == 8 ? d.charCodeAt(0) | d.charCodeAt(1) << 8 | d.charCodeAt(2) << 16 | d.charCodeAt(3) << 24 : 0;
    if (!o) { f.close(); return undefined; }
    return { f: f, o: o, n: d.charCodeAt(4) | d.charCodeAt(5) << 8 | d.charCodeAt(6) << 16 };
  }
  function wpx(lat, lon, n) {                  // web-mercator world pixels, n = 256 * 2^zoom
    var s = Math.sin(lat * Math.PI / 180);
    return [(lon + 180) / 360 * n, (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n];
  }
  // draw a tile (from tileAt) and close it: 4 strips of 64 rows, each u16 len + heatshrink'd 2bpp, read one
  // strip at a time - a whole tile can be 15 KB and the Pip-Boy has ~35 KB free. sc: enlarged (a tile from a
  // lower zoom standing in for a missing one); only strips reaching rows y0..y1 are read
  function drawTile(t, sx, sy, sc, y0, y1) {
    var f = t.f, p = t.o, end = t.o + t.n, H = 64 * sc, k, d, n, y;
    for (k = 0; k < 4 && p + 2 <= end; k++) {
      f.seek(p);
      d = f.read(2);
      if (!d || d.length < 2) break;
      n = d.charCodeAt(0) | d.charCodeAt(1) << 8;
      y = sy + k * H;
      if (n && y + H > y0 && y <= y1) {
        d = hs.decompress(f.read(n));
        if (sc > 1) h.drawImage({ width: 256, height: 64, bpp: 2, palette: PAL4, buffer: d }, sx, y, { scale: sc });
        else h.drawImage({ width: 256, height: 64, bpp: 2, palette: PAL4, buffer: d }, sx, y);
        d = undefined;
      }
      p += 2 + n;
    }
    f.close();
  }
  function icon(t, x, y, col) {
    var img = { width: 16, height: 16, bpp: 1, transparent: 0, buffer: E.toArrayBuffer(mp.icons.substr(t * 32, 32)) };
    h.setColor(0);                             // dark outline so it reads on top of roads
    h.drawImage(img, x - 9, y - 8); h.drawImage(img, x - 7, y - 8); h.drawImage(img, x - 8, y - 9); h.drawImage(img, x - 8, y - 7);
    h.setColor(col); h.drawImage(img, x - 8, y - 8);
  }
  function mark(x, y, t, label, col) {
    if (x < -60 || x > 540 || y < TOP - 20 || y > BOT + 20) return;
    x = Math.round(x); y = Math.round(y);
    if (icons() && t >= 0 && t * 32 < mp.icons.length) icon(t, x, y, col);
    else { h.setColor(col); h.fillRect(x - 3, y - 3, x + 3, y + 3); }
    if (!label) return;
    h.setFont("Monofonto14"); h.setFontAlign(-1, 0);
    var w = h.stringWidth(label);
    h.setColor(0); h.fillRect(x + 10, y - 8, x + 12 + w, y + 7);
    h.setColor(col); h.drawString(label, x + 11, y);
  }
  function player(x, y, hd) {                  // Fallout-style arrow, heading in degrees (-1 = unknown)
    var a = (hd >= 0 ? hd : 0) * Math.PI / 180, s = Math.sin(a), c = Math.cos(a);
    function poly(k) {
      var pts = [0, -13, 9, 10, 0, 5, -9, 10], o = [];
      for (var i = 0; i < 8; i += 2) { var px = pts[i] * k, py = pts[i + 1] * k; o.push(x + px * c - py * s, y + px * s + py * c); }
      return o;
    }
    h.setColor(0); h.fillPoly(poly(1.4));
    h.setColor(3); h.fillPoly(poly(1));
    if (hd < 0) { h.setColor(0); h.fillCircle(x, y + 1, 2); }
  }
  // the crosshair's 25x25 box as whole bytes of h's 2-bit buffer (120 bytes a row, x 228..255 = bytes
  // 57..63): kept so a pan can take the crosshair off before scrolling the picture
  function unCross() {
    var u = mp.under, b = new Uint8Array(h.buffer), j;
    mp.under = null;
    if (u) for (j = 0; j < 25; j++) b.set(new Uint8Array(u.buffer, j * 7, 7), (MID - 12 + j) * 120 + 57);
  }
  function cross() {                           // view-centre crosshair while the map isn't following you
    var x = 240, y = MID, u = mp.under = new Uint8Array(175), j;
    for (j = 0; j < 25; j++) u.set(new Uint8Array(h.buffer, (y - 12 + j) * 120 + 57, 7), j * 7);
    h.setColor(0);
    h.fillRect(x - 12, y - 1, x - 3, y + 1); h.fillRect(x + 3, y - 1, x + 12, y + 1);
    h.fillRect(x - 1, y - 12, x + 1, y - 3); h.fillRect(x - 1, y + 3, x + 1, y + 12);
    h.setColor(3);
    h.drawLine(x - 11, y, x - 4, y); h.drawLine(x + 4, y, x + 11, y);
    h.drawLine(x, y - 11, x, y - 4); h.drawLine(x, y + 4, x, y + 11);
  }
  // NAV: dotted line from you to the target, clipped to the map area; returns the visible part
  function navLine(x0, y0, x1, y1) {
    var dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1, p = [-dx, dx, -dy, dy], q = [x0, 479 - x0, y0 - TOP, BOT - y0];
    for (var i = 0; i < 4; i++) {
      if (p[i] == 0) { if (q[i] < 0) return null; continue; }
      var r = q[i] / p[i];
      if (p[i] < 0) { if (r > t1) return null; if (r > t0) t0 = r; }
      else { if (r < t0) return null; if (r < t1) t1 = r; }
    }
    var ax = x0 + t0 * dx, ay = y0 + t0 * dy, bx = x0 + t1 * dx, by = y0 + t1 * dy;
    var L = Math.sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    if (L < 2) return null;
    var ux = (bx - ax) / L, uy = (by - ay) / L, s, e;
    h.setColor(0);                             // dark edge so the dots read on top of roads
    for (s = 0; s < L; s += 12) {
      e = Math.min(s + 7, L);
      h.fillPoly([ax + ux * s - uy * 2.5, ay + uy * s + ux * 2.5, ax + ux * e - uy * 2.5, ay + uy * e + ux * 2.5,
                  ax + ux * e + uy * 2.5, ay + uy * e - ux * 2.5, ax + ux * s + uy * 2.5, ay + uy * s - ux * 2.5]);
    }
    h.setColor(3);
    for (s = 0; s < L; s += 12) {
      e = Math.min(s + 7, L);
      h.fillPoly([ax + ux * s - uy, ay + uy * s + ux, ax + ux * e - uy, ay + uy * e + ux,
                  ax + ux * e + uy, ay + uy * e - ux, ax + ux * s + uy, ay + uy * s - ux]);
    }
    return [ax, ay, bx, by];
  }
  // distance + azimuth written along the line (kept upright), on a dark plate
  function navLabel(s, seg) {
    if (!s || !seg) return;
    var a = Math.atan2(seg[3] - seg[1], seg[2] - seg[0]);
    if (a > Math.PI / 2) a -= Math.PI; else if (a < -Math.PI / 2) a += Math.PI;
    var F = "Monofonto23", p = s.split("\xB0").join(" ");
    h.setFont(F);
    var W = h.stringWidth(p) + 12, H = h.getFontHeight() + 2, t = Graphics.createArrayBuffer(W, H, 1, { msb: true });
    t.setFont(F); t.setColor(1); t.setFontAlign(-1, -1);
    dtext(t, s, 6, 2);
    var mx = (seg[0] + seg[2]) / 2, my = (seg[1] + seg[3]) / 2, c = Math.cos(a), n = Math.sin(a);
    mx += n * (H / 2 + 5); my -= c * (H / 2 + 5);          // just above the line
    var hw = W / 2, hh = H / 2, pts = [];
    [[-hw, -hh], [hw, -hh], [hw, hh], [-hw, hh]].forEach(function (k) { pts.push(mx + k[0] * c - k[1] * n, my + k[0] * n + k[1] * c); });
    h.setColor(0); h.fillPoly(pts);
    h.setColor(3); h.drawImage({ width: W, height: H, bpp: 1, transparent: 0, buffer: t.buffer }, mx, my, { rotate: a });
  }
  // the map tiles for the screen area x0..x1, y0..y1 (ox, oy: the screen's top-left in world pixels);
  // returns [tiles drawn, stand-ins from a lower zoom]
  function tiles(z, ox, oy, x0, y0, x1, y1) {
    var got = 0, lo = 0, miss = [], tx, ty, s, i, k, x, y, c0, c1;
    h.setClipRect(x0, y0, x1, y1);
    h.clearRect(x0, y0, x1, y1);
    for (tx = Math.floor((ox + x0) / 256); tx * 256 <= ox + x1; tx++)
      for (ty = Math.floor((oy + y0) / 256); ty * 256 <= oy + y1; ty++) {
        s = tileAt(z, tx, ty);
        if (s === undefined) miss.push(tx, ty);
        else { got++; drawTile(s, tx * 256 - ox, ty * 256 - oy, 1, y0, y1); }
      }
    // a spot this zoom doesn't cover (the map has less detail there): the same tile from 1-2 zooms out,
    // enlarged. Bundled maps only - an older map can't tell a missing tile from an empty one
    if (mp.info.bs) for (i = 0; i < miss.length; i += 2) {
      tx = miss[i]; ty = miss[i + 1];
      for (k = 1; k <= 2 && z - k >= mp.info.zmin; k++) {
        s = tileAt(z - k, tx >> k, ty >> k);
        if (s === undefined) continue;
        x = tx * 256 - ox; y = ty * 256 - oy; c0 = Math.max(y0, y); c1 = Math.min(y1, y + 255);
        h.setClipRect(Math.max(x0, x), c0, Math.min(x1, x + 255), c1);
        drawTile(s, (tx >> k) * (256 << k) - ox, (ty >> k) * (256 << k) - oy, 1 << k, c0, c1);
        lo++;
        break;
      }
    }
    h.setClipRect(0, TOP, 479, BOT);
    return [got, lo];
  }
  // a marker from the last view that the new list dropped but whose old picture would still be on screen
  function gone(od, nd, N, ox, oy) {
    if (!od || od == nd) return false;
    var n = ";" + nd + ";", a = od.split(";"), i, p, w;
    for (i = 0; i < a.length; i++) {
      if (!a[i] || n.indexOf(";" + a[i] + ";") >= 0) continue;
      p = a[i].split(",");
      if (p.length < 5) continue;
      w = wpx(+p[0], +p[1], N);
      if (w[0] - ox > -200 && w[0] - ox < 500 && w[1] - oy > TOP - 30 && w[1] - oy < BOT + 30) return true;
    }
    return false;
  }
  function drawMap() {
    mp.dirty = false;
    var z = mp.z, N = 256 * Math.pow(2, z), c = wpx(mp.lat, mp.lon, N), L = mp.last;
    var ox = Math.round(c[0] - 240), oy = Math.round(c[1] - MID), got = 0, lo = 0, me = null, tx, ty, s, k;
    // from the hub: lat,lon,type,heading,label;...  type -1 = you, -3 = NAV line to there (label = distance)
    var dyn = [], nav = null, seg = null;
    if (mp.dyn) mp.dyn.split(";").forEach(function (m) {
      var p = m.split(",");
      if (p.length < 5) return;
      var w = wpx(+p[0], +p[1], N), o = { x: w[0] - ox, y: w[1] - oy, t: +p[2], hd: +p[3], l: p.slice(4).join(",") };
      if (o.t == -1) me = o; else if (o.t == -3) nav = o; else dyn.push(o);
    });
    // panning (flag 2) a short way: scroll what's on screen and draw only the strip that comes into view -
    // the picture moves in ~0.15 s instead of the ~0.9 s a whole screen takes. Everything on top is
    // geo-anchored and simply drawn again; the NAV label and the NO TILES note aren't, so those views are
    // always drawn whole
    var dx = L ? ox - L.ox : 0, dy = L ? oy - L.oy : 0;
    var inc = L && (mp.fl & 2) && L.z == z && L.fl == mp.fl && !nav && (dx || dy) && Math.abs(dx) < 320 && Math.abs(dy) < 180 &&
              !gone(L.dyn, mp.dyn, N, ox, oy);
    h.setClipRect(0, TOP, 479, BOT);
    if (inc) {
      unCross();
      h.scroll(-dx, -dy);
      if (mp.fl & 1) cross();
      h.flip();                                // the picture moves at once; the strip coming into view follows
      unCross();
      if (dx) tiles(z, ox, oy, dx > 0 ? 480 - dx : 0, TOP, dx > 0 ? 479 : -dx - 1, BOT);
      if (dy) tiles(z, ox, oy, 0, dy > 0 ? BOT + 1 - dy : TOP, 479, dy > 0 ? BOT : TOP - dy - 1);
      got = 1;
    } else {
      mp.under = null;
      k = tiles(z, ox, oy, 0, TOP, 479, BOT);
      got = k[0]; lo = k[1];
    }
    var b = mp.info.bucket;
    if (z >= b - 2) {                          // KMZ pins + landmarks, bucketed by zoom-13 tile
      var bs = 256 * Math.pow(2, z - b);
      for (tx = Math.floor(ox / bs); tx * bs < ox + 480; tx++)
        for (ty = Math.floor((oy + TOP) / bs); ty * bs <= oy + BOT; ty++) {
          var s = rd(MAPDIR + "M/" + tx + "_" + ty + ".TXT");
          if (s) s.split("\n").forEach(function (l) {
            var p = l.split(",");
            if (p.length < 4) return;
            var w = wpx(+p[0], +p[1], N);
            mark(w[0] - ox, w[1] - oy, +p[2], z >= 15 ? p.slice(3).join(",") : "", 3);
          });
        }
    }
    if (nav && me) seg = navLine(me.x, me.y, nav.x, nav.y);
    dyn.forEach(function (o) { mark(o.x, o.y, o.t, o.l, 3); });
    if (seg) navLabel(nav.l, seg);
    if (mp.fl & 1) cross();
    if (me) player(me.x, me.y, me.hd);
    mp.last = got && !nav ? { z: z, ox: ox, oy: oy, fl: mp.fl, dyn: mp.dyn } : null;
    if (!got && !lo && mp.info.bs) {           // nothing near enough: is there anything further out?
      c = wpx(mp.lat, mp.lon, 256);
      for (k = 3; z - k >= mp.info.zmin && !lo; k++) {
        s = tileAt(z - k, Math.floor(c[0] * Math.pow(2, z - k) / 256), Math.floor(c[1] * Math.pow(2, z - k) / 256));
        if (s) { s.f.close(); lo = 1; }
      }
    }
    if (!got) {
      h.setColor(1); h.setFont(FONT); h.setFontAlign(0, 0);
      h.drawString(mp.info.zmax < 0 ? "NO MAP DATA (send a map with PIP LINK Setup)" :
                   lo ? "LESS DETAIL HERE - ZOOM OUT" : "NO MAP TILES HERE", 240, BOT - 20);
    }
    h.setClipRect(0, 0, 479, 319);
    mp.drawnAt = getTime();
  }
  return { info: mapInfo, draw: drawMap };
})()
