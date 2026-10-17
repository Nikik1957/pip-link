#include "pip_link.h"
#include <stdarg.h>
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "mbedtls/base64.h"
#include "esp_system.h"

namespace {
const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_EXT:       return "reset-pin";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    default:                return "other";
  }
}
const size_t WINDOW = 1400;              // Espruino's RX buffer is 2048 B; stay well under
const uint32_t HANDSHAKE_RETRY_MS = 4000;
const char* LOGFILE = "HOLO/TERM/ESP.LOG";
const uint32_t LOG_MAX_BYTES = 48 * 1024; // per session, so a chatty bug can't fill the SD card

struct Line { char s[256]; };
QueueHandle_t lineQ = nullptr;
char acc[256];
size_t accLen = 0;
volatile bool devGone = false;

void usbLibTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

// CDC driver task context: split the byte stream into lines for poll().
bool onRx(const uint8_t* d, size_t n, void*) {
  for (size_t i = 0; i < n; i++) {
    char c = (char)d[i];
    if (c == '\n') {
      Line L;
      acc[accLen] = 0;
      memcpy(L.s, acc, accLen + 1);
      xQueueSend(lineQ, &L, 0);
      accLen = 0;
    } else if (c != '\r' && accLen < sizeof(acc) - 1) {
      acc[accLen++] = c;
    }
  }
  return true;
}

void onDevEvent(const cdc_acm_host_dev_event_data_t* e, void*) {
  if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) devGone = true;
}
}  // namespace

// ------------------------------------------------------------------ helpers
String PipLink::b64enc(const String& s) {
  size_t cap = ((s.length() + 2) / 3) * 4 + 1, olen = 0;
  String out;
  char* buf = (char*)malloc(cap);
  if (!buf) return out;
  if (mbedtls_base64_encode((unsigned char*)buf, cap, &olen, (const unsigned char*)s.c_str(), s.length()) == 0) {
    buf[olen] = 0;
    out = buf;
  }
  free(buf);
  return out;
}

String PipLink::b64dec(const String& s) {
  size_t cap = (s.length() / 4) * 3 + 4, olen = 0;
  String out;
  char* buf = (char*)malloc(cap);
  if (!buf) return out;
  if (mbedtls_base64_decode((unsigned char*)buf, cap - 1, &olen, (const unsigned char*)s.c_str(), s.length()) == 0) {
    buf[olen] = 0;
    out = buf;
  }
  free(buf);
  return out;
}

// ------------------------------------------------------------------ lifecycle
bool PipLink::begin(uint16_t v, uint16_t p) {
  vid = v; pid = p;
  lineQ = xQueueCreate(32, sizeof(Line));
  usb_host_config_t host_cfg = {};
  host_cfg.skip_phy_setup = false;
  host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&host_cfg) != ESP_OK) { installFailed = true; Serial.println("[link] usb_host_install failed"); return false; }
  xTaskCreate(usbLibTask, "usblib", 4096, nullptr, 20, nullptr);
  if (cdc_acm_host_install(nullptr) != ESP_OK) { installFailed = true; Serial.println("[link] cdc_acm_host_install failed"); return false; }
  installed = true;
  esp_reset_reason_t why = esp_reset_reason();
  bootWhy = resetName(why);
  log("==== ESP boot, reset reason: %s (%d) ====", bootWhy, (int)why);
  log("link: USB host up, waiting for %04X:%04X", vid, pid);
  return true;
}

void PipLink::tryOpen() {
  cdc_acm_host_device_config_t dc = {};
  dc.connection_timeout_ms = 300;
  dc.out_buffer_size = 512;
  dc.in_buffer_size = 512;
  dc.event_cb = onDevEvent;
  dc.data_cb = onRx;
  dc.user_arg = nullptr;
  cdc_acm_dev_hdl_t h = nullptr;
  if (cdc_acm_host_open(vid, pid, 0, &dc, &h) != ESP_OK) return;   // not there yet
  dev = h;
  cdc_acm_host_set_control_line_state(h, true, false);
  delay(300);
  rawLine("");                    // prime: the first line after open can be swallowed
  delay(300);
  rawLine("echo(0)");             // no input echo / result printing on the wire
  delay(100);
  xQueueReset(lineQ);
  txq.clear(); txBytes = 0; inflight = 0; ackPending = false;
  st = HANDSHAKE;
  opens++;
  if (opens == 1) {                                    // keep history across boots; trim when it gets big
    send(String("try{var f=require(\"fs\");if(f.statSync(\"") + LOGFILE + "\").size>32768)f.writeFileSync(\"" + LOGFILE + "\",\"\")}catch(e){}");
    logBytes = 0;
  }
  log("link: Pip-Boy opened (connection #%u), handshaking", (unsigned)opens);
  probe();
}

void PipLink::closeDev() {
  if (dev) cdc_acm_host_close((cdc_acm_dev_hdl_t)dev);
  dev = nullptr;
  st = NO_DEVICE;
  txq.clear(); txBytes = 0; inflight = 0; ackPending = false;
  log("link: Pip-Boy disconnected (log resumes on reconnect)");
}

void PipLink::probe() {
  send("print(\"<<T ID \"+(Pip.CURRENT&&Pip.CURRENT.id)+\">>\");setTimeout(function(){print(\" \")},15)");
  phaseAt = millis();
}

// ------------------------------------------------------------------ poll
void PipLink::poll() {
  if (!installed) return;
  if (devGone) { devGone = false; closeDev(); }
  if (st == NO_DEVICE) { tryOpen(); return; }

  Line L;
  while (xQueueReceive(lineQ, &L, 0) == pdTRUE) handleLine(L.s);

  // Always compare against a fresh millis(): the timestamps below get set *during*
  // this call, and a stale `now` would make (now - t) wrap to ~4 billion.
  if (st == HANDSHAKE && millis() - phaseAt > HANDSHAKE_RETRY_MS) {
    log("link: no answer yet, probing again");
    probe();
  }
  pump();
  if (txq.empty() && inflight > 0 && !ackPending && millis() - lastTx > 250) requestAck();
  if (ackPending && millis() - ackAt > ackTimeoutMs) {
    ackTimeouts++;
    ackPending = false;
    inflight = 0;
    log("link: ack %u timed out", (unsigned)ackSeq);
  }
  if (logBuf.length() && millis() - lastLogFlush > 1500) flushLog();
}

void PipLink::handleLine(const char* s) {
  const char* p = strstr(s, "<<T ");
  if (!p) {
    if (strstr(s, "Uncaught") || strstr(s, "rror") || strstr(s, "MEMORY")) { deviceErrors++; log("pip error: %s", s); }   // incl. "New interpreter error: LOW_MEMORY"
    return;
  }
  String ev(p + 4);
  int e = ev.indexOf(">>");
  if (e >= 0) ev = ev.substring(0, e);

  if (ev.startsWith("ACK ")) {
    if ((uint32_t)ev.substring(4).toInt() == ackSeq) { ackPending = false; inflight = 0; acks++; }
    return;
  }
  if (ev.startsWith("ID ")) {
    if (st != HANDSHAKE) return;
    if (ev == "ID TERM") send("global.T&&T.hello()");
    else { log("link: launching terminal (was %s)", ev.c_str() + 3); send("try{Pip.loadHolotape(\"HOLO/TERM/APP.js\")}catch(e){print(\"<<T LAUNCHFAIL>>\")}"); }
    phaseAt = millis();
    return;
  }
  if (ev.startsWith("HI ")) {
    int a = ev.indexOf(' ', 3), b = ev.indexOf(' ', a + 1);
    nCols = ev.substring(3, a).toInt();
    nRows = ev.substring(a + 1, b).toInt();
    termVer = ev.substring(b + 1);
    termVer.trim();
    st = READY;
    log("link: terminal ready %dx%d v%s  (ESP up %lus, last boot: %s)", nCols, nRows, ev.substring(b + 1).c_str(),
        (unsigned long)(millis() / 1000), bootWhy);
    if (readyH) readyH();
    return;
  }
  if (ev == "BYE") {                     // user left the terminal; wait for them to come back
    st = IDLE;
    log("link: terminal closed by user");
    return;
  }
  if (evH) evH(ev);
}

// ------------------------------------------------------------------ transmit
void PipLink::send(const String& js) { txq.push_back(js); txBytes += js.length() + 1; }

void PipLink::pump() {
  while (!txq.empty() && !ackPending) {
    size_t n = txq.front().length() + 1;
    if (inflight + n > WINDOW && inflight > 0) { requestAck(); return; }
    rawLine(txq.front());
    inflight += n;
    txBytes -= n;
    txq.pop_front();
  }
}

void PipLink::requestAck() {
  ackSeq++;
  // REPL-level, works with or without the terminal. The Pip-Boy holds back its latest output until
  // something else prints, so a delayed padding print follows to push the ACK out.
  rawLine("print(\"<<T ACK " + String(ackSeq) + ">>\");setTimeout(function(){print(\" \")},15)");
  ackPending = true;
  ackAt = millis();
}

// One USB packet (64 bytes) per transfer. The driver rejects anything over its 512-byte buffer, and a transfer
// that times out while the Pip-Boy is busy can stop part-way - half a command glued to the next one corrupts
// both, and an unclosed quote leaves Espruino collecting input until it runs out of memory. A single packet
// either arrives whole or not at all, so after a timeout the same piece is simply sent again.
void PipLink::rawLine(const String& s) {
  if (!dev) return;
  String line = s + "\n";
  const uint8_t* p = (const uint8_t*)line.c_str();
  size_t n = line.length(), off = 0;
  int tries = 0;
  while (off < n && dev) {
    size_t k = n - off < 64 ? n - off : 64;
    if (cdc_acm_host_data_tx_blocking((cdc_acm_dev_hdl_t)dev, p + off, k, 2000) == ESP_OK) { off += k; tries = 0; continue; }
    txRetries++;
    if (++tries >= 5) { log("link: Pip-Boy stopped taking data (%u of %u bytes sent)", (unsigned)off, (unsigned)n); break; }
  }
  bytesSent += off;
  if (off) lastTx = millis();
}

// ------------------------------------------------------------------ logging
void PipLink::log(const char* fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char line[220];
  snprintf(line, sizeof(line), "[%8.3f] %s\n", millis() / 1000.0, msg);
  Serial.print(line);
  logBuf += line;
  if (logBuf.length() > 4096) logBuf = logBuf.substring(logBuf.length() - 4096);   // keep the newest
}

void PipLink::flushLog() {
  lastLogFlush = millis();
  // Not during the handshake: the Pip-Boy is busy loading the terminal then, and the
  // first log chunk used to vanish (taking the boot/reset-reason lines with it).
  if (st == NO_DEVICE || st == HANDSHAKE || logBytes >= LOG_MAX_BYTES) return;
  String chunk = logBuf.substring(0, 400);
  logBuf = logBuf.substring(chunk.length());
  logBytes += chunk.length();
  // try/catch: any uncaught error on the Pip-Boy kicks the user out of the running app
  send(String("try{require(\"fs\").appendFileSync(\"") + LOGFILE + "\",atob(\"" + b64enc(chunk) + "\"))}catch(e){}");
}
