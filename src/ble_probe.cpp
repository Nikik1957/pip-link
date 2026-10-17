// Pip-Boy Companion Hub â€” step 3: connect to the RAK's MeshCore companion over BLE.
// Scans for BYRD 650, connects, discovers the Nordic UART Service, subscribes
// to the TX characteristic, sends APP_START, and dumps every frame it receives.
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>
#include <vector>

static const char* TARGET   = "MeshCore-BYRD 650";
static const NimBLEUUID NUS ("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static const NimBLEUUID RX_C("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"); // app -> radio (write)
static const NimBLEUUID TX_C("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"); // radio -> app (notify)

static NimBLEAddress targetAddr;
static volatile bool found = false;
static bool connected = false;
static volatile uint32_t framesRx = 0;
static volatile bool selfInfoSeen = false;
static NimBLERemoteCharacteristic* g_rx = nullptr;  // write commands here
static volatile bool needGetContacts = false;
static volatile bool needSync = false;
static std::vector<std::string> g_contacts;         // parsed contact names
static std::string g_lastMsg;                       // last message text (printable)
static std::vector<std::string> g_recent;           // recent non-contact frames (diagnostic)

// printable string from [start..], stop at NUL
static std::string grabStr(const uint8_t* d, size_t len, size_t start, size_t maxn) {
  std::string s;
  for (size_t i = start; i < len && s.size() < maxn; i++) {
    if (d[i] == 0) break;
    char ch = (char)d[i];
    s += (ch >= 32 && ch < 127) ? ch : '.';
  }
  return s;
}
// full printable gutter from [start..end], NUL shown as '.'
static std::string gutter(const uint8_t* d, size_t len, size_t start) {
  std::string s;
  for (size_t i = start; i < len && s.size() < 120; i++) {
    char ch = (char)d[i];
    s += (ch >= 32 && ch < 127) ? ch : '.';
  }
  return s;
}

static const char* frameName(uint8_t t) {
  switch (t) {
    case 0x00: return "OK";
    case 0x01: return "ERR";
    case 0x02: return "CONTACTS_START";
    case 0x03: return "CONTACT";
    case 0x04: return "CONTACTS_END";
    case 0x05: return "SELF_INFO";
    case 0x06: return "SENT";
    case 0x07: return "CONTACT_MSG";
    case 0x08: return "CHANNEL_MSG";
    case 0x09: return "CURR_TIME";
    case 0x0A: return "NO_MORE_MSGS";
    case 0x0D: return "DEVICE_INFO";
    case 0x10: return "CONTACT_MSG_V3";
    case 0x11: return "CHANNEL_MSG_V3";
    case 0x80: return "PUSH_ADVERT";
    case 0x82: return "PUSH_SEND_CONF";
    case 0x83: return "PUSH_MSG_WAIT";
    default:   return "?";
  }
}

static void onNotify(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  uint8_t t = len ? data[0] : 0;
  framesRx++;
  size_t show = len < 200 ? len : 200;
  Serial.printf("<< len=%3u type=0x%02X %-14s |", (unsigned)len, t, frameName(t));
  for (size_t i = 1; i < show; i++) { char ch = data[i]; Serial.print((ch >= 32 && ch < 127) ? (char)ch : '.'); }
  Serial.println("|");

  // keep a rolling log of everything except the (spammy) contact frames
  if (t != 0x03) {
    char hdr[28]; snprintf(hdr, sizeof(hdr), "0x%02X %-14s |", t, frameName(t));
    g_recent.push_back(std::string(hdr) + gutter(data, len, 1) + "|");
    if (g_recent.size() > 12) g_recent.erase(g_recent.begin());
  }

  switch (t) {
    case 0x05: selfInfoSeen = true; needGetContacts = true; break;  // SELF_INFO -> pull contacts
    case 0x03: {                                                    // CONTACT -> adv_name at offset 100
      std::string nm = grabStr(data, len, 100, 32);
      if (!nm.empty()) g_contacts.push_back(nm);
      break;
    }
    case 0x08: g_lastMsg = grabStr(data, len, 8,  180); needSync = true; break;  // CHANNEL_MSG (plain)
    case 0x11: g_lastMsg = grabStr(data, len, 11, 180); needSync = true; break;  // CHANNEL_MSG_V3 (+SNR)
    case 0x07: g_lastMsg = grabStr(data, len, 13, 180); needSync = true; break;  // CONTACT_MSG (plain)
    case 0x10: g_lastMsg = grabStr(data, len, 16, 180); needSync = true; break;  // CONTACT_MSG_V3 (+SNR)
    case 0x83: needSync = true; break;                                           // MSGS_WAITING -> drain
    default: break;
  }
}

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (found) return;
    if (dev->getName() == TARGET) {
      targetAddr = dev->getAddress();
      found = true;
      NimBLEDevice::getScan()->stop();
      Serial.printf("target %s @ %s - stopping scan\n", TARGET, targetAddr.toString().c_str());
    }
  }
};

static const uint32_t BLE_PASSKEY = 123456;   // MeshCore default (change if you set a custom PIN)

class ClientCB : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    Serial.printf("  [sec] passkey requested -> injecting %lu\n", (unsigned long)BLE_PASSKEY);
    NimBLEDevice::injectPassKey(connInfo, BLE_PASSKEY);
  }
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    Serial.printf("  [sec] confirm passkey %lu -> accept\n", (unsigned long)pin);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    Serial.printf("  [sec] auth complete: encrypted=%d bonded=%d\n",
                  connInfo.isEncrypted(), connInfo.isBonded());
  }
};

static NimBLEClient* pClient = nullptr;

static bool doConnect() {
  if (pClient) { NimBLEDevice::deleteClient(pClient); pClient = nullptr; }
  pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCB(), false);
  Serial.printf("connecting to %s ...\n", targetAddr.toString().c_str());
  if (!pClient->connect(targetAddr)) {
    Serial.println("  connect FAILED");
    return false;
  }
  Serial.println("  connected. pairing / encrypting link...");
  if (!pClient->secureConnection()) {
    Serial.println("  secureConnection() failed (peripheral may require a passkey)");
    return false;
  }
  Serial.println("  discovering Nordic UART Service...");
  NimBLERemoteService* svc = pClient->getService(NUS);
  if (!svc) { Serial.println("  NUS not found!"); return false; }

  NimBLERemoteCharacteristic* rx = svc->getCharacteristic(RX_C);
  NimBLERemoteCharacteristic* tx = svc->getCharacteristic(TX_C);
  if (!rx || !tx) { Serial.println("  RX/TX characteristic missing!"); return false; }
  g_rx = rx;

  Serial.println("  subscribing to TX notifications...");
  if (!tx->subscribe(true, onNotify)) { Serial.println("  subscribe failed"); return false; }
  delay(250);

  // APP_START: 0x01, app_ver, 6 reserved, then app-name string
  uint8_t appstart[] = { 0x01, 0x01, 0,0,0,0,0,0, 'p','i','p','b','o','y' };
  Serial.println(">> sending APP_START (expect SELF_INFO 0x05 back)");
  rx->writeValue(appstart, sizeof(appstart), false);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("== Companion Hub :: step 3 - BLE companion handshake ==");
  NimBLEDevice::init("pipboy-hub");
  NimBLEDevice::setMTU(517);
  // MeshCore's nRF52 companion uses MITM + bonding with a fixed passkey (default 123456)
  NimBLEDevice::deleteAllBonds();                            // clear any stale just-works bond
  NimBLEDevice::setSecurityAuth(true, true, true);           // bonding, MITM, secure-connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);   // we supply the passkey
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new ScanCB());
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(100);
  Serial.printf("scanning for %s ...\n", TARGET);
  pScan->start(0, false);
}

void loop() {
  static uint32_t last = 0;
  if (found && !connected) {
    connected = doConnect();
    if (!connected) {
      Serial.println("  retrying connect in 3s...");
      delay(3000);            // keep targetAddr; just retry the connect, no re-scan
    } else {
      Serial.println("link up - listening for frames. (mesh traffic pushes CONTACT/CHANNEL_MSG)");
    }
  }
  // issue characteristic writes from loop context (not the notify callback)
  if (connected && g_rx) {
    if (needGetContacts) {
      needGetContacts = false;
      uint8_t cmd[] = { 0x04, 0, 0, 0, 0 };   // GET_CONTACTS, since=0 (all)
      Serial.println(">> GET_CONTACTS");
      g_rx->writeValue(cmd, sizeof(cmd), false);
    }
    if (needSync) {
      needSync = false;
      uint8_t cmd[] = { 0x0A };               // SYNC_NEXT_MESSAGE
      g_rx->writeValue(cmd, sizeof(cmd), false);
    }
  }

  if (millis() - last > 5000) {
    last = millis();
    Serial.printf("[status] connected=%d  frames=%lu  contacts=%u  SELF_INFO=%s\n",
                  connected ? 1 : 0, (unsigned long)framesRx,
                  (unsigned)g_contacts.size(), selfInfoSeen ? "YES" : "no");
    if (!g_contacts.empty()) {
      Serial.print("   nodes:");
      for (auto& n : g_contacts) { Serial.print(' '); Serial.print(n.c_str()); Serial.print(" |"); }
      Serial.println();
    }
    if (!g_lastMsg.empty()) { Serial.print("   last_msg: "); Serial.println(g_lastMsg.c_str()); }
    if (!g_recent.empty()) {
      Serial.println("   recent frames:");
      for (auto& l : g_recent) { Serial.print("     "); Serial.println(l.c_str()); }
    }
  }
  delay(50);
}
