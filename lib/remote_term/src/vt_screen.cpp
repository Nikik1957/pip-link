#include "vt_screen.h"
#include <stdio.h>

void VtScreen::resize(int cols, int rows) {
  nc = cols < 1 ? 1 : cols;
  nr = rows < 1 ? 1 : rows;
  reset();
}

void VtScreen::resizeKeep(int cols, int rows) {
  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols == nc && rows == nr) return;
  if (onAlt) altScreen(false, true);            // full-screen apps redraw for the new size anyway
  int shift = cy >= rows ? cy - rows + 1 : 0;   // keep the cursor's row on screen
  std::vector<std::string> g(rows, std::string(cols, ' '));
  for (int r = 0; r < rows && r + shift < nr; r++) {
    std::string line = grid[r + shift];
    line.resize(cols, ' ');
    g[r] = line;
  }
  for (int r = 0; r < shift; r++) { hist.push_back(grid[r]); if (hist.size() > HISTORY) hist.pop_front(); }
  grid = g;
  nc = cols; nr = rows;
  cy -= shift;
  if (cx > nc - 1) cx = nc - 1;
  wrapPending = false;
  top = 0; bot = nr - 1;
  dirt.assign(nr, true);
}

std::string VtScreen::viewRow(int r, int back) const {
  int H = hist.size();
  if (back > H) back = H;
  int idx = H - back + r;
  std::string s = idx < H ? hist[idx] : (idx - H < nr ? grid[idx - H] : std::string());
  s.resize(nc, ' ');
  return s;
}

void VtScreen::reset() {
  grid.assign(nr, std::string(nc, ' '));
  hist.clear();
  altGrid.clear();
  dirt.assign(nr, true);
  cx = cy = 0;
  wrapPending = false;
  top = 0; bot = nr - 1;
  savedX = savedY = 0;
  autowrap = curOn = true;
  appCursor = onAlt = originMode = g0Graphics = false;
  st = GROUND;
  np = 0;
  u8left = 0;
}

bool VtScreen::anyDirty() const {
  for (bool d : dirt) if (d) return true;
  return false;
}

void VtScreen::clearDirty() { dirt.assign(nr, false); }

// ------------------------------------------------------------------ grid helpers
void VtScreen::clearRange(int r, int c0, int c1) {
  if (r < 0 || r >= nr) return;
  if (c0 < 0) c0 = 0;
  if (c1 > nc) c1 = nc;
  if (c0 >= c1) return;
  grid[r].replace(c0, c1 - c0, c1 - c0, ' ');
  mark(r);
}

void VtScreen::scrollUp(int t, int b, int n) {
  if (n < 1) return;
  if (n > b - t + 1) n = b - t + 1;
  if (t == 0 && !onAlt)                         // lines leaving the top of the main screen: keep for scrollback
    for (int r = 0; r < n; r++) { hist.push_back(grid[r]); if (hist.size() > HISTORY) hist.pop_front(); }
  for (int r = t; r <= b; r++) {
    grid[r] = r + n <= b ? grid[r + n] : std::string(nc, ' ');
    mark(r);
  }
}

void VtScreen::scrollDown(int t, int b, int n) {
  if (n < 1) return;
  if (n > b - t + 1) n = b - t + 1;
  for (int r = b; r >= t; r--) {
    grid[r] = r - n >= t ? grid[r - n] : std::string(nc, ' ');
    mark(r);
  }
}

void VtScreen::lineFeed() {
  if (cy == bot) scrollUp(top, bot, 1);
  else if (cy < nr - 1) cy++;
}

void VtScreen::reverseIndex() {
  if (cy == top) scrollDown(top, bot, 1);
  else if (cy > 0) cy--;
}

void VtScreen::put(char c) {
  if (wrapPending) {
    wrapPending = false;
    if (autowrap) { cx = 0; lineFeed(); }
  }
  if (cx >= nc) cx = nc - 1;
  grid[cy][cx] = c;
  mark(cy);
  if (cx == nc - 1) wrapPending = true;
  else cx++;
}

// ------------------------------------------------------------------ text: UTF-8 / line drawing -> ASCII
void VtScreen::codepoint(uint32_t cp) {
  char c = mapCp(cp);
  put(c ? c : '?');
}

char VtScreen::mapCp(uint32_t cp) {
  // Latin-1 letters -> the plain letter (the Pip-Boy fonts are ASCII only)
  static const char LATIN[65] = "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYPs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuypy";
  char c = 0;
  if (cp >= 0x20 && cp < 0x7f) c = (char)cp;
  else if (cp >= 0xC0 && cp <= 0xFF) c = LATIN[cp - 0xC0];
  else if (cp == 0xA0) c = ' ';
  else if (cp == 0xB0) c = (char)0xB0;                       // degree: the Pip-Boy terminal draws it
  else if (cp == 0xB7 || cp == 0x2022 || cp == 0x25CF) c = '*';
  else if (cp == 0x2018 || cp == 0x2019) c = '\'';
  else if (cp == 0x201C || cp == 0x201D) c = '"';
  else if (cp == 0x2013 || cp == 0x2014) c = '-';
  else if (cp == 0x2026) c = '.';
  else if (cp >= 0x2500 && cp <= 0x257F) {                  // box drawing
    uint32_t o = cp - 0x2500;
    if (o <= 0x01 || (o >= 0x04 && o <= 0x05) || (o >= 0x08 && o <= 0x09) || o == 0x4C || o == 0x4D || o == 0x50 ||
        (o >= 0x54 && o <= 0x55) || o == 0x74 || o == 0x76 || o == 0x78 || o == 0x7A || o == 0x7C || o == 0x7E) c = '-';
    else if ((o >= 0x02 && o <= 0x03) || (o >= 0x06 && o <= 0x07) || (o >= 0x0A && o <= 0x0B) || o == 0x4E ||
             o == 0x4F || o == 0x51 || o == 0x75 || o == 0x77 || o == 0x79 || o == 0x7B || o == 0x7D || o == 0x7F) c = '|';
    else c = '+';
  } else if (cp >= 0x2580 && cp <= 0x259F) c = '#';          // blocks / shades
  return c;
}

// IBM PC code page 437, 0x80..0xFF -> ASCII look-alikes (0xF8, the degree sign, stays 0xB0: the terminal draws it)
static const char CP437[129] =
    "CueaaaaceeeiiiAA" "EaAooouuyOUcLYPf" "aiounNao?--//!<>" "###|++++++|+++++"
    "++++-++++++++=++" "+++++++++++#####" "aBGpSsutFOOd8fen" "=+><()/~\xB0..vn2# ";

void VtScreen::cp437(uint8_t c) { put(c >= 0x80 ? CP437[c - 0x80] : (char)c); }

// ------------------------------------------------------------------ the parser
void VtScreen::feed(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = d[i];
    // UTF-8 continuation bytes
    if (u8left) {
      if ((c & 0xC0) == 0x80) {
        u8cp = (u8cp << 6) | (c & 0x3F);
        if (u8n < 4) u8raw[u8n++] = c;
        if (--u8left == 0) {
          // valid UTF-8 we can't show is more likely code page 437 that happens to look like UTF-8
          // (BBS box drawing: 0xCD 0xBB is a Greek letter in UTF-8, but "=+" in CP437)
          char m = mapCp(u8cp);
          if (m) put(m);
          else for (int k = 0; k < u8n; k++) cp437(u8raw[k]);
        }
        continue;
      }
      u8left = 0;                                             // not UTF-8 after all: code page 437 bytes
      for (int k = 0; k < u8n; k++) cp437(u8raw[k]);
    }
    if (st == OSC) {                                          // ESC ] ... BEL / ESC \  (window title etc.)
      if (c == 0x07) st = GROUND;
      else if (c == 0x1B) st = OSC_ESC;
      continue;
    }
    if (st == OSC_ESC) { st = c == '\\' ? GROUND : OSC; continue; }
    if (c == 0x1B) { st = ESC; continue; }
    if (c < 0x20 || c == 0x7F) {                              // C0 controls work inside sequences too
      if (c == 0x18 || c == 0x1A) { st = GROUND; continue; }  // CAN / SUB abort a sequence
      control(c);
      continue;
    }
    switch (st) {
      case GROUND:
        if (c < 0x80) {
          if (g0Graphics && c >= 0x60 && c <= 0x7E) {         // DEC special graphics (line drawing)
            char m;
            switch (c) {
              case 'o': case 'p': case 'q': case 'r': case 's': m = '-'; break;
              case 'x': m = '|'; break;
              case 'j': case 'k': case 'l': case 'm': case 'n': case 't': case 'u': case 'v': case 'w': m = '+'; break;
              case 'a': m = '#'; break;
              case 'f': m = (char)0xB0; break;
              case 'y': m = '<'; break;
              case 'z': m = '>'; break;
              default: m = '*'; break;
            }
            put(m);
          } else put((char)c);
        } else if (bbs) cp437(c);                            // BBS: always the PC character set
        else if (c >= 0xC2 && c <= 0xDF) { u8cp = c & 0x1F; u8left = 1; u8raw[0] = c; u8n = 1; }
        else if ((c & 0xF0) == 0xE0) { u8cp = c & 0x0F; u8left = 2; u8raw[0] = c; u8n = 1; }
        else if (c >= 0xF0 && c <= 0xF4) { u8cp = c & 0x07; u8left = 3; u8raw[0] = c; u8n = 1; }
        else cp437(c);                                        // a stray byte: code page 437
        break;
      case ESC:
        if (c == '[') { st = CSI; np = 0; params[0] = 0; priv = gtPriv = false; }
        else if (c == ']') st = OSC;
        else if (c == '(' ) st = CHARSET;
        else if (c == ')' || c == '*' || c == '+') st = CHARSET;   // G1..G3: read and ignore the next byte
        else if (c == '#') st = HASH;
        else { st = GROUND; escFinal(c); }
        break;
      case CHARSET:
        // only G0 matters (we never get SO/SI-shifted text in practice)
        g0Graphics = c == '0';
        st = GROUND;
        break;
      case HASH:
        if (c == '8') {                                       // DECALN: fill with E (screen test)
          for (int r = 0; r < nr; r++) { grid[r].assign(nc, 'E'); mark(r); }
        }
        st = GROUND;
        break;
      case CSI:
        if (c >= '0' && c <= '9') {
          if (np == 0) np = 1;
          if (params[np - 1] < 10000) params[np - 1] = params[np - 1] * 10 + (c - '0');
        } else if (c == ';' || c == ':') {
          if (np == 0) np = 1;
          if (np < 16) params[np++] = 0;
        } else if (c == '?') priv = true;
        else if (c == '>' || c == '=' || c == '<') gtPriv = true;
        else if (c >= 0x20 && c <= 0x2F) { /* intermediate bytes: ignored */ }
        else if (c >= 0x40 && c <= 0x7E) { st = GROUND; csiFinal(c); }
        else st = GROUND;
        break;
      default:
        st = GROUND;
        break;
    }
  }
}

void VtScreen::control(uint8_t c) {
  switch (c) {
    case 0x07: bellCount++; break;
    case 0x08: if (cx > 0) { if (cx >= nc) cx = nc - 1; cx--; } wrapPending = false; break;
    case 0x09: {
      if (cx >= nc - 1) break;
      int t = (cx / 8 + 1) * 8;
      cx = t < nc ? t : nc - 1;
      break;
    }
    case 0x0A: case 0x0B: case 0x0C: lineFeed(); wrapPending = false; break;
    case 0x0D: cx = 0; wrapPending = false; break;
    case 0x0E: case 0x0F: break;                              // SO / SI
    default: break;
  }
}

void VtScreen::escFinal(uint8_t c) {
  switch (c) {
    case '7': savedX = cx; savedY = cy; break;
    case '8': cx = savedX; cy = savedY; wrapPending = false; break;
    case 'D': lineFeed(); break;
    case 'E': cx = 0; lineFeed(); break;
    case 'M': reverseIndex(); break;
    case 'c': reset(); break;
    default: break;                                           // '=' / '>' keypad modes and others
  }
}

void VtScreen::setMode(bool p, int m, bool on) {
  if (!p) return;                                             // ANSI modes (insert mode etc.): ignored
  switch (m) {
    case 1:  appCursor = on; break;
    case 6:  originMode = on; cx = 0; cy = on ? top : 0; break;
    case 7:  autowrap = on; break;
    case 25: curOn = on; break;
    case 47: case 1047: altScreen(on, false); break;
    case 1049: altScreen(on, true); break;
    default: break;                                           // mouse, bracketed paste, ...
  }
}

void VtScreen::altScreen(bool on, bool saveCursor) {
  if (on == onAlt) return;
  if (on) {
    if (saveCursor) { savedX = cx; savedY = cy; }
    altGrid = grid;
    grid.assign(nr, std::string(nc, ' '));
  } else {
    if (altGrid.size() == (size_t)nr) grid = altGrid;
    altGrid.clear();
    if (saveCursor) { cx = savedX; cy = savedY; }
  }
  onAlt = on;
  wrapPending = false;
  markAll();
}

void VtScreen::csiFinal(uint8_t c) {
  int n = param(0, 1);
  if (gtPriv) {                                               // CSI > c (secondary DA) etc.
    if (c == 'c') replies += "\x1b[>0;95;0c";
    return;
  }
  switch (c) {
    case 'A': { int lim = (cy >= top && cy <= bot) ? top : 0; cy -= n; if (cy < lim) cy = lim; break; }
    case 'B': case 'e': { int lim = (cy >= top && cy <= bot) ? bot : nr - 1; cy += n; if (cy > lim) cy = lim; break; }
    case 'C': case 'a': cx += n; if (cx > nc - 1) cx = nc - 1; break;
    case 'D': if (cx >= nc) cx = nc - 1; cx -= n; if (cx < 0) cx = 0; break;
    case 'E': cx = 0; cy += n; if (cy > nr - 1) cy = nr - 1; break;
    case 'F': cx = 0; cy -= n; if (cy < 0) cy = 0; break;
    case 'G': case '`': cx = n - 1; if (cx > nc - 1) cx = nc - 1; break;
    case 'd': cy = n - 1 + (originMode ? top : 0); if (cy > nr - 1) cy = nr - 1; break;
    case 'H': case 'f':
      cy = param(0, 1) - 1 + (originMode ? top : 0);
      cx = param(1, 1) - 1;
      if (cy > nr - 1) cy = nr - 1;
      if (cx > nc - 1) cx = nc - 1;
      if (cy < 0) cy = 0;
      if (cx < 0) cx = 0;
      break;
    case 'J': {
      int m = param(0, 0);
      if (m == 0) { clearRange(cy, cx, nc); for (int r = cy + 1; r < nr; r++) clearRange(r, 0, nc); }
      else if (m == 1) { for (int r = 0; r < cy; r++) clearRange(r, 0, nc); clearRange(cy, 0, cx + 1); }
      else {
        for (int r = 0; r < nr; r++) clearRange(r, 0, nc);
        if (bbs) { cx = 0; cy = 0; }                          // ANSI.SYS homes the cursor; art relies on it
      }
      break;
    }
    case 'K': {
      int m = param(0, 0);
      if (m == 0) clearRange(cy, cx, nc);
      else if (m == 1) clearRange(cy, 0, cx + 1);
      else clearRange(cy, 0, nc);
      break;
    }
    case 'L': if (cy >= top && cy <= bot) scrollDown(cy, bot, n); break;
    case 'M': if (cy >= top && cy <= bot) scrollUp(cy, bot, n); break;
    case '@': {                                               // insert blanks
      if (cx >= nc) break;
      std::string& s = grid[cy];
      s.insert(cx, std::string(n > nc - cx ? nc - cx : n, ' '));
      s.resize(nc);
      mark(cy);
      break;
    }
    case 'P': {                                               // delete characters
      if (cx >= nc) break;
      std::string& s = grid[cy];
      int k = n > nc - cx ? nc - cx : n;
      s.erase(cx, k);
      s.append(k, ' ');
      mark(cy);
      break;
    }
    case 'X': clearRange(cy, cx, cx + n); break;
    case 'S': scrollUp(top, bot, n); break;
    case 'T': scrollDown(top, bot, n); break;
    case 'r': {
      int t = param(0, 1) - 1, b = param(1, nr) - 1;
      if (b > nr - 1) b = nr - 1;
      if (t < 0) t = 0;
      if (t < b) { top = t; bot = b; }
      else { top = 0; bot = nr - 1; }
      cx = 0; cy = originMode ? top : 0;
      break;
    }
    case 's': savedX = cx; savedY = cy; break;
    case 'u': cx = savedX; cy = savedY; break;
    case 'h': for (int i = 0; i < (np ? np : 1); i++) setMode(priv, params[i], true); break;
    case 'l': for (int i = 0; i < (np ? np : 1); i++) setMode(priv, params[i], false); break;
    case 'n':
      if (param(0, 0) == 5) replies += "\x1b[0n";
      else if (param(0, 0) == 6) {
        char b[24];
        snprintf(b, sizeof(b), "\x1b[%d;%dR", cy + 1, (cx < nc ? cx : nc - 1) + 1);
        replies += b;
      }
      break;
    case 'c': if (!priv) replies += "\x1b[?1;2c"; break;      // "VT100 with advanced video"
    case 'm': break;                                          // colours / bold / inverse: not shown
    default: break;
  }
  // anything that moves the cursor or edits around it cancels a pending wrap
  if (c != 'm' && c != 'n' && c != 'c' && c != 'h' && c != 'l' && c != 'J' && c != 'K' && c != 'X') wrapPending = false;
}
