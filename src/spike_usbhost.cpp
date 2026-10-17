// M2 spike: the XIAO ESP32-S3 as USB host to the Pip-Boy's Espruino REPL.
// Proves the link blind (no serial console): LED blink codes show progress, and
// once the round trip works it takes over the Pip-Boy screen via the RobCo Terminal.
//   LED: 1 = booted, 2 = USB host up / waiting, 3 = Pip-Boy opened, 4 = Pip-Boy answered,
//        fast flicker = driver install failed.
#include <Arduino.h>
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "mbedtls/base64.h"

static const uint16_t PIP_VID = 0x0483, PIP_PID = 0xA4F1;   // Pip-Boy (Espruino STM32 CDC)

static volatile int stage = 1;
static volatile bool failed = false;
static volatile bool pipGone = false;
static cdc_acm_dev_hdl_t pip = nullptr;

struct Line { char s[200]; };
static QueueHandle_t lineQ;
static char acc[200];
static size_t accLen = 0;

// ---------------- LED status ----------------
static void ledTask(void*) {
  pinMode(LED_BUILTIN, OUTPUT);
  for (;;) {
    if (failed) {
      digitalWrite(LED_BUILTIN, LOW);  vTaskDelay(pdMS_TO_TICKS(70));
      digitalWrite(LED_BUILTIN, HIGH); vTaskDelay(pdMS_TO_TICKS(70));
      continue;
    }
    for (int i = 0; i < stage; i++) {
      digitalWrite(LED_BUILTIN, LOW);  vTaskDelay(pdMS_TO_TICKS(150));   // XIAO LED is active-low
      digitalWrite(LED_BUILTIN, HIGH); vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
}

// ---------------- USB host plumbing ----------------
static void usbLibTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

// Runs in the CDC driver task: split the byte stream into lines for loop().
static bool onRx(const uint8_t* d, size_t n, void*) {
  for (size_t i = 0; i < n; i++) {
    char c = (char)d[i];
    if (c == '\n') {
      Line L;
      acc[accLen] = 0;
      strncpy(L.s, acc, sizeof(L.s) - 1);
      L.s[sizeof(L.s) - 1] = 0;
      xQueueSend(lineQ, &L, 0);
      accLen = 0;
    } else if (c != '\r' && accLen < sizeof(acc) - 1) {
      acc[accLen++] = c;
    }
  }
  return true;
}

static void onEvent(const cdc_acm_host_dev_event_data_t* e, void*) {
  if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) pipGone = true;
  else if (e->type == CDC_ACM_HOST_ERROR) Serial.printf("cdc error %d\n", e->data.error);
}

static void pipSend(const String& s) {
  if (!pip) return;
  String line = s + "\n";
  cdc_acm_host_data_tx_blocking(pip, (const uint8_t*)line.c_str(), line.length(), 1000);
}

static String b64(const String& s) {
  unsigned char out[400];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out) - 1, &olen, (const unsigned char*)s.c_str(), s.length());
  out[olen] = 0;
  return String((char*)out);
}
static void tLn(int row, const String& text, int attr) { pipSend("T.ln(" + String(row) + ",\"" + b64(text) + "\"," + String(attr) + ")"); }
static void tHdr(const String& text) { pipSend("T.hdr(\"" + b64(text) + "\")"); }
static void tFtr(const String& text) { pipSend("T.ftr(\"" + b64(text) + "\")"); }

// ---------------- spike state machine ----------------
enum Phase { WAIT_DEV, WAIT_PONG, WAIT_HI, RUN };
static Phase ph = WAIT_DEV;
static uint32_t tMark = 0;

void setup() {
  Serial.begin(115200);                       // UART0 (D6/D7) if a TTL adapter is attached
  lineQ = xQueueCreate(24, sizeof(Line));
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  Serial.println("\n[spike] usb host spike booting");

  usb_host_config_t host_cfg = {};
  host_cfg.skip_phy_setup = false;
  host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&host_cfg) != ESP_OK) { failed = true; Serial.println("[spike] usb_host_install FAILED"); return; }
  xTaskCreate(usbLibTask, "usblib", 4096, nullptr, 20, nullptr);
  if (cdc_acm_host_install(nullptr) != ESP_OK) { failed = true; Serial.println("[spike] cdc_acm_host_install FAILED"); return; }
  stage = 2;
  Serial.println("[spike] host up, waiting for Pip-Boy 0483:A4F1");
}

void loop() {
  if (failed) { delay(200); return; }

  if (pipGone) {
    pipGone = false;
    if (pip) { cdc_acm_host_close(pip); pip = nullptr; }
    ph = WAIT_DEV; stage = 2;
    Serial.println("[spike] Pip-Boy disconnected");
  }

  if (ph == WAIT_DEV) {
    cdc_acm_host_device_config_t dc = {};
    dc.connection_timeout_ms = 1000;
    dc.out_buffer_size = 512;
    dc.in_buffer_size = 512;
    dc.event_cb = onEvent;
    dc.data_cb = onRx;
    dc.user_arg = nullptr;
    if (cdc_acm_host_open(PIP_VID, PIP_PID, 0, &dc, &pip) == ESP_OK) {
      stage = 3;
      Serial.println("[spike] Pip-Boy opened");
      cdc_acm_host_set_control_line_state(pip, true, false);
      delay(300);
      pipSend("");                            // prime: first line after open can be swallowed
      delay(300);
      pipSend("echo(0)");
      delay(200);
      pipSend("print(\"<<T PONG \"+process.env.BOARD+\">>\")");
      ph = WAIT_PONG; tMark = millis();
    } else {
      pip = nullptr;                          // open timed out; try again next loop
    }
    return;
  }

  Line L;
  while (xQueueReceive(lineQ, &L, 0) == pdTRUE) {
    Serial.printf("pip> %s\n", L.s);
    if (strstr(L.s, "<<T PONG")) {
      stage = 4;
      pipSend("Pip.loadHolotape(\"HOLO/TERM/APP.js\")");
      ph = WAIT_HI; tMark = millis();
    } else if (strstr(L.s, "<<T HI")) {
      ph = RUN; tMark = 0;
      tHdr("ESP32 USB HOST :: ONLINE");
      tFtr("link: XIAO ESP32-S3 USB host (no PC)");
      tLn(0, "Hello from the XIAO ESP32-S3!", 3);
      tLn(2, "Turn or click the knobs - events echo below.", 2);
    } else if (ph == RUN && strstr(L.s, "<<T K")) {
      String ev = String(L.s + 4);
      ev.replace(">>", "");
      tLn(4, "knob event: " + ev, 4);
    } else if (strstr(L.s, "<<T BYE")) {
      ph = WAIT_HI; tMark = millis();
    }
  }

  if (ph == WAIT_PONG && millis() - tMark > 3000) {   // retry the probe
    pipSend("print(\"<<T PONG \"+process.env.BOARD+\">>\")");
    tMark = millis();
  }
  if (ph == WAIT_HI && millis() - tMark > 5000) {     // terminal not up yet: nudge it
    pipSend("if(typeof T!=\"undefined\")T.hello()");
    tMark = millis();
  }
  if (ph == RUN && millis() - tMark > 1000) {
    tMark = millis();
    tLn(6, "uptime " + String(millis() / 1000) + "s   heap " + String(ESP.getFreeHeap()), 1);
  }
  delay(10);
}
