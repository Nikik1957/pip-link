// PipLink - the ESP32-S3 as USB host to the Pip-Boy's Espruino REPL.
// Owns the USB host + CDC-ACM driver, the connect/handshake lifecycle, the
// RobCo Terminal launch, ack-window flow control, and a log that is written
// to the Pip-Boy's SD card (HOLO/TERM/ESP.LOG) so we can debug without a TTL cable.
#pragma once
#include <Arduino.h>
#include <deque>
#include <functional>

class PipLink {
public:
  enum State {
    NO_DEVICE,   // nothing plugged in (or USB host failed)
    HANDSHAKE,   // REPL open, launching / waiting for the terminal's HI
    IDLE,        // REPL open, but the user left the terminal (BYE)
    READY        // terminal up: T.* calls are live
  };

  bool begin(uint16_t vid = 0x0483, uint16_t pid = 0xA4F1);
  void poll();                                   // call often from loop()

  void send(const String& js);                   // queue one JS line (flow-controlled)
  size_t queued() const { return txBytes + inflight; }   // bytes not yet acknowledged by the Pip-Boy
  void setAckTimeout(uint32_t ms) { ackTimeoutMs = ms; } // how long the Pip-Boy may take to answer (busy drawing)
  void log(const char* fmt, ...);                // Serial + SD log on the Pip-Boy

  State state() const { return st; }
  bool failed() const { return installFailed; }
  int cols() const { return nCols; }
  int rows() const { return nRows; }
  const String& termVersion() const { return termVer; }   // the holotape's version, from its HI

  // Terminal events (the text inside <<T ... >>, minus the handshake ones)
  void onEvent(std::function<void(const String&)> h) { evH = h; }
  void onReady(std::function<void()> h) { readyH = h; }

  static String b64enc(const String& s);
  static String b64dec(const String& s);

  uint32_t bytesSent = 0, acks = 0, ackTimeouts = 0, deviceErrors = 0, opens = 0, txRetries = 0;

private:
  String termVer;
  void tryOpen();
  void closeDev();
  void probe();
  void handleLine(const char* s);
  void pump();
  void requestAck();
  void rawLine(const String& s);
  void flushLog();

  uint16_t vid = 0, pid = 0;
  bool installed = false, installFailed = false;
  const char* bootWhy = "?";                     // ESP reset reason, repeated in the log on every terminal start
  void* dev = nullptr;                           // cdc_acm_dev_hdl_t
  State st = NO_DEVICE;
  int nCols = 0, nRows = 0;
  uint32_t phaseAt = 0, lastTx = 0, ackAt = 0, lastLogFlush = 0;

  std::deque<String> txq;
  size_t txBytes = 0;                            // in txq
  size_t inflight = 0;
  uint32_t ackSeq = 0;
  bool ackPending = false;
  uint32_t ackTimeoutMs = 1500;

  String logBuf;
  uint32_t logBytes = 0;

  std::function<void(const String&)> evH;
  std::function<void()> readyH;
};
