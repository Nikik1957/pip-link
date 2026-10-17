#include "ble_radiacode.h"
#include <time.h>
#include <freertos/stream_buffer.h>
#include "ble_hub.h"

namespace {
const NimBLEUUID RC_SERVICE("e63215e5-7003-49d8-96b0-b024798fb901");   // RadiaCode data service
const NimBLEUUID RC_WRITE("e63215e6-7003-49d8-96b0-b024798fb901");
const NimBLEUUID RC_NOTIFY("e63215e7-7003-49d8-96b0-b024798fb901");

// command ids (radiacode/types.py COMMAND / VS / VSFR)
const uint16_t CMD_SET_EXCHANGE = 0x0007;
const uint16_t CMD_WR_VIRT_SFR = 0x0825;
const uint16_t CMD_RD_VIRT_STRING = 0x0826;
const uint16_t CMD_SET_TIME = 0x0A04;
const uint32_t VS_DATA_BUF = 0x100;
const uint32_t VSFR_DEVICE_TIME = 0x0504;

const uint32_t POLL_MS = 1000;
const uint32_t RESP_TIMEOUT_MS = 4000;
const size_t RX_MAX = 32 * 1024;

NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* wr = nullptr;
StreamBufferHandle_t rxStream = nullptr;          // notify task -> poll()
volatile bool lostLink = false;

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int) override { lostLink = true; }
};
ClientCB clientCB;

void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  xStreamBufferSend(rxStream, data, len, 0);
}

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
float lef(const uint8_t* p) { float f; uint32_t u = le32(p); memcpy(&f, &u, 4); return f; }
void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
}  // namespace

void BleRadiaCode::begin() { rxStream = xStreamBufferCreate(16 * 1024, 1); }

void BleRadiaCode::use(const String& a, uint8_t t, const String& n) {
  forget();
  addrStr = a; addrType = t; rcName = n;
  retryAt = millis();
  setState(wanted ? SEARCHING : STANDBY);
  log("rc: using " + n);
}

void BleRadiaCode::setActive(bool on) {
  if (on == wanted) return;
  wanted = on;
  if (st == NONE) return;
  if (on) {
    retryAt = millis();
    setState(SEARCHING);
    log("rc: active");
  } else {
    waiting = false;
    if (client && client->isConnected()) client->disconnect();
    setState(STANDBY);
    log("rc: standby");
  }
}

void BleRadiaCode::forget() {
  if (client && client->isConnected()) client->disconnect();
  addrStr = ""; rcName = "";
  dataSvc = false;
  waiting = false;
  lv = RcLive();
  rr = RcRare();
  setState(NONE);
}

void BleRadiaCode::setState(State s) {
  if (s == st) return;
  st = s;
  if (stateH) stateH(s);
}

bool BleRadiaCode::connectNow() {
  NimBLEAddress a;
  if (!BleHub::lookup(BleHub::RADIACODE, addrStr, a) && !(rcName.length() && BleHub::lookup(BleHub::RADIACODE, rcName, a)))
    return false;
  if (client) { NimBLEDevice::deleteClient(client); client = nullptr; wr = nullptr; }
  client = NimBLEDevice::createClient();
  client->setClientCallbacks(&clientCB, false);
  client->setConnectTimeout(5000);
  setState(CONNECTING);
  log("rc: connecting to " + rcName);
  if (!client->connect(a)) { log("rc: connect failed"); return false; }
  NimBLERemoteService* svc = client->getService(RC_SERVICE);
  dataSvc = svc != nullptr;
  NimBLERemoteCharacteristic* nt = svc ? svc->getCharacteristic(RC_NOTIFY) : nullptr;
  wr = svc ? svc->getCharacteristic(RC_WRITE) : nullptr;
  if (!wr || !nt || !nt->subscribe(true, onNotify)) {
    log("rc: data service/characteristics missing - not a RadiaCode?");
    client->disconnect();
    return false;
  }
  log("rc: online (" + rcName + ", mtu " + String(client->getMTU()) + ")");
  return true;
}

void BleRadiaCode::dropLink(const char* why) {
  log(String("rc: ") + why + " - reconnecting");
  waiting = false;
  if (client && client->isConnected()) client->disconnect();
  setState(wanted ? SEARCHING : STANDBY);
  retryAt = millis() + 3000;
}

// ------------------------------------------------------------------ requests
// frame: u32 len | u16 cmd | u8 0 | u8 seq(0x80 + n) | args ; the reply echoes the 4-byte header
bool BleRadiaCode::request(uint16_t cmd, const uint8_t* args, size_t n) {
  if (!wr) return false;
  std::vector<uint8_t> f(8 + n);
  put32(&f[0], 4 + n);
  f[4] = cmd & 0xFF; f[5] = cmd >> 8; f[6] = 0; f[7] = 0x80 + seq;
  seq = (seq + 1) % 32;
  if (n) memcpy(&f[8], args, n);
  memcpy(pendHdr, &f[4], 4);
  for (size_t i = 0; i < f.size(); i += 18) {                 // the device takes 18-byte writes
    size_t k = min((size_t)18, f.size() - i);
    if (!wr->writeValue(&f[i], k, false)) return false;
  }
  waiting = true;
  sentAt = millis();
  return true;
}

void BleRadiaCode::startSession() {
  xStreamBufferReset(rxStream);
  rx.clear(); need = 0;
  seq = 0; timeouts = 0;
  phase = P_EXCHANGE;
  const uint8_t ex[4] = { 0x01, 0xFF, 0x12, 0xFF };
  if (!request(CMD_SET_EXCHANGE, ex, 4)) dropLink("write failed");
}

void BleRadiaCode::pump() {
  uint8_t buf[256];
  size_t got;
  while ((got = xStreamBufferReceive(rxStream, buf, sizeof(buf), 0)) > 0) {
    if (rx.size() + got > RX_MAX) { rx.clear(); need = 0; }  // garbage: resync
    rx.insert(rx.end(), buf, buf + got);
  }
  while (true) {
    if (!need) {
      if (rx.size() < 4) return;
      need = 4 + le32(&rx[0]);
      if (need > RX_MAX) { log("rc: bad response length, resyncing"); rx.clear(); need = 0; return; }
    }
    if (rx.size() < need) return;
    onResponse(&rx[4], need - 4);
    rx.erase(rx.begin(), rx.begin() + need);
    need = 0;
  }
}

void BleRadiaCode::onResponse(const uint8_t* d, size_t n) {
  if (n < 4 || memcmp(d, pendHdr, 4) != 0) {                 // stale or unexpected reply
    log("rc: unexpected reply header, ignored");
    return;
  }
  waiting = false;
  timeouts = 0;
  d += 4; n -= 4;
  switch (phase) {
    case P_EXCHANGE: {
      uint32_t now = clockF ? clockF() : 0;
      if (now > 1700000000UL) {
        time_t t = now;
        struct tm lt;
        localtime_r(&t, &lt);
        uint8_t a[8] = { (uint8_t)lt.tm_mday, (uint8_t)(lt.tm_mon + 1), (uint8_t)(lt.tm_year - 100), 0,
                         (uint8_t)lt.tm_sec, (uint8_t)lt.tm_min, (uint8_t)lt.tm_hour, 0 };
        phase = P_SETTIME;
        request(CMD_SET_TIME, a, 8);
        return;
      }
    }  // no clock yet: skip setting the time
    // fall through
    case P_SETTIME: {
      uint8_t a[8];
      put32(a, VSFR_DEVICE_TIME); put32(a + 4, 0);
      phase = P_DEVTIME;
      request(CMD_WR_VIRT_SFR, a, 8);
      return;
    }
    case P_DEVTIME:
      phase = POLLING;
      nextPoll = millis();
      log("rc: streaming");
      return;
    case POLLING: {
      // RD_VIRT_STRING reply: u32 retcode (1 = ok), u32 length, data
      if (n < 8 || le32(d) != 1) { log("rc: DATA_BUF read refused"); return; }
      size_t flen = le32(d + 4);
      if (flen > n - 8) flen = n - 8;
      decodeDataBuf(d + 8, flen);
      return;
    }
  }
}

// DATA_BUF: records of  u8 seq | u8 eid | u8 gid | i32 ts(10 ms) | payload   (radiacode/decoders/databuf.py)
// Dose values are in the protocol's R-based units: x10000 = uSv.
void BleRadiaCode::decodeDataBuf(const uint8_t* d, size_t n) {
  size_t p = 0;
  int nextSeq = -1;
  bool gotLive = false;
  while (n - p >= 7) {
    uint8_t sq = d[p], eid = d[p + 1], gid = d[p + 2];
    if (nextSeq >= 0 && sq != nextSeq) break;                  // corrupted / out of step
    nextSeq = (sq + 1) % 256;
    p += 7;
    const uint8_t* r = d + p;
    size_t left = n - p, sz;
    if (eid == 0) {
      switch (gid) {
        case 0: sz = 15; break;   // RealTimeData  <ffHHHB
        case 1: sz = 8; break;    // RawData       <ff
        case 2: sz = 16; break;   // DoseRateDB    <IffHH
        case 3: sz = 14; break;   // RareData      <IfHHH
        case 4: sz = 16; break;   // UserData
        case 5: sz = 16; break;   // ScheduleData
        case 6: sz = 6; break;    // AccelData
        case 7: sz = 4; break;    // Event         <BBH
        case 8: sz = 6; break;    // RawCountRate  <fH
        case 9: sz = 6; break;    // RawDoseRate   <fH
        default: return;          // unknown: can't know its size, stop here
      }
    } else if (eid == 1 && gid >= 1 && gid <= 3) {             // sample blocks: <HI + samples
      if (left < 6) return;
      static const int per[4] = { 0, 8, 16, 14 };
      sz = 6 + (size_t)le16(r) * per[gid];
    } else {
      return;
    }
    if (left < sz) return;
    if (eid == 0 && gid == 0) {
      lv.cps = lef(r);
      lv.doseRate = lef(r + 4) * 10000.0f;
      lv.cpsErr = le16(r + 8) / 10.0f;
      lv.doseRateErr = le16(r + 10) / 10.0f;
      lv.at = millis();
      gotLive = true;
    } else if (eid == 0 && gid == 3) {
      rr.duration = le32(r);
      rr.dose = lef(r + 4) * 10000.0f;
      rr.tempC = ((int)le16(r + 8) - 2000) / 100.0f;
      rr.battery = le16(r + 10) / 100.0f;
      rr.at = millis();
    }
    p += sz;
  }
  if (gotLive && dataH) dataH();
}

// ------------------------------------------------------------------ poll
void BleRadiaCode::poll() {
  if (lostLink) {
    lostLink = false;
    if ((st == ONLINE || st == CONNECTING) && !(client && client->isConnected())) {
      waiting = false;
      log("rc: link lost, reconnecting");
      setState(wanted ? SEARCHING : STANDBY);
      retryAt = millis() + 3000;
    }
  }
  if (st == SEARCHING && (int32_t)(millis() - retryAt) >= 0) {
    retryAt = millis() + 3000;
    BleHub::pause(true);
    bool ok = connectNow();
    BleHub::pause(false);
    if (ok) { setState(ONLINE); startSession(); }
    else if (st == CONNECTING) setState(SEARCHING);
  }
  if (st != ONLINE) return;

  pump();
  if (waiting) {
    if (millis() - sentAt > RESP_TIMEOUT_MS) {
      waiting = false;
      log("rc: no reply (phase " + String((int)phase) + ")");
      if (++timeouts >= 3) { dropLink("not answering"); return; }
      if (phase != POLLING) startSession();
    }
    return;
  }
  if (phase == POLLING && (int32_t)(millis() - nextPoll) >= 0) {
    nextPoll = millis() + POLL_MS;
    uint8_t a[4];
    put32(a, VS_DATA_BUF);
    if (!request(CMD_RD_VIRT_STRING, a, 4)) dropLink("write failed");
  }
}
