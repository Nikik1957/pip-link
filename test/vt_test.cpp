// PC unit tests for lib/remote_term (VtScreen + TelnetCodec). Run: python tools/vt_test.py
#include <stdio.h>
#include <string>
#include "vt_screen.h"
#include "telnet_codec.h"

static int fails = 0, passes = 0;
static std::string trim(const std::string& s) { size_t e = s.find_last_not_of(' '); return e == std::string::npos ? "" : s.substr(0, e + 1); }
static void eq(const char* what, const std::string& got, const std::string& want) {
  if (got == want) { passes++; return; }
  fails++;
  printf("FAIL %s\n  got  [%s]\n  want [%s]\n", what, got.c_str(), want.c_str());
}
static void eqi(const char* what, int got, int want) { eq(what, std::to_string(got), std::to_string(want)); }
static std::string hex(const std::string& s) {
  std::string o; char b[4];
  for (unsigned char c : s) { snprintf(b, sizeof b, "%02x ", c); o += b; }
  return o;
}

int main() {
  {
    VtScreen v(10, 4);
    v.feed("hello\r\nworld");
    eq("text row0", trim(v.row(0)), "hello");
    eq("text row1", trim(v.row(1)), "world");
    eqi("cursor col", v.cursorCol(), 5);
  }
  {
    VtScreen v(10, 3);                           // exact-width line then CRLF: no blank line (pending wrap)
    v.feed("0123456789\r\nabc");
    eq("pending wrap row0", v.row(0), "0123456789");
    eq("pending wrap row1", trim(v.row(1)), "abc");
    v.feed("\r\n1\r\n2\r\n3");                   // scrolls
    eq("scroll row0", trim(v.row(0)), "1");
    eq("scroll row2", trim(v.row(2)), "3");
  }
  {
    VtScreen v(10, 3);                           // autowrap
    v.feed("0123456789AB");
    eq("wrap row1", trim(v.row(1)), "AB");
  }
  {
    VtScreen v(10, 4);
    v.feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc");
    v.feed("\x1b[2;4H\x1b[K");                   // CUP + erase to end of line
    eq("EL", v.row(1), "bbb       ");
    v.feed("\x1b[1;6H\x1b[1K");
    eq("EL1", v.row(0), "      aaaa");
    v.feed("\x1b[2J");
    eq("ED2", trim(v.row(2)), "");
  }
  {
    VtScreen v(8, 5);                            // scroll region, like a status line app
    v.feed("\x1b[1;1Htop\x1b[5;1Hstatus\x1b[2;4r\x1b[4;1H");
    v.feed("x\r\ny\r\nz");
    eq("region top fixed", trim(v.row(0)), "top");
    eq("region bottom fixed", trim(v.row(4)), "status");
    eq("region scrolled", trim(v.row(2)), "y");
    eq("region last", trim(v.row(3)), "z");
    v.feed("\x1b[r");
  }
  {
    VtScreen v(8, 4);                            // insert / delete lines and chars
    v.feed("1\r\n2\r\n3\r\n4\x1b[2;1H\x1b[L");
    eq("IL row1", trim(v.row(1)), "");
    eq("IL row2", trim(v.row(2)), "2");
    v.feed("\x1b[M");
    eq("DL row1", trim(v.row(1)), "2");
    v.feed("\x1b[1;1Habcdef\x1b[1;2H\x1b[2P");
    eq("DCH", trim(v.row(0)), "adef");
    v.feed("\x1b[2@");
    eq("ICH", trim(v.row(0)), "a  def");
  }
  {
    VtScreen v(10, 3);                           // alternate screen (nano, less, top)
    v.feed("shell $");
    v.feed("\x1b[?1049h\x1b[H\x1b[2Jnano!");
    eq("alt shows", trim(v.row(0)), "nano!");
    v.feed("\x1b[?1049l");
    eq("alt restored", trim(v.row(0)), "shell $");
    eqi("alt cursor restored", v.cursorCol(), 7);
  }
  {
    VtScreen v(20, 2);                           // a colourful bash prompt with a window title
    v.feed("\x1b]0;pi@pi4: ~\x07\x1b[01;32mpi@pi4\x1b[00m:\x1b[01;34m~ $\x1b[00m ");
    eq("bash prompt", v.row(0), "pi@pi4:~ $          ");
  }
  {
    VtScreen v(12, 2);                           // UTF-8 + DEC line drawing
    v.feed("\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90 21\xc2\xb0" "C");
    eq("utf8 box", v.row(0).substr(0, 8), std::string("+-+ 21") + (char)0xB0 + "C");
    v.feed("\r\n\x1b(0lqqk\x1b(Bok");
    eq("dec graphics", trim(v.row(1)), "+--+ok");
  }
  {
    VtScreen v(20, 3);                           // BBS: code page 437 box drawing (not UTF-8)
    v.feed("\xC9\xCD\xCD\xBB\r\n\xBA hi \xBA\r\n\xC8\xCD\xCD\xBC \xDB\xB1 \x81 25\xF8");
    eq("cp437 top", trim(v.row(0)), "+==+");
    eq("cp437 mid", trim(v.row(1)), "| hi |");
    eq("cp437 bottom", trim(v.row(2)), std::string("+==+ ## u 25") + (char)0xB0);
    VtScreen u(10, 1);                           // real UTF-8 still decodes
    u.feed("\xc3\xa9t\xc3\xa9 \xe2\x94\x80");
    eq("utf8 still", trim(u.row(0)), "ete -");
  }
  {
    VtScreen v(6, 3);                            // scrollback + resize keeping the text
    v.feed("1\r\n2\r\n3\r\n4\r\n5");
    eqi("history", v.historySize(), 2);
    eq("view back 2 row0", trim(v.viewRow(0, 2)), "1");
    eq("view back 1 row2", trim(v.viewRow(2, 1)), "4");
    eq("view live row2", trim(v.viewRow(2, 0)), "5");
    v.resizeKeep(10, 2);
    eq("resize keeps cursor row", trim(v.row(1)), "5");
    eq("resize row0", trim(v.row(0)), "4");
    eqi("resize history", v.historySize(), 3);
    v.resizeKeep(4, 4);
    eq("grow keeps text", trim(v.row(0)), "4");
    v.feed("\x1b[?1049hfull\x1b[2;1Hx\r\ny\r\nz\r\nw");
    eqi("alt screen adds no history", v.historySize(), 3);
  }
  {
    VtScreen v(20, 3);                           // BBS mode: PC characters only, ESC[2J homes the cursor
    v.setBbs(true);
    v.feed("abc\x1b[2J\xC3\xB3\xC4\xC4\xB4");
    eq("bbs cp437 pairs", trim(v.row(0)), "+|--+");
    eqi("bbs clear homes", v.cursorRow(), 0);
    TelnetCodec t;
    t.begin(80, 24, "ANSI");
    std::string data, reply;
    const uint8_t in[] = { 255, 250, 24, 1, 255, 240 };
    t.receive(in, sizeof in, data, reply);
    eq("ttype ANSI", hex(reply), hex(std::string("\xff\xfa\x18\x00" "ANSI\xff\xf0", 10)));
  }
  {
    VtScreen v(10, 5);                           // device status report
    v.feed("\x1b[3;7H\x1b[6n\x1b[c");
    eq("DSR+DA", v.takeReplies(), "\x1b[3;7R\x1b[?1;2c");
    v.feed("\x1b[?1h");
    eqi("DECCKM", v.appCursorKeys(), 1);
    v.feed("\x1b[?25l");
    eqi("cursor hidden", v.cursorVisible(), 0);
  }
  {
    VtScreen v(10, 3);                           // backspace + tabs
    v.feed("abc\b\bX\tY");
    eq("bs tab", trim(v.row(0)), "aXc     Y");
    v.clearDirty();
    v.feed("\x1b[3;1Hz");
    eqi("dirty only row2", v.dirty(2) && !v.dirty(0) && !v.dirty(1), 1);
  }
  {
    TelnetCodec t;                               // negotiation
    t.begin(58, 12);
    std::string data, reply;
    const uint8_t in[] = { 255, 253, 24, 255, 253, 31, 255, 251, 1, 255, 251, 3, 255, 253, 39,
                           255, 250, 24, 1, 255, 240, 'h', 'i', '\r', 0, '\n', 255, 255 };
    t.receive(in, sizeof in, data, reply);
    eq("telnet data", hex(data), hex(std::string("hi\r\n\xff")));
    std::string want;
    want += std::string("\xff\xfb\x18");                         // WILL TTYPE
    want += std::string("\xff\xfb\x1f");                         // WILL NAWS
    want += std::string("\xff\xfa\x1f\x00\x3a\x00\x0c\xff\xf0", 9);   // NAWS 58x12
    want += std::string("\xff\xfd\x01");                         // DO ECHO
    want += std::string("\xff\xfd\x03");                         // DO SGA
    want += std::string("\xff\xfc\x27");                         // WONT NEW-ENVIRON
    want += std::string("\xff\xfa\x18\x00VT100\xff\xf0", 11);    // TTYPE IS VT100
    eq("telnet reply", hex(reply), hex(want));
    eqi("server echoes", t.serverEchoes(), 1);
    eq("encode", hex(t.encode(std::string("ls\r\xff", 4))), hex(std::string("ls\r\0\xff\xff", 6)));
  }
  printf("%d passed, %d failed\n", passes, fails);
  return fails ? 1 : 0;
}
