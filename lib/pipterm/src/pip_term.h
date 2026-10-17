// PipTerm - app-facing API for the RobCo Terminal holotape.
// Apps draw rows/header/footer and get knob + keyboard events. Rows are cached
// so only real changes go over the wire (each row redraw costs ~21 ms on the Pip-Boy).
#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>
#include "pip_link.h"

class PipTerm {
public:
  // attr: 1 = dim, 2 = normal, 3 = bright; add INV for an inverse highlight bar
  static const int DIM = 1, NORMAL = 2, BRIGHT = 3, INV = 4;

  explicit PipTerm(PipLink& l) : link(l) {}
  void begin();

  bool ready() const { return link.state() == PipLink::READY; }
  int cols() const { return link.cols(); }
  int rows() const { return link.rows(); }

  void cls();
  // icon >= 0: a map marker icon (tools/map_icons.py order) drawn in the row's first two columns
  void line(int row, const String& text, int attr = NORMAL, int icon = -1);
  void print(const String& text);                 // scroll-mode stream (\n \r \b \f)
  // terminal v0.5+: large centred text, and a bar graph (one 0-254 value per bar; mark = dashed
  // level line, 0 = none), each covering rows row..row+n-1
  void big(int row, int n, const String& text, int attr = BRIGHT);
  void bars(int row, int n, const std::vector<uint8_t>& values, uint8_t mark = 0, int attr = BRIGHT);
  void header(const String& text);
  void footer(const String& text);
  void keyboard(const String& id, const String& title, const String& init = "");
  // wheel clicks per step for the on-screen keyboard (terminal v1.2+), like the unit's own screens use
  void keyboardSteps(int knob1, int knob2) { kbStep1 = knob1; kbStep2 = knob2; }
  void sound(const char* name = "SELECT");        // firmware UI blip (quiet)
  void play(const String& wavPath);               // WAV on the Pip-Boy SD (16-bit mono 16 kHz)
  void dump();                                    // debug: terminal reports its grid

  // terminal v0.6+: map viewer over the grid area (tiles/markers from MAP/ on the Pip-Boy SD).
  // mapMode(true) answers through onMapInfo; dyn = "lat,lon,type,heading,label;..." (type -1 = player)
  void mapMode(bool on);
  // dyn type -3 = a NAV line from the player to that point (label = distance); flags 1 = centre crosshair,
  // 2 = panning (terminal v1.2+ scrolls the picture instead of redrawing it)
  void mapView(double lat, double lon, int zoom, const String& dyn, int flags = 0);
  // colour theme for PIP LINK (0 green, 1 amber, -1 = the Pip-Boy's own colour from its settings); the
  // Pip-Boy's own colour comes back when PIP LINK closes
  void theme(int which);
  // terminal v1.2+: take over the STATS / ITEMS / DATA buttons and the side switch (onButton / onSwitch;
  // holding STATS then leaves PIP LINK), and light one of the buttons (0-2, -1 = none)
  void controls(bool on);
  void led(int which);
  // all three button lights at once: "012" = STATS off, ITEMS on, DATA blinking (terminal v1.2+)
  void leds(const String& states);
  // text cursor (an underline) for the TERM tab; row -1 hides it
  void cursor(int row, int col);
  // TERM canvas over the grid area with its own font: size 0 large (58x12), 1 medium (66x16), 2 small (78x24),
  // 3 BBS (80x24), -1 = off; margin = left edge in pixels (-1 = the terminal's default). vtLine() fills it
  // (cached like line()). The terminal confirms with onVtSize.
  void vtMode(int size, int margin = -1);
  void vtLine(int row, const String& text);
  int vtSize() const { return vtSz; }
  void onVtSize(std::function<void(int cols, int rows)> h) { vtSizeH = h; }
  // saved locations file on the Pip-Boy SD (PIPLINK/WPT.TXT): loadPlaces() streams it back as
  // "WPF <b64>" ... "WPF END <bytes>" events, appendPlaces() appends text and answers "WAD 1|0"
  void loadPlaces();
  void appendPlaces(const String& text);
  bool mapOn() const { return mapActive; }
  bool keyboardOpen() const { return kbActive; }

  // knob: 1 = side wheel, 2 = second knob; dir: +1 / -1, 0 = click
  void onKnob(std::function<void(int knob, int dir)> h) { knobH = h; }
  void onKeyboard(std::function<void(const String& id, bool ok, const String& text)> h) { kbH = h; }
  void onReady(std::function<void()> h) { readyH = h; }
  // HOLO/TERM/CONFIG.TXT from the Pip-Boy SD card (key=value lines), sent by the terminal on start
  void onConfig(std::function<void(const String& text)> h) { cfgH = h; }
  // zmax < zmin when the SD card has no map pack
  void onMapInfo(std::function<void(int zmin, int zmax, double lat, double lon)> h) { mapInfoH = h; }
  // button under the screen pressed: 0 STATS, 1 ITEMS, 2 DATA; side switch moved to position 0-4
  void onButton(std::function<void(int which)> h) { buttonH = h; }
  void onSwitch(std::function<void(int pos)> h) { switchH = h; }
  // any other terminal event (WPF / WAD ...)
  void onOther(std::function<void(const String& ev)> h) { otherH = h; }

  static String clean(const String& s);           // printable ASCII + the degree sign (Monofonto has no emoji)

private:
  void reset();
  bool claimSpan(int row, int n, const String& key);
  void handleEvent(const String& ev);

  PipLink& link;
  std::vector<String> rowText;
  std::vector<int> rowAttr;
  std::vector<int> rowIcon;
  int curRow = -2, curCol = 0;
  int vtSz = -1, vtMargin = -1;
  int kbStep1 = 1, kbStep2 = 1;
  int ledSel = -2;
  String ledStates;
  std::function<void(int)> buttonH, switchH;
  std::vector<String> vtText;
  std::function<void(int, int)> vtSizeH;
  String hdrText, ftrText;
  bool kbActive = false;
  bool mapActive = false;
  String mapKey;
  std::function<void(int, int)> knobH;
  std::function<void(const String&, bool, const String&)> kbH;
  std::function<void()> readyH;
  std::function<void(const String&)> cfgH;
  std::function<void(int, int, double, double)> mapInfoH;
  std::function<void(const String&)> otherH;
};
