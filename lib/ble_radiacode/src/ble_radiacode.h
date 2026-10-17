// BleRadiaCode - RadiaCode 10x dosimeter over BLE: connect/reconnect plus live readings.
// Protocol (from the open-source cdump/radiacode library, see docs/radiacode_protocol.md):
// requests are written in 18-byte chunks to e63215e6, responses arrive as notifications on
// e63215e7 with a 4-byte length prefix. We poll the device's DATA_BUF every second.
#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

struct RcLive {             // latest real-time record
  float doseRate = 0;       // uSv/h
  float doseRateErr = 0;    // %
  float cps = 0;            // counts per second
  float cpsErr = 0;         // %
  uint32_t at = 0;          // millis() when received (0 = never)
};

struct RcRare {             // slow-changing status (sent by the device about once a minute)
  float dose = 0;           // accumulated dose, uSv
  uint32_t duration = 0;    // seconds the dose has been accumulating
  float battery = -1;       // %  (-1 = unknown)
  float tempC = 0;
  uint32_t at = 0;
};

class BleRadiaCode {
public:
  enum State { NONE, STANDBY, SEARCHING, CONNECTING, ONLINE };   // STANDBY = set up but not wanted right now

  void begin();
  void poll();
  void use(const String& addr, uint8_t addrType, const String& name);
  void forget();
  // the app says when it wants live data (e.g. only while the RADIATION tab is open);
  // inactive = disconnected, no reconnect attempts
  void setActive(bool on);
  bool active() const { return wanted; }

  State state() const { return st; }
  const String& name() const { return rcName; }
  bool hasDataService() const { return dataSvc; }
  bool streaming() const { return st == ONLINE && phase == POLLING; }
  const RcLive& live() const { return lv; }
  const RcRare& rare() const { return rr; }

  // Unix time source for setting the dosimeter's clock (return 0 if unknown)
  void clock(std::function<uint32_t()> f) { clockF = f; }
  void onState(std::function<void(State)> h) { stateH = h; }
  void onLog(std::function<void(const String&)> h) { logH = h; }
  void onData(std::function<void()> h) { dataH = h; }    // a new real-time reading arrived

private:
  enum Phase { P_EXCHANGE, P_SETTIME, P_DEVTIME, POLLING };
  bool connectNow();
  void setState(State s);
  void log(const String& s) { if (logH) logH(s); }
  void startSession();
  bool request(uint16_t cmd, const uint8_t* args, size_t n);
  void pump();                                  // assemble notifications into responses
  void onResponse(const uint8_t* d, size_t n);
  void decodeDataBuf(const uint8_t* d, size_t n);
  void dropLink(const char* why);

  State st = NONE;
  String addrStr, rcName;
  uint8_t addrType = 0;
  uint32_t retryAt = 0;
  bool dataSvc = false;
  bool wanted = true;

  Phase phase = P_EXCHANGE;
  uint8_t seq = 0;
  uint8_t pendHdr[4] = { 0 };
  bool waiting = false;
  uint32_t sentAt = 0, nextPoll = 0;
  int timeouts = 0;
  std::vector<uint8_t> rx;
  size_t need = 0;

  RcLive lv;
  RcRare rr;
  std::function<uint32_t()> clockF;
  std::function<void(State)> stateH;
  std::function<void(const String&)> logH;
  std::function<void()> dataH;
};
