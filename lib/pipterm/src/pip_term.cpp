#include "pip_term.h"

// row-cache keys for rows owned by big()/bars(): no printable text can match these
#define SPAN_BIG "\x02" "B"
#define SPAN_BARS "\x02" "G"
#define SPAN_COVERED "\x02"

void PipTerm::begin() {
  link.onReady([this]() {
    reset();                       // a fresh terminal has a blank screen; forget our cache
    if (readyH) readyH();
  });
  link.onEvent([this](const String& ev) { handleEvent(ev); });
}

void PipTerm::reset() {
  rowText.assign(rows() > 0 ? rows() : 0, String());
  rowAttr.assign(rowText.size(), (int)NORMAL);   // (int): by value - no out-of-line NORMAL
  rowIcon.assign(rowText.size(), -1);
  curRow = -2;
  vtSz = -1;
  vtText.clear();
  ledSel = -2;
  ledStates = "";
  hdrText = "\x01";                // impossible values so the first header/footer always sends
  ftrText = "\x01";
  kbActive = false;
  mapActive = false;
  mapKey = "";
  link.send("global.T&&T.map&&T.map(0)");     // a terminal we reconnect to may still be showing the map
  link.send("global.T&&T.vt&&T.vt(-1)");      // ... or the TERM canvas
}

String PipTerm::clean(const String& s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    o += ((c >= 32 && c < 127) || c == '\xB0') ? c : '?';   // 0xB0 = degree sign (the terminal draws it)
  }
  return o;
}

void PipTerm::cls() {
  if (!ready()) return;
  for (size_t i = 0; i < rowText.size(); i++) { rowText[i] = ""; rowAttr[i] = NORMAL; rowIcon[i] = -1; }
  link.send("global.T&&T.cls()");
}

void PipTerm::line(int row, const String& text, int attr, int icon) {
  if (!ready() || row < 0 || row >= (int)rowText.size()) return;
  if (rowText[row].startsWith(SPAN_BIG) || rowText[row].startsWith(SPAN_BARS)) {
    for (size_t j = row + 1; j < rowText.size() && rowText[j] == SPAN_COVERED; j++) rowText[j] = "";   // the terminal blanks them
  }
  String t = clean(text);
  if ((int)t.length() > cols()) t = t.substring(0, cols());
  if (t == rowText[row] && attr == rowAttr[row] && icon == rowIcon[row]) return;     // unchanged: skip the redraw
  rowText[row] = t;
  rowAttr[row] = attr;
  rowIcon[row] = icon;
  link.send("global.T&&T.ln(" + String(row) + ",\"" + PipLink::b64enc(t) + "\"," + String(attr) +
            (icon >= 0 ? "," + String(icon) : String("")) + ")");
}

// big()/bars() own several rows: mark them in the row cache with a key nothing else produces,
// so the next line() on any of those rows always resends.
bool PipTerm::claimSpan(int row, int n, const String& key) {
  if (!ready() || row < 0 || n < 1 || row + n > (int)rowText.size()) return false;
  if (rowText[row] == key) return false;            // unchanged
  rowText[row] = key;
  for (int j = 1; j < n; j++) rowText[row + j] = SPAN_COVERED;
  return true;
}

void PipTerm::big(int row, int n, const String& text, int attr) {
  String t = clean(text);
  if (!claimSpan(row, n, SPAN_BIG + String(n) + "/" + String(attr) + "/" + t)) return;
  link.send("global.T&&T.big&&T.big(" + String(row) + "," + String(n) + ",\"" + PipLink::b64enc(t) + "\"," + String(attr) + ")");
}

void PipTerm::bars(int row, int n, const std::vector<uint8_t>& values, uint8_t mark, int attr) {
  String raw;
  raw.reserve(values.size());
  for (uint8_t v : values) raw += (char)(1 + (v > 254 ? 254 : v));   // 1..255 on the wire: no NUL bytes
  String b64 = PipLink::b64enc(raw);
  if (!claimSpan(row, n, SPAN_BARS + String(n) + "/" + String(mark) + "/" + String(attr) + "/" + b64)) return;
  link.send("global.T&&T.bars&&T.bars(" + String(row) + "," + String(n) + ",\"" + b64 + "\"," + String(mark) + "," + String(attr) + ")");
}

void PipTerm::print(const String& text) {
  if (!ready()) return;
  // scroll mode rewrites rows on the device; drop our cache so line() resends next time
  for (size_t i = 0; i < rowText.size(); i++) rowText[i] = "\x01";
  String t;
  for (size_t i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '\n' || c == '\r' || c == '\b' || c == '\f' || (c >= 32 && c < 127)) t += c;
  }
  // keep each command comfortably inside the flow-control window
  for (size_t i = 0; i < t.length(); i += 600)
    link.send("global.T&&T.pr(\"" + PipLink::b64enc(t.substring(i, i + 600)) + "\")");
}

void PipTerm::header(const String& text) {
  if (!ready()) return;
  String t = clean(text);
  if (t == hdrText) return;
  hdrText = t;
  link.send("global.T&&T.hdr(\"" + PipLink::b64enc(t) + "\")");
}

void PipTerm::footer(const String& text) {
  if (!ready()) return;
  String t = clean(text);
  if (t == ftrText) return;
  ftrText = t;
  link.send("global.T&&T.ftr(\"" + PipLink::b64enc(t) + "\")");
}

void PipTerm::keyboard(const String& id, const String& title, const String& init) {
  if (!ready()) return;
  kbActive = true;
  link.send("global.T&&T.kb(\"" + clean(id) + "\",\"" + PipLink::b64enc(clean(title)) + "\",\"" + PipLink::b64enc(clean(init)) + "\"," +
            String(kbStep1) + "," + String(kbStep2) + ")");
}

void PipTerm::controls(bool on) {
  if (ready()) link.send(String("global.T&&T.ctl&&T.ctl(") + (on ? "1" : "0") + ")");
}

void PipTerm::led(int which) {
  if (!ready() || which == ledSel) return;
  ledSel = which;
  link.send("global.T&&T.led&&T.led(" + String(which) + ")");
}

void PipTerm::leds(const String& states) {
  if (!ready() || states == ledStates) return;
  ledStates = states;
  link.send("global.T&&T.leds&&T.leds(\"" + states + "\")");
}

void PipTerm::sound(const char* name) {
  if (!ready()) return;
  link.send(String("global.T&&T.snd(\"") + name + "\")");
}

void PipTerm::play(const String& wavPath) {
  if (!ready()) return;
  link.send("global.T&&T.play&&T.play(\"" + PipLink::b64enc(wavPath) + "\")");
}

void PipTerm::mapMode(bool on) {
  if (!ready() || on == mapActive) return;
  mapActive = on;
  mapKey = "";
  if (!on)                                        // the terminal blanks the grid; resend every row
    for (size_t i = 0; i < rowText.size(); i++) rowText[i] = "\x01";
  link.send(String("global.T&&T.map&&T.map(") + (on ? "1" : "0") + ")");
}

void PipTerm::mapView(double lat, double lon, int zoom, const String& dyn, int flags) {
  if (!ready() || !mapActive) return;
  String args = String(lat, 6) + "," + String(lon, 6) + "," + String(zoom) + ",\"" + PipLink::b64enc(clean(dyn)) + "\"" +
                (flags ? "," + String(flags) : String(""));
  if (args == mapKey) return;                     // a redraw costs ~0.5 s on the Pip-Boy: only when needed
  mapKey = args;
  link.send("global.T&&T.mv&&T.mv(" + args + ")");
}

void PipTerm::theme(int which) {
  if (ready()) link.send("global.T&&T.theme&&T.theme(" + String(which) + ")");
}

void PipTerm::cursor(int row, int col) {
  if (!ready()) return;
  if (row < 0) { row = -1; col = 0; }
  if (row == curRow && col == curCol) return;
  curRow = row; curCol = col;
  link.send("global.T&&T.cur&&T.cur(" + String(row) + "," + String(col) + ")");
}

void PipTerm::vtMode(int size, int margin) {
  if (!ready() || (size == vtSz && (size < 0 || margin == vtMargin))) return;
  vtSz = size;
  vtMargin = margin;
  vtText.assign(size == 0 ? 12 : size == 1 ? 16 : size >= 2 ? 24 : 0, String());
  curRow = -2;                                    // the canvas starts without a cursor
  if (size < 0)                                   // the terminal redraws its grid: resend every row
    for (size_t i = 0; i < rowText.size(); i++) rowText[i] = "\x01";
  link.send("global.T&&T.vt&&T.vt(" + String(size) + (size >= 0 && margin >= 0 ? "," + String(margin) : String("")) + ")");
}

void PipTerm::vtLine(int row, const String& text) {
  if (!ready() || row < 0 || row >= (int)vtText.size()) return;
  String t = clean(text);
  if (t == vtText[row]) return;
  vtText[row] = t;
  link.send("global.T&&T.vl&&T.vl(" + String(row) + ",\"" + PipLink::b64enc(t) + "\")");
}

void PipTerm::loadPlaces() {
  if (ready()) link.send("global.T&&T.wload&&T.wload()");
}

void PipTerm::appendPlaces(const String& text) {
  if (!ready()) return;
  for (size_t i = 0; i < text.length(); i += 600)        // stay inside the flow-control window
    link.send("global.T&&T.wadd&&T.wadd(\"" + PipLink::b64enc(text.substring(i, i + 600)) + "\")");
}

void PipTerm::dump() {
  if (ready()) link.send("global.T&&T.dump()");
}

void PipTerm::handleEvent(const String& ev) {
  // "K1 +1" / "K1 -1" / "K1 C"   and   "KB <id> OK <b64>" / "KB <id> CANCEL"
  if ((ev.startsWith("K1 ") || ev.startsWith("K2 ")) && knobH) {
    int knob = ev[1] - '0';
    String d = ev.substring(3);
    knobH(knob, d == "+1" ? 1 : d == "-1" ? -1 : 0);
    return;
  }
  if (ev.startsWith("BTN ")) { if (buttonH) buttonH(ev.substring(4).toInt()); return; }
  if (ev.startsWith("SW ")) { if (switchH) switchH(ev.substring(3).toInt()); return; }
  if (ev.startsWith("CFG ")) {
    if (cfgH) cfgH(PipLink::b64dec(ev.substring(4)));
    return;
  }
  if (ev.startsWith("VT ")) {                     // "VT cols rows": the TERM canvas is up
    int c = 0, r = 0;
    sscanf(ev.c_str() + 3, "%d %d", &c, &r);
    if (vtSizeH) vtSizeH(c, r);
    return;
  }
  if (ev.startsWith("MAP ")) {                    // "MAP zmin zmax lat lon"
    int zmin = 0, zmax = -1;
    double lat = 0, lon = 0;
    sscanf(ev.c_str() + 4, "%d %d %lf %lf", &zmin, &zmax, &lat, &lon);
    if (mapInfoH) mapInfoH(zmin, zmax, lat, lon);
    return;
  }
  if (ev.startsWith("KB ")) {
    kbActive = false;
    int a = ev.indexOf(' ', 3);
    String id = ev.substring(3, a);
    String rest = ev.substring(a + 1);
    bool ok = rest.startsWith("OK");
    String text = ok && rest.length() > 3 ? PipLink::b64dec(rest.substring(3)) : String();
    // the terminal redraws its whole grid after the keyboard closes; our cache is still right
    if (kbH) kbH(id, ok, text);
    return;
  }
  if (otherH) otherH(ev);
}
