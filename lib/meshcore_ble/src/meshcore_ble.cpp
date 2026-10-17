#include "meshcore_ble.h"
#include "ble_hub.h"

namespace {
const NimBLEUUID NUS ("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
const NimBLEUUID RX_C("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");   // ESP -> radio (write)
const NimBLEUUID TX_C("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");   // radio -> ESP (notify)

struct Frame { uint16_t len; uint8_t d[250]; };
QueueHandle_t frameQ = nullptr;
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* rxChar = nullptr;
NimBLEAddress connectAddr;
volatile bool lostLink = false;
uint32_t gPass = 123456;

void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  Frame f;
  f.len = len > sizeof(f.d) ? sizeof(f.d) : len;
  memcpy(f.d, data, f.len);
  xQueueSend(frameQ, &f, 0);
}

class ClientCB : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& ci) override { NimBLEDevice::injectPassKey(ci, gPass); }
  void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t) override { NimBLEDevice::injectConfirmPasskey(ci, true); }
  void onDisconnect(NimBLEClient*, int) override { lostLink = true; }
};
ClientCB clientCB;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
float deg(const uint8_t* p) { return (int32_t)le32(p) / 1e6f; }      // lat/lon are int32 x 1e6

// printable ASCII from [start..], stops at NUL (the Pip-Boy font has no other glyphs anyway)
String str(const uint8_t* d, size_t n, size_t start, size_t maxn) {
  String s;
  for (size_t i = start; i < n && s.length() < maxn; i++) {
    if (!d[i]) break;
    char c = (char)d[i];
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}
}  // namespace

// ------------------------------------------------------------------ setup / selection
void MeshCoreBLE::begin() {
  frameQ = xQueueCreate(40, sizeof(Frame));
  log("mesh: ready (discovery via BleHub)");
}

void MeshCoreBLE::selectRadio(const String& advName, uint32_t passkey) {
  if (st == ONLINE || st == CONNECTING) forgetRadio();
  target = advName;
  gPass = passkey;
  self = "";
  contactList.clear();
  channelList.clear();
  radioTime = 0;
  selfLat = selfLon = 0; posAt = 0;
  retryAt = millis();
  setState(SEARCHING);
  log("mesh: selected " + advName);
}

void MeshCoreBLE::forgetRadio() {
  target = "";
  self = "";
  contactList.clear();
  channelList.clear();
  cmdq.clear();
  rxChar = nullptr;
  if (client && client->isConnected()) client->disconnect();
  setState(IDLE);
}

MeshCoreBLE::ConnResult MeshCoreBLE::connectNow() {
  if (client) { NimBLEDevice::deleteClient(client); client = nullptr; }
  rxChar = nullptr;
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCB, false);
  client->setConnectTimeout(6000);
  log("mesh: connecting to " + target);
  if (!client->connect(connectAddr)) { log("mesh: connect failed (out of range or busy?)"); return CONN_FAIL; }
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);   // MeshCore shows/has a fixed PIN; we type it
  if (!client->secureConnection()) {
    log("mesh: pairing failed - PIN rejected?");
    client->disconnect();
    NimBLEDevice::deleteBond(connectAddr);                   // retry with a new PIN must pair fresh
    return CONN_BADPIN;
  }
  NimBLERemoteService* svc = client->getService(NUS);
  NimBLERemoteCharacteristic* rx = svc ? svc->getCharacteristic(RX_C) : nullptr;
  NimBLERemoteCharacteristic* tx = svc ? svc->getCharacteristic(TX_C) : nullptr;
  if (!rx || !tx || !tx->subscribe(true, onNotify)) { log("mesh: service/subscribe failed"); client->disconnect(); return CONN_FAIL; }
  rxChar = rx;
  cmdq.clear();
  const uint8_t appstart[] = { 0x01, 0x01, 0, 0, 0, 0, 0, 0, 'P', 'I', 'P', ' ', 'L', 'I', 'N', 'K' };
  queueCmd(appstart, sizeof(appstart));
  log("mesh: paired, APP_START sent");
  return CONN_OK;
}

// ------------------------------------------------------------------ poll
void MeshCoreBLE::poll() {
  if (lostLink) {
    lostLink = false;
    bool really = !(client && client->isConnected());       // ignore stale events from old attempts
    if (really && (st == ONLINE || st == CONNECTING)) {
      rxChar = nullptr;
      cmdq.clear();
      log("mesh: radio link lost, reconnecting");
      setState(SEARCHING);
      retryAt = millis() + 3000;
    }
  }

  if (st == SEARCHING && (int32_t)(millis() - retryAt) >= 0) {
    if (BleHub::lookup(BleHub::MESHCORE, target, connectAddr)) {
      BleHub::pause(true);                                  // no scanning while connecting
      setState(CONNECTING);
      ConnResult r = connectNow();
      BleHub::pause(false);
      if (r == CONN_OK) setState(ONLINE);
      else if (r == CONN_BADPIN) setState(PAIR_FAILED);
      else { setState(SEARCHING); retryAt = millis() + 3000; }
    } else if (millis() - lastDiag > 10000) {
      lastDiag = millis();
      String names;
      for (const BleHub::Found& r : BleHub::found(BleHub::MESHCORE)) names += (names.length() ? ", " : "") + r.name + " (" + r.rssi + ")";
      log("mesh: looking for " + target + " - " + String((unsigned)BleHub::advertsHeard()) + " adverts heard, radios: " +
          (names.length() ? names : String("none")));
    }
  }

  Frame f;
  while (xQueueReceive(frameQ, &f, 0) == pdTRUE) handleFrame(f.d, f.len);

  // one command per ~60 ms, always from loop() context (never from the notify callback)
  if (st == ONLINE && rxChar && !cmdq.empty() && millis() - lastCmd > 60) {
    std::vector<uint8_t>& c = cmdq.front();
    rxChar->writeValue(c.data(), c.size(), false);
    cmdq.pop_front();
    lastCmd = millis();
  }
}

void MeshCoreBLE::refreshPosition() {
  if (st != ONLINE) return;
  quietSelf = true;
  const uint8_t appstart[] = { 0x01, 0x01, 0, 0, 0, 0, 0, 0, 'P', 'I', 'P', ' ', 'L', 'I', 'N', 'K' };
  queueCmd(appstart, sizeof(appstart));
}

void MeshCoreBLE::refreshContacts() {
  if (st != ONLINE) return;
  const uint8_t getContacts[] = { 0x04, 0, 0, 0, 0 };
  queueCmd(getContacts, sizeof(getContacts));
}

void MeshCoreBLE::queueCmd(const uint8_t* d, size_t n) { cmdq.push_back(std::vector<uint8_t>(d, d + n)); }

// ------------------------------------------------------------------ frames
void MeshCoreBLE::handleFrame(const uint8_t* d, size_t n) {
  if (!n) return;
  const uint8_t sync[] = { 0x0A };
  switch (d[0]) {
    case 0x05: {                                           // SELF_INFO: lat 36, lon 40, name at 58
      if (n >= 44) {
        float la = deg(d + 36), lo = deg(d + 40);
        if (la != selfLat || lo != selfLon || !posAt)
          log("mesh: position " + String(la, 6) + "," + String(lo, 6) + (la == 0 && lo == 0 ? " (none set)" : ""));
        selfLat = la; selfLon = lo; posAt = millis();
      }
      if (quietSelf) { quietSelf = false; break; }
      self = str(d, n, 58, 32);
      log("mesh: online as \"" + self + "\"");
      const uint8_t getTime[] = { 0x05 };
      const uint8_t getContacts[] = { 0x04, 0, 0, 0, 0 };
      queueCmd(getTime, sizeof(getTime));
      queueCmd(getContacts, sizeof(getContacts));
      break;
    }
    case 0x09:                                             // CURR_TIME
      if (n >= 5) setClock(le32(d + 1));
      break;
    case 0x02: pending.clear(); pendingKeys.clear(); break;   // CONTACTS_START
    case 0x03:                                             // CONTACT
      if (n >= 132) {
        McContact c;
        c.name = str(d, n, 100, 32);
        memcpy(c.prefix, d + 1, 6);
        c.type = d[33];
        if (n >= 144) { c.lastAdvert = le32(d + 132); c.lat = deg(d + 136); c.lon = deg(d + 140); }
        if (c.name.length()) {
          Key k;
          memcpy(k.k, d + 1, 32);
          pending.push_back(c);
          pendingKeys.push_back(k);
        }
      }
      break;
    case 0x04:                                             // CONTACTS_END -> ask for channel names
      contactList = pending;
      keys = pendingKeys;
      pending.clear();
      pendingKeys.clear();
      log("mesh: " + String(contactList.size()) + " contacts");
      if (channelList.empty())                             // first pull only; refreshes keep the names
        for (uint8_t i = 0; i < 8; i++) { const uint8_t g[] = { 0x1F, i }; queueCmd(g, sizeof(g)); }
      if (rosterH) rosterH();
      break;
    case 0x12:                                             // CHANNEL_INFO: idx 1, name 2..33
      if (n >= 34) {
        int idx = d[1];
        String name = str(d, n, 2, 32);
        if (!name.length() && idx == 0) name = "PUBLIC";
        if (name.length()) {
          channelList.push_back({ idx, name });
          if (rosterH) rosterH();
        }
        if (idx == 7) log("mesh: " + String(channelList.size()) + " channels");
      }
      break;
    case 0x83: queueCmd(sync, sizeof(sync)); break;        // MSG_WAITING -> drain
    case 0x10: case 0x07: {                                // direct message
      McMessage m;
      m.isChannel = false; m.chan = -1;
      size_t off;
      if (d[0] == 0x10) { if (n < 16) break; m.snr = (int8_t)d[1] / 4.0f; memcpy(m.prefix, d + 4, 6); m.ts = le32(d + 12); off = d[11] == 2 ? 20 : 16; }
      else              { if (n < 13) break; m.snr = 0;                   memcpy(m.prefix, d + 1, 6); m.ts = le32(d + 9);  off = d[8] == 2 ? 17 : 13; }
      m.text = str(d, n, off, 200);
      m.from = contactName(m.prefix);
      if (msgH) msgH(m);
      queueCmd(sync, sizeof(sync));
      break;
    }
    case 0x11: case 0x08: {                                // channel message: "NAME: text"
      McMessage m;
      m.isChannel = true;
      memset(m.prefix, 0, sizeof(m.prefix));
      size_t off;
      if (d[0] == 0x11) { if (n < 11) break; m.snr = (int8_t)d[1] / 4.0f; m.chan = d[4]; m.ts = le32(d + 7); off = 11; }
      else              { if (n < 8) break;  m.snr = 0;                   m.chan = d[1]; m.ts = le32(d + 4); off = 8; }
      String t = str(d, n, off, 200);
      int c = t.indexOf(": ");
      if (c > 0 && c <= 24) { m.from = t.substring(0, c); m.text = t.substring(c + 2); }
      else { m.from = "?"; m.text = t; }
      if (msgH) msgH(m);
      queueCmd(sync, sizeof(sync));
      break;
    }
    default: break;                                        // OK/ERR/SENT/0x88 telemetry etc.
  }
}

// ------------------------------------------------------------------ helpers / send
bool MeshCoreBLE::sendChannel(int idx, const String& text) {
  if (st != ONLINE) return false;
  String full = self.length() ? self + ": " + text : text;   // channel convention: sender prefixes name
  uint32_t ts = now();
  std::vector<uint8_t> b = { 0x03, 0x00, (uint8_t)idx,
                             (uint8_t)ts, (uint8_t)(ts >> 8), (uint8_t)(ts >> 16), (uint8_t)(ts >> 24) };
  for (size_t i = 0; i < full.length(); i++) b.push_back((uint8_t)full[i]);
  cmdq.push_back(b);
  log("mesh: sent to channel " + String(idx));
  return true;
}

bool MeshCoreBLE::advertise() {
  if (st != ONLINE) return false;
  cmdq.push_back({ 0x07, 0x01 });                          // CMD_SEND_SELF_ADVERT, 1 = flood (0 = zero hop)
  log("mesh: sending a flood advert");
  return true;
}

bool MeshCoreBLE::removeContact(const uint8_t prefix[6]) {
  if (st != ONLINE) return false;
  for (size_t i = 0; i < contactList.size() && i < keys.size(); i++) {
    if (memcmp(contactList[i].prefix, prefix, 6) != 0) continue;
    std::vector<uint8_t> b = { 0x0F };                     // CMD_REMOVE_CONTACT <public key 32>
    b.insert(b.end(), keys[i].k, keys[i].k + 32);
    cmdq.push_back(b);
    log("mesh: removing contact " + contactList[i].name);
    contactList.erase(contactList.begin() + i);
    keys.erase(keys.begin() + i);
    if (rosterH) rosterH();
    return true;
  }
  return false;
}

bool MeshCoreBLE::sendDirect(const uint8_t prefix[6], const String& text) {
  if (st != ONLINE) return false;
  uint32_t ts = now();
  std::vector<uint8_t> b = { 0x02, 0x00, 0x00,
                             (uint8_t)ts, (uint8_t)(ts >> 8), (uint8_t)(ts >> 16), (uint8_t)(ts >> 24) };
  b.insert(b.end(), prefix, prefix + 6);
  for (size_t i = 0; i < text.length(); i++) b.push_back((uint8_t)text[i]);
  cmdq.push_back(b);
  log("mesh: sent DM to " + contactName(prefix));
  return true;
}
