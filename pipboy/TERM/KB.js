// PIP LINK holotape: the on-screen keyboard. APP.js evals this inside its own function when the host asks
// for text (so it sees APP.js's variables: h, up, FONT, kbDone ...) and drops it when the keyboard closes -
// its code only takes up the Pip-Boy's memory while it's on screen.
(function () {
  var KROWS = [
    ["1","2","3","4","5","6","7","8","9","0"],
    ["Q","W","E","R","T","Y","U","I","O","P"],
    ["A","S","D","F","G","H","J","K","L","/","@"],
    ["Z","X","C","V","B","N","M",".",",","?",":"],
    ["SP","DEL","-","Aa","SEND","CANC"]
  ];
  // s1 / s2: wheel clicks per key step (the unit's "Scroll wheel" / "Tab wheel" settings); sr, sc: the key
  // shown highlighted; ask: the CANCEL question is up (1 = NO picked, 2 = YES)
  var kb = { open: false, id: "", title: "", buf: "", r: 1, c: 0, blink: true, lower: false, s1: 1, s2: 1,
             sr: -1, sc: -1, ask: 0, t: 0, full: false, inp: false };
  var kacc = [0, 0, 0], klast = [0, 0, 0], kclick = 0;
  function kcap(t) { return t.length == 1 && kb.lower ? t.toLowerCase() : t; }
  function kgeo(r) { var n = KROWS[r].length, w = r === 4 ? 76 : 42; return { n: n, w: w, x0: 240 - n * w / 2, y: 98 + r * 40 }; }
  function kbKey(r, c) {
    var G = kgeo(r), cx = G.x0 + c * G.w + G.w / 2, on = r === kb.r && c === kb.c, t = KROWS[r][c];
    h.clearRect(cx - G.w / 2 + 3, G.y - 16, cx + G.w / 2 - 3, G.y + 16);
    if (on) { h.setColor(1); h.fillRect(cx - G.w / 2 + 3, G.y - 16, cx + G.w / 2 - 3, G.y + 16); }
    h.setColor(on ? 3 : 2); h.setFont(r === 4 ? "Monofonto16" : "Monofonto18"); h.setFontAlign(0, 0);
    h.drawString(t === "SP" ? "SPC" : t === "Aa" ? (kb.lower ? "ABC" : "abc") : kcap(t), cx, G.y);
  }
  function kbInput() {
    h.clearRect(15, 31, 465, 69);
    var shown = kb.buf + (kb.blink ? "_" : " ");
    h.setColor(3); h.setFont("Monofonto23"); h.setFontAlign(-1, 0);
    while (h.stringWidth(shown) > 440 && shown.length > 1) shown = shown.slice(1);
    h.drawString(shown, 22, 50);
  }
  function kbAll() {
    h.clear();
    h.setColor(2); h.setFont(FONT); h.setFontAlign(-1, 0);
    h.drawString(kb.title, 14, 14);
    h.setColor(1); h.drawRect(14, 30, 466, 70);
    kbInput();
    for (var r = 0; r < KROWS.length; r++) for (var c = 0; c < KROWS[r].length; c++) kbKey(r, c);
    h.setColor(1); h.setFont("Monofonto14"); h.setFontAlign(0, 1);
    h.drawString("knob1: row   knob2: col   click: key", 240, 316);
    kb.sr = kb.r; kb.sc = kb.c;
  }
  function kbAsk() {                        // "really cancel?" box over the keys, NO picked to start with
    var x0 = 100, x1 = 380, y0 = 112, y1 = 232;
    h.setColor(0); h.fillRect(x0, y0, x1, y1);
    h.setColor(3); h.drawRect(x0, y0, x1, y1); h.drawRect(x0 + 2, y0 + 2, x1 - 2, y1 - 2);
    h.setFont("Monofonto18"); h.setFontAlign(0, 0);
    h.drawString(kb.buf ? "DISCARD YOUR TEXT?" : "CLOSE THE KEYBOARD?", 240, y0 + 28);
    [["NO", 185], ["YES", 295]].forEach(function (o, i) {
      var on = kb.ask === i + 1;
      h.setColor(on ? 1 : 0); h.fillRect(o[1] - 42, y0 + 52, o[1] + 42, y0 + 84);
      h.setColor(on ? 3 : 2); h.drawString(o[0], o[1], y0 + 68);
    });
    h.setColor(2); h.setFont("Monofonto14");
    h.drawString("turn: choose   click: OK", 240, y1 - 14);
  }
  // knob turns only change state; the drawing happens once the burst of turns is in (a full keyboard
  // redraw used to follow every single wheel click, so the highlight lagged behind and overshot)
  function kbShow(full) {
    if (full) kb.full = true;
    if (!kb.t) kb.t = setTimeout(kbRender, 0);
  }
  function kbRender() {
    kb.t = 0;
    if (!kb.open) return;
    if (kb.full) { kb.full = false; kbAll(); if (kb.ask) kbAsk(); }
    else if (kb.ask) kbAsk();
    else {
      if (kb.sr !== kb.r || kb.sc !== kb.c) {
        var r = kb.sr, c = kb.sc;
        kb.sr = kb.r; kb.sc = kb.c;
        if (r >= 0 && r < KROWS.length && c < KROWS[r].length) kbKey(r, c);
        kbKey(kb.r, kb.c);
      }
      if (kb.inp) kbInput();
    }
    kb.inp = false;
    h.flip();
  }
  function kbOpen(id, title, init, s1, s2) {
    kb.open = true; kb.id = id; kb.title = title; kb.buf = init;
    kb.r = 1; kb.c = 0; kb.blink = true; kb.ask = 0;
    kb.s1 = s1 > 0 ? s1 : 1; kb.s2 = s2 > 0 ? s2 : 1;
    kacc = [0, 0, 0];
    kb.full = true;
    if (kb.t) { clearTimeout(kb.t); kb.t = 0; }
    kbRender();
  }
  function kbClose(result) {
    kb.open = false; kb.ask = 0;
    if (kb.t) { clearTimeout(kb.t); kb.t = 0; }
    kbDone(kb.id, result);
  }
  function kbPress() {
    if (kb.ask) {
      if (kb.ask === 2) { kbClose(null); return; }
      kb.ask = 0; kbShow(true); return;
    }
    var tok = KROWS[kb.r][kb.c];
    if (tok === "SEND") { kbClose(kb.buf); return; }
    if (tok === "CANC") kb.ask = 1;
    else if (tok === "Aa") { kb.lower = !kb.lower; kb.full = true; }
    else {
      if (tok === "SP") kb.buf += " ";
      else if (tok === "DEL") kb.buf = kb.buf.slice(0, -1);
      else kb.buf += kcap(tok);
      kb.blink = true; kb.inp = true;
    }
    kbShow();
  }
  // the same wheel handling as the unit's own screens: n clicks in one direction make one step, a pause
  // or a change of direction starts over, and the scroll wheel ignores the nudge that comes with a press
  function kstep(k, d) {
    var now = getTime(), need = k === 1 ? kb.s1 : kb.s2;
    if (k === 1 && now - kclick < 0.35) return 0;
    if (now - klast[k] > 0.8 || (kacc[k] > 0) !== (d > 0)) kacc[k] = 0;
    klast[k] = now;
    kacc[k] += d > 0 ? 1 : -1;
    if (Math.abs(kacc[k]) < need) return 0;
    kacc[k] = 0;
    return d > 0 ? 1 : -1;
  }
  // moving to another row lands on the key under the same spot (the bottom row's keys are wider)
  function knear(r, c, nr) {
    var G = kgeo(r), N = kgeo(nr), x = G.x0 + c * G.w + G.w / 2;
    return Math.max(0, Math.min(N.n - 1, Math.floor((x - N.x0) / N.w)));
  }
  // rows and columns stop at the edges instead of wrapping round (wrapping is how CANCEL got hit)
  function kbKnob(k, d) {
    if (d === 0) { if (k === 1) { kclick = getTime(); kbPress(); } return; }
    var s = kstep(k, d);
    if (!s) return;
    if (kb.ask) { kb.ask = s > 0 ? 2 : 1; kbShow(); return; }
    if (k === 1) {
      var nr = Math.max(0, Math.min(KROWS.length - 1, kb.r + s));
      if (nr !== kb.r) { kb.c = knear(kb.r, kb.c, nr); kb.r = nr; }
    } else kb.c = Math.max(0, Math.min(KROWS[kb.r].length - 1, kb.c + s));
    kbShow();
  }
  return {
    open: kbOpen,
    knob: kbKnob,
    blink: function () { if (kb.open && !kb.ask) { kb.blink = !kb.blink; kb.inp = true; kbShow(); } },
    stop: function () { kb.open = false; if (kb.t) clearTimeout(kb.t); kb.t = 0; }
  };
})()
