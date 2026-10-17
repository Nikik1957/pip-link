#include "ble_hub.h"
#include <algorithm>

namespace {
const NimBLEUUID HID_SERVICE((uint16_t)0x1812);
const NimBLEUUID RADIACODE_SERVICE("e63215e5-7003-49d8-96b0-b024798fb901");
const NimBLEUUID MESHTASTIC_SERVICE("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
const uint16_t APPEARANCE_HID = 0x03C0;         // generic HID (some keyboards use it)
const uint16_t APPEARANCE_KEYBOARD = 0x03C1;
const uint16_t APPEARANCE_MOUSE = 0x03C2;
const uint16_t LOGITECH = 0x01DA;               // Bluetooth company ID in the manufacturer data

std::vector<BleHub::Found> table;       // written in the NimBLE task, read from loop()
SemaphoreHandle_t mx = nullptr;
volatile uint32_t heardCount = 0;
volatile bool paused = false;
volatile bool debugOn = false;          // log devices we don't recognise (keyboard troubleshooting)
std::vector<std::string> dbgSeen;       // addresses already reported
std::vector<String> dbgOut;             // lines waiting for the log
BleHub::ScanMode mode = BleHub::SCAN_FAST, running = BleHub::SCAN_OFF;

bool classify(const NimBLEAdvertisedDevice* d, const std::string& name, BleHub::Kind& kind) {
  if (name.rfind("MeshCore", 0) == 0) { kind = BleHub::MESHCORE; return true; }
  if (d->isAdvertisingService(MESHTASTIC_SERVICE)) { kind = BleHub::MESHTASTIC; return true; }
  if (name.rfind("RC-", 0) == 0 || d->isAdvertisingService(RADIACODE_SERVICE)) { kind = BleHub::RADIACODE; return true; }
  uint16_t app = d->getAppearance();
  if (app == APPEARANCE_MOUSE) return false;
  String n = String(name.c_str());
  n.toLowerCase();
  bool kbName = n.indexOf("keyboard") >= 0 || n.indexOf("keys") >= 0 ||          // "Keyboard K380", "MX Keys"
                (n.length() >= 4 && n[0] == 'k' && isDigit(n[1]) && isDigit(n[2]) && isDigit(n[3]));   // "K380"
  // Logitech keyboards in pairing mode don't always put a name or the HID service in the advert itself
  std::string md = d->getManufacturerData();
  bool logi = md.size() >= 2 && ((uint8_t)md[0] | (uint8_t)md[1] << 8) == LOGITECH && d->isConnectable();
  if (app == APPEARANCE_KEYBOARD || app == APPEARANCE_HID || d->isAdvertisingService(HID_SERVICE) || kbName || logi) {
    kind = BleHub::KEYBOARD;
    return true;
  }
  // a keyboard we've paired with reconnects with a bare advert (often directed at us): no name, no services.
  // The radios and the RadiaCode always say who they are (above), so a bonded device left over is the keyboard.
  if (NimBLEDevice::isBonded(d->getAddress())) {
    kind = BleHub::KEYBOARD;
    return true;
  }
  return false;
}

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    heardCount++;
    std::string name = d->getName();
    BleHub::Kind kind;
    if (!classify(d, name, kind)) {
      if (debugOn && d->getRSSI() > -80 && xSemaphoreTake(mx, 0) == pdTRUE) {
        std::string a = d->getAddress().toString();
        if (std::find(dbgSeen.begin(), dbgSeen.end(), a) == dbgSeen.end() && dbgSeen.size() < 60) {
          dbgSeen.push_back(a);
          char buf[160];
          snprintf(buf, sizeof(buf), "%s type%d \"%s\" app 0x%04x rssi %d %s%s svc", a.c_str(), d->getAddress().getType(),
                   name.c_str(), d->haveAppearance() ? d->getAppearance() : 0, d->getRSSI(),
                   d->isConnectable() ? "conn" : "nonconn", d->getAdvType() == 1 ? " DIRECTED" : "");
          String line(buf);
          for (int i = 0; i < d->getServiceUUIDCount() && i < 3; i++) line += String(" ") + d->getServiceUUID(i).toString().c_str();
          std::string md = d->getManufacturerData();
          if (md.size() >= 2) { snprintf(buf, sizeof(buf), " mfr 0x%02x%02x", (uint8_t)md[1], (uint8_t)md[0]); line += buf; }
          dbgOut.push_back(line);
        }
        xSemaphoreGive(mx);
      }
      return;
    }
    if (xSemaphoreTake(mx, 0) != pdTRUE) return;
    NimBLEAddress a = d->getAddress();
    bool hit = false;
    for (BleHub::Found& f : table) {
      if (f.addr == a) {
        if (name.size()) f.name = name.c_str();
        f.rssi = d->getRSSI(); f.seen = millis(); f.kind = kind;
        hit = true;
        break;
      }
    }
    if (!hit) {
      // full: reuse the slot heard from longest ago (devices with rotating addresses would fill it for good)
      BleHub::Found f = { String(name.c_str()), a, d->getRSSI(), millis(), kind };
      if (table.size() < 32) table.push_back(f);
      else *std::min_element(table.begin(), table.end(), [](const BleHub::Found& x, const BleHub::Found& y) { return x.seen < y.seen; }) = f;
    }
    xSemaphoreGive(mx);
  }
};
}  // namespace

namespace BleHub {

void begin() {
  mx = xSemaphoreCreateMutex();
  NimBLEDevice::init("PIP LINK");
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setSecurityAuth(true, true, true);          // bonding, MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);  // drivers switch this per device before pairing
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB());
  scan->setActiveScan(true);                                // names arrive in the scan response
  scan->setDuplicateFilter(false);                          // keep hearing devices: live RSSI, reconnects
  scan->setMaxResults(0);                                   // callbacks only
}

void setScan(ScanMode m) { mode = m; }

void poll() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  ScanMode want = paused ? SCAN_OFF : mode;
  if (want != running) {
    if (scan->isScanning()) scan->stop();
    if (want == SCAN_FAST) { scan->setInterval(100); scan->setWindow(60); }
    else if (want == SCAN_SLOW) { scan->setInterval(600); scan->setWindow(40); }
    running = want;
  }
  if (running != SCAN_OFF && !scan->isScanning()) scan->start(0, false);
}

void pause(bool on) {
  paused = on;
  if (on) { NimBLEDevice::getScan()->stop(); running = SCAN_OFF; }
}

std::vector<Found> found(Kind k, uint32_t maxAgeMs) {
  std::vector<Found> out;
  if (xSemaphoreTake(mx, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (const Found& f : table)
      if (f.kind == k && millis() - f.seen < maxAgeMs) out.push_back(f);
    xSemaphoreGive(mx);
  }
  std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) { return a.rssi > b.rssi; });
  return out;
}

bool lookup(Kind k, const String& key, NimBLEAddress& out, uint32_t maxAgeMs) {
  bool hit = false;
  if (xSemaphoreTake(mx, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (const Found& f : table) {
      if (f.kind != k || millis() - f.seen >= maxAgeMs) continue;
      if ((f.name.length() && f.name == key) || String(f.addr.toString().c_str()) == key) { out = f.addr; hit = true; break; }
    }
    xSemaphoreGive(mx);
  }
  return hit;
}

uint32_t advertsHeard() { return heardCount; }

void debugUnknown(bool on) {
  if (on == debugOn) return;
  debugOn = on;
  if (xSemaphoreTake(mx, pdMS_TO_TICKS(20)) == pdTRUE) { dbgSeen.clear(); dbgOut.clear(); xSemaphoreGive(mx); }
}

std::vector<String> takeDebug() {
  std::vector<String> v;
  if (xSemaphoreTake(mx, pdMS_TO_TICKS(20)) == pdTRUE) { v.swap(dbgOut); xSemaphoreGive(mx); }
  return v;
}

String summary() {
  String s;
  int n[4] = { 0, 0, 0, 0 };
  if (xSemaphoreTake(mx, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (const Found& f : table) {
      if (f.kind < 4) n[f.kind]++;
      if (f.kind == KEYBOARD && millis() - f.seen < 30000)
        s += String(" kb[") + f.name + " " + f.addr.toString().c_str() + " " + String(f.rssi) + "]";
    }
    xSemaphoreGive(mx);
  }
  return String(heardCount) + " adverts; table meshcore " + String(n[MESHCORE]) + " keyboard " + String(n[KEYBOARD]) +
         " radiacode " + String(n[RADIACODE]) + " meshtastic " + String(n[MESHTASTIC]) + s;
}

String label(const Found& f) {
  if (f.name.startsWith("MeshCore-")) return f.name.substring(9);
  if (f.name.length()) return f.name;
  return String(f.addr.toString().c_str());
}

}  // namespace BleHub
