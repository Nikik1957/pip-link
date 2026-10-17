#include "meshtastic_ble.h"
#include <algorithm>
#include "ble_hub.h"
#include "pb_lite.h"

// Field numbers: meshtastic/protobufs mesh.proto, channel.proto, config.proto (checked 2026-10).
namespace {
const NimBLEUUID SVC("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
const NimBLEUUID TO_RADIO("f75c76d2-129e-4dad-a1dd-7866124401e7");
const NimBLEUUID FROM_RADIO("2c55e69e-4993-11ed-b878-0242ac120002");
const NimBLEUUID FROM_NUM("ed9da18c-a800-4f66-a670-aa7547e34453");
const uint32_t MT_BROADCAST = 0xFFFFFFFF;
const uint32_t NONCE_ONLY_NODES = 69421;              // firmware 2.6+: just the node DB (older: full config)
const uint32_t NODE_MAX_AGE = 7 * 86400;              // list nodes heard in the last week...
const size_t NODE_MAX = 120;                          // ...at most this many, most recent first
const int READS_PER_POLL = 4;                         // FromRadio reads block ~10-50 ms each: keep the UI moving
const char* PRESETS[] = { "LongFast", "LongSlow", "VLongSlow", "MediumSlow", "MediumFast",
                          "ShortSlow", "ShortFast", "LongMod", "ShortTurbo", "LongTurbo" };

NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic *toRadio = nullptr, *fromRadio = nullptr;
NimBLEAddress connectAddr;
volatile bool lostLink = false, fromNumFlag = false;
volatile bool pinAsked = false, authDone = false, authOk = false;
NimBLEConnInfo* pinInfo = nullptr;                    // the pairing that's waiting for a passkey

class ClientCB : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& ci) override {   // the radio shows a PIN: answered from poll()
    delete pinInfo;
    pinInfo = new NimBLEConnInfo(ci);
    pinAsked = true;
  }
  void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t) override { NimBLEDevice::injectConfirmPasskey(ci, true); }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    authOk = ci.isEncrypted();
    authDone = true;
  }
  void onDisconnect(NimBLEClient*, int) override { lostLink = true; }
};
ClientCB clientCB;

void onFromNum(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool) { fromNumFlag = true; }

void numToPrefix(uint32_t num, uint8_t p[6]) {
  p[0] = num; p[1] = num >> 8; p[2] = num >> 16; p[3] = num >> 24; p[4] = p[5] = 0;
}
uint32_t prefixToNum(const uint8_t p[6]) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

uint8_t roleToType(uint32_t role) {                   // Config.DeviceConfig.Role -> PIP LINK contact type
  switch (role) {
    case 2: case 3: case 4: case 11: return 2;        // ROUTER, ROUTER_CLIENT, REPEATER, ROUTER_LATE
    case 6: return 4;                                 // SENSOR
    default: return 1;
  }
}
float fixedDeg(uint32_t v) { return (int32_t)v / 1e7f; }   // sfixed32 latitude_i / longitude_i
float fixedFloat(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
}  // namespace

// ------------------------------------------------------------------ selection / connection
void MeshtasticBLE::begin() { log("mt: ready"); }

void MeshtasticBLE::selectRadio(const String& advName, uint32_t p) {
  if (st != IDLE) forgetRadio();
  target = advName;
  pin = p;
  self = "";
  myNum = 0;
  contactList.clear();
  channelList.clear();
  radioTime = 0;
  selfLat = selfLon = 0; posAt = 0;
  retryAt = millis();
  setState(SEARCHING);
  log("mt: selected " + advName);
}

void MeshtasticBLE::forgetRadio() {
  target = "";
  self = "";
  contactList.clear();
  channelList.clear();
  toRadio = fromRadio = nullptr;
  phase = P_IDLE;
  if (client && client->isConnected()) client->disconnect();
  setState(IDLE);
}

void MeshtasticBLE::providePin(uint32_t p) {
  pin = p;
  if (st == NEED_PIN && pinInfo) {
    log("mt: sending PIN");
    NimBLEDevice::injectPassKey(*pinInfo, p);
    setState(CONNECTING);
  } else if (st == PAIR_FAILED) {
    selectRadio(target, p);                           // try again from the top with the new PIN
  }
}

bool MeshtasticBLE::connectNow() {
  if (client) { NimBLEDevice::deleteClient(client); client = nullptr; }
  toRadio = fromRadio = nullptr;
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCB, false);
  client->setConnectTimeout(6000);
  log("mt: connecting to " + target);
  if (!client->connect(connectAddr)) { log("mt: connect failed (out of range or busy?)"); return false; }
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);   // the radio displays the PIN, we type it
  pinAsked = authDone = authOk = false;
  pairStart = millis();
  phase = P_PAIRING;
  if (!client->secureConnection(true)) {             // async: the PIN (if any) is answered in poll()
    log("mt: couldn't start pairing");
    client->disconnect();
    return false;
  }
  return true;
}

void MeshtasticBLE::startSession() {
  NimBLERemoteService* svc = client->getService(SVC);
  toRadio = svc ? svc->getCharacteristic(TO_RADIO) : nullptr;
  fromRadio = svc ? svc->getCharacteristic(FROM_RADIO) : nullptr;
  NimBLERemoteCharacteristic* num = svc ? svc->getCharacteristic(FROM_NUM) : nullptr;
  if (!toRadio || !fromRadio || !num || !num->subscribe(true, onFromNum)) {
    log("mt: Meshtastic service missing - is this a Meshtastic radio?");
    client->disconnect();
    return;
  }
  log("mt: paired (mtu " + String(client->getMTU()) + "), downloading nodes + channels");
  pendingChannels.clear();
  requestConfig(esp_random() % 1000000 + 1000);       // full config: my info, nodes, channels, config
}

void MeshtasticBLE::requestConfig(uint32_t n) {
  nonce = n;
  pendingNodes.clear();
  if (n != NONCE_ONLY_NODES) pendingChannels.clear();
  sawConfigNodes = false;
  PbWriter w;
  w.u32(3, n);                                        // ToRadio.want_config_id
  toRadio->writeValue(w.b.data(), w.b.size(), true);
  phase = P_CONFIG;
  lastRefresh = millis();
}

// ------------------------------------------------------------------ poll
void MeshtasticBLE::poll() {
  if (lostLink) {
    lostLink = false;
    if (st != IDLE && st != SEARCHING && !(client && client->isConnected())) {
      toRadio = fromRadio = nullptr;
      phase = P_IDLE;
      if (st == ONLINE || st == CONNECTING) {
        log("mt: radio link lost, reconnecting");
        setState(SEARCHING);
        retryAt = millis() + 3000;
      }
    }
  }

  if (st == SEARCHING && (int32_t)(millis() - retryAt) >= 0) {
    if (BleHub::lookup(BleHub::MESHTASTIC, target, connectAddr)) {
      BleHub::pause(true);
      setState(CONNECTING);
      bool ok = connectNow();
      BleHub::pause(false);
      if (!ok) { setState(SEARCHING); retryAt = millis() + 3000; }
    } else if (millis() - lastDiag > 10000) {
      lastDiag = millis();
      String names;
      for (const BleHub::Found& r : BleHub::found(BleHub::MESHTASTIC)) names += (names.length() ? ", " : "") + r.name;
      log("mt: looking for " + target + " - Meshtastic radios heard: " + (names.length() ? names : String("none")));
    }
  }

  if (phase == P_PAIRING) {
    if (pinAsked) {
      pinAsked = false;
      if (pin) { log("mt: radio asked for its PIN - using the saved one"); NimBLEDevice::injectPassKey(*pinInfo, pin); }
      else { log("mt: radio asked for its PIN"); setState(NEED_PIN); }
    }
    if (authDone) {
      authDone = false;
      if (authOk) { setState(CONNECTING); startSession(); }
      else {
        log("mt: pairing failed - wrong PIN?");
        NimBLEDevice::deleteBond(connectAddr);        // a retry must pair fresh
        if (client) client->disconnect();
        phase = P_IDLE;
        pin = 0;
        setState(PAIR_FAILED);
      }
    } else if (millis() - pairStart > 45000) {        // nobody answered the PIN prompt
      log("mt: pairing timed out");
      if (client) client->disconnect();
      phase = P_IDLE;
      setState(PAIR_FAILED);
    }
    return;
  }

  if ((phase == P_CONFIG || phase == P_RUN) && fromRadio) {
    bool drain = phase == P_CONFIG || fromNumFlag;
    if (drain) {
      fromNumFlag = false;
      for (int i = 0; i < READS_PER_POLL; i++) {
        if (!readFromRadio()) break;
        if (i == READS_PER_POLL - 1) fromNumFlag = true;   // more may be waiting: continue next poll
      }
    }
  }
}

bool MeshtasticBLE::readFromRadio() {
  NimBLEAttValue v = fromRadio->readValue();
  if (!v.size()) return false;
  handleFromRadio(v.data(), v.size());
  return true;
}

// ------------------------------------------------------------------ FromRadio
void MeshtasticBLE::handleFromRadio(const uint8_t* d, size_t n) {
  PbReader r(d, n);
  uint32_t f; uint8_t wt;
  while (r.next(f, wt)) {
    if (f == 2 && wt == 2) {                            // packet: MeshPacket
      PbReader m = r.sub();
      handlePacket(m.p, m.end - m.p);
    } else if (f == 3 && wt == 2) {                     // my_info: MyNodeInfo
      PbReader m = r.sub();
      uint32_t g; uint8_t w2;
      while (m.next(g, w2)) { if (g == 1 && w2 == 0) myNum = m.varint(); else m.skip(w2); }
    } else if (f == 4 && wt == 2) {                     // node_info: NodeInfo
      PbReader m = r.sub();
      handleNodeInfo(m.p, m.end - m.p, phase == P_CONFIG);
    } else if (f == 5 && wt == 2) {                     // config: Config -> lora (6) -> modem_preset (2)
      PbReader c = r.sub();
      uint32_t g; uint8_t w2;
      while (c.next(g, w2)) {
        if (g == 6 && w2 == 2) {
          PbReader l = c.sub();
          uint32_t h; uint8_t w3;
          uint32_t preset = 0;                          // proto3: absent = 0 = LONG_FAST
          bool usePreset = false;
          while (l.next(h, w3)) {
            if (h == 1 && w3 == 0) usePreset = l.varint();
            else if (h == 2 && w3 == 0) preset = l.varint();
            else l.skip(w3);
          }
          presetName = !usePreset ? String("Primary") : preset < 10 ? String(PRESETS[preset]) : String("Primary");
        } else c.skip(w2);
      }
    } else if (f == 7 && wt == 0) {                     // config_complete_id
      uint32_t id = r.varint();
      if (id == nonce && phase == P_CONFIG) {
        commitNodes();
        if (!pendingChannels.empty()) channelList = pendingChannels;
        for (McChannel& c : channelList) if (c.idx == 0 && !c.name.length()) c.name = presetName;
        phase = P_RUN;
        if (st != ONLINE) {
          log("mt: online as \"" + self + "\" - " + String(contactList.size()) + " nodes, " +
              String(channelList.size()) + " channels");
          setState(ONLINE);
        }
        if (rosterH) rosterH();
      }
    } else if (f == 10 && wt == 2) {                    // channel: Channel
      PbReader m = r.sub();
      handleChannel(m.p, m.end - m.p);
    } else if (f == 8 && wt == 0) {                     // rebooted: config again
      r.varint();
      log("mt: radio rebooted, reloading");
      requestConfig(esp_random() % 1000000 + 1000);
    } else {
      r.skip(wt);
    }
  }
}

void MeshtasticBLE::handleChannel(const uint8_t* d, size_t n) {
  PbReader r(d, n);
  uint32_t f; uint8_t wt;
  int index = 0;
  uint32_t role = 0;                                    // 0 DISABLED, 1 PRIMARY, 2 SECONDARY
  String name;
  while (r.next(f, wt)) {
    if (f == 1 && wt == 0) index = r.varint();
    else if (f == 2 && wt == 2) {                       // settings: ChannelSettings, name = 3
      PbReader s = r.sub();
      uint32_t g; uint8_t w2;
      while (s.next(g, w2)) { if (g == 3 && w2 == 2) name = s.str(); else s.skip(w2); }
    } else if (f == 3 && wt == 0) role = r.varint();
    else r.skip(wt);
  }
  if (!role) return;
  if (!name.length()) name = index == 0 ? presetName : "CH" + String(index);
  for (McChannel& c : pendingChannels)                 // older firmware resends channels on every refresh
    if (c.idx == index) { c.name = name; index = -1; break; }
  if (index >= 0) pendingChannels.push_back({ index, name });
  if (phase != P_CONFIG) { channelList = pendingChannels; if (rosterH) rosterH(); }
}

McContact* MeshtasticBLE::contactFor(uint32_t num, bool create) {
  uint8_t p[6];
  numToPrefix(num, p);
  for (McContact& c : contactList) if (memcmp(c.prefix, p, 6) == 0) return &c;
  if (!create) return nullptr;
  McContact c;
  char hex[10];
  snprintf(hex, sizeof(hex), "!%08x", (unsigned)num);
  c.name = hex;
  memcpy(c.prefix, p, 6);
  c.type = 1;
  contactList.push_back(c);
  return &contactList.back();
}

void MeshtasticBLE::handleNodeInfo(const uint8_t* d, size_t n, bool configuring) {
  PbReader r(d, n);
  uint32_t f; uint8_t wt;
  uint32_t num = 0, lastHeard = 0, role = 0;
  String longName, shortName;
  std::vector<uint8_t> userRaw;
  float la = 0, lo = 0;
  uint32_t posTime = 0;
  while (r.next(f, wt)) {
    if (f == 1 && wt == 0) num = r.varint();
    else if (f == 2 && wt == 2) {                       // user: User
      PbReader u = r.sub();
      userRaw.assign(u.p, u.end);                       // kept for our own node: advertise() sends it
      uint32_t g; uint8_t w2;
      while (u.next(g, w2)) {
        if (g == 2 && w2 == 2) longName = u.str();
        else if (g == 3 && w2 == 2) shortName = u.str();
        else if (g == 7 && w2 == 0) role = u.varint();
        else u.skip(w2);
      }
    } else if (f == 3 && wt == 2) {                     // position: Position
      PbReader p = r.sub();
      uint32_t g; uint8_t w2;
      while (p.next(g, w2)) {
        if (g == 1 && w2 == 5) la = fixedDeg(p.fixed32());
        else if (g == 2 && w2 == 5) lo = fixedDeg(p.fixed32());
        else if (g == 4 && w2 == 5) posTime = p.fixed32();
        else p.skip(w2);
      }
    } else if (f == 5 && wt == 5) lastHeard = r.fixed32();
    else r.skip(wt);
  }
  if (!num) return;
  if (lastHeard > radioTime) setClock(lastHeard);       // best clock we get: the freshest "last heard"
  if (posTime > radioTime) setClock(posTime);
  String name = longName.length() ? longName : shortName;
  if (num == myNum) {
    if (name.length()) self = name;
    if (!userRaw.empty()) selfUser = userRaw;
    if (la != 0 || lo != 0) {
      if (la != selfLat || lo != selfLon) log("mesh: position " + String(la, 6) + "," + String(lo, 6));
      selfLat = la; selfLon = lo;
    }
    posAt = millis();
    return;
  }
  McContact c;
  numToPrefix(num, c.prefix);
  char hex[10];
  snprintf(hex, sizeof(hex), "!%08x", (unsigned)num);
  c.name = name.length() ? name : String(hex);
  c.type = roleToType(role);
  c.lat = la; c.lon = lo;
  c.lastAdvert = lastHeard;
  if (configuring) { pendingNodes.push_back(c); sawConfigNodes = true; }
  else {
    McContact* e = contactFor(num, true);
    *e = c;
    if (rosterH) rosterH();
  }
}

void MeshtasticBLE::commitNodes() {
  if (!sawConfigNodes) return;
  uint32_t now = this->now();
  std::vector<McContact> keep;
  for (const McContact& c : pendingNodes)
    if (!now || !c.lastAdvert || now - c.lastAdvert < NODE_MAX_AGE) keep.push_back(c);
  std::sort(keep.begin(), keep.end(), [](const McContact& a, const McContact& b) { return a.lastAdvert > b.lastAdvert; });
  if (keep.size() > NODE_MAX) keep.resize(NODE_MAX);
  contactList = keep;
  pendingNodes.clear();
}

void MeshtasticBLE::handlePacket(const uint8_t* d, size_t n) {
  PbReader r(d, n);
  uint32_t f; uint8_t wt;
  uint32_t from = 0, to = 0, channel = 0, rxTime = 0, portnum = 0;
  float snr = 0;
  const uint8_t* payload = nullptr;
  size_t plen = 0;
  while (r.next(f, wt)) {
    if (f == 1 && wt == 5) from = r.fixed32();
    else if (f == 2 && wt == 5) to = r.fixed32();
    else if (f == 3 && wt == 0) channel = r.varint();
    else if (f == 4 && wt == 2) {                       // decoded: Data
      PbReader dd = r.sub();
      uint32_t g; uint8_t w2;
      while (dd.next(g, w2)) {
        if (g == 1 && w2 == 0) portnum = dd.varint();
        else if (g == 2 && w2 == 2) dd.bytes(payload, plen);
        else dd.skip(w2);
      }
    } else if (f == 7 && wt == 5) rxTime = r.fixed32();
    else if (f == 8 && wt == 5) snr = fixedFloat(r.fixed32());
    else r.skip(wt);
  }
  if (rxTime) setClock(rxTime);
  if (!payload) return;                                 // still encrypted (not for us) or empty

  if (portnum == 1) {                                   // TEXT_MESSAGE_APP
    if (from == myNum) return;
    McMessage m;
    m.isChannel = to == MT_BROADCAST;
    m.chan = m.isChannel ? (int)channel : -1;
    numToPrefix(from, m.prefix);
    McContact* c = contactFor(from, true);
    m.from = c->name;
    m.text = "";
    for (size_t i = 0; i < plen; i++) {                 // payload is UTF-8: keep ASCII
      if (payload[i] >= 32 && payload[i] < 127) m.text += (char)payload[i];
      else if (payload[i] == '\n') m.text += ' ';
    }
    m.snr = snr;
    m.ts = rxTime ? rxTime : now();
    if (msgH) msgH(m);
  } else if (portnum == 3) {                            // POSITION_APP: Position
    PbReader p(payload, plen);
    uint32_t g; uint8_t w2;
    float la = 0, lo = 0;
    while (p.next(g, w2)) {
      if (g == 1 && w2 == 5) la = fixedDeg(p.fixed32());
      else if (g == 2 && w2 == 5) lo = fixedDeg(p.fixed32());
      else p.skip(w2);
    }
    if (la == 0 && lo == 0) return;
    if (from == myNum) { selfLat = la; selfLon = lo; posAt = millis(); return; }
    McContact* c = contactFor(from, true);
    c->lat = la; c->lon = lo;
    if (rxTime) c->lastAdvert = rxTime;
  } else if (portnum == 4) {                            // NODEINFO_APP: User
    PbReader u(payload, plen);
    uint32_t g; uint8_t w2;
    String longName, shortName;
    uint32_t role = 0;
    while (u.next(g, w2)) {
      if (g == 2 && w2 == 2) longName = u.str();
      else if (g == 3 && w2 == 2) shortName = u.str();
      else if (g == 7 && w2 == 0) role = u.varint();
      else u.skip(w2);
    }
    if (from == myNum) return;
    McContact* c = contactFor(from, true);
    if (longName.length() || shortName.length()) c->name = longName.length() ? longName : shortName;
    c->type = roleToType(role);
    if (rxTime) c->lastAdvert = rxTime;
    if (rosterH) rosterH();
  }
}

// ------------------------------------------------------------------ refresh / send
void MeshtasticBLE::refreshPosition() {
  // the node list carries our own position too; a pull is a few hundred reads, so at most once a minute
  if (st != ONLINE || phase != P_RUN || millis() - lastRefresh < 60000) return;
  requestConfig(NONCE_ONLY_NODES);
}

void MeshtasticBLE::refreshContacts() { refreshPosition(); }

bool MeshtasticBLE::sendText(uint32_t to, int channel, const String& text) {
  if (st != ONLINE || !toRadio) return false;
  String t = text.length() > 200 ? text.substring(0, 200) : text;
  PbWriter data;
  data.u32(1, 1);                                       // portnum = TEXT_MESSAGE_APP
  data.str(2, t);                                       // payload
  PbWriter pkt;
  pkt.fixed32(2, to);                                   // to
  if (channel) pkt.u32(3, channel);                     // channel index
  pkt.msg(4, data);                                     // decoded
  pkt.fixed32(6, esp_random() | 1);                     // id
  pkt.u32(10, 1);                                       // want_ack
  PbWriter msg;
  msg.msg(1, pkt);                                      // ToRadio.packet
  return toRadio->writeValue(msg.b.data(), msg.b.size(), true);
}

bool MeshtasticBLE::sendChannel(int idx, const String& text) {
  bool ok = sendText(MT_BROADCAST, idx, text);             // Meshtastic shows the sender itself: no name prefix
  log(ok ? "mt: sent to channel " + String(idx) : String("mt: channel send failed"));
  return ok;
}

bool MeshtasticBLE::advertise() {
  if (st != ONLINE || !toRadio || selfUser.empty()) return false;
  PbWriter data;
  data.u32(1, 4);                                       // portnum = NODEINFO_APP
  data.bytes(2, selfUser.data(), selfUser.size());      // payload: our User, exactly as the radio has it
  PbWriter pkt;
  pkt.fixed32(2, MT_BROADCAST);
  pkt.msg(4, data);
  pkt.fixed32(6, esp_random() | 1);
  PbWriter msg;
  msg.msg(1, pkt);
  bool ok = toRadio->writeValue(msg.b.data(), msg.b.size(), true);
  log(ok ? String("mt: sent our node info (advert)") : String("mt: advert failed"));
  return ok;
}

bool MeshtasticBLE::removeContact(const uint8_t prefix[6]) {
  if (st != ONLINE || !toRadio || !myNum) return false;
  uint32_t num = prefixToNum(prefix);
  PbWriter admin;
  admin.u32(38, num);                                   // AdminMessage.remove_by_nodenum
  PbWriter data;
  data.u32(1, 6);                                       // portnum = ADMIN_APP
  data.msg(2, admin);
  PbWriter pkt;
  pkt.fixed32(2, myNum);                                // to: the radio itself (the app link counts as local)
  pkt.msg(4, data);
  pkt.fixed32(6, esp_random() | 1);
  PbWriter msg;
  msg.msg(1, pkt);
  bool ok = toRadio->writeValue(msg.b.data(), msg.b.size(), true);
  log(ok ? "mt: removing node " + contactName(prefix) : String("mt: remove failed"));
  if (!ok) return false;
  for (size_t i = 0; i < contactList.size(); i++)
    if (memcmp(contactList[i].prefix, prefix, 6) == 0) { contactList.erase(contactList.begin() + i); break; }
  if (rosterH) rosterH();
  return true;
}

bool MeshtasticBLE::sendDirect(const uint8_t prefix[6], const String& text) {
  bool ok = sendText(prefixToNum(prefix), 0, text);
  log(ok ? "mt: sent DM to " + contactName(prefix) : String("mt: DM send failed"));
  return ok;
}
