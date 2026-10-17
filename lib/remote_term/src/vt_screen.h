// VtScreen - a small VT100/xterm-subset terminal emulator for the TERM tab.
// Bytes from the remote computer go in; a cols x rows grid of plain ASCII comes out, with dirty-row flags
// so only changed rows are sent to the Pip-Boy. Colours and per-character attributes are dropped (the
// Pip-Boy row renderer is one shade per row). UTF-8, DEC line drawing and - for bytes that aren't valid
// UTF-8 - IBM PC code page 437 (BBS box drawing) are mapped to ASCII look-alikes.
// Plain C++ (no Arduino) so it can be unit-tested on the PC: tools/vt_test.py.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <deque>
#include <string>
#include <vector>

class VtScreen {
public:
  VtScreen(int cols = 58, int rows = 12) { resize(cols, rows); }
  void resize(int cols, int rows);              // clears the screen
  void resizeKeep(int cols, int rows);          // keeps the text (the bottom part, where the cursor is)
  // BBS mode ("ANSI-BBS"): every byte >= 0x80 is code page 437 (no UTF-8 guessing, which can swallow
  // a pair of art characters), and clearing the screen (ESC[2J) also homes the cursor like ANSI.SYS
  void setBbs(bool on) { bbs = on; }
  bool bbsMode() const { return bbs; }
  void reset();

  void feed(const uint8_t* d, size_t n);
  void feed(const std::string& s) { feed((const uint8_t*)s.data(), s.size()); }

  int cols() const { return nc; }
  int rows() const { return nr; }
  const std::string& row(int r) const { return grid[r]; }
  // scrollback: lines that scrolled off the top of the main screen (newest last, at most HISTORY)
  static const size_t HISTORY = 500;
  int historySize() const { return (int)hist.size(); }
  std::string viewRow(int r, int back) const;   // row r of the screen scrolled back by `back` lines
  bool dirty(int r) const { return dirt[r]; }
  bool anyDirty() const;
  void clearDirty();

  int cursorRow() const { return cy; }
  int cursorCol() const { return cx < nc ? cx : nc - 1; }
  bool cursorVisible() const { return curOn; }
  bool appCursorKeys() const { return appCursor; }   // DECCKM: arrows send ESC O x instead of ESC [ x
  int bells() const { return bellCount; }

  std::string takeReplies() { std::string r; r.swap(replies); return r; }   // DSR / DA answers for the host

private:
  enum State { GROUND, ESC, CSI, OSC, OSC_ESC, CHARSET, HASH };
  void put(char c);
  void control(uint8_t c);
  void escFinal(uint8_t c);
  void csiFinal(uint8_t c);
  void lineFeed();
  void reverseIndex();
  void scrollUp(int top, int bot, int n);
  void scrollDown(int top, int bot, int n);
  void clearRange(int r, int c0, int c1);       // c1 exclusive
  void mark(int r) { if (r >= 0 && r < nr) dirt[r] = true; }
  void markAll() { for (int r = 0; r < nr; r++) dirt[r] = true; }
  int param(int i, int def) const { return (i < np && params[i] > 0) ? params[i] : def; }
  void setMode(bool priv, int p, bool on);
  void altScreen(bool on, bool saveCursor);
  void codepoint(uint32_t cp);
  static char mapCp(uint32_t cp);               // Unicode -> a displayable byte, 0 = nothing sensible

  int nc = 0, nr = 0;
  std::vector<std::string> grid, altGrid;
  std::deque<std::string> hist;
  std::vector<bool> dirt;
  int cx = 0, cy = 0;
  bool wrapPending = false;
  int top = 0, bot = 0;                         // scroll region (inclusive)
  int savedX = 0, savedY = 0;
  bool autowrap = true, curOn = true, appCursor = false, onAlt = false, originMode = false;
  bool g0Graphics = false;                      // ESC ( 0: DEC special graphics (line drawing)
  bool bbs = false;
  int bellCount = 0;

  State st = GROUND;
  int params[16];
  int np = 0;
  bool priv = false;                            // CSI ? ...
  bool gtPriv = false;                          // CSI > ...
  uint32_t u8cp = 0;
  int u8left = 0;
  uint8_t u8raw[4];                             // the bytes of the sequence so far (for the CP437 fallback)
  int u8n = 0;
  void cp437(uint8_t c);                        // a byte that isn't UTF-8: old PC / BBS character set
  std::string replies;
};
