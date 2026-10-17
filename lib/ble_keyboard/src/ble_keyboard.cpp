#include "ble_keyboard.h"
#include "ble_hub.h"

namespace {
const NimBLEUUID HID_SERVICE((uint16_t)0x1812);
const NimBLEUUID PROTOCOL_MODE((uint16_t)0x2A4E);
const NimBLEUUID BOOT_KB_INPUT((uint16_t)0x2A22);
const NimBLEUUID REPORT((uint16_t)0x2A4D);

struct Report { uint8_t len; uint8_t d[16]; };
QueueHandle_t reportQ = nullptr;
NimBLEClient* client = nullptr;
volatile bool lostLink = false;
uint32_t gCode = 0;

void onReport(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  Report r;
  r.len = len > sizeof(r.d) ? sizeof(r.d) : len;
  memcpy(r.d, data, r.len);
  xQueueSend(reportQ, &r, 0);
}

class ClientCB : public NimBLEClientCallbacks {
  uint32_t onPassKeyDisplay(NimBLEConnInfo&) override { return gCode; }      // we show it; user types it
  void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t) override { NimBLEDevice::injectConfirmPasskey(ci, true); }
  void onDisconnect(NimBLEClient*, int) override { lostLink = true; }
};
ClientCB clientCB;

// HID usage -> ASCII (US layout). Index = usage - 0x04 for letters; separate tables below.
const char NUM[]       = "1234567890";
const char NUM_SHIFT[] = "!@#$%^&*()";
//                        2D   2E   2F   30   31   32   33   34   35   36   37   38
const char PUNCT[]       = { '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/' };
const char PUNCT_SHIFT[] = { '_', '+', '{', '}', '|',  '~', ':', '"',  '~', '<', '>', '?' };
}  // namespace

// ------------------------------------------------------------------ lifecycle
void BleKeyboard::begin() { reportQ = xQueueCreate(16, sizeof(Report)); }

void BleKeyboard::pair(const String& a, uint8_t t, const String& n) {
  if (addrStr.length() && addrStr != a) unpair();             // a different keyboard replaces the old one
  forget();
  addrStr = a; addrType = t; kbName = n;
  NimBLEDevice::deleteBond(NimBLEAddress(std::string(a.c_str()), t));   // pair fresh
  pairing = true;
  code = 100000 + (esp_random() % 900000);                             // never the 123456 default
  gCode = code;
  retryAt = millis() + 600;                // give the Pip-Boy time to show the code before we block
  setState(SEARCHING);
  log("kbd: pairing " + n + " (code " + String(code) + ")");
}

void BleKeyboard::use(const String& a, uint8_t t, const String& n) {
  forget();
  addrStr = a; addrType = t; kbName = n;
  pairing = false;
  retryAt = millis();
  setState(SEARCHING);
  log("kbd: using " + n);
}

void BleKeyboard::unpair() {
  String a = addrStr;
  uint8_t t = addrType;
  forget();
  if (a.length()) NimBLEDevice::deleteBond(NimBLEAddress(std::string(a.c_str()), t));
}

void BleKeyboard::forget() {
  if (client && client->isConnected()) client->disconnect();
  addrStr = ""; kbName = "";
  pairing = false;
  memset(prevKeys, 0, sizeof(prevKeys));
  setState(NONE);
}

void BleKeyboard::setState(State s) {
  if (s == st) return;
  st = s;
  if (stateH) stateH(s);
}

bool BleKeyboard::connectNow() {
  NimBLEAddress a;
  // keyboards advertise when woken (any key); match by address, or by name if it rotated its address
  if (!BleHub::lookup(BleHub::KEYBOARD, addrStr, a) && !(kbName.length() && BleHub::lookup(BleHub::KEYBOARD, kbName, a)))
    return false;
  if (client) { NimBLEDevice::deleteClient(client); client = nullptr; }
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCB, false);
  client->setConnectTimeout(5000);
  setState(CONNECTING);
  log("kbd: connecting to " + kbName);
  // set before connecting: many keyboards send a security request the moment the link is up
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // we display the code, the keyboard types it
  NimBLEDevice::setSecurityPasskey(gCode ? gCode : 1);
  if (!client->connect(a)) {
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
    log("kbd: connect failed");
    return false;
  }
  bool secure = client->secureConnection();                 // blocks while the user types the code
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);  // back to the default for other devices
  if (!secure) { log("kbd: pairing failed"); client->disconnect(); return false; }

  NimBLERemoteService* hid = client->getService(HID_SERVICE);
  if (!hid) { log("kbd: no HID service"); client->disconnect(); return false; }
  bool ok = false;
  NimBLERemoteCharacteristic* proto = hid->getCharacteristic(PROTOCOL_MODE);
  NimBLERemoteCharacteristic* boot = hid->getCharacteristic(BOOT_KB_INPUT);
  if (proto && boot && boot->canNotify()) {                 // boot protocol = simple fixed 8-byte reports
    uint8_t bootMode = 0;
    proto->writeValue(&bootMode, 1, false);
    ok = boot->subscribe(true, onReport);
  }
  if (!ok)                                                  // fall back to its regular input reports
    for (NimBLERemoteCharacteristic* c : hid->getCharacteristics(true))
      if (c->getUUID() == REPORT && c->canNotify() && c->subscribe(true, onReport)) ok = true;
  if (!ok) { log("kbd: could not subscribe to key reports"); client->disconnect(); return false; }
  addrStr = String(a.toString().c_str());
  addrType = a.getType();
  log("kbd: online (" + kbName + ")");
  return true;
}

// ------------------------------------------------------------------ poll
void BleKeyboard::poll() {
  if (lostLink) {
    lostLink = false;
    if (st == ONLINE && !(client && client->isConnected())) {
      log("kbd: keyboard went to sleep / out of range - will reconnect when it wakes");
      memset(prevKeys, 0, sizeof(prevKeys));
      setState(SEARCHING);
      retryAt = millis() + 1000;
    }
  }
  if (st == SEARCHING && (int32_t)(millis() - retryAt) >= 0) {
    retryAt = millis() + 2000;
    BleHub::pause(true);
    bool ok = connectNow();
    BleHub::pause(false);
    if (ok) { pairing = false; setState(ONLINE); }
    else if (st == CONNECTING) { setState(pairing ? FAILED : SEARCHING); }
  }
  Report r;
  while (xQueueReceive(reportQ, &r, 0) == pdTRUE) handleReport(r.d, r.len);
}

// ------------------------------------------------------------------ reports -> keys
void BleKeyboard::handleReport(const uint8_t* d, size_t n) {
  // boot format: [modifiers, reserved, k0..k5]; some report maps drop the reserved byte
  const uint8_t* keys;
  if (n >= 8) keys = d + 2;
  else if (n == 7) keys = d + 1;
  else return;
  uint8_t mods = d[0];
  bool shift = mods & 0x22, ctrl = mods & 0x11, alt = mods & 0x44;
  for (int i = 0; i < 6; i++) {
    uint8_t k = keys[i];
    if (k < 4) continue;                                    // 0 = none, 1..3 = error/rollover
    bool held = false;
    for (int j = 0; j < 6; j++) if (prevKeys[j] == k) held = true;
    if (!held) emit(k, shift, ctrl, alt);
  }
  memcpy(prevKeys, keys, 6);
}

void BleKeyboard::emit(uint8_t u, bool shift, bool ctrl, bool alt) {
  KeyEvent e{ KeyEvent::CHAR, 0, ctrl, alt };
  if (u >= 0x04 && u <= 0x1D) {                             // a..z
    bool upper = shift != capsLock;
    e.ch = (upper ? 'A' : 'a') + (u - 0x04);
  } else if (u >= 0x1E && u <= 0x27) e.ch = (shift ? NUM_SHIFT : NUM)[u - 0x1E];
  else if (u >= 0x2D && u <= 0x38) e.ch = (shift ? PUNCT_SHIFT : PUNCT)[u - 0x2D];
  else if (u >= 0x59 && u <= 0x61) e.ch = '1' + (u - 0x59);  // keypad 1..9
  else switch (u) {
    case 0x2C: e.ch = ' '; break;
    case 0x62: e.ch = '0'; break;
    case 0x63: e.ch = '.'; break;
    case 0x54: e.ch = '/'; break;
    case 0x55: e.ch = '*'; break;
    case 0x56: e.ch = '-'; break;
    case 0x57: e.ch = '+'; break;
    case 0x28: case 0x58: e.type = KeyEvent::ENTER; break;
    case 0x29: e.type = KeyEvent::ESC; break;
    case 0x2A: e.type = KeyEvent::BACKSPACE; break;
    case 0x2B: e.type = KeyEvent::TAB; break;
    case 0x4C: e.type = KeyEvent::DEL; break;
    case 0x4B: e.type = KeyEvent::PGUP; break;
    case 0x4E: e.type = KeyEvent::PGDN; break;
    case 0x4A: e.type = KeyEvent::HOME; break;
    case 0x4D: e.type = KeyEvent::END; break;
    case 0x4F: e.type = KeyEvent::RIGHT; break;
    case 0x50: e.type = KeyEvent::LEFT; break;
    case 0x51: e.type = KeyEvent::DOWN; break;
    case 0x52: e.type = KeyEvent::UP; break;
    case 0x39: capsLock = !capsLock; return;
    default: return;                                        // F-keys, media keys etc.
  }
  if (keyH) keyH(e);
}
