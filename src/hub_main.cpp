// PIP LINK: a MeshCore or Meshtastic radio over BLE shown on the Pip-Boy through the PIP LINK terminal
// holotape (USB host),
// plus a BLE keyboard and RadiaCode, all managed from a SETTINGS page.
//   Tabs       : MESH / MAP / RADIATION / TERM / SETTINGS (the header). knob2 cycles them; the side switch's five
//                positions pick them. STATS = the newest chat (unread first), ITEMS = MAP (or a location someone
//                just shared), DATA = RADIATION; a button blinks when something is waiting behind it. Holding
//                STATS leaves PIP LINK (the holotape hands the buttons back to the Pip-Boy).
//   MESH       : channels, favourite contacts, CONTACTS (every contact: MESSAGE / FAVORITE / REMOVE FROM RADIO),
//                ADVERTISE (the radio announces itself). knob1 move, click open
//   MAP        : street map from PIPLINK/MAP/ on the Pip-Boy SD + your position (radio GPS), mesh contacts and
//                your saved locations; click opens the mode menu (ZOOM / PAN / CONTACTS / GO TO / CENTER ON ME):
//                knob2 highlights, click chooses. PAN: scroll wheel = north/south, knob2 = east/west, click = done.
//                GO TO: NEW (save the crosshair spot), MANUAL ENTRY (lat,lon), then the saved locations, each
//                with GO TO / NAV (dotted line + distance + azimuth) / SHARE (to the mesh) / DELETE.
//   RADIATION  : live RadiaCode dose rate (shown in rads), totals, rate graph; click cycles the alarm level
//   TERM       : SSH (user@host) or Telnet (host) to a computer over Wi-Fi (VT100 screen). Wi-Fi runs only while this tab is open or a
//                session is live. In a session: knob1 scrolls back through the last 500 lines (or sends Up/Down:
//                WHEEL), click = back to live / the TYPE / CTRL-C / ESC / TAB / SIZE / WHEEL / DISCONNECT menu
//                (knob2 highlights), BLE keyboard keys all go to the remote computer, knob2 = tabs.
//                SIZE: 58x12 / 66x16 / 78x24 text (prefs "term_size"); WHEEL: scrollback or arrows ("term_wheel").
//   SETTINGS   : LoRa radio / Keyboard / RadiaCode (click = pick + pair), rad alarm, message sound,
//                wheels, theme (the Pip-Boy's own / green / amber), map locations shown / hidden, text margin
//   Chat view  : knob1 scroll / click reply, knob2 back
//   Keyboard   : Left/Right/Tab/1-5 = tabs, Up/Down move, Enter select, Esc back, typing in a chat replies;
//                map: Up/Down or +/- zoom, P pan (arrows), C contacts, G go to (list), H centre on me
//   LED        : 2 = waiting for Pip-Boy, 3 = handshaking, 4 = terminal up, flicker = USB fail
// Logs: HOLO/TERM/ESP.LOG on the Pip-Boy SD card (python tools/term_sim.py inspect).
#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <algorithm>
#include <deque>
#include <vector>
#include "pip_link.h"
#include "pip_term.h"
#include "ble_hub.h"
#include "meshcore_ble.h"
#include "meshtastic_ble.h"
#include "ble_keyboard.h"
#include "ble_radiacode.h"
#include "ssh_link.h"
#include "telnet_codec.h"
#include "vt_screen.h"

static const char* PIPLINK_VERSION = "1.2";

static PipLink pip;
static PipTerm term(pip);
static MeshCoreBLE meshcore;
static MeshtasticBLE meshtastic;
static MeshRadio* radio = &meshcore;           // the radio in use: one at a time, MeshCore or Meshtastic
enum RadioKind { RK_MESHCORE = 1, RK_MESHTASTIC = 2 };
static BleKeyboard kbd;
static BleRadiaCode rc;
static Preferences prefs;      // NVS "hub": radio, pin, kbd_addr/kbd_type/kbd_name, rc_*, sound

static const char* TIMEZONE = "PST8PDT,M3.2.0,M11.1.0";   // default: US Pacific (POSIX TZ); the PC setup app
                                                           // can set another via CONFIG.TXT -> prefs "tz"
static const char* MSG_SOUND = "HOLO/TERM/MSG.WAV";        // new-message chirp on the Pip-Boy SD
static const char* WPT_SOUND = "HOLO/TERM/WPT.WAV";        // someone shared a location with us
// radiation alarm: Geiger-counter crackle, faster the further over the alarm level (tools/gen_geiger.py)
static const char* GEIGER_SOUND[] = { "HOLO/TERM/GEIGER1.WAV", "HOLO/TERM/GEIGER2.WAV", "HOLO/TERM/GEIGER3.WAV" };
static bool soundOn = true;

// radiation alarm levels (uSv/h); index stored in prefs "rad_alarm", 0 = off
static const float ALARM_LEVELS[] = { 0, 0.3, 0.5, 1, 2.5, 5, 10, 25, 100 };
static const int NALARM = sizeof(ALARM_LEVELS) / sizeof(ALARM_LEVELS[0]);
static int alarmIdx = 3;
static String fmtRadH(float uSvH);              // a rate shown in rads (the radiation page)
static bool alarming = false;
static uint32_t lastAlarmSound = 0;

// dose-rate history for the graph: one bar = average of BUCKET_MS of readings
static const int HIST_BARS = 96;
static const uint32_t BUCKET_MS = 5000;
static std::deque<float> rateHist;
static float bucketSum = 0;
static int bucketN = 0;
static uint32_t bucketStart = 0;

// ---------------------------------------------------------------- model
struct Msg { bool out; String from, text; uint32_t ts; };
struct Convo {
  String name;
  bool isCh = false;
  int chan = -1;
  uint8_t prefix[6] = { 0 };
  int unread = 0;
  float snr = 0;
  bool hasSnr = false;
  std::deque<Msg> msgs;
};
static std::vector<Convo*> convos;            // front = most recent activity
static const size_t MAX_MSGS = 40;

// CONTACTS / CMENU: the MESH tab's contacts page and the options for one contact
enum View { PICK, PAIRKB, LIST, CHAT, PROMPT, CONTACTS, CMENU };
static View view = PICK;
// tab: 0 MESH (channels, then nodes), 1 map, 2 radiation, 3 term, 4 settings - also the side switch's five
// positions, in that order
static int page = 0;
static const char* PAGES[] = { "MESH", "MAP", "RADIATION", "TERM", "SETTINGS" };
static const int PG_MESH = 0, PG_MAP = 1, PG_RAD = 2, PG_TERM = 3, PG_SET = 4, NPAGES = 5;
// the Pip-Boy's wheels are very sensitive: a step only counts after N clicks in one direction
static int tabSteps = 3;                      // knob2 (tabs)        prefs "tab_steps"
static int scrollSteps = 2;                   // knob1 (scroll wheel) prefs "scroll_steps"
static int termMargin = 6;                    // prefs "term_margin": px from the screen's left edge to SSH/Telnet text
static bool rcAlways = true;                  // keep the RadiaCode linked off its tab (alarm) prefs "rc_always"
// 0 = the Pip-Boy's own colour (whatever is picked in its settings, custom colours too), 1 green, 2 amber
// (overrides for PIP LINK only)                                                      prefs "ui_theme"
static int themeSel = 0;
static int themeArg() { return themeSel == 1 ? 0 : themeSel == 2 ? 1 : -1; }   // T.theme(): 0 green, 1 amber, -1 own
static bool showPlaces = true;                // saved locations drawn on the map              prefs "show_places"
static uint32_t radTabSeen = 0;               // last time the RADIATION tab was on screen
static int knobAcc[3] = { 0, 0, 0 };
static uint32_t knobLast[3] = { 0, 0, 0 };
static uint32_t knob1ClickAt = 0;

// map page
static int mapZoom = 15;                      // prefs "map_z"
static int mapZmin = 10, mapZmax = 17;        // from MAP/INFO.TXT via the terminal
static bool mapHasData = false;
static double mapDefLat = 35.8325, mapDefLon = -115.4319;   // pack centre, used until the radio reports a position
                                                // (before a map is found: the Pioneer Saloon, Goodsprings NV)
static int mapFocus = 0;                      // CONTACTS mode: n = n-th nearest located contact
enum MapMode { MM_ZOOM, MM_PAN, MM_FIND };
static MapMode mapMode = MM_ZOOM;
static const char* MAP_MENU[] = { "ZOOM", "PAN", "CONTACTS", "GO TO", "CENTER ON ME" };
static const int NMAPMENU = 5;
enum { MENU_ZOOM, MENU_PAN, MENU_CONTACTS, MENU_GOTO, MENU_ME };
static bool mapMenu = false;                  // footer menu open
static int mapMenuSel = 0;
static bool mapFollow = true;                 // centre on me; false = centre on viewLat/viewLon
static double viewLat = 0, viewLon = 0;
// GO TO list and its sub-screens (shown instead of the map while open)
enum PlaceUi { PU_NONE, PU_LIST, PU_ACT, PU_DEL, PU_SHARE, PU_ICON };
static PlaceUi pu = PU_NONE;
static int puSel = 0, puTop = 0, puListSel = 0;
static double newLat = 0, newLon = 0;         // NEW saves this spot: the crosshair when the list was opened
static String newName;
static float heading = -1;                    // degrees, from successive fixes (-1 = unknown)
static double fixLat = 0, fixLon = 0;
static uint32_t fixSeen = 0;
static Convo* selC = nullptr;                 // list highlight, kept by identity across bumps
static int topRow = 0;
static Convo* active = nullptr;               // open chat
static int chatBack = 0;                      // lines scrolled up from the newest
static int setSel = 0;                        // settings highlight
static const int NSETTINGS = 11;
static BleHub::Kind pickKind = BleHub::MESHCORE;
static String pickSel;                        // highlighted picker entry (key)
static String pendingRadio;                   // picked radio waiting for its PIN
static String flash;                          // short-lived footer note
static uint32_t flashUntil = 0;

// text entry with the physical keyboard (the on-screen keyboard is used when none is connected)
static bool inputActive = false;
static String inputId, inputTitle, inputBuf;
static View inputReturn = LIST;

static Convo* findChannel(int idx) {
  for (Convo* c : convos) if (c->isCh && c->chan == idx) return c;
  return nullptr;
}
static Convo* findDirect(const uint8_t p[6]) {
  for (Convo* c : convos) if (!c->isCh && memcmp(c->prefix, p, 6) == 0) return c;
  return nullptr;
}
static bool isFav(const uint8_t p[6]);
static Convo contactsRow;                     // the MESH list's "CONTACTS (n)" entry: opens the contacts page
static Convo advertRow;                       // ... and its "ADVERTISE" entry: the radio announces itself now
// button lights blink for something waiting: unread messages (STATS), a location someone shared (ITEMS: the
// press shows it on the map), a radiation alarm (DATA: until the RADIATION tab has been looked at)
static bool radAlert = false, placeAlert = false;
static double alertLat = 0, alertLon = 0;
static String alertName;
// the MESH tab's list: the channels, the favourite contacts, then the CONTACTS entry (most recent first)
static std::vector<Convo*> visible() {
  std::vector<Convo*> v;
  for (Convo* c : convos) if (c->isCh) v.push_back(c);
  for (Convo* c : convos) if (!c->isCh && isFav(c->prefix)) v.push_back(c);
  v.push_back(&contactsRow);
  v.push_back(&advertRow);
  return v;
}
// the contacts page: favourites first, then the rest, most recently active first
static std::vector<Convo*> contactConvos() {
  std::vector<Convo*> v;
  for (Convo* c : convos) if (!c->isCh && isFav(c->prefix)) v.push_back(c);
  for (Convo* c : convos) if (!c->isCh && !isFav(c->prefix)) v.push_back(c);
  return v;
}
static int ctSel = 0, ctTop = 0, cmSel = 0, cmTop = 0;   // contacts page / contact options
static Convo* ctC = nullptr;                  // the contact the options are for
static bool cmConfirm = false;                // REMOVE was clicked once: the next click removes
static View chatFrom = LIST;                  // where knob2 goes back to from a chat
static void bump(Convo* c) {
  for (size_t i = 0; i < convos.size(); i++)
    if (convos[i] == c) { convos.erase(convos.begin() + i); break; }
  convos.insert(convos.begin(), c);
}
static void clearConvos() {                   // new radio = different contacts/channels
  for (Convo* c : convos) delete c;
  convos.clear();
  selC = active = nullptr;
  ctC = nullptr;
  if (view == CHAT || view == CONTACTS || view == CMENU) view = LIST;
  topRow = 0;
}

// ---------------------------------------------------------------- history (XIAO flash)
// One file per radio. Each line: ts \t C|D \t chanIdx|dmPrefixHex \t convoName \t out \t from \t text
static bool fsOk = false;
static String histPath;
static const size_t HIST_MAX_BYTES = 64 * 1024;

static String hex6(const uint8_t p[6]) {
  char b[13];
  snprintf(b, sizeof(b), "%02x%02x%02x%02x%02x%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
  return String(b);
}
static bool unhex6(const String& h, uint8_t p[6]) {
  if (h.length() != 12) return false;
  for (int i = 0; i < 6; i++) p[i] = (uint8_t)strtoul(h.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  return true;
}
// favourite contacts: ",<prefix hex>,<prefix hex>," (prefs "favs"), shown on the MESH tab under the channels
static String favs = ",";
static bool isFav(const uint8_t p[6]) { return favs.indexOf("," + hex6(p) + ",") >= 0; }
static void setFav(const uint8_t p[6], bool on) {
  if (on == isFav(p)) return;
  if (on) favs += hex6(p) + ",";
  else favs.replace("," + hex6(p) + ",", ",");
  prefs.putString("favs", favs);
}

static String noTabs(const String& s) {
  String o = s;
  o.replace('\t', ' '); o.replace('\n', ' '); o.replace('\r', ' ');
  return o;
}
static String histFileFor(const String& radio) {
  String p = "/h_";
  for (size_t i = 0; i < radio.length(); i++) if (isAlphaNumeric(radio[i])) p += radio[i];
  return p + ".log";
}
static void histWrite(File& f, const Convo* c, const Msg& m) {
  f.printf("%lu\t%c\t%s\t%s\t%d\t%s\t%s\n", (unsigned long)m.ts, c->isCh ? 'C' : 'D',
           (c->isCh ? String(c->chan) : hex6(c->prefix)).c_str(), noTabs(c->name).c_str(),
           m.out ? 1 : 0, noTabs(m.from).c_str(), noTabs(m.text).c_str());
}
static void histCompact() {
  String tmp = histPath + ".tmp";
  File f = LittleFS.open(tmp, FILE_WRITE);
  if (!f) return;
  for (int i = (int)convos.size() - 1; i >= 0; i--)          // oldest activity first, so reload keeps the order
    for (const Msg& m : convos[i]->msgs) histWrite(f, convos[i], m);
  f.close();
  LittleFS.remove(histPath);
  LittleFS.rename(tmp, histPath);
}
static void histAppend(const Convo* c, const Msg& m) {
  if (!fsOk || !histPath.length()) return;
  File f = LittleFS.open(histPath, FILE_APPEND);
  if (!f) return;
  histWrite(f, c, m);
  size_t size = f.size();
  f.close();
  if (size > HIST_MAX_BYTES) histCompact();
}
static void histLoad() {
  if (!fsOk || !histPath.length() || !LittleFS.exists(histPath)) return;
  File f = LittleFS.open(histPath, FILE_READ);
  if (!f) return;
  size_t loaded = 0, size = f.size();
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String fld[7];
    int k = 0, start = 0;
    for (int i = 0; i <= (int)line.length() && k < 7; i++)
      if (i == (int)line.length() || (line[i] == '\t' && k < 6)) { fld[k++] = line.substring(start, i); start = i + 1; }
    if (k < 7) continue;
    Convo* c;
    if (fld[1] == "C") {
      int idx = fld[2].toInt();
      c = findChannel(idx);
      if (!c) { c = new Convo(); c->isCh = true; c->chan = idx; convos.push_back(c); }
    } else {
      uint8_t p[6];
      if (!unhex6(fld[2], p)) continue;
      c = findDirect(p);
      if (!c) { c = new Convo(); memcpy(c->prefix, p, 6); convos.push_back(c); }
    }
    c->name = fld[3];
    c->msgs.push_back({ fld[4] == "1", fld[5], fld[6], (uint32_t)strtoul(fld[0].c_str(), nullptr, 10) });
    while (c->msgs.size() > MAX_MSGS) c->msgs.pop_front();
    bump(c);
    loaded++;
  }
  f.close();
  pip.log("hub: restored %u messages from %s (%u bytes)", (unsigned)loaded, histPath.c_str(), (unsigned)size);
  if (size > HIST_MAX_BYTES / 2) histCompact();
}

// ---------------------------------------------------------------- text helpers
static String fit(const String& s, int w) {
  if ((int)s.length() >= w) return s.substring(0, w);
  String o = s;
  while ((int)o.length() < w) o += ' ';
  return o;
}
static std::vector<String> wrap(String rest, int w, const String& indent) {
  std::vector<String> out;
  bool first = true;
  while (rest.length()) {
    int avail = first ? w : w - (int)indent.length();
    if ((int)rest.length() <= avail) { out.push_back((first ? "" : indent) + rest); break; }
    int cut = rest.lastIndexOf(' ', avail);
    if (cut <= 0) cut = avail;
    out.push_back((first ? "" : indent) + rest.substring(0, cut));
    rest = rest.substring(cut);
    rest.trim();
    first = false;
  }
  return out;
}
// "14:32" for today, "10/04 14:32" for older; "--:--" if we never learned the time
static String stamp(uint32_t ts) {
  if (ts < 1700000000UL) return "--:--";
  time_t t = ts;
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[16];
  uint32_t now = radio->now();
  strftime(buf, sizeof(buf), (now && now > ts + 20 * 3600) ? "%m/%d %H:%M" : "%H:%M", &lt);
  return String(buf);
}
static String shortName(const String& adv) { return adv.startsWith("MeshCore-") ? adv.substring(9) : adv; }
static String radioState() {
  switch (radio->state()) {
    case MeshRadio::ONLINE:      return "online";
    case MeshRadio::CONNECTING:  return "pairing...";
    case MeshRadio::SEARCHING:   return "searching";
    case MeshRadio::PAIR_FAILED: return "PIN rejected";
    case MeshRadio::NEED_PIN:    return "enter the PIN";
    default:                       return "-";
  }
}
static String kbdState() {
  switch (kbd.state()) {
    case BleKeyboard::ONLINE:     return "connected";
    case BleKeyboard::CONNECTING: return "connecting";
    case BleKeyboard::SEARCHING:  return "asleep (press a key)";
    case BleKeyboard::FAILED:     return "pairing failed";
    default:                      return "-";
  }
}
static String rcState() {
  switch (rc.state()) {
    case BleRadiaCode::ONLINE:     return "connected";
    case BleRadiaCode::CONNECTING: return "connecting";
    case BleRadiaCode::SEARCHING:  return "searching";
    case BleRadiaCode::STANDBY:    return "standby";
    default:                       return "-";
  }
}
static String radioStatus() {
  String s = radio->state() == MeshRadio::ONLINE ? "radio: " + radio->selfName() : "radio: " + radioState();
  if (kbd.state() == BleKeyboard::ONLINE) s += "  [KB]";
  return s;
}
static String footerOr(const String& normal) {
  return (flash.length() && millis() < flashUntil) ? flash : normal;
}
// header for the tab pages: " MESH*  [MAP]  RADIATION  TERM  SETTINGS" (* = unread messages)
static String tabBar() {
  int unread = 0;
  for (Convo* c : convos) unread += c->unread;
  String s;
  for (int i = 0; i < NPAGES; i++) {
    String name = String(PAGES[i]) + (i == PG_MESH && unread ? "*" : "");
    s += i == page ? "[" + name + "]" : " " + name + " ";
    s += " ";
  }
  s.trim();
  int pad = (term.cols() - (int)s.length()) / 2;
  String sp;
  for (int i = 0; i < pad; i++) sp += ' ';
  return sp + s;
}

static void note(const String& s, uint32_t ms = 4000) { flash = s; flashUntil = millis() + ms; }

// ---------------------------------------------------------------- device picker
struct PickItem { bool none; String key, label, name; int rssi; uint8_t addrType; int radioKind; };
static const char* kindName(BleHub::Kind k) {
  return k == BleHub::MESHCORE ? "LORA RADIO" : k == BleHub::KEYBOARD ? "KEYBOARD" : "RADIACODE";
}
static std::vector<PickItem> pickItems() {
  std::vector<PickItem> v;
  bool configured = (pickKind == BleHub::KEYBOARD && kbd.state() != BleKeyboard::NONE) ||
                    (pickKind == BleHub::RADIACODE && rc.state() != BleRadiaCode::NONE);
  if (configured) v.push_back({ true, "(none)", "-- none: disconnect and forget --", "", 0, 0, 0 });
  if (pickKind == BleHub::MESHCORE) {             // radios: MeshCore and Meshtastic, selected by name
    std::vector<BleHub::Found> all = BleHub::found(BleHub::MESHCORE);
    for (const BleHub::Found& f : BleHub::found(BleHub::MESHTASTIC)) all.push_back(f);
    std::sort(all.begin(), all.end(), [](const BleHub::Found& a, const BleHub::Found& b) { return a.rssi > b.rssi; });
    for (const BleHub::Found& f : all) {
      bool mt = f.kind == BleHub::MESHTASTIC;
      v.push_back({ false, f.name, BleHub::label(f) + (mt ? " [Meshtastic]" : " [MeshCore]"), f.name, f.rssi,
                    f.addr.getType(), mt ? RK_MESHTASTIC : RK_MESHCORE });
    }
    return v;
  }
  for (const BleHub::Found& f : BleHub::found(pickKind)) {
    String addr = f.addr.toString().c_str();
    v.push_back({ false, addr, BleHub::label(f), f.name, f.rssi, f.addr.getType(), 0 });
  }
  return v;
}
static bool isCurrent(const PickItem& it) {
  if (it.none) return false;
  if (pickKind == BleHub::MESHCORE) return it.name == radio->radioName();
  if (pickKind == BleHub::KEYBOARD) return it.label == kbd.name();
  return it.label == rc.name();
}

// ---------------------------------------------------------------- rendering
static void renderPick() {
  int R = term.rows(), C = term.cols();
  std::vector<PickItem> items = pickItems();
  int real = 0;
  for (const PickItem& it : items) if (!it.none) real++;
  term.header(fit(String("SELECT ") + kindName(pickKind) + "   " + (real ? String(real) + " heard" : "scanning..."), C));
  bool canBack = pickKind != BleHub::MESHCORE || radio->radioName().length();
  term.footer(footerOr(canBack ? "knob1: move   click: pick   knob2: back" : "knob1: move   click: pick"));
  int sel = -1;
  for (size_t i = 0; i < items.size(); i++) if (items[i].key == pickSel) sel = i;
  if (sel < 0 && !items.empty()) { sel = 0; pickSel = items[0].key; }
  // help under the list: a keyboard that's still paired to something else (another PIP LINK, a phone, a PC)
  // only talks to that device until it's put in pairing mode - multi-device Logitech keyboards keep one
  // pairing per Easy-Switch key
  std::vector<String> help;
  if (pickKind == BleHub::KEYBOARD) {
    help = { "  Put the keyboard in pairing mode. Logitech: press an",
             "  Easy-Switch key (1-3), then hold it ~3 s until it",
             "  blinks fast. A keyboard paired to another PIP LINK,",
             "  phone or PC stays hidden until then. (Bluetooth LE",
             "  keyboards only)" };
  } else if (!real) {
    help = { "", pickKind == BleHub::MESHCORE ? "  listening for MeshCore and Meshtastic radios..." : "  turn the RadiaCode on",
             "", "  (it won't advertise while connected to your phone)" };
  }
  int h0 = items.size() + 1;                    // the help's first row
  if (h0 + (int)help.size() > R) help.clear();
  for (int r = 0; r < R; r++) {
    if (r >= (int)items.size()) {
      term.line(r, r >= h0 && r - h0 < (int)help.size() ? help[r - h0] : String(""), PipTerm::DIM);
      continue;
    }
    const PickItem& it = items[r];
    String row = it.none ? it.label
                         : fit(it.label, 34) + String(it.rssi) + " dBm" + (isCurrent(it) ? "  (current)" : "");
    term.line(r, row, r == sel ? (PipTerm::BRIGHT | PipTerm::INV) : PipTerm::NORMAL);
  }
}

static void renderPairKb() {
  int R = term.rows(), C = term.cols();
  term.header(fit("PAIR KEYBOARD :: " + kbd.name(), C));
  term.footer(footerOr("knob2: cancel"));
  String code = String(kbd.passkey());
  String spaced;
  for (size_t i = 0; i < code.length(); i++) spaced += String(code[i]) + "  ";
  for (int r = 0; r < R; r++) term.line(r, "", PipTerm::NORMAL);
  term.line(1, "  On the keyboard, type this code:", PipTerm::NORMAL);
  term.line(3, "             " + spaced, PipTerm::BRIGHT | PipTerm::INV);
  term.line(5, "  then press ENTER.", PipTerm::NORMAL);
  term.line(8, kbd.state() == BleKeyboard::CONNECTING ? "  connected - waiting for the code..." :
               "  waiting for the keyboard (make sure it's in pairing mode)", PipTerm::DIM);
}

static void renderSettings() {
  int R = term.rows(), C = term.cols();
  term.header(fit(tabBar(), C));
  term.footer(footerOr("knob1: move   click: change   knob2: tabs"));
  String rows[NSETTINGS] = {
    fit("LoRa radio", 16) + fit(radio->radioName().length() ? shortName(radio->radioName()) : String("(none)"), 24) + radioState(),
    fit("Keyboard", 16) + fit(kbd.name().length() ? kbd.name() : String("(none)"), 24) + kbdState(),
    fit("RadiaCode", 16) + fit(rc.name().length() ? rc.name() : String("(none)"), 24) + rcState(),
    fit("Rad alarm", 16) + (alarmIdx ? fmtRadH(ALARM_LEVELS[alarmIdx]) : String("OFF")) +
        (alarmIdx && !rcAlways ? "  (only on RADIATION tab)" : ""),
    fit("RadiaCode link", 16) + (rcAlways ? "ALWAYS (background alarm)" : "only on RADIATION tab"),
    fit("Message sound", 16) + (soundOn ? "ON" : "OFF"),
    fit("Tab wheel", 16) + String(tabSteps) + (tabSteps == 1 ? " click per tab" : " clicks per tab"),
    fit("Scroll wheel", 16) + String(scrollSteps) + (scrollSteps == 1 ? " click per step" : " clicks per step"),
    fit("Theme", 16) + (themeSel == 1 ? "GREEN" : themeSel == 2 ? "AMBER" : "PIP-BOY (its own colour setting)"),
    fit("Map locations", 16) + (showPlaces ? "SHOWN" : "HIDDEN") + "   (KML pins + your saved spots)",
    fit("Text margin", 16) + String(termMargin) + " px from the left edge (SSH / Telnet)",
  };
  for (int r = 0; r < R; r++) {
    if (r < NSETTINGS) term.line(r, "  " + rows[r], r == setSel ? (PipTerm::BRIGHT | PipTerm::INV) : PipTerm::NORMAL);
    else term.line(r, "", PipTerm::NORMAL);
  }
  if (radio->state() == MeshRadio::ONLINE && NSETTINGS < R)
    term.line(min(NSETTINGS + 1, R - 1), "  " + String(radio->contacts().size()) + " contacts, " + String(radio->channels().size()) + " channels on " + radio->selfName(), PipTerm::DIM);
}

// ---------------------------------------------------------------- radiation page
static String fmtRate(float v) {
  return String(v, v < 1 ? 3 : v < 10 ? 2 : v < 100 ? 1 : 0);
}
// shown in rads, like the game: the RadiaCode measures sieverts, and for gamma 1 Sv = 100 rad (1 uSv = 100 urad).
// urad / mrad / rad by size, e.g. background ~10 urad/h
static String fmtRad(float uSv) {
  float urad = uSv * 100;
  if (urad < 1000) return fmtRate(urad) + " urad";
  if (urad < 1000000) return fmtRate(urad / 1000) + " mrad";
  return fmtRate(urad / 1000000) + " rad";
}
static String fmtRadH(float uSvH) { return fmtRad(uSvH) + "/h"; }
static String fmtDose(float uSv) { return fmtRad(uSv); }
static String fmtSpan(uint32_t s) {
  if (s >= 86400) return String(s / 86400) + "d " + String(s % 86400 / 3600) + "h";
  if (s >= 3600) return String(s / 3600) + "h " + String(s % 3600 / 60) + "m";
  return String(s / 60) + "m";
}

static void renderRad() {
  int R = term.rows(), C = term.cols();
  bool fresh = rc.streaming() && rc.live().at && millis() - rc.live().at < 10000;
  String st = rc.state() == BleRadiaCode::NONE ? "not set up" : rc.streaming() ? "online" : rcState();
  term.header(fit(tabBar(), C));
  term.footer(footerOr("click: alarm level   knob2: tabs"));
  if (R < 12) return;
  if (rc.state() == BleRadiaCode::NONE) {
    for (int r = 0; r < R; r++) term.line(r, "", PipTerm::NORMAL);
    term.line(4, "  no RadiaCode selected - pick one in SETTINGS", PipTerm::DIM);
    term.line(6, "  (close the RadiaCode phone app first: it can only", PipTerm::DIM);
    term.line(7, "   talk to one device at a time)", PipTerm::DIM);
    return;
  }
  const RcLive& lv = rc.live();
  const RcRare& rr = rc.rare();
  String top = "  " + rc.name() + ": " + st + "   ";
  if (rr.battery >= 0) top += "battery " + String((int)(rr.battery + 0.5)) + "%   " + String(rr.tempC, 1) + "C";
  else top += fresh ? "" : "waiting for data...";
  term.line(0, top, PipTerm::DIM);
  term.big(1, 3, fresh ? fmtRadH(lv.doseRate) : String("--- rad/h"), PipTerm::BRIGHT);
  term.line(4, fresh ? "  count rate " + fit(String(lv.cps, 1) + " cps", 12) + " +/-" + String((int)(lv.cpsErr + 0.5)) +
                       "%     dose rate +/-" + String((int)(lv.doseRateErr + 0.5)) + "%" : String(""), PipTerm::NORMAL);
  term.line(5, rr.at ? "  total dose " + fmtDose(rr.dose) + "  over " + fmtSpan(rr.duration) : String(""), PipTerm::NORMAL);
  float lvl = ALARM_LEVELS[alarmIdx];
  if (alarming) term.line(6, "  !! ALARM !!  above " + fmtRadH(lvl), PipTerm::BRIGHT | PipTerm::INV);
  else term.line(6, "  alarm at   " + (alarmIdx ? fmtRadH(lvl) : String("OFF")), PipTerm::NORMAL);

  // graph: auto-scaled, never below 0.25 uSv/h (25 urad/h), alarm level shown as a dashed line
  float mx = 0.25f;
  for (float v : rateHist) mx = max(mx, v);
  if (alarmIdx && lvl <= mx * 2) mx = max(mx, lvl * 1.15f);
  std::vector<uint8_t> bars;
  for (int i = 0; i < HIST_BARS - (int)rateHist.size(); i++) bars.push_back(0);
  for (float v : rateHist) bars.push_back((uint8_t)constrain((int)(v / mx * 254 + 0.5f), 0, 254));
  uint8_t mark = (alarmIdx && lvl <= mx) ? (uint8_t)constrain((int)(lvl / mx * 254), 1, 254) : 0;
  term.bars(7, 4, bars, mark, alarming ? PipTerm::BRIGHT : PipTerm::NORMAL);
  term.line(11, "  last " + String(HIST_BARS * BUCKET_MS / 60000) + " min   top " + fmtRadH(mx) +
                (mark ? "   - - - alarm" : ""), PipTerm::DIM);
}

static void render();
static int tabLed();

// ---------------------------------------------------------------- map page
static double distKm(double la1, double lo1, double la2, double lo2) {
  double p = M_PI / 180, a = sin((la2 - la1) * p / 2), b = sin((lo2 - lo1) * p / 2);
  double h = a * a + cos(la1 * p) * cos(la2 * p) * b * b;
  return 12742.0 * asin(sqrt(h));
}
static double bearingDeg(double la1, double lo1, double la2, double lo2) {
  double p = M_PI / 180, y = sin((lo2 - lo1) * p) * cos(la2 * p);
  double x = cos(la1 * p) * sin(la2 * p) - sin(la1 * p) * cos(la2 * p) * cos((lo2 - lo1) * p);
  return fmod(atan2(y, x) / p + 360, 360);
}
static const char* compass(double deg) {
  static const char* d[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  return d[(int)((deg + 22.5) / 45) % 8];
}
static String fmtKm(double km) { return km < 1 ? String((int)(km * 1000)) + " m" : String(km, km < 10 ? 1 : 0) + " km"; }
static String ago(uint32_t secs) {
  if (secs < 90) return String(secs) + "s";
  if (secs < 5400) return String(secs / 60) + "m";
  if (secs < 172800) return String(secs / 3600) + "h";
  return String(secs / 86400) + "d";
}
static int contactIcon(uint8_t type) {          // indexes into tools/map_icons.py TYPES
  return type == 2 ? 7 /*RADIO*/ : type == 3 ? 2 /*TOWN*/ : type == 4 ? 21 /*INFO*/ : 22 /*PERSON*/;
}
struct Located { const McContact* c; double km; };
static std::vector<Located> locatedContacts(double fromLat, double fromLon) {
  std::vector<Located> v;
  for (const McContact& c : radio->contacts())
    if (c.lat != 0 || c.lon != 0) v.push_back({ &c, distKm(fromLat, fromLon, c.lat, c.lon) });
  std::sort(v.begin(), v.end(), [](const Located& a, const Located& b) { return a.km < b.km; });
  return v;
}
// half extent of the 480 x 264 px map in degrees (+30%) - which contacts are worth sending
static void viewSpan(double lat, int z, double& dLat, double& dLon) {
  double degPerPx = 360.0 / (256.0 * pow(2, z));
  dLon = 240 * degPerPx * 1.3;
  dLat = 132 * degPerPx * cos(lat * M_PI / 180) * 1.3;
}

// ---------------------------------------------------------------- saved locations (the GO TO list)
// PIPLINK/WPT.TXT on the Pip-Boy SD: one "lat,lon,ICON,name" per line; a line starting with '-' deletes an
// earlier one with the same name and spot. PIP LINK only ever appends to it (T.wadd); the PC setup app merges
// KMZ pins into it (never bringing back deleted ones) and can save it out as a KML.
static const char* ICON_NAMES[] = {                // = TYPES in tools/map_icons.py (same order as ICONS.BIN)
  "LANDMARK", "VAULT", "TOWN", "CAMP", "CAVE", "MILITARY", "FACTORY", "RADIO",
  "MEDICAL", "WATER", "DANGER", "HOME", "QUEST", "TARGET", "FLAG", "VEHICLE",
  "AIRPORT", "STORE", "DINER", "GAS", "NATURE", "INFO", "PERSON", "CAPITOL", "STADIUM" };
static const int NICONS = sizeof(ICON_NAMES) / sizeof(ICON_NAMES[0]);
static const int ICON_TARGET = 13, ICON_FLAG = 14;
struct Place { double lat = 0, lon = 0; int icon = 0; String name; };
static std::vector<Place> places;
static bool placesLoaded = false;
static Place puPlace;                         // the location a sub-screen is acting on (a copy)
static bool navOn = false;                    // NAV target                                  prefs "nav"
static Place navTo;

static int iconIndex(String s, int def) {
  s.trim();
  s.toUpperCase();
  if (s.length() && isDigit(s[0])) { int i = s.toInt(); return i >= 0 && i < NICONS ? i : def; }
  for (int i = 0; i < NICONS; i++) if (s == ICON_NAMES[i]) return i;
  return def;
}
static String placeName(const String& s) {    // safe in the file, in map markers and in a share
  String o;
  for (size_t i = 0; i < s.length() && o.length() < 24; i++) {
    char c = s[i];
    o += (c >= 32 && c < 127 && c != ';') ? c : ' ';
  }
  o.trim();
  return o;
}
static bool validPos(double la, double lo) { return la >= -90 && la <= 90 && lo >= -180 && lo <= 180 && !(la == 0 && lo == 0); }
static bool parsePlace(const String& line, Place& p) {
  int a = line.indexOf(','), b = line.indexOf(',', a + 1), c = line.indexOf(',', b + 1);
  if (a <= 0 || b < 0 || c < 0) return false;
  p.lat = line.substring(0, a).toDouble();
  p.lon = line.substring(a + 1, b).toDouble();
  p.icon = iconIndex(line.substring(b + 1, c), 0);
  p.name = placeName(line.substring(c + 1));
  return validPos(p.lat, p.lon) && p.name.length();
}
static String placeLine(const Place& p) {
  return String(p.lat, 6) + "," + String(p.lon, 6) + "," + ICON_NAMES[p.icon] + "," + p.name + "\n";
}
static bool samePlace(const Place& a, const Place& b) {
  return a.name.equalsIgnoreCase(b.name) && distKm(a.lat, a.lon, b.lat, b.lon) < 0.03;
}
static int findPlace(const Place& p) {
  for (size_t i = 0; i < places.size(); i++) if (samePlace(places[i], p)) return i;
  return -1;
}
static void parsePlaces(const String& text) {
  places.clear();
  int start = 0, deleted = 0;
  while (start < (int)text.length()) {
    int nl = text.indexOf('\n', start);
    if (nl < 0) nl = text.length();
    String line = text.substring(start, nl);
    start = nl + 1;
    line.trim();
    bool del = line.startsWith("-");
    Place p;
    if (!parsePlace(del ? line.substring(1) : line, p)) continue;
    int i = findPlace(p);
    if (del) { if (i >= 0) places.erase(places.begin() + i); deleted++; }
    else if (i < 0) places.push_back(p);
  }
  placesLoaded = true;
  pip.log("hub: %u saved locations (%d deleted)", (unsigned)places.size(), deleted);
}

// "[WPT] Pioneer Saloon @ 35.83250,-115.43190 DINER" - readable by anyone, picked up by other PIP LINKs
static String shareText(const Place& p) {
  return "[WPT] " + p.name + " @ " + String(p.lat, 5) + "," + String(p.lon, 5) + " " + ICON_NAMES[p.icon];
}
static bool parseShare(const String& t, Place& p) {
  int s = t.indexOf("[WPT] ");
  int at = s >= 0 ? t.indexOf(" @ ", s) : -1;
  if (at < 0) return false;
  String rest = t.substring(at + 3);
  rest.trim();
  int sp = rest.indexOf(' '), c = rest.indexOf(',');
  if (c <= 0) return false;
  p.lat = rest.substring(0, c).toDouble();
  p.lon = rest.substring(c + 1, sp > 0 ? sp : rest.length()).toDouble();
  p.icon = iconIndex(sp > 0 ? rest.substring(sp + 1) : String(""), ICON_FLAG);
  p.name = placeName(t.substring(s + 6, at));
  if (!p.name.length()) p.name = "SHARED";
  return validPos(p.lat, p.lon);
}

// distances: from you when the radio has a position, otherwise from the crosshair
static void refPos(double& la, double& lo) {
  bool me = radio->hasPosition();
  la = me ? radio->lat() : newLat;
  lo = me ? radio->lon() : newLon;
}
static String fmtDist(double km) {
  return km < 1 ? String((int)(km * 1000 + 0.5)) + " m" : String(km, km < 10 ? 2 : km < 100 ? 1 : 0) + " km";
}
static int azimuth(double la, double lo, const Place& p) { return (int)(bearingDeg(la, lo, p.lat, p.lon) + 0.5) % 360; }
static String azText(double la, double lo, const Place& p) {
  int az = azimuth(la, lo, p);
  return String(az) + "\xB0" " " + compass(az);
}

// the saved locations in list order: the NAV target first, then nearest first
static std::vector<int> placeOrder() {
  double la, lo;
  refPos(la, lo);
  std::vector<std::pair<double, int>> d;
  int star = -1;
  for (size_t i = 0; i < places.size(); i++) {
    if (navOn && star < 0 && samePlace(places[i], navTo)) { star = i; continue; }
    d.push_back({ distKm(la, lo, places[i].lat, places[i].lon), (int)i });
  }
  std::sort(d.begin(), d.end());
  std::vector<int> v;
  if (star >= 0) v.push_back(star);
  for (auto& x : d) v.push_back(x.second);
  return v;
}
static std::vector<Convo*> shareTargets() {    // channels (by number), then contacts (most recent first)
  std::vector<Convo*> v;
  for (Convo* c : convos) if (c->isCh) v.push_back(c);
  std::sort(v.begin(), v.end(), [](Convo* a, Convo* b) { return a->chan < b->chan; });
  for (Convo* c : convos) if (!c->isCh) v.push_back(c);
  return v;
}
static int puCount() {
  switch (pu) {
    case PU_LIST:  return 2 + places.size();
    case PU_ACT:   return 4;
    case PU_DEL:   return 2;
    case PU_SHARE: return shareTargets().size();
    case PU_ICON:  return NICONS;
    default:       return 0;
  }
}

struct UiRow { String text; int attr; int icon; };
// info rows on top, then a scrolling list of items with the puSel highlight
static void renderRows(const String& hdr, const String& foot, const std::vector<UiRow>& info, const std::vector<UiRow>& items,
                       int& sel, int& top) {
  int R = term.rows(), C = term.cols(), r0 = min((int)info.size(), R), avail = R - r0, n = items.size();
  term.header(fit(hdr, C));
  term.footer(footerOr(foot));
  for (int r = 0; r < r0; r++) term.line(r, info[r].text, info[r].attr, info[r].icon);
  sel = constrain(sel, 0, max(0, n - 1));
  if (sel < top) top = sel;
  if (sel >= top + avail) top = sel - avail + 1;
  top = max(0, min(top, n - avail));
  for (int r = 0; r < avail; r++) {
    int i = top + r;
    if (i >= n) { term.line(r0 + r, "", PipTerm::NORMAL); continue; }
    term.line(r0 + r, items[i].text, i == sel ? (PipTerm::BRIGHT | PipTerm::INV) : items[i].attr, items[i].icon);
  }
}

static void renderPlaces() {
  std::vector<UiRow> info, items;
  bool me = radio->hasPosition();
  double la, lo;
  refPos(la, lo);
  const UiRow blank = { "", PipTerm::NORMAL, -1 };
  switch (pu) {
    case PU_LIST: {
      items.push_back({ "   NEW            save the spot under the crosshair", PipTerm::BRIGHT, ICON_FLAG });
      items.push_back({ "   MANUAL ENTRY   type a latitude,longitude", PipTerm::BRIGHT, ICON_TARGET });
      for (int i : placeOrder()) {
        const Place& p = places[i];
        bool star = navOn && samePlace(p, navTo);
        items.push_back({ "   " + String(star ? "*" : " ") + fit(p.name, 25) + fit(fmtDist(distKm(la, lo, p.lat, p.lon)), 10) +
                          compass(bearingDeg(la, lo, p.lat, p.lon)), star ? PipTerm::BRIGHT : PipTerm::NORMAL, p.icon });
      }
      String hdr = "GO TO   " + (placesLoaded ? String(places.size()) + (places.size() == 1 ? " location" : " locations")
                                              : String("loading locations...")) +
                   (me ? "   (distance from you)" : "   (distance from the crosshair)");
      renderRows(hdr, placesLoaded && places.empty() ? "no saved locations yet   knob2: back to the map"
                                                     : "knob1: move   click: pick   knob2: back to the map", info, items, puSel, puTop);
      return;
    }
    case PU_ACT: {
      bool navHere = navOn && samePlace(navTo, puPlace);
      info.push_back({ "   " + puPlace.name, PipTerm::BRIGHT, puPlace.icon });
      info.push_back({ "   " + String(puPlace.lat, 5) + "," + String(puPlace.lon, 5) +
                       "   " + fmtDist(distKm(la, lo, puPlace.lat, puPlace.lon)) + "  " + azText(la, lo, puPlace) +
                       (me ? "" : "  from the crosshair"), PipTerm::DIM, -1 });
      info.push_back(blank);
      items.push_back({ "   GO TO      centre the map here", PipTerm::NORMAL, -1 });
      items.push_back({ navHere ? "   STOP NAV" : "   NAV        dotted line, distance + azimuth from you", PipTerm::NORMAL, -1 });
      items.push_back({ "   SHARE      send it to a channel or contact", PipTerm::NORMAL, -1 });
      items.push_back({ "   DELETE", PipTerm::NORMAL, -1 });
      renderRows("GO TO :: " + puPlace.name, "knob1: move   click: pick   knob2: back", info, items, puSel, puTop);
      return;
    }
    case PU_DEL:
      info.push_back({ "   Delete " + puPlace.name + "?", PipTerm::BRIGHT, puPlace.icon });
      info.push_back(blank);
      items.push_back({ "   NO  - keep it", PipTerm::NORMAL, -1 });
      items.push_back({ "   YES - delete it", PipTerm::NORMAL, -1 });
      renderRows("DELETE :: " + puPlace.name, "knob1: move   click: pick   knob2: back", info, items, puSel, puTop);
      return;
    case PU_SHARE: {
      info.push_back({ "   Send " + puPlace.name + " to:", PipTerm::BRIGHT, puPlace.icon });
      std::vector<Convo*> t = shareTargets();
      info.push_back(t.empty() ? UiRow{ "   (no channels or contacts yet - is the radio connected?)", PipTerm::DIM, -1 } : blank);
      for (Convo* c : t) items.push_back({ String(c->isCh ? "   channel   " : "   contact   ") + c->name, PipTerm::NORMAL, -1 });
      renderRows("SHARE :: " + puPlace.name, radio->state() == MeshRadio::ONLINE ? "knob1: move   click: send   knob2: back"
                                                                                : "radio " + radioState() + "   knob2: back", info, items, puSel, puTop);
      return;
    }
    case PU_ICON:
      for (int i = 0; i < NICONS; i++) items.push_back({ String("   ") + ICON_NAMES[i], PipTerm::NORMAL, i });
      renderRows("ICON for " + newName, "knob1: move   click: save it here   knob2: cancel", info, items, puSel, puTop);
      return;
    default: return;
  }
}

// ---------------------------------------------------------------- TERM tab: state + screens
// A Telnet session to a computer on the Wi-Fi network, shown through a VT100 emulator (lib/remote_term).
// Wi-Fi credentials and the computer list live in NVS (prefs "wifi_ssid" / "wifi_pass" / "hosts").
static VtScreen vt;
static TelnetCodec telnet;
static WiFiClient tcp;
static SshLink ssh;
static bool tsSsh = false;                    // this session is SSH (else Telnet)
static String tsUser, tsKey;                  // SSH login name; "host:port" for the saved host key
static bool termAsk = false;                  // asking: accept this computer's key?
static int termAskSel = 0;                    // 0 accept, 1 reject
enum TermState { TS_IDLE, TS_CONNECTING, TS_ONLINE, TS_CLOSED };
static TermState ts = TS_IDLE;
static String tsHost;                         // "pi4.local:23" while a session exists
static String tsInfo;                         // why a session ended
enum TermUi { TU_LIST, TU_WIFI, TU_HOST };    // screens while no session is shown
static TermUi tu = TU_LIST;
static int tuSel = 0, tuTop = 0, tuHost = 0, tuWifiSel = 0, tuWifiTop = 0, tuHostSel = 0, tuHostTop = 0;

// a saved computer: "user@host[:port]" = SSH (port 22), "host[:port]" = Telnet (port 23), then optionally
// " bbs" / " std": BBS mode (ANSI terminal type, PC characters, 80x24) - the default for Telnet - or standard
struct HostEntry { bool ssh, bbs; String user, host; uint16_t port; };
static String entryBase(const String& e) { int sp = e.indexOf(' '); return sp > 0 ? e.substring(0, sp) : e; }
static HostEntry parseEntry(const String& full) {
  HostEntry h;
  String e = entryBase(full), flags = full.substring(e.length());
  int at = e.indexOf('@');
  h.ssh = at > 0;
  h.user = h.ssh ? e.substring(0, at) : String("");
  String rest = h.ssh ? e.substring(at + 1) : e;
  int colon = rest.lastIndexOf(':');
  h.host = colon > 0 ? rest.substring(0, colon) : rest;
  h.port = colon > 0 ? (uint16_t)rest.substring(colon + 1).toInt() : 0;
  if (!h.port) h.port = h.ssh ? 22 : 23;
  h.bbs = flags.indexOf("bbs") >= 0 || (!h.ssh && flags.indexOf("std") < 0);
  return h;
}
static String entryLabel(const String& e) {
  HostEntry h = parseEntry(e);
  return String(h.ssh ? "ssh    " : "telnet ") + (h.bbs ? "BBS " : "    ") + (h.ssh ? h.user + "@" : String("")) + h.host +
         (h.port != (h.ssh ? 22 : 23) ? ":" + String(h.port) : String(""));
}
static bool termMenu = false;                 // session footer menu
static int termMenuSel = 0;
static const char* TERM_MENU[] = { "TYPE", "CTRL-C", "ESC", "TAB", "SIZE", "WHEEL", "DISCONNECT" };
static const int NTERMMENU = 7;
enum { TM_TYPE, TM_CTRLC, TM_ESC, TM_TAB, TM_SIZE, TM_WHEEL, TM_DISCONNECT };
// text sizes for the TERM canvas (same table as VT_SIZES in the holotape)
// (3 = BBS: 80 columns fill the whole width, so with a margin the last column runs off the right edge -
// BBS screens rarely use it, since writing there makes many terminals wrap)
static const int VT_COLS[] = { 58, 66, 78, 80 }, VT_ROWS[] = { 12, 16, 24, 24 };
static const char* VT_NAMES[] = { "LARGE 58x12", "MEDIUM 66x16", "SMALL 78x24", "BBS 80x24" };
static int termSize = 0;                      // prefs "term_size" (0-2; BBS mode always uses 3)
static bool tsBbs = false;                    // this session is in BBS mode (always 80x24)
static int canvasSize() { return tsBbs ? 3 : termSize; }
static int termBack = 0;                      // lines scrolled back (0 = the live screen)
static bool termRedraw = false;               // the screen needs drawing (termPoll does it when the link is free)
static bool termArrows = false;               // wheel sends Up/Down instead of scrolling back  prefs "term_wheel"
static std::vector<String> hosts;             // "host:port", most recent first
static String wifiSsid, wifiPass;
static bool wifiOn = false, wifiScanning = false;
struct WifiNet { String ssid; int rssi; bool open; };
static std::vector<WifiNet> wifiNets;
static String pendingSsid;

static String wifiStatus() {
  if (!wifiSsid.length()) return "Wi-Fi: not set up - pick a network below";
  if (!wifiOn) return "Wi-Fi: " + wifiSsid + " (off)";
  if (WiFi.status() == WL_CONNECTED) return "Wi-Fi: " + wifiSsid + "   this unit is " + WiFi.localIP().toString();
  return "Wi-Fi: " + wifiSsid + "   connecting...";
}

static void renderTermTab() {
  int C = term.cols(), R = term.rows();
  if (ts != TS_IDLE) {                        // the remote screen
    term.header(fit(tabBar(), C));
    String foot;
    if (termAsk) {
      foot = String(termAskSel == 0 ? "[ACCEPT KEY]   REJECT " : " ACCEPT KEY   [REJECT]") + "   knob2: choose   click: answer";
    } else if (termBack > 0) {
      foot = "SCROLLBACK  " + String(termBack) + " of " + String(vt.historySize()) + " lines up   click: back to live";
    } else if (termMenu) {
      for (int i = 0; i < NTERMMENU; i++) foot += i == termMenuSel ? "[" + String(TERM_MENU[i]) + "] " : " " + String(TERM_MENU[i]) + " ";
    } else if (ts == TS_CONNECTING) {
      SshLink::State ss = ssh.state();
      foot = !tsSsh ? "connecting to " + tsHost + "..."
           : ss == SshLink::LOGGING_IN ? String("logging in...")
           : ss == SshLink::ASK_PASSWORD ? String("waiting for the password")
           : "SSH: connecting to " + tsHost + "...   click: cancel";
    } else if (ts == TS_CLOSED) foot = tsInfo + "   click: back to the list";
    else foot = String(tsSsh ? "ssh " + tsUser + "@" : String("telnet ")) + tsHost + (kbd.state() == BleKeyboard::ONLINE ? "  [KB]" : "") +
                (termArrows ? "   knob1: up/down keys" : "   knob1: scroll back") + "   click: menu";
    term.footer(footerOr(foot));
    for (int r = 0; r < vt.rows(); r++)
      term.vtLine(r, String((termBack > 0 ? vt.viewRow(r, termBack) : vt.row(r)).c_str()));
    term.cursor(ts == TS_ONLINE && termBack == 0 && vt.cursorVisible() ? vt.cursorRow() : -1, vt.cursorCol());
    vt.clearDirty();
    return;
  }
  term.cursor(-1, 0);
  std::vector<UiRow> info, items;
  const UiRow blank = { "", PipTerm::NORMAL, -1 };
  switch (tu) {
    case TU_LIST:
      info.push_back({ "  " + wifiStatus(), PipTerm::DIM, -1 });
      info.push_back(blank);
      for (const String& h : hosts) items.push_back({ "   " + entryLabel(h), PipTerm::NORMAL, -1 });
      items.push_back({ "   NEW CONNECTION    user@computer for SSH", PipTerm::BRIGHT, -1 });
      items.push_back({ "   WI-FI NETWORK     " + (wifiSsid.length() ? wifiSsid : String("pick one")), PipTerm::BRIGHT, -1 });
      renderRows(tabBar(), "knob1: move   click: pick   knob2: tabs", info, items, tuSel, tuTop);
      return;
    case TU_WIFI:
      info.push_back({ wifiScanning ? "  looking for Wi-Fi networks..." : "  Pick your Wi-Fi network:", PipTerm::DIM, -1 });
      info.push_back(blank);
      for (const WifiNet& w : wifiNets)
        items.push_back({ "   " + fit(w.ssid, 32) + fit(String(w.rssi) + " dBm", 9) + (w.open ? "open" : ""), PipTerm::NORMAL, -1 });
      items.push_back({ "   (type the name of a hidden network)", PipTerm::DIM, -1 });
      renderRows("WI-FI NETWORKS", "knob1: move   click: pick   knob2: back", info, items, tuWifiSel, tuWifiTop);
      return;
    case TU_HOST: {
      String e = tuHost < (int)hosts.size() ? hosts[tuHost] : String("");
      info.push_back({ "   " + entryLabel(e), PipTerm::BRIGHT, -1 });
      info.push_back(blank);
      HostEntry he = parseEntry(e);
      items.push_back({ "   CONNECT", PipTerm::NORMAL, -1 });
      items.push_back({ "   DELETE", PipTerm::NORMAL, -1 });
      items.push_back({ he.bbs ? "   MODE: BBS       (ANSI art, PC characters, 80x24)"
                               : "   MODE: STANDARD  (VT100, UTF-8 - Linux / a Pi)", PipTerm::NORMAL, -1 });
      if (he.ssh) items.push_back({ "   FORGET KEY   (after the computer was reinstalled)", PipTerm::NORMAL, -1 });
      renderRows("TERM :: " + entryLabel(e), "knob1: move   click: pick   knob2: back", info, items, tuHostSel, tuHostTop);
      return;
    }
  }
}

static bool mapPending = false;                 // a map redraw is waiting for the Pip-Boy to finish the last one
static void renderMap() {
  // a screen over an empty part of the map takes the Pip-Boy 2-3 s; fast knob turns used to stack new views
  // behind it until its input buffer overflowed (a command lost characters -> SyntaxError). Send the newest
  // view only once the last one has been taken - loop() calls render() again then.
  if (pip.queued() > 0) { mapPending = true; return; }
  mapPending = false;
  int C = term.cols();
  bool me = radio->hasPosition();
  double myLat = me ? radio->lat() : mapDefLat, myLon = me ? radio->lon() : mapDefLon;
  std::vector<Located> loc = locatedContacts(myLat, myLon);
  if (mapFocus > (int)loc.size()) mapFocus = loc.empty() ? 0 : 1;
  if (mapMode == MM_FIND && mapFocus) { viewLat = loc[mapFocus - 1].c->lat; viewLon = loc[mapFocus - 1].c->lon; mapFollow = false; }
  double cLat = mapFollow ? myLat : viewLat, cLon = mapFollow ? myLon : viewLon;

  term.header(fit(tabBar(), C));
  String pos = "z" + String(mapZoom) + "  ";
  if (me) pos += String(radio->lat(), 5) + "," + String(radio->lon(), 5) + "  fix " + ago((millis() - radio->positionAt()) / 1000) + " ago";
  else pos += radio->state() == MeshRadio::ONLINE ? String("radio has no position") : "radio " + radioState();
  String foot;
  if (mapMenu) {                                // footer menu: knob1 picks, click chooses
    for (int i = 0; i < NMAPMENU; i++) foot += i == mapMenuSel ? "[" + String(MAP_MENU[i]) + "] " : " " + String(MAP_MENU[i]) + "  ";
  } else if (!mapHasData) {
    foot = "no map pack on the SD - see tools/map_build.py";
  } else if (mapMode == MM_FIND && mapFocus) {
    const McContact* c = loc[mapFocus - 1].c;
    uint32_t now = radio->now();
    foot = String(mapFocus) + "/" + String(loc.size()) + " " + c->name + "  " + fmtKm(loc[mapFocus - 1].km) + " " +
           compass(bearingDeg(myLat, myLon, c->lat, c->lon)) +
           (now && c->lastAdvert && now > c->lastAdvert ? "  heard " + ago(now - c->lastAdvert) : String("")) + "  click: menu";
  } else if (navOn && mapMode == MM_ZOOM) {
    foot = "NAV *" + navTo.name + "  " + (me ? fmtDist(distKm(myLat, myLon, navTo.lat, navTo.lon)) + "  " + azText(myLat, myLon, navTo)
                                           : String("- waiting for your GPS position")) + "   click: menu";
  } else {
    // following: zoom + our fix; panned away: how far the view is from us (keeps the footer short)
    String away = me ? fmtKm(distKm(myLat, myLon, cLat, cLon)) + " " + compass(bearingDeg(myLat, myLon, cLat, cLon)) : String("");
    if (mapMode == MM_PAN)
      foot = "PAN  wheel N/S  knob2 E/W  click: done" + (me ? "   " + away : String(""));
    else
      foot = String(mapMode == MM_FIND ? "CONTACTS (none)" : "ZOOM") + "  " +
             (mapFollow || !me ? pos : "z" + String(mapZoom) + "  view " + away + " of you") + "  click: menu";
  }
  term.footer(footerOr(foot));

  double dLat, dLon;
  viewSpan(cLat, mapZoom, dLat, dLon);
  // markers, most important first; the whole list stays well inside the Pip-Boy's 2 KB input buffer
  String dyn;
  auto add = [&](const String& e) { if (dyn.length() + e.length() > 1100) return false; dyn += e; return true; };
  auto inView = [&](double la, double lo) { return fabs(la - cLat) <= dLat && fabs(lo - cLon) <= dLon; };
  if (me) add(String(radio->lat(), 6) + "," + String(radio->lon(), 6) + ",-1," + String((int)heading) + ",;");
  if (navOn) {
    if (me) add(String(navTo.lat, 6) + "," + String(navTo.lon, 6) + ",-3,0," +
                fmtDist(distKm(myLat, myLon, navTo.lat, navTo.lon)) + "  " + String(azimuth(myLat, myLon, navTo)) + "\xB0" ";");
    add(String(navTo.lat, 6) + "," + String(navTo.lon, 6) + "," + String(navTo.icon) + ",-1,*" + navTo.name.substring(0, 16) + ";");
  }
  if (mapMode == MM_FIND && mapFocus) {
    const McContact* c = loc[mapFocus - 1].c;
    String label = c->name.substring(0, 16);
    label.replace(";", " ");
    add(String(c->lat, 5) + "," + String(c->lon, 5) + "," + String(contactIcon(c->type)) + ",-1," + label + ";");
  }
  if (showPlaces) {
    std::vector<std::pair<double, int>> vis;
    for (size_t i = 0; i < places.size(); i++)
      if (inView(places[i].lat, places[i].lon) && !(navOn && samePlace(places[i], navTo)))
        vis.push_back({ distKm(cLat, cLon, places[i].lat, places[i].lon), (int)i });
    std::sort(vis.begin(), vis.end());
    for (auto& v : vis) {
      const Place& p = places[v.second];
      if (!add(String(p.lat, 5) + "," + String(p.lon, 5) + "," + String(p.icon) + ",-1," +
               (mapZoom >= 15 ? p.name.substring(0, 14) : String("")) + ";")) break;
    }
  }
  int n = 0;
  for (size_t i = 0; i < loc.size() && n < 24; i++) {
    const McContact* c = loc[i].c;
    if (!inView(c->lat, c->lon) || (mapMode == MM_FIND && mapFocus && (int)i == mapFocus - 1)) continue;
    String label = mapZoom >= 14 ? c->name.substring(0, 16) : String("");
    label.replace(";", " ");
    if (!add(String(c->lat, 5) + "," + String(c->lon, 5) + "," + String(contactIcon(c->type)) + ",-1," + label + ";")) break;
    n++;
  }
  // 1 = crosshair at the view centre, 2 = panning: the Pip-Boy scrolls the picture instead of redrawing it
  term.mapView(cLat, cLon, mapZoom, dyn, (mapFollow ? 0 : 1) | (mapMode == MM_PAN && !mapMenu ? 2 : 0));
}

// heading from successive fixes, once we've moved far enough (15 m) to trust it
static void updateFix() {
  if (!radio->hasPosition() || radio->positionAt() == fixSeen) return;
  fixSeen = radio->positionAt();
  double la = radio->lat(), lo = radio->lon();
  if (fixLat == 0 && fixLon == 0) { fixLat = la; fixLon = lo; }
  else if (distKm(fixLat, fixLon, la, lo) >= 0.015) {
    heading = bearingDeg(fixLat, fixLon, la, lo);
    fixLat = la; fixLon = lo;
  }
  if (view == LIST && page == PG_MAP) render();
}

static void renderList() {
  if (page == PG_SET) { renderSettings(); return; }
  if (page == PG_TERM) { renderTermTab(); return; }
  if (page == PG_RAD) { renderRad(); return; }
  if (page == PG_MAP) { if (pu != PU_NONE) renderPlaces(); else renderMap(); return; }
  std::vector<Convo*> vis = visible();
  int R = term.rows(), C = term.cols();
  term.header(fit(tabBar(), C));
  term.footer(footerOr(radioStatus() + "   knob1: move / open"));
  // rows: a CHANNELS heading, the channels, a FAVORITES heading + favourites (if any), then the CONTACTS
  // entry (headings: c == nullptr)
  std::vector<Convo*> rows;
  std::vector<String> heads;
  auto heading = [&](const String& t) { rows.push_back(nullptr); heads.push_back(t); };
  int nCh = 0, nFav = 0, nContacts = 0, unreadDm = 0;
  for (Convo* c : vis) { if (c == &contactsRow || c == &advertRow) continue; if (c->isCh) nCh++; else nFav++; }
  for (Convo* c : convos) if (!c->isCh) { nContacts++; unreadDm += c->unread; }
  heading("  CHANNELS (" + String(nCh) + ")");
  for (int i = 0; i < nCh; i++) rows.push_back(vis[i]);
  if (nFav) heading("  FAVORITES");
  for (int i = nCh; i < nCh + nFav; i++) rows.push_back(vis[i]);
  rows.push_back(&contactsRow);
  rows.push_back(&advertRow);
  int sel = -1;
  for (size_t i = 0; i < rows.size(); i++) if (rows[i] && rows[i] == selC) sel = i;
  if (sel < 0) { for (size_t i = 0; i < rows.size(); i++) if (rows[i]) { sel = i; selC = rows[i]; break; } }
  if (sel - 1 < topRow) topRow = rows[sel - 1] ? sel : sel - 1;    // keep the heading above the first entry in view
  if (sel >= topRow + R) topRow = sel - R + 1;
  if (topRow > (int)rows.size() - R) topRow = max(0, (int)rows.size() - R);
  for (int r = 0; r < R; r++) {
    int i = topRow + r;
    if (i >= (int)rows.size()) { term.line(r, "", PipTerm::NORMAL); continue; }
    if (!rows[i]) {
      int k = 0;
      for (int j = 0; j < i; j++) if (!rows[j]) k++;
      term.line(r, heads[k], PipTerm::DIM);
      continue;
    }
    if (rows[i] == &advertRow) {
      term.line(r, fit("ADVERTISE", 22) + "  tell the mesh you're here now",
                rows[i] == selC ? (PipTerm::BRIGHT | PipTerm::INV) : PipTerm::NORMAL);
      continue;
    }
    if (rows[i] == &contactsRow) {
      term.line(r, fit("CONTACTS (" + String(nContacts) + ")", 22) + " " + (unreadDm ? "(" + String(unreadDm) + ")" : String("")),
                rows[i] == selC ? (PipTerm::BRIGHT | PipTerm::INV) : unreadDm ? PipTerm::BRIGHT : PipTerm::NORMAL);
      continue;
    }
    Convo* c = rows[i];
    String unread = c->unread ? "(" + String(c->unread) + ")" : "";
    String preview;
    if (!c->msgs.empty()) {
      const Msg& m = c->msgs.back();
      preview = (m.out ? String("you") : m.from) + ": " + m.text;
    }
    int attr = c == selC ? (PipTerm::BRIGHT | PipTerm::INV) : c->unread ? PipTerm::BRIGHT : PipTerm::NORMAL;
    term.line(r, fit(c->name, 22) + " " + fit(unread, 5) + " " + preview, attr);
  }
}

static String inputLine(int width) {                 // "> text_" scrolled to keep the cursor visible
  String shown = inputBuf;
  if (inputId == "wifipass" || inputId == "sshpass") { shown = ""; for (size_t i = 0; i < inputBuf.length(); i++) shown += '*'; }
  String s = "> " + shown + "_";
  return (int)s.length() > width ? s.substring(s.length() - width) : s;
}

static void renderChat() {
  Convo* c = active;
  int R = term.rows(), C = term.cols();
  bool typing = inputActive && inputId == "msg";
  int msgRows = typing ? R - 1 : R;
  String snr = c->hasSnr ? "  snr " + String(c->snr, 1) : "";
  term.header(fit(c->name + snr + "   " + radioStatus(), C));
  term.footer(footerOr(typing ? "ENTER: send   ESC: cancel   BACKSPACE: edit" : "knob1: scroll   click: reply   knob2: back"));
  struct L { String t; int a; };
  std::vector<L> lines;
  for (const Msg& m : c->msgs) {
    std::vector<String> w = wrap(stamp(m.ts) + " " + (m.out ? String("you") : m.from) + ": " + m.text, C, "      ");
    for (const String& s : w) lines.push_back({ s, m.out ? PipTerm::NORMAL : PipTerm::BRIGHT });   // received bright, yours dimmer
  }
  int total = lines.size();
  chatBack = constrain(chatBack, 0, max(0, total - msgRows));
  int start = max(0, total - msgRows - chatBack);
  for (int r = 0; r < msgRows; r++) {
    int i = start + r;
    if (i < total) term.line(r, lines[i].t, lines[i].a);
    else term.line(r, "", PipTerm::NORMAL);
  }
  if (lines.empty()) term.line(msgRows / 2 - 1, "  no messages yet - click (or start typing) to write one", PipTerm::DIM);
  if (typing) term.line(R - 1, inputLine(C), PipTerm::BRIGHT | PipTerm::INV);
}

static void renderPrompt() {
  int R = term.rows(), C = term.cols();
  term.header(fit(inputTitle, C));
  term.footer(footerOr("type on the keyboard    ENTER: ok    ESC: cancel"));
  for (int r = 0; r < R; r++) term.line(r, "", PipTerm::NORMAL);
  term.line(3, "  " + inputLine(C - 4), PipTerm::BRIGHT | PipTerm::INV);
}

// ---------------------------------------------------------------- contacts page + one contact's options
static const McContact* radioContact(const Convo* c) {
  for (const McContact& m : radio->contacts()) if (memcmp(m.prefix, c->prefix, 6) == 0) return &m;
  return nullptr;
}
static String contactInfo(const Convo* c) {             // "RPTR  heard 3h ago  2.4 km NE"
  const McContact* m = radioContact(c);
  if (!m) return "not in the radio's contacts";
  String t = m->type == 2 ? "RPTR" : m->type == 3 ? "ROOM" : m->type == 4 ? "SENS" : "CHAT";
  uint32_t now = radio->now();
  if (now && m->lastAdvert && now > m->lastAdvert) t += "  heard " + ago(now - m->lastAdvert) + " ago";
  if ((m->lat != 0 || m->lon != 0) && radio->hasPosition())
    t += "  " + fmtKm(distKm(radio->lat(), radio->lon(), m->lat, m->lon)) + " " + compass(bearingDeg(radio->lat(), radio->lon(), m->lat, m->lon));
  return t;
}
static void renderContacts() {
  std::vector<Convo*> cs = contactConvos();
  std::vector<UiRow> info, items;
  for (Convo* c : cs)
    items.push_back({ String(isFav(c->prefix) ? " * " : "   ") + fit(c->name, 18) + " " + fit(c->unread ? "(" + String(c->unread) + ")" : String(""), 5) +
                      contactInfo(c), c->unread ? PipTerm::BRIGHT : PipTerm::NORMAL, -1 });
  if (items.empty()) info.push_back({ "  no contacts yet", PipTerm::DIM, -1 });
  renderRows("MESH :: CONTACTS (" + String(cs.size()) + ")   * = favorite", "knob1: move   click: options   knob2: back",
             info, items, ctSel, ctTop);
}
static const int CM_MESSAGE = 0, CM_FAV = 1, CM_REMOVE = 2;
static void renderContactMenu() {
  if (!ctC) { view = CONTACTS; renderContacts(); return; }
  std::vector<UiRow> info, items;
  info.push_back({ "  " + ctC->name + (isFav(ctC->prefix) ? "  *" : ""), PipTerm::BRIGHT, -1 });
  info.push_back({ "  " + contactInfo(ctC), PipTerm::DIM, -1 });
  info.push_back({ "", PipTerm::NORMAL, -1 });
  items.push_back({ "   MESSAGE", PipTerm::NORMAL, -1 });
  items.push_back({ isFav(ctC->prefix) ? "   UNFAVORITE" : "   FAVORITE      (list it on the MESH tab)", PipTerm::NORMAL, -1 });
  items.push_back({ cmConfirm ? "   REALLY REMOVE IT FROM THE RADIO? click again" : "   REMOVE FROM RADIO", PipTerm::NORMAL, -1 });
  renderRows("MESH :: " + ctC->name, "knob1: move   click: pick   knob2: back", info, items, cmSel, cmTop);
}
static void openChat(Convo* c, View from) {
  active = c;
  active->unread = 0;
  chatBack = 0;
  chatFrom = from;
  view = CHAT;
}

static void render() {
  if (!term.ready() || term.keyboardOpen()) return;
  if (!(view == LIST && page == PG_TERM)) term.cursor(-1, 0);
  term.vtMode(view == LIST && page == PG_TERM && ts != TS_IDLE ? canvasSize() : -1, termMargin);
  term.mapMode(view == LIST && page == PG_MAP && pu == PU_NONE);
  switch (view) {
    case PICK:   renderPick(); break;
    case PAIRKB: renderPairKb(); break;
    case PROMPT: renderPrompt(); break;
    case CONTACTS: renderContacts(); break;
    case CMENU:  renderContactMenu(); break;
    case CHAT:   if (active) { renderChat(); break; } view = LIST;  // fall through
    default:     view = LIST; renderList(); break;
  }
}

// ---------------------------------------------------------------- text entry
static void onTextResult(const String& id, bool ok, const String& text);
static void beginInput(const String& id, const String& title, const String& init) {
  if (kbd.state() == BleKeyboard::ONLINE) {         // physical keyboard: edit on the ESP side
    inputActive = true;
    inputId = id; inputTitle = title; inputBuf = init;
    if (id != "msg") { inputReturn = view; view = PROMPT; }
    render();
  } else {
    term.keyboard(id, title, init);                 // fall back to the Pip-Boy's on-screen keyboard
  }
}
static void endInput(bool ok) {
  String id = inputId, text = inputBuf;
  inputActive = false;
  if (view == PROMPT) view = inputReturn;
  onTextResult(id, ok, text);
}

// ---------------------------------------------------------------- saved locations: loading, editing, sharing
static String wptBuf;
static int wptTries = 0;
static void requestPlaces() { wptBuf = ""; term.loadPlaces(); }
static void onTermOther(const String& ev) {           // WPF = the locations file coming in, WAD = append result
  if (ev.startsWith("WPF END")) {
    size_t n = (size_t)ev.substring(8).toInt();
    if (n != wptBuf.length()) {                       // a line went missing: read it again
      pip.log("hub: locations file: got %u of %u bytes", (unsigned)wptBuf.length(), (unsigned)n);
      if (++wptTries <= 3) requestPlaces();
      return;
    }
    wptTries = 0;
    parsePlaces(wptBuf);
    wptBuf = "";
    render();
    return;
  }
  if (ev.startsWith("WPF ")) { wptBuf += PipLink::b64dec(ev.substring(4)); return; }
  if (ev.startsWith("WAD ") && ev.substring(4) != "1") note("couldn't save to the Pip-Boy SD card!");
  if (ev.startsWith("MEM ")) pip.log("pip: %s memory blocks free", ev.substring(4).c_str());
}
static void savePlace(const Place& p) { places.push_back(p); term.appendPlaces(placeLine(p)); }
static void setNav(const Place* p) {
  navOn = p != nullptr;
  if (p) { navTo = *p; prefs.putString("nav", placeLine(*p)); }
  else prefs.remove("nav");
}
static void deletePlace(const Place& p) {
  int i = findPlace(p);
  if (i >= 0) places.erase(places.begin() + i);
  term.appendPlaces("-" + placeLine(p));
  if (navOn && samePlace(navTo, p)) setNav(nullptr);
}
// send a text to a channel / contact and keep it in that conversation
static bool sendTo(Convo* c, const String& text) {
  bool sent = c->isCh ? radio->sendChannel(c->chan, text) : radio->sendDirect(c->prefix, text);
  if (!sent) return false;
  c->msgs.push_back({ true, "you", text, radio->now() });
  histAppend(c, c->msgs.back());
  while (c->msgs.size() > MAX_MSGS) c->msgs.pop_front();
  bump(c);
  pip.log("sent [%s]: %s", c->name.c_str(), text.c_str());
  return true;
}
static void receivedPlace(const Place& p, const String& from) {
  if (findPlace(p) >= 0) { pip.log("hub: shared location %s is already saved", p.name.c_str()); return; }
  savePlace(p);
  String where;
  if (radio->hasPosition()) where = "  " + fmtDist(distKm(radio->lat(), radio->lon(), p.lat, p.lon)) + " " +
                                    compass(bearingDeg(radio->lat(), radio->lon(), p.lat, p.lon));
  note("NEW LOCATION " + p.name + " from " + from + where + "  (press ITEMS)", 12000);
  placeAlert = true; alertLat = p.lat; alertLon = p.lon; alertName = p.name;
  term.play(WPT_SOUND);
  pip.log("hub: location %s from %s added", p.name.c_str(), from.c_str());
}

static void openPlaces() {
  bool me = radio->hasPosition();
  newLat = mapFollow ? (me ? radio->lat() : mapDefLat) : viewLat;
  newLon = mapFollow ? (me ? radio->lon() : mapDefLon) : viewLon;
  pu = PU_LIST;
  puSel = puTop = 0;
  mapMenu = false;
  if (!placesLoaded) requestPlaces();
  term.sound("SELECT");
  render();
}
static void placesClick() {
  switch (pu) {
    case PU_LIST: {
      if (puSel == 0) { beginInput("newname", "NEW LOCATION - give it a name", ""); return; }
      if (puSel == 1) { beginInput("goto", "GO TO  latitude,longitude  e.g. 35.8325,-115.4319", ""); return; }
      std::vector<int> order = placeOrder();
      if (puSel - 2 >= (int)order.size()) return;
      puPlace = places[order[puSel - 2]];
      puListSel = puSel;
      pu = PU_ACT;
      puSel = puTop = 0;
      break;
    }
    case PU_ACT:
      if (puSel == 0) {                               // GO TO: centre the map there
        viewLat = puPlace.lat; viewLon = puPlace.lon;
        mapFollow = false; mapMode = MM_ZOOM; mapFocus = 0;
        pu = PU_NONE;
        note("map centred on " + puPlace.name);
      } else if (puSel == 1) {
        if (navOn && samePlace(navTo, puPlace)) {      // STOP NAV
          setNav(nullptr);
          note("NAV stopped");
          pu = PU_LIST; puSel = puListSel;
        } else {                                       // NAV: back to following you, line to the target
          setNav(&puPlace);
          mapFollow = true; mapMode = MM_ZOOM; mapFocus = 0;
          pu = PU_NONE;
          note("NAV to " + puPlace.name);
          pip.log("hub: NAV to %s", puPlace.name.c_str());
        }
      } else if (puSel == 2) { pu = PU_SHARE; puSel = puTop = 0; }
      else { pu = PU_DEL; puSel = puTop = 0; }
      break;
    case PU_DEL:
      if (puSel == 1) { deletePlace(puPlace); note("deleted " + puPlace.name); }
      pu = PU_LIST; puSel = puListSel; puTop = 0;
      break;
    case PU_SHARE: {
      std::vector<Convo*> t = shareTargets();
      if (puSel >= (int)t.size()) return;
      if (sendTo(t[puSel], shareText(puPlace))) note("shared " + puPlace.name + " to " + t[puSel]->name);
      else note("radio offline - NOT shared");
      pu = PU_ACT; puSel = 2; puTop = 0;
      break;
    }
    case PU_ICON: {
      Place p;
      p.lat = newLat; p.lon = newLon; p.icon = constrain(puSel, 0, NICONS - 1); p.name = newName;
      if (findPlace(p) >= 0) note(p.name + " is already saved there");
      else { savePlace(p); note("saved " + p.name); pip.log("hub: saved location %s", p.name.c_str()); }
      pu = PU_NONE;
      break;
    }
    default: return;
  }
  term.sound("SELECT");
  render();
}
static void placesBack() {
  switch (pu) {
    case PU_LIST:  pu = PU_NONE; break;
    case PU_ACT:   pu = PU_LIST; puSel = puListSel; break;
    case PU_DEL:   pu = PU_ACT; puSel = 3; break;
    case PU_SHARE: pu = PU_ACT; puSel = 2; break;
    case PU_ICON:  pu = PU_LIST; puSel = 0; break;
    default: break;
  }
  puTop = 0;
  term.sound("SELECT");
  render();
}
// GO TO screens: knob1 moves / click picks, knob2 goes back
static void placesKnob(int knob, int dir) {
  if (knob == 2) { if (dir) placesBack(); return; }
  if (dir == 0) { placesClick(); return; }
  int s = constrain(puSel + dir, 0, max(0, puCount() - 1));
  if (s == puSel) return;
  puSel = s;
  term.sound("SCROLL");
  render();
}

// ---------------------------------------------------------------- TERM tab: Wi-Fi, connecting, keys
static volatile int connResult = 0;          // set by the connect task: 1 ok, 2 name not found, 3 no answer, 4 resolved (SSH)
static IPAddress connIp;
static bool connSsh = false;

static const char* KNOWN_HOSTS = "/known_hosts";
static String knownKey(const String& key) {
  if (!fsOk || !LittleFS.exists(KNOWN_HOSTS)) return "";
  File f = LittleFS.open(KNOWN_HOSTS, FILE_READ);
  String found;
  while (f && f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int sp = line.indexOf(' ');
    if (sp > 0 && line.substring(0, sp).equalsIgnoreCase(key)) found = line.substring(sp + 1);
  }
  if (f) f.close();
  return found;
}
static void setKnownKey(const String& key, const String& fp) {   // fp "" = forget
  if (!fsOk) return;
  String keep;
  if (LittleFS.exists(KNOWN_HOSTS)) {
    File f = LittleFS.open(KNOWN_HOSTS, FILE_READ);
    while (f && f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      int sp = line.indexOf(' ');
      if (sp > 0 && !line.substring(0, sp).equalsIgnoreCase(key)) keep += line + "\n";
    }
    if (f) f.close();
  }
  if (fp.length()) keep += key + " " + fp + "\n";
  File f = LittleFS.open(KNOWN_HOSTS, FILE_WRITE);
  if (f) { f.print(keep); f.close(); }
}
static char connHost[64];
static uint16_t connPort = 23;

static void saveHosts() {
  String all;
  for (const String& h : hosts) all += h + "\n";
  prefs.putString("hosts", all);
}
static void loadHosts() {
  hosts.clear();
  String all = prefs.getString("hosts", "");
  int start = 0;
  while (start < (int)all.length()) {
    int nl = all.indexOf('\n', start);
    if (nl < 0) nl = all.length();
    String h = all.substring(start, nl);
    h.trim();
    if (h.length()) hosts.push_back(h);
    start = nl + 1;
  }
}

static void wifiStart() {
  if (!wifiOn) {
    WiFi.mode(WIFI_STA);                      // modem sleep stays on: required next to Bluetooth
    wifiOn = true;
    pip.log("wifi: on");
  }
  if (wifiSsid.length() && WiFi.status() != WL_CONNECTED) {
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    pip.log("wifi: joining %s", wifiSsid.c_str());
  }
}
static void wifiStop() {
  if (!wifiOn) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
  pip.log("wifi: off");
}

static void connectTask(void*) {
  String host(connHost);
  IPAddress ip;
  bool found = false;
  if (host.endsWith(".local")) {               // mDNS name, e.g. raspberrypi.local
    static bool mdns = false;
    if (!mdns) mdns = MDNS.begin("piplink");
    ip = MDNS.queryHost(host.substring(0, host.length() - 6), 4000);
    found = ip != IPAddress((uint32_t)0);
  }
  if (!found) found = WiFi.hostByName(host.c_str(), ip) == 1;
  if (!found) connResult = 2;
  else if (connSsh) { connIp = ip; connResult = 4; }     // SSH: the SshLink task takes it from here
  else connResult = tcp.connect(ip, connPort, 6000) ? 1 : 3;
  vTaskDelete(nullptr);
}

static void termConnect(const String& entry) {
  if (ts == TS_CONNECTING) return;
  if (!wifiSsid.length()) { note("set up Wi-Fi first (WI-FI NETWORK)"); render(); return; }
  if (WiFi.status() != WL_CONNECTED) { note("Wi-Fi isn't connected yet - try again in a moment"); render(); return; }
  HostEntry h = parseEntry(entry);
  if (h.ssh && ssh.state() != SshLink::IDLE && ssh.state() != SshLink::CLOSED) { note("still closing the last session"); render(); return; }
  connPort = h.port;
  connSsh = tsSsh = h.ssh;
  tsBbs = h.bbs;
  tsUser = h.user;
  strlcpy(connHost, h.host.c_str(), sizeof(connHost));
  if (tcp.connected()) tcp.stop();
  tsHost = String(connHost) + ":" + String(connPort);
  tsKey = tsHost;
  termAsk = false;
  vt.resize(VT_COLS[canvasSize()], VT_ROWS[canvasSize()]);
  vt.setBbs(tsBbs);
  termBack = 0;
  vt.feed("Trying " + std::string(tsHost.c_str()) + " ...\r\n");
  ts = TS_CONNECTING;
  termMenu = false;
  connResult = 0;
  pip.log("term: connecting to %s%s", tsHost.c_str(), tsBbs ? " (BBS mode)" : "");
  xTaskCreate(connectTask, "tconn", 6144, nullptr, 1, nullptr);
  render();
}

// SIZE: change the text size and tell the remote computer (Telnet NAWS / SSH pty)
static void setTermSize(int n) {
  termSize = n;
  prefs.putUChar("term_size", n);
  vt.resizeKeep(VT_COLS[n], VT_ROWS[n]);
  if (ts == TS_ONLINE && !tsSsh) { std::string w = telnet.setSize(vt.cols(), vt.rows()); if (w.size()) tcp.write((const uint8_t*)w.data(), w.size()); }
  if (ts == TS_ONLINE && tsSsh) ssh.resize(vt.cols(), vt.rows());
  termBack = 0;
  note(String("text size ") + VT_NAMES[n]);
  pip.log("term: size %s", VT_NAMES[n]);
}

static void termClose(const String& why) {
  if (tcp.connected()) tcp.stop();
  if (tsSsh) ssh.cancel();
  termAsk = false;
  if (ts == TS_IDLE) return;
  ts = TS_CLOSED;
  tsInfo = why;
  termMenu = false;
  pip.log("term: %s (%s)", why.c_str(), tsHost.c_str());
  render();
}

static void termSend(const std::string& keys) {
  if (ts != TS_ONLINE || !keys.size()) return;
  if (tsSsh) { ssh.write((const uint8_t*)keys.data(), keys.size()); return; }
  std::string wire = telnet.encode(keys);
  tcp.write((const uint8_t*)wire.data(), wire.size());
}

// a BLE keyboard key -> what a VT100 sends
static std::string keyBytes(const KeyEvent& e) {
  bool app = vt.appCursorKeys();
  switch (e.type) {
    case KeyEvent::CHAR: {
      char c = e.ch;
      if (e.ctrl) {
        if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
        else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
        else if (c == '[') c = 27;
        else if (c == '\\') c = 28;
        else if (c == ']') c = 29;
        else if (c == '^' || c == '6') c = 30;
        else if (c == '_' || c == '-') c = 31;
        else if (c == ' ' || c == '@' || c == '2') c = 0;
      }
      std::string s;
      if (e.alt) s += '\x1b';
      s += c;
      return s;
    }
    case KeyEvent::ENTER:     return "\r";
    case KeyEvent::BACKSPACE: return "\x7f";
    case KeyEvent::TAB:       return "\t";
    case KeyEvent::ESC:       return "\x1b";
    case KeyEvent::UP:        return app ? "\x1bOA" : "\x1b[A";
    case KeyEvent::DOWN:      return app ? "\x1bOB" : "\x1b[B";
    case KeyEvent::RIGHT:     return app ? "\x1bOC" : "\x1b[C";
    case KeyEvent::LEFT:      return app ? "\x1bOD" : "\x1b[D";
    case KeyEvent::HOME:      return "\x1b[1~";
    case KeyEvent::END:       return "\x1b[4~";
    case KeyEvent::DEL:       return "\x1b[3~";
    case KeyEvent::PGUP:      return "\x1b[5~";
    case KeyEvent::PGDN:      return "\x1b[6~";
  }
  return "";
}

static void openWifiPicker() {
  wifiStart();
  wifiNets.clear();
  WiFi.scanDelete();
  wifiScanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
  tu = TU_WIFI;
  tuWifiSel = tuWifiTop = 0;
  render();
}

// TERM tab knobs; false = not handled here (knob2 then changes tabs)
static void answerKey(bool yes) {
  termAsk = false;
  if (yes) {
    setKnownKey(tsKey, ssh.fingerprint());
    vt.feed("Key accepted and remembered.\r\n");
    pip.log("term: accepted the host key of %s", tsKey.c_str());
  }
  ssh.acceptHostKey(yes);
  render();
}

static bool termKnob(int knob, int dir) {
  if (termAsk) {                                // accept this computer's key?
    if (knob == 2) { if (dir) { termAskSel ^= 1; term.sound("SCROLL"); render(); } return true; }
    if (dir == 0) { term.sound("SELECT"); answerKey(termAskSel == 0); }
    return true;
  }
  if (ts == TS_CONNECTING) {                    // the connect task owns the socket until it answers
    if (knob == 1 && dir == 0 && tsSsh) termClose("cancelled");
    return knob != 2;
  }
  if (ts != TS_IDLE) {
    if (termMenu) {
      if (knob == 2) {
        if (dir) { termMenuSel = (termMenuSel + (dir > 0 ? 1 : NTERMMENU - 1)) % NTERMMENU; term.sound("SCROLL"); render(); }
        return true;
      }
      if (dir) return true;
      termMenu = false;
      switch (termMenuSel) {
        case TM_TYPE: beginInput("ttype", "TYPE - sent to " + tsHost + " followed by Enter", ""); return true;
        case TM_CTRLC: termSend("\x03"); break;
        case TM_ESC: termSend("\x1b"); break;
        case TM_TAB: termSend("\t"); break;
        case TM_SIZE:
          if (tsBbs) { note("BBS mode is always 80x24 - change MODE in the computer's menu"); break; }
          setTermSize((termSize + 1) % 3); termMenu = true; termMenuSel = TM_SIZE; break;   // stay: click again
        case TM_WHEEL:
          termArrows = !termArrows;
          prefs.putBool("term_wheel", termArrows);
          note(termArrows ? "wheel: Up/Down keys (for menus, nano...)" : "wheel: scroll back");
          break;
        case TM_DISCONNECT: termClose("disconnected"); return true;
      }
      term.sound("SELECT");
      render();
      return true;
    }
    if (knob == 2) return false;
    if (termBack > 0 && dir == 0) { termBack = 0; term.sound("SELECT"); render(); return true; }   // click: live again
    if (ts == TS_CLOSED) {                    // click: back to the list
      if (dir == 0) { ts = TS_IDLE; tu = TU_LIST; term.sound("SELECT"); render(); }
      return true;
    }
    if (dir == 0) { termMenu = true; termMenuSel = 0; term.sound("SELECT"); render(); return true; }
    if (termArrows && termBack == 0) {        // WHEEL = keys: Up / Down to the remote
      if (ts == TS_ONLINE) termSend(vt.appCursorKeys() ? (dir > 0 ? "\x1bOB" : "\x1bOA") : (dir > 0 ? "\x1b[B" : "\x1b[A"));
      return true;
    }
    int back = constrain(termBack + (dir < 0 ? 3 : -3), 0, vt.historySize());   // turn up = older, 3 lines a step
    if (back != termBack) { termBack = back; termRedraw = true; }               // drawn by termPoll()
    return true;
  }
  switch (tu) {
    case TU_LIST: {
      if (knob == 2) return false;
      int n = hosts.size() + 2;
      if (dir) { tuSel = constrain(tuSel + dir, 0, n - 1); term.sound("SCROLL"); render(); return true; }
      if (tuSel < (int)hosts.size()) { tuHost = tuSel; tuHostSel = tuHostTop = 0; tu = TU_HOST; }
      else if (tuSel == (int)hosts.size()) { beginInput("thost", "user@computer = SSH   computer = Telnet   :port optional", ""); return true; }
      else { openWifiPicker(); return true; }
      break;
    }
    case TU_WIFI: {
      if (knob == 2) { if (dir) tu = TU_LIST; break; }
      int n = wifiNets.size() + 1;
      if (dir) { tuWifiSel = constrain(tuWifiSel + dir, 0, n - 1); term.sound("SCROLL"); render(); return true; }
      if (tuWifiSel < (int)wifiNets.size()) {
        pendingSsid = wifiNets[tuWifiSel].ssid;
        if (wifiNets[tuWifiSel].open) { onTextResult("wifipass", true, ""); return true; }
        beginInput("wifipass", "PASSWORD for " + pendingSsid, "");
      } else beginInput("wifissid", "WI-FI NETWORK NAME (hidden network)", "");
      return true;
    }
    case TU_HOST:
      if (knob == 2) { if (dir) tu = TU_LIST; break; }
      if (dir) {
        int n = tuHost < (int)hosts.size() && parseEntry(hosts[tuHost]).ssh ? 4 : 3;
        tuHostSel = constrain(tuHostSel + dir, 0, n - 1); term.sound("SCROLL"); render(); return true;
      }
      if (tuHost < (int)hosts.size()) {
        HostEntry h = parseEntry(hosts[tuHost]);
        if (tuHostSel == 0) { String e = hosts[tuHost]; tu = TU_LIST; termConnect(e); return true; }
        if (tuHostSel == 2) {                   // MODE: BBS <-> standard
          hosts[tuHost] = entryBase(hosts[tuHost]) + (h.bbs ? " std" : " bbs");
          saveHosts();
          note(h.bbs ? "standard mode (VT100, UTF-8)" : "BBS mode (ANSI, PC characters, 80x24)");
          term.sound("SELECT");
          render();
          return true;
        }
        if (tuHostSel == 3) {
          setKnownKey(h.host + ":" + String(h.port), "");
          note("key forgotten - you'll be asked to accept it next time");
          pip.log("term: forgot the host key of %s:%u", h.host.c_str(), (unsigned)h.port);
          break;
        }
        hosts.erase(hosts.begin() + tuHost);
        saveHosts();
        note("removed");
      }
      tu = TU_LIST;
      break;
  }
  term.sound("SELECT");
  render();
  return true;
}

// the TERM tab's share of loop(): Wi-Fi on/off, scan results, the session's data, redraws
static void termPoll() {
  bool onTab = view == LIST && page == PG_TERM;
  bool need = onTab || ts == TS_CONNECTING || ts == TS_ONLINE;
  static uint32_t lastNeed = 0;
  if (need) lastNeed = millis();
  if (need && !wifiOn && (wifiSsid.length() || tu == TU_WIFI)) wifiStart();
  if (!need && wifiOn && millis() - lastNeed > 30000) wifiStop();   // off 30 s after leaving the tab

  static wl_status_t lastSt = WL_IDLE_STATUS;
  wl_status_t st = wifiOn ? WiFi.status() : WL_DISCONNECTED;
  if (st != lastSt) {
    if (st == WL_CONNECTED) pip.log("wifi: connected to %s as %s", wifiSsid.c_str(), WiFi.localIP().toString().c_str());
    else if (lastSt == WL_CONNECTED) pip.log("wifi: lost the connection");
    else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) pip.log("wifi: can't join %s (%d)", wifiSsid.c_str(), (int)st);
    lastSt = st;
    if (onTab) render();
  }

  if (wifiScanning) {
    int n = WiFi.scanComplete();
    if (n >= 0 || n == WIFI_SCAN_FAILED) {
      wifiScanning = false;
      wifiNets.clear();
      for (int i = 0; i < n; i++) {
        String ss = WiFi.SSID(i);
        if (!ss.length()) continue;
        bool dup = false;
        for (WifiNet& w : wifiNets) if (w.ssid == ss) { dup = true; w.rssi = max(w.rssi, (int)WiFi.RSSI(i)); }
        if (!dup) wifiNets.push_back({ ss, (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN });
      }
      std::sort(wifiNets.begin(), wifiNets.end(), [](const WifiNet& a, const WifiNet& b) { return a.rssi > b.rssi; });
      WiFi.scanDelete();
      pip.log("wifi: %u networks found", (unsigned)wifiNets.size());
      if (onTab) render();
    }
  }

  if (ts == TS_CONNECTING && connResult == 4) {           // SSH: name resolved, start the session task
    connResult = 0;
    String known = knownKey(tsKey);
    vt.feed("SSH to " + std::string(tsUser.c_str()) + "@" + std::string(connIp.toString().c_str()) + " ...\r\n");
    if (!ssh.start(tsUser, connIp.toString(), connPort, vt.cols(), vt.rows(), known, tsBbs ? "ansi" : "vt100")) termClose(ssh.reason());
    if (onTab) render();
  }
  if (tsSsh && (ts == TS_CONNECTING || ts == TS_ONLINE)) {
    static SshLink::State last = SshLink::IDLE;
    SshLink::State ss = ssh.state();
    if (ss != last) {
      if (ss == SshLink::ASK_HOSTKEY) {
        vt.feed("\r\nFirst time connecting to this computer.\r\nIts key fingerprint is:\r\n  " +
                std::string(ssh.fingerprint().c_str()) +
                "\r\nAccept it if you trust this computer\r\n(on the Pi: ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub)\r\n");
        termAsk = true;
        termAskSel = 0;
        term.sound("SELECT");
      } else if (ss == SshLink::ASK_PASSWORD) {
        if (ssh.triesLeft() < 3) vt.feed("Permission denied, try again.\r\n");
        beginInput("sshpass", "PASSWORD for " + tsUser + "@" + String(connHost), "");
      } else if (ss == SshLink::ONLINE) {
        ts = TS_ONLINE;
        vt.reset();
        pip.log("term: ssh logged in to %s@%s", tsUser.c_str(), tsHost.c_str());
      } else if (ss == SshLink::CLOSED) {
        if (ssh.fingerprintChanged())
          vt.feed("\r\n!! WARNING: THIS COMPUTER'S KEY HAS CHANGED !!\r\nSomeone may be pretending to be it - or it was\r\n"
                  "reinstalled. If you're sure, pick it in the list\r\nand choose FORGET KEY, then connect again.\r\n");
        termClose(ssh.reason());
        pip.log("term: ssh task stack left %u bytes", (unsigned)ssh.stackLeft());
      }
      last = ss;
      if (onTab) render();
    }
    if (ts == TS_ONLINE) {
      uint8_t buf[512];
      size_t total = 0, k;
      while (total < 4096 && (k = ssh.read(buf, sizeof(buf))) > 0) {
        total += k;
        vt.feed(buf, k);
        std::string answers = vt.takeReplies();
        if (answers.size()) termSend(answers);
      }
    }
  }
  if (ts == TS_CONNECTING && connResult) {
    if (connResult == 1) {
      ts = TS_ONLINE;
      tcp.setNoDelay(true);
      telnet.begin(vt.cols(), vt.rows(), tsBbs ? "ANSI" : "VT100");
      vt.feed(tsBbs ? "Connected (BBS mode: ANSI, 80x24).\r\n" : "Connected.\r\n");
      pip.log("term: connected to %s", tsHost.c_str());
    } else {
      termClose(connResult == 2 ? "can't find " + String(connHost) : "no answer from " + tsHost);
    }
    connResult = 0;
    if (onTab) render();
  }

  if (ts == TS_ONLINE && !tsSsh) {
    uint8_t buf[512];
    size_t total = 0;
    int avail;
    while ((avail = tcp.available()) > 0 && total < 4096) {   // a slice per loop: the rest of PIP LINK keeps running
      int k = tcp.read(buf, min((int)sizeof(buf), avail));
      if (k <= 0) break;
      total += k;
      std::string data, reply;
      telnet.receive(buf, k, data, reply);
      if (reply.size()) tcp.write((const uint8_t*)reply.data(), reply.size());
      vt.feed(data);
      std::string answers = vt.takeReplies();
      if (answers.size()) termSend(answers);
    }
    if (!tcp.connected() && !tcp.available()) termClose("connection closed");
  }
  static int bells = 0;                         // BEL: a blip, at most 3 a second (BBSes can send dozens)
  static uint32_t lastBell = 0;
  if (vt.bells() != bells) { bells = vt.bells(); if (onTab && millis() - lastBell > 300) { lastBell = millis(); term.sound("SCROLL"); } }
  static int lastHist = 0;                      // scrolled back: new lines mustn't drag the view along
  int hs = vt.historySize();
  if (termBack > 0 && hs > lastHist) termBack = min(termBack + (hs - lastHist), hs);
  lastHist = hs;
  static uint32_t lastDraw = 0;                 // remote output: redraw at most ~12 times a second
  // a full 80x24 screen is ~3.6 KB of commands and the Pip-Boy draws ~4 a second: only queue the next frame
  // once the last one has mostly gone, so floods and fast scrolling can't pile up behind the link
  if (onTab && ts != TS_IDLE && (vt.anyDirty() || termRedraw) && millis() - lastDraw > 80 && pip.queued() < 800) {
    lastDraw = millis();
    termRedraw = false;
    render();
  }
}

// ---------------------------------------------------------------- radio selection
static int pendingKind = RK_MESHCORE;          // which kind of radio the PIN prompt is for
static void askPin(int kind, const String& name, const String& why) {
  pendingRadio = name;
  pendingKind = kind;
  beginInput("pin", why + (kind == RK_MESHTASTIC ? "PIN shown on " : "PIN for ") + shortName(name), "");
}
static void useRadio(int kind, const String& name, uint32_t pin) {
  MeshRadio* next = kind == RK_MESHTASTIC ? (MeshRadio*)&meshtastic : (MeshRadio*)&meshcore;
  MeshRadio* other = next == &meshcore ? (MeshRadio*)&meshtastic : (MeshRadio*)&meshcore;
  if (other->state() != MeshRadio::IDLE) other->forgetRadio();   // one radio at a time
  radio = next;
  prefs.putString("radio", name);
  prefs.putUChar("radio_kind", kind);
  prefs.putUInt("pin", pin);
  clearConvos();
  histPath = histFileFor(name);               // each radio keeps its own history
  histLoad();
  radio->selectRadio(name, pin);
  pip.log("hub: using %s radio %s", radio->kind(), name.c_str());
  view = LIST;
  page = 0;
}

static void choosePick(const PickItem& it) {
  if (it.none) {
    if (pickKind == BleHub::KEYBOARD) {
      kbd.unpair();                               // also drops the bond: the keyboard is free to pair elsewhere
      prefs.remove("kbd_addr"); prefs.remove("kbd_type"); prefs.remove("kbd_name");
      note("keyboard removed - to use it elsewhere, put it in pairing mode there");
    } else {
      rc.forget();
      prefs.remove("rc_addr"); prefs.remove("rc_type"); prefs.remove("rc_name");
      note("RadiaCode removed");
    }
    view = LIST; page = PG_SET;
    render();
    return;
  }
  switch (pickKind) {
    case BleHub::MESHCORE:
      if (it.radioKind == RK_MESHTASTIC) {          // Meshtastic: connect now, the radio shows its PIN when pairing
        useRadio(RK_MESHTASTIC, it.name, 0);
        render();
      } else {
        askPin(RK_MESHCORE, it.name, "");           // MeshCore: fixed PIN, asked up front
      }
      return;
    case BleHub::KEYBOARD:
      prefs.putString("kbd_addr", it.key); prefs.putUChar("kbd_type", it.addrType); prefs.putString("kbd_name", it.label);
      kbd.pair(it.key, it.addrType, it.label);
      view = PAIRKB;
      break;
    case BleHub::RADIACODE:
      prefs.putString("rc_addr", it.key); prefs.putUChar("rc_type", it.addrType); prefs.putString("rc_name", it.label);
      rc.use(it.key, it.addrType, it.label);
      note("RadiaCode selected - connecting");
      view = LIST; page = PG_SET;
      break;
  }
  render();
}

static void openPicker(BleHub::Kind k) {
  pickKind = k;
  pickSel = "";
  view = PICK;
  render();
}

// ---------------------------------------------------------------- mesh events
static void syncRoster() {
  for (const McChannel& ch : radio->channels()) {
    Convo* c = findChannel(ch.idx);
    if (!c) { c = new Convo(); c->isCh = true; c->chan = ch.idx; convos.push_back(c); }
    c->name = ch.name;
  }
  for (const McContact& ct : radio->contacts()) {
    Convo* c = findDirect(ct.prefix);
    if (!c) { c = new Convo(); memcpy(c->prefix, ct.prefix, 6); convos.push_back(c); }
    c->name = ct.name;
  }
  render();
}

static void onMessage(const McMessage& m) {
  Convo* c = m.isChannel ? findChannel(m.chan) : findDirect(m.prefix);
  if (!c) {
    c = new Convo();
    c->isCh = m.isChannel;
    c->chan = m.chan;
    memcpy(c->prefix, m.prefix, 6);
    c->name = m.isChannel ? "CH" + String(m.chan) : m.from;
    convos.push_back(c);
  }
  // trust the sender's clock unless it's clearly wrong; then use our receive time
  uint32_t rx = radio->now(), ts = m.ts;
  if (ts < 1700000000UL || (rx && ts > rx + 86400)) ts = rx;
  c->msgs.push_back({ false, m.from, m.text, ts });
  histAppend(c, c->msgs.back());
  while (c->msgs.size() > MAX_MSGS) c->msgs.pop_front();
  c->snr = m.snr;
  c->hasSnr = true;
  if (view == CHAT && active == c) chatBack = 0;
  else c->unread++;
  bump(c);
  pip.log("msg [%s] %s: %s (snr %.1f)", c->name.c_str(), m.from.c_str(), m.text.c_str(), m.snr);
  Place shared;
  static uint32_t lastChirp = 0;                // a burst of messages chirps once
  if (parseShare(m.text, shared)) receivedPlace(shared, m.from);          // "[WPT] name @ lat,lon ICON"
  else if (soundOn && millis() - lastChirp > 1500) { lastChirp = millis(); term.play(MSG_SOUND); }
  render();
}

static void onMeshState(MeshRadio::State s) {
  if (s == MeshRadio::PAIR_FAILED && term.ready() && !term.keyboardOpen() && !inputActive) {
    term.sound("SELECT");
    askPin(radio == &meshtastic ? RK_MESHTASTIC : RK_MESHCORE, radio->radioName(), "REJECTED - ");
    return;
  }
  render();                                     // NEED_PIN is prompted from loop() once nothing else is typing
}

static void onKbdState(BleKeyboard::State s) {
  if (view == PAIRKB) {
    if (s == BleKeyboard::ONLINE) { note("keyboard paired: " + kbd.name()); view = LIST; page = PG_SET; }
    else if (s == BleKeyboard::FAILED) {
      note("keyboard pairing failed - try again");
      prefs.remove("kbd_addr"); prefs.remove("kbd_type"); prefs.remove("kbd_name");
      kbd.forget();
      view = LIST; page = PG_SET;
    }
  }
  if (s != BleKeyboard::ONLINE && inputActive) endInput(false);   // keyboard vanished mid-typing
  render();
}

static void cycleAlarm() {
  alarmIdx = (alarmIdx + 1) % NALARM;
  prefs.putUChar("rad_alarm", alarmIdx);
  if (!alarmIdx) alarming = false;
  note(alarmIdx ? "rad alarm at " + fmtRadH(ALARM_LEVELS[alarmIdx]) : String("rad alarm OFF"));
  if (alarmIdx) term.play(GEIGER_SOUND[0]); else term.sound("SELECT");   // preview the crackle
  render();
}

static void onRadData() {
  const RcLive& lv = rc.live();
  // graph buckets
  if (!bucketStart) bucketStart = millis();
  bucketSum += lv.doseRate;
  bucketN++;
  if (millis() - bucketStart >= BUCKET_MS) {
    rateHist.push_back(bucketSum / bucketN);
    while ((int)rateHist.size() > HIST_BARS) rateHist.pop_front();
    bucketSum = 0; bucketN = 0; bucketStart = millis();
  }
  // alarm with hysteresis (clears below 80% of the level)
  float lvl = ALARM_LEVELS[alarmIdx];
  bool was = alarming;
  if (alarmIdx && lv.doseRate >= lvl) alarming = true;
  else if (!alarmIdx || lv.doseRate < lvl * 0.8f) alarming = false;
  if (alarming && !was) { pip.log("rad: ALARM %.3f uSv/h (level %.2f)", lv.doseRate, lvl); radAlert = true; }
  if (!alarming && was) pip.log("rad: alarm cleared (%.3f uSv/h)", lv.doseRate);
  if (alarming) {
    if (millis() - lastAlarmSound > 1500) {      // the clips are 1.5 s: back to back = continuous crackle
      lastAlarmSound = millis();
      float over = lv.doseRate / lvl;
      term.play(GEIGER_SOUND[over < 2 ? 0 : over < 5 ? 1 : 2]);
    }
    if (!(view == LIST && page == PG_RAD)) note("!! RAD ALARM " + fmtRadH(lv.doseRate) + " - press DATA");
  }
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 60000) {
    lastLog = millis();
    pip.log("rad: %.3f uSv/h, %.1f cps, dose %.3f uSv, batt %.0f%%", lv.doseRate, lv.cps, rc.rare().dose, rc.rare().battery);
  }
  if (view == LIST && page == PG_RAD) render();
  else if (alarming) render();               // footer note
}

// ---------------------------------------------------------------- input
static void goBack() {
  if (view == CHAT) { view = chatFrom == CONTACTS ? CONTACTS : LIST; active = nullptr; }
  else if (view == CMENU) view = CONTACTS;
  else if (view == CONTACTS) view = LIST;
  else if (view == PICK) { if (pickKind != BleHub::MESHCORE || radio->radioName().length()) view = LIST; }
  else if (view == PAIRKB) {
    kbd.forget();
    prefs.remove("kbd_addr"); prefs.remove("kbd_type"); prefs.remove("kbd_name");
    note("keyboard pairing cancelled");
    view = LIST; page = PG_SET;
  }
  render();
}

// move the map view east / north by 32 pixels a step: fine enough to put the crosshair on a spot. The
// Pip-Boy scrolls the picture rather than redrawing it (flag 2 below), and turns made while it's busy add
// up into the next view (renderMap sends only the newest), so a fast spin still covers ground
static const int PAN_STEP = 32;
static void mapPan(int east, int north) {
  if (mapFollow) {
    bool me = radio->hasPosition();
    viewLat = me ? radio->lat() : mapDefLat; viewLon = me ? radio->lon() : mapDefLon;
    mapFollow = false;
  }
  double degPerPx = 360.0 / (256.0 * pow(2, mapZoom));
  viewLon += east * PAN_STEP * degPerPx;
  viewLat += north * PAN_STEP * degPerPx * cos(viewLat * M_PI / 180);
  term.sound("SCROLL");
  render();
}

static bool mapPanning() { return view == LIST && page == PG_MAP && mapMode == MM_PAN && !mapMenu; }

static void mapZoomBy(int d) {
  mapZoom = constrain(mapZoom + d, mapZmin, mapZmax);
  prefs.putUChar("map_z", mapZoom);
  term.sound("SCROLL");
  render();
}

// map page, knob1: with the menu open it picks a mode; otherwise it zooms / pans / steps contacts
static void mapKnob(int dir) {
  bool me = radio->hasPosition();
  double myLat = me ? radio->lat() : mapDefLat, myLon = me ? radio->lon() : mapDefLon;
  if (mapMenu) {
    if (dir) return;                              // the menu is driven by knob2; knob1 only clicks
    mapMenu = false;
    if (mapFollow && mapMenuSel != MENU_ME) { viewLat = myLat; viewLon = myLon; }   // panning starts from where we are
    switch (mapMenuSel) {
      case MENU_ZOOM:     mapMode = MM_ZOOM; break;
      case MENU_PAN:      mapMode = MM_PAN; break;
      case MENU_CONTACTS: mapMode = MM_FIND; mapFocus = 1; break;
      case MENU_GOTO:                             // the list: NEW, MANUAL ENTRY, saved locations
        mapMode = MM_ZOOM;
        openPlaces();
        return;
      case MENU_ME:       mapMode = MM_ZOOM; mapFollow = true; mapFocus = 0; break;
    }
    term.sound("SELECT");
    render();
    return;
  }
  if (dir == 0 && mapMode == MM_PAN) {            // click while panning: done (the view stays put)
    mapMode = MM_ZOOM;
    term.sound("SELECT");
    render();
    return;
  }
  if (dir == 0) {                                 // click: open the menu on the current mode
    mapMenu = true;
    mapMenuSel = mapMode == MM_PAN ? MENU_PAN : mapMode == MM_FIND ? MENU_CONTACTS : MENU_ZOOM;
    term.sound("SELECT");
    render();
    return;
  }
  // wheel direction: v1.1 had zoom and N/S panning the other way round - people kept zooming out when
  // they meant in, and panning north when they meant south. dir +1 now zooms out and pans south
  switch (mapMode) {
    case MM_ZOOM:
      mapZoom = constrain(mapZoom - dir, mapZmin, mapZmax);
      prefs.putUChar("map_z", mapZoom);
      break;
    case MM_PAN:
      mapPan(0, -dir);
      return;
    case MM_FIND: {
      int n = locatedContacts(myLat, myLon).size();
      if (n) mapFocus = (mapFocus - 1 + (dir > 0 ? 1 : n - 1)) % n + 1;
      break;
    }
  }
  term.sound("SCROLL");
  render();
}

static void onKnob(int knob, int dir);

// raw wheel events from the Pip-Boy -> debounced steps. Keyboard keys call onKnob() directly.
static void onWheel(int knob, int dir) {
  if (knob < 1 || knob > 2) return;
  uint32_t now = millis();
  if (dir == 0) {
    knobAcc[knob] = 0;
    if (knob == 1) knob1ClickAt = now;
    onKnob(knob, 0);
    return;
  }
  // pressing the scroll wheel tends to nudge it: ignore turns just after a click
  if (knob == 1 && now - knob1ClickAt < 350) return;
  int need = (knob == 1 || mapPanning()) ? scrollSteps : tabSteps;
  if (now - knobLast[knob] > 800 || (knobAcc[knob] > 0) != (dir > 0)) knobAcc[knob] = 0;
  knobLast[knob] = now;
  knobAcc[knob] += dir;
  if (abs(knobAcc[knob]) < need) return;
  knobAcc[knob] = 0;
  onKnob(knob, dir > 0 ? 1 : -1);
}

static void onKnob(int knob, int dir) {
  pip.log("hub: knob%d %s (view %d page %d)", knob, dir > 0 ? "+1" : dir < 0 ? "-1" : "click", (int)view, page);

  if (view == PAIRKB) { if (knob == 2 && dir != 0) goBack(); return; }

  if (view == PICK) {
    if (knob == 2) { if (dir != 0) goBack(); return; }
    std::vector<PickItem> items = pickItems();
    if (items.empty()) return;
    int sel = 0;
    for (size_t i = 0; i < items.size(); i++) if (items[i].key == pickSel) sel = i;
    if (dir == 0) { choosePick(items[sel]); return; }
    sel = constrain(sel + dir, 0, (int)items.size() - 1);
    pickSel = items[sel].key;
    term.sound("SCROLL");
    render();
    return;
  }

  if (view == LIST) {
    if (page == PG_MAP && pu != PU_NONE) { placesKnob(knob, dir); return; }
    if (page == PG_TERM && termKnob(knob, dir)) return;
    if (knob == 2) {
      if (dir == 0) return;
      if (mapPanning()) { mapPan(dir, 0); return; }   // panning: knob2 = east / west
      if (page == PG_MAP && mapMenu) {            // map menu open: knob2 moves the highlight
        mapMenuSel = (mapMenuSel + (dir > 0 ? 1 : NMAPMENU - 1)) % NMAPMENU;
        term.sound("SCROLL");
        render();
        return;
      }
      page = (page + (dir > 0 ? 1 : NPAGES - 1)) % NPAGES;
      selC = nullptr; topRow = 0;
      mapMenu = false;
      pu = PU_NONE;
      termMenu = false;
      if (mapMode == MM_PAN) mapMode = MM_ZOOM;   // leaving the map ends panning
      term.sound("SELECT");                     // audible confirmation that the tab really changed
      render();
      return;
    }
    if (page == PG_MAP) {
      mapKnob(dir);
      return;
    }
    if (page == PG_RAD) {                         // click: next alarm level
      if (dir == 0) cycleAlarm();
      return;
    }
    if (page == PG_SET) {                         // settings
      if (dir != 0) { setSel = constrain(setSel + dir, 0, NSETTINGS - 1); term.sound("SCROLL"); render(); return; }
      switch (setSel) {
        case 0: openPicker(BleHub::MESHCORE); break;
        case 1: openPicker(BleHub::KEYBOARD); break;
        case 2: openPicker(BleHub::RADIACODE); break;
        case 3: cycleAlarm(); break;
        case 4: rcAlways = !rcAlways; prefs.putBool("rc_always", rcAlways); render(); break;
        case 5: soundOn = !soundOn; prefs.putBool("sound", soundOn); if (soundOn) term.play(MSG_SOUND); render(); break;
        case 6: tabSteps = tabSteps % 5 + 1; prefs.putUChar("tab_steps", tabSteps); term.keyboardSteps(scrollSteps, tabSteps); render(); break;
        case 7: scrollSteps = scrollSteps % 5 + 1; prefs.putUChar("scroll_steps", scrollSteps); term.keyboardSteps(scrollSteps, tabSteps); render(); break;
        case 8: themeSel = (themeSel + 1) % 3; prefs.putUChar("ui_theme", themeSel); term.theme(themeArg()); render(); break;
        case 9: showPlaces = !showPlaces; prefs.putBool("show_places", showPlaces); render(); break;
        case 10: termMargin = (termMargin + 2) % 14; prefs.putUChar("term_margin", termMargin); render(); break;
      }
      return;
    }
    std::vector<Convo*> vis = visible();
    if (vis.empty()) return;
    int sel = 0;
    for (size_t i = 0; i < vis.size(); i++) if (vis[i] == selC) sel = i;
    if (dir == 0) {
      if (vis[sel] == &contactsRow) { view = CONTACTS; ctSel = ctTop = 0; }
      else if (vis[sel] == &advertRow) {
        bool ok = radio->advertise();
        note(ok ? "advert sent - the mesh hears " + radio->selfName() + " in a moment" : String("couldn't advertise - is the radio connected?"));
        pip.log("hub: advertise %s", ok ? "sent" : "failed");
        term.sound("SELECT");
      }
      else openChat(vis[sel], LIST);
    } else {
      sel = constrain(sel + dir, 0, (int)vis.size() - 1);
      selC = vis[sel];
      term.sound("SCROLL");
    }
    render();
    return;
  }

  if (view == CONTACTS) {
    if (knob == 2) { if (dir != 0) goBack(); return; }
    std::vector<Convo*> cs = contactConvos();
    if (cs.empty()) return;
    ctSel = constrain(ctSel, 0, (int)cs.size() - 1);
    if (dir == 0) { ctC = cs[ctSel]; cmSel = 0; cmConfirm = false; view = CMENU; term.sound("SELECT"); }
    else { ctSel = constrain(ctSel + dir, 0, (int)cs.size() - 1); term.sound("SCROLL"); }
    render();
    return;
  }
  if (view == CMENU) {
    if (knob == 2) { if (dir != 0) goBack(); return; }
    if (dir != 0) { cmSel = constrain(cmSel + dir, 0, CM_REMOVE); cmConfirm = false; term.sound("SCROLL"); render(); return; }
    if (cmSel == CM_MESSAGE) { openChat(ctC, CONTACTS); render(); return; }
    if (cmSel == CM_FAV) {
      setFav(ctC->prefix, !isFav(ctC->prefix));
      note(isFav(ctC->prefix) ? ctC->name + " is a favorite: it's on the MESH tab" : ctC->name + " is no longer a favorite");
      term.sound("SELECT");
      render();
      return;
    }
    if (!cmConfirm) { cmConfirm = true; term.sound("SELECT"); render(); return; }
    cmConfirm = false;
    Convo* c = ctC;
    if (!radio->removeContact(c->prefix)) { note("couldn't remove it - is the radio connected?"); render(); return; }
    pip.log("hub: removed contact %s", c->name.c_str());
    note(c->name + " removed from the radio");
    setFav(c->prefix, false);
    for (size_t i = 0; i < convos.size(); i++) if (convos[i] == c) { convos.erase(convos.begin() + i); break; }
    if (selC == c) selC = nullptr;
    if (active == c) active = nullptr;
    delete c;
    ctC = nullptr;
    view = CONTACTS;
    render();
    return;
  }

  // CHAT
  if (knob == 2) { if (dir != 0) goBack(); return; }
  if (dir == 0) { beginInput("msg", "MSG to " + active->name, ""); return; }
  chatBack += dir > 0 ? -1 : 1;                 // turning "up" shows newer lines
  render();
}

static void switchTab(int dir) { onKnob(2, dir); }

static void gotoTab(int p) {
  if (view != LIST || p == page) return;
  if (p < 0 || p >= NPAGES) return;
  page = p;
  selC = nullptr; topRow = 0;
  mapMenu = false;
  pu = PU_NONE;
  termMenu = false;
  if (mapMode == MM_PAN) mapMode = MM_ZOOM;
  term.sound("SELECT");
  render();
}

// the buttons under the screen and the side switch (PIP LINK has them while it's open - holding STATS
// leaves): the switch's five positions are the five tabs, STATS / ITEMS / DATA jump to MESH / MAP /
// RADIATION. From a chat too, but not while typing, picking a device or answering a prompt
static bool onMeshViews() { return view == LIST || view == CHAT || view == CONTACTS || view == CMENU; }
static void jumpTab(int p) {
  if (term.keyboardOpen() || inputActive || !onMeshViews()) return;
  if (view != LIST) { view = LIST; active = nullptr; if (p == page) { term.sound("SELECT"); render(); return; } }
  gotoTab(p);
}
// STATS: straight into the most recently active chat (to read what just came in); pressed in that chat, back to
// the MESH list
static void statsButton() {
  if (term.keyboardOpen() || inputActive || !onMeshViews()) return;
  Convo* last = nullptr;
  for (Convo* c : convos) if (c->unread) { last = c; break; }             // waiting messages first (STATS blinks)
  if (!last) for (Convo* c : convos) if (!c->msgs.empty()) { last = c; break; }
  if (!last || (view == CHAT && active == last)) { jumpTab(PG_MESH); return; }
  page = PG_MESH;
  mapMenu = false; pu = PU_NONE; termMenu = false;
  if (mapMode == MM_PAN) mapMode = MM_ZOOM;
  selC = last;
  openChat(last, LIST);
  term.sound("SELECT");
  render();
}
// ITEMS with a shared location waiting (it blinks): the map, centred on it
static void itemsButton() {
  if (!placeAlert) { jumpTab(PG_MAP); return; }
  if (term.keyboardOpen() || inputActive || !onMeshViews()) return;
  placeAlert = false;
  view = LIST; active = nullptr;
  page = PG_MAP;
  mapMenu = false; pu = PU_NONE; termMenu = false;
  viewLat = alertLat; viewLon = alertLon;
  mapFollow = false; mapMode = MM_ZOOM; mapFocus = 0;
  note("map centred on " + alertName);
  term.sound("SELECT");
  render();
}
// the three button lights: the one for the tab on screen is on, any with something waiting blinks
static String ledStates() {
  String s = "000";
  int cur = tabLed();
  if (cur >= 0) s[cur] = '1';
  for (Convo* c : convos) if (c->unread) { s[0] = '2'; break; }
  if (placeAlert) s[1] = '2';
  if (radAlert) s[2] = '2';
  return s;
}
static int tabLed() {                         // the button that belongs to the tab on screen
  if (!onMeshViews()) return -1;
  return page == PG_MESH ? 0 : page == PG_MAP ? 1 : page == PG_RAD ? 2 : -1;
}

// Bluetooth keyboard: arrows / Tab / 1-5 switch tabs, Enter selects, Esc goes back; map keys below.
static bool mapKey(const KeyEvent& e) {
  if (!(view == LIST && page == PG_MAP)) return false;
  char c = e.type == KeyEvent::CHAR ? (char)tolower(e.ch) : 0;
  if (pu != PU_NONE) {                            // GO TO screens
    switch (e.type) {
      case KeyEvent::UP:    placesKnob(1, -1); return true;
      case KeyEvent::DOWN:  placesKnob(1, 1); return true;
      case KeyEvent::PGUP:  for (int i = 0; i < 5; i++) placesKnob(1, -1); return true;
      case KeyEvent::PGDN:  for (int i = 0; i < 5; i++) placesKnob(1, 1); return true;
      case KeyEvent::ENTER: case KeyEvent::RIGHT: placesKnob(1, 0); return true;
      case KeyEvent::ESC:   case KeyEvent::LEFT:  placesKnob(2, -1); return true;
      default: return e.type != KeyEvent::CHAR;   // digits still switch tabs
    }
  }
  if (mapMenu) {
    switch (e.type) {
      case KeyEvent::LEFT: case KeyEvent::UP:    mapMenuSel = (mapMenuSel + NMAPMENU - 1) % NMAPMENU; break;
      case KeyEvent::RIGHT: case KeyEvent::DOWN: case KeyEvent::TAB: mapMenuSel = (mapMenuSel + 1) % NMAPMENU; break;
      case KeyEvent::ENTER: mapKnob(0); return true;
      case KeyEvent::ESC:   mapMenu = false; break;
      default: return true;
    }
    term.sound("SCROLL");
    render();
    return true;
  }
  if (mapMode == MM_PAN) {                        // arrows pan, Enter / Esc = done
    switch (e.type) {
      case KeyEvent::UP:    mapPan(0, 1); return true;
      case KeyEvent::DOWN:  mapPan(0, -1); return true;
      case KeyEvent::LEFT:  mapPan(-1, 0); return true;
      case KeyEvent::RIGHT: mapPan(1, 0); return true;
      case KeyEvent::ENTER: case KeyEvent::ESC: mapKnob(0); return true;
      default: break;
    }
  }
  if (c == '+' || c == '=') { mapZoomBy(1); return true; }
  if (c == '-' || c == '_') { mapZoomBy(-1); return true; }
  if (c == 'p') { mapMenuSel = MENU_PAN; mapMenu = true; mapKnob(0); return true; }
  if (c == 'c') { mapMenuSel = MENU_CONTACTS; mapMenu = true; mapKnob(0); return true; }
  if (c == 'g') { mapMenuSel = MENU_GOTO; mapMenu = true; mapKnob(0); return true; }      // GO TO list
  if (c == 'h' || c == 'm') { mapMenuSel = MENU_ME; mapMenu = true; mapKnob(0); return true; }   // home: centre on me
  if (mapMode == MM_PAN) return true;
  switch (e.type) {
    case KeyEvent::UP:   if (mapMode == MM_ZOOM) mapZoomBy(1); else mapKnob(-1); return true;   // up = in / previous
    case KeyEvent::DOWN: if (mapMode == MM_ZOOM) mapZoomBy(-1); else mapKnob(1); return true;
    case KeyEvent::PGUP: mapZoomBy(1); return true;
    case KeyEvent::PGDN: mapZoomBy(-1); return true;
    case KeyEvent::ESC:  if (!mapFollow || mapMode != MM_ZOOM) { mapMenuSel = MENU_ME; mapMenu = true; mapKnob(0); } return true;
    default: return false;                        // Enter / Left / Right / Tab / digits: the general handling
  }
}

static void onKey(const KeyEvent& e) {
  if (inputActive) {                            // editing a reply / PIN
    switch (e.type) {
      case KeyEvent::CHAR:      if (inputBuf.length() < 150) inputBuf += e.ch; break;
      case KeyEvent::BACKSPACE:
      case KeyEvent::DEL:       if (inputBuf.length()) inputBuf.remove(inputBuf.length() - 1); break;
      case KeyEvent::ENTER:     endInput(true); return;
      case KeyEvent::ESC:       endInput(false); return;
      default: break;
    }
    render();
    return;
  }
  if (term.keyboardOpen()) return;              // the on-screen keyboard owns input right now
  if (view == LIST && page == PG_TERM) {
    if (termAsk) {                              // accept the key? y / n, or Enter on the highlighted answer
      char c = e.type == KeyEvent::CHAR ? (char)tolower(e.ch) : 0;
      if (c == 'y') answerKey(true);
      else if (c == 'n' || e.type == KeyEvent::ESC) answerKey(false);
      else if (e.type == KeyEvent::ENTER) answerKey(termAskSel == 0);
      else if (e.type == KeyEvent::LEFT || e.type == KeyEvent::RIGHT) termKnob(2, 1);
      return;
    }
    if (termBack > 0 && ts != TS_IDLE) {        // looking at the scrollback: arrows / PgUp / PgDn move it,
      int screen = vt.rows() - 1;               // Esc goes back to live, anything else is typed (and goes live)
      bool typed = false;
      switch (e.type) {
        case KeyEvent::UP:   termBack += 1; break;
        case KeyEvent::DOWN: termBack -= 1; break;
        case KeyEvent::PGUP: termBack += screen; break;
        case KeyEvent::PGDN: termBack -= screen; break;
        case KeyEvent::ESC:  termBack = 0; break;
        default: termBack = 0; typed = true; break;
      }
      termBack = constrain(termBack, 0, vt.historySize());
      render();
      if (!typed) return;
    }
    if (ts == TS_ONLINE && !termMenu) { termSend(keyBytes(e)); return; }   // a session: every key goes to the remote
    if (ts == TS_IDLE && tu != TU_LIST && (e.type == KeyEvent::ESC || e.type == KeyEvent::LEFT)) { termKnob(2, -1); return; }
    if (termMenu && e.type == KeyEvent::ESC) { termMenu = false; render(); return; }
    if (termMenu && (e.type == KeyEvent::LEFT || e.type == KeyEvent::RIGHT)) { termKnob(2, e.type == KeyEvent::RIGHT ? 1 : -1); return; }
  }
  if (mapKey(e)) return;
  switch (e.type) {
    case KeyEvent::UP:    onKnob(1, -1); break;
    case KeyEvent::DOWN:  onKnob(1, 1); break;
    case KeyEvent::PGUP:  for (int i = 0; i < 5; i++) onKnob(1, -1); break;
    case KeyEvent::PGDN:  for (int i = 0; i < 5; i++) onKnob(1, 1); break;
    case KeyEvent::ENTER: onKnob(1, 0); break;
    case KeyEvent::ESC:   goBack(); break;
    case KeyEvent::TAB:
    case KeyEvent::RIGHT: if (view == LIST) switchTab(1); break;
    case KeyEvent::LEFT:  if (view == LIST) switchTab(-1); else goBack(); break;   // in a chat / picker: back
    case KeyEvent::CHAR:
      if (view == CHAT && active) beginInput("msg", "MSG to " + active->name, String(e.ch));   // typing = reply
      else if (view == LIST && e.ch >= '1' && e.ch < '1' + NPAGES) gotoTab(e.ch - '1');      // 1-5 = tab
      break;
    default: break;
  }
}

static void onTextResult(const String& id, bool ok, const String& text) {
  if (id == "pin") {
    if (!ok) { render(); return; }              // cancelled: stay where we were
    String digits;
    for (size_t i = 0; i < text.length(); i++) if (isDigit(text[i])) digits += text[i];
    if (!digits.length() || digits.length() > 6) { askPin(pendingKind, pendingRadio, "DIGITS ONLY (MAX 6) - "); return; }
    uint32_t pin = (uint32_t)digits.toInt();
    if (pendingKind == RK_MESHTASTIC && radio == &meshtastic && radio->radioName() == pendingRadio &&
        (radio->state() == MeshRadio::NEED_PIN || radio->state() == MeshRadio::PAIR_FAILED)) {
      prefs.putUInt("pin", pin);                // a fixed-PIN radio can reuse it if the bond is ever lost
      radio->providePin(pin);
    } else {
      useRadio(pendingKind, pendingRadio, pin);
    }
    render();
    return;
  }
  if (id == "thost") {                          // TERM: a new computer to connect to
    String h = text;
    h.trim();
    h.replace(" ", "");
    if (!ok || !h.length()) { render(); return; }
    for (size_t i = 0; i < hosts.size(); i++)
      if (entryLabel(entryBase(hosts[i])) == entryLabel(h)) { h = hosts[i]; hosts.erase(hosts.begin() + i); break; }   // keeps its MODE
    hosts.insert(hosts.begin(), h);
    while (hosts.size() > 8) hosts.pop_back();
    saveHosts();
    tuSel = 0;
    termConnect(h);
    return;
  }
  if (id == "sshpass") {                        // SSH password: handed to the session task, never kept
    ssh.password(ok ? text : String(""));
    render();
    return;
  }
  if (id == "ttype") {                          // TERM: text from the on-screen keyboard + Enter
    if (ok) termSend(std::string(text.c_str()) + "\r");
    render();
    return;
  }
  if (id == "wifissid") {                       // hidden network: name, then password
    if (!ok || !text.length()) { render(); return; }
    pendingSsid = text;
    beginInput("wifipass", "PASSWORD for " + pendingSsid + " (blank if none)", "");
    return;
  }
  if (id == "wifipass") {
    if (!ok) { render(); return; }
    wifiSsid = pendingSsid;
    wifiPass = text;
    prefs.putString("wifi_ssid", wifiSsid);
    prefs.putString("wifi_pass", wifiPass);
    if (wifiOn) WiFi.disconnect();
    wifiStart();
    tu = TU_LIST;
    note("joining " + wifiSsid + "...");
    render();
    return;
  }
  if (id == "newname") {                        // NEW: name given, now pick its icon
    if (!ok) { render(); return; }
    newName = placeName(text);
    if (!newName.length()) { render(); return; }
    pu = PU_ICON; puSel = ICON_FLAG; puTop = 0;
    render();
    return;
  }
  if (id == "goto") {                           // "lat,lon" or "lat lon" in decimal degrees
    if (!ok) { render(); return; }              // back to the GO TO list
    String t = text;
    t.replace(" ", ",");
    int comma = t.indexOf(',');
    while (comma >= 0 && comma + 1 < (int)t.length() && t[comma + 1] == ',') { t.remove(comma + 1, 1); }
    double la = t.toDouble(), lo = comma > 0 ? t.substring(comma + 1).toDouble() : 0;
    if (comma <= 0 || la < -90 || la > 90 || lo < -180 || lo > 180 || (la == 0 && lo == 0)) {
      beginInput("goto", "TRY AGAIN: latitude,longitude  e.g. 35.8325,-115.4319", text);
      return;
    }
    viewLat = la; viewLon = lo;
    mapFollow = false;
    mapMode = MM_ZOOM;
    mapFocus = 0;
    pu = PU_NONE;
    note("map centred on " + String(la, 5) + "," + String(lo, 5));
    pip.log("hub: map go to %.5f,%.5f", la, lo);
    render();
    return;
  }
  if (id == "msg" && ok && text.length() && active) {
    if (sendTo(active, text)) chatBack = 0;
    else note("radio offline - message NOT sent");
  }
  render();
}

// ---------------------------------------------------------------- LED + main
static void ledTask(void*) {
  pinMode(LED_BUILTIN, OUTPUT);
  for (;;) {
    if (pip.failed()) {
      digitalWrite(LED_BUILTIN, LOW);  vTaskDelay(pdMS_TO_TICKS(70));
      digitalWrite(LED_BUILTIN, HIGH); vTaskDelay(pdMS_TO_TICKS(70));
      continue;
    }
    int n = pip.state() == PipLink::NO_DEVICE ? 2 : pip.state() == PipLink::READY ? 4 : 3;
    for (int i = 0; i < n; i++) {
      digitalWrite(LED_BUILTIN, LOW);  vTaskDelay(pdMS_TO_TICKS(150));   // active-low LED
      digitalWrite(LED_BUILTIN, HIGH); vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
}

void setup() {
  Serial.begin(115200);                       // UART0 (D6/D7) if a TTL adapter is attached
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  prefs.begin("hub", false);
  setenv("TZ", prefs.getString("tz", TIMEZONE).c_str(), 1);   // message times shown in local time
  tzset();
  soundOn = prefs.getBool("sound", true);
  alarmIdx = constrain((int)prefs.getUChar("rad_alarm", 3), 0, NALARM - 1);
  mapZoom = prefs.getUChar("map_z", 15);
  tabSteps = constrain((int)prefs.getUChar("tab_steps", 3), 1, 5);
  scrollSteps = constrain((int)prefs.getUChar("scroll_steps", 2), 1, 5);
  rcAlways = prefs.getBool("rc_always", true);
  themeSel = constrain((int)prefs.getUChar("ui_theme", 0), 0, 2);   // (v1.1's "theme" key forced green by default)
  showPlaces = prefs.getBool("show_places", true);
  favs = prefs.getString("favs", ",");
  if (!favs.startsWith(",")) favs = "," + favs;
  navOn = parsePlace(prefs.getString("nav", ""), navTo);
  wifiSsid = prefs.getString("wifi_ssid", "");
  wifiPass = prefs.getString("wifi_pass", "");
  loadHosts();
  ssh.begin();
  termSize = constrain((int)prefs.getUChar("term_size", 0), 0, 2);
  termMargin = constrain((int)prefs.getUChar("term_margin", 6), 0, 12);
  term.keyboardSteps(scrollSteps, tabSteps);
  termArrows = prefs.getBool("term_wheel", false);
  WiFi.mode(WIFI_OFF);                          // Wi-Fi only while the TERM tab needs it

  term.begin();
  term.onKnob(onWheel);
  term.onKeyboard(onTextResult);
  term.onReady([]() {
    pip.log("hub: terminal ready (PIP LINK v%s)  heap %u (largest %u)  psram %u free", PIPLINK_VERSION,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)ESP.getFreePsram());
    term.theme(themeArg());
    term.controls(true);                        // the buttons + side switch are ours while PIP LINK is open
    // this software needs the holotape that came with it (e.g. the BBS screen size, the keyboard's wheel
    // steps): flashing the unit doesn't update the Pip-Boy side, PIP LINK Setup's PIP-BOY page does
    if (pip.termVersion().toFloat() < String(PIPLINK_VERSION).toFloat())
      note("Pip-Boy holotape is v" + pip.termVersion() + " - install v" + String(PIPLINK_VERSION) + " with PIP LINK Setup", 15000);
    wptTries = 0;
    requestPlaces();                            // the saved locations live on the Pip-Boy SD
    render();
  });
  term.onOther(onTermOther);
  term.onButton([](int b) {
    pip.log("hub: button %d", b);
    if (b == 0) statsButton(); else if (b == 1) itemsButton(); else jumpTab(PG_RAD);
  });
  term.onSwitch([](int pos) { pip.log("hub: switch %d", pos); jumpTab(pos); });
  term.onVtSize([](int c, int r) {
    if (c != vt.cols() || r != vt.rows()) pip.log("term: canvas is %dx%d, expected %dx%d", c, r, vt.cols(), vt.rows());
  });
  term.onConfig([](const String& cfg) {          // key=value lines from HOLO/TERM/CONFIG.TXT
    int start = 0;
    while (start < (int)cfg.length()) {
      int nl = cfg.indexOf('\n', start);
      if (nl < 0) nl = cfg.length();
      String line = cfg.substring(start, nl);
      line.trim();
      start = nl + 1;
      if (line.startsWith("tz=") && line.length() > 3 && line.substring(3) != prefs.getString("tz", TIMEZONE)) {
        prefs.putString("tz", line.substring(3));
        setenv("TZ", line.substring(3).c_str(), 1);
        tzset();
        pip.log("hub: time zone %s", line.substring(3).c_str());
        render();
      }
    }
  });
  term.onMapInfo([](int zmin, int zmax, double lat, double lon) {
    mapHasData = zmax >= zmin;
    if (mapHasData) { mapZmin = zmin; mapZmax = zmax; mapDefLat = lat; mapDefLon = lon; }
    else { mapZmin = 10; mapZmax = 17; }
    mapZoom = constrain(mapZoom, mapZmin, mapZmax);
    pip.log("hub: map pack %s (zoom %d-%d)", mapHasData ? "found" : "missing", zmin, zmax);
    render();
  });
  pip.begin();

  BleHub::begin();
  for (MeshRadio* r : { (MeshRadio*)&meshcore, (MeshRadio*)&meshtastic }) {
    r->onLog([](const String& s) { pip.log("%s", s.c_str()); });
    r->onRoster(syncRoster);
    r->onMessage(onMessage);
    r->onState(onMeshState);
    r->begin();
  }
  kbd.onLog([](const String& s) { pip.log("%s", s.c_str()); });
  kbd.onKey(onKey);
  kbd.onState(onKbdState);
  kbd.begin();
  rc.onLog([](const String& s) { pip.log("%s", s.c_str()); });
  rc.onState([](BleRadiaCode::State) { render(); });
  rc.onData(onRadData);
  rc.clock([]() { return radio->now(); });
  rc.begin();

  fsOk = LittleFS.begin(true);                // formats the flash filesystem on first use
  if (!fsOk) pip.log("hub: flash filesystem unavailable - history won't persist");

  String saved = prefs.getString("radio", "");
  if (saved.length()) {                       // reconnect to the last radio automatically
    pip.log("hub: saved radio %s", saved.c_str());
    histPath = histFileFor(saved);
    histLoad();
    radio = prefs.getUChar("radio_kind", RK_MESHCORE) == RK_MESHTASTIC ? (MeshRadio*)&meshtastic : (MeshRadio*)&meshcore;
    radio->selectRadio(saved, prefs.getUInt("pin", radio == &meshcore ? 123456 : 0));
    view = LIST;
  } else {
    pickKind = BleHub::MESHCORE;              // first run: choose a radio
    view = PICK;
  }
  String k = prefs.getString("kbd_addr", "");
  if (k.length()) kbd.use(k, prefs.getUChar("kbd_type", 0), prefs.getString("kbd_name", ""));
  String r = prefs.getString("rc_addr", "");
  if (r.length()) rc.use(r, prefs.getUChar("rc_type", 0), prefs.getString("rc_name", ""));
}

void loop() {
  pip.poll();
  BleHub::poll();
  meshcore.poll();
  meshtastic.poll();
  static bool pinPrompted = false;              // Meshtastic: the radio is showing a PIN - ask for it
  if (radio->state() == MeshRadio::NEED_PIN) {
    if (!pinPrompted && term.ready() && !term.keyboardOpen() && !inputActive) {
      pinPrompted = true;
      term.sound("SELECT");
      askPin(RK_MESHTASTIC, radio->radioName(), "");
    }
  } else {
    pinPrompted = false;
  }
  kbd.poll();
  rc.poll();
  termPoll();

  // only the mesh link runs all the time; everything else works while its tab is open
  bool onMap = view == LIST && page == PG_MAP;
  bool onRad = view == LIST && page == PG_RAD;
  pip.setAckTimeout(onMap ? 5000 : 1500);       // a detailed map screen takes the Pip-Boy 1-3 s to draw
  if (mapPending && pip.queued() == 0) {
    mapPending = false;
    if (onMap) render();                          // the newest view, now that the Pip-Boy is ready for it
  }
  if (onRad) radTabSeen = millis();
  if (onRad) radAlert = false;                  // the alarm has been seen
  static uint32_t lastLeds = 0;                 // button lights (PipTerm only sends changes)
  if (millis() - lastLeds > 250) { lastLeds = millis(); term.leds(ledStates()); }
  rc.setActive(rcAlways || onRad || (radTabSeen && millis() - radTabSeen < 15000));   // short grace while flipping tabs
  bool lookingFor = view == PICK || view == PAIRKB || radio->state() == MeshRadio::SEARCHING ||
                    (rc.active() && rc.state() == BleRadiaCode::SEARCHING);
  BleHub::setScan(lookingFor ? BleHub::SCAN_FAST : kbd.state() == BleKeyboard::SEARCHING ? BleHub::SCAN_SLOW : BleHub::SCAN_OFF);

  // position: every 15 s while the map is open; contacts every 5 min on the map
  static uint32_t lastPos = 0, lastContacts = 0;
  if (onMap && radio->state() == MeshRadio::ONLINE && millis() - lastPos > 15000UL) {
    lastPos = millis();
    radio->refreshPosition();
  }
  if (onMap && radio->state() == MeshRadio::ONLINE && millis() - lastContacts > 300000UL) {
    lastContacts = millis();
    radio->refreshContacts();
  }
  updateFix();
  static uint32_t lastMapHdr = 0;             // keep "fix Ns ago" honest
  if (onMap && millis() - lastMapHdr > 5000) { lastMapHdr = millis(); render(); }

  BleHub::debugUnknown(view == PICK && pickKind == BleHub::KEYBOARD);   // keyboard picker: log unknown devices
  for (const String& l : BleHub::takeDebug()) pip.log("scan: unknown %s", l.c_str());
  static uint32_t lastScanLog = 0;            // pickers: log what the scan hears (keyboard troubleshooting)
  if (view == PICK && millis() - lastScanLog > 10000) {
    lastScanLog = millis();
    pip.log("scan: %s", BleHub::summary().c_str());
  }
  static uint32_t lastMem = 0;                // the Pip-Boy's free memory in the log every 2 minutes
  if (term.ready() && millis() - lastMem > 120000) {
    lastMem = millis();
    pip.send("print(\"<<T MEM \"+process.memory().free+\"/\"+process.memory().total+\">>\");setTimeout(function(){print(\" \")},15)");
  }
  static uint32_t lastLive = 0;               // pickers and the pairing screen are live views
  if ((view == PICK || view == PAIRKB || (view == LIST && (page == PG_SET || (page == PG_TERM && ts == TS_IDLE)))) &&
      millis() - lastLive > 1000) {
    lastLive = millis();
    render();
  }
  static bool flashShown = false;             // clear the footer note when it expires
  bool showing = flash.length() && millis() < flashUntil;
  if (flashShown && !showing) render();
  flashShown = showing;
  delay(5);
}
