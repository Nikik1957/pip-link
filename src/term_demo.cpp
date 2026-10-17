// M3 demo: PipLink + PipTerm driving the RobCo Terminal from the ESP32-S3.
// Exercises knob navigation, the on-screen keyboard round trip, scroll-mode
// streaming and live stats. Everything is logged to HOLO/TERM/ESP.LOG on the
// Pip-Boy SD card (read it later with: python tools/term_sim.py inspect).
//   LED: 2 = waiting for Pip-Boy, 3 = handshaking / terminal closed, 4 = ready, flicker = USB fail
#include <Arduino.h>
#include "pip_link.h"
#include "pip_term.h"

static PipLink pip;
static PipTerm term(pip);

enum Screen { MENU, SCROLL, STATS };
static Screen screen = MENU;
static const char* ITEMS[] = { "KEYBOARD TEST", "SCROLL TEST", "LINK STATS", "PLAY A SOUND" };
static const int NITEMS = 4;
static int sel = 0;
static String lastResult = "nothing yet";
static uint32_t kbCount = 0;

// ---------------------------------------------------------------- LED status
static void ledTask(void*) {
  pinMode(LED_BUILTIN, OUTPUT);
  for (;;) {
    if (pip.failed()) {
      digitalWrite(LED_BUILTIN, LOW);  vTaskDelay(pdMS_TO_TICKS(70));
      digitalWrite(LED_BUILTIN, HIGH); vTaskDelay(pdMS_TO_TICKS(70));
      continue;
    }
    int n = pip.state() == PipLink::NO_DEVICE ? 2 : pip.state() == PipLink::READY ? 4 : 3;
    for (int i = 0; i < n; i++) {
      digitalWrite(LED_BUILTIN, LOW);  vTaskDelay(pdMS_TO_TICKS(150));   // active-low LED
      digitalWrite(LED_BUILTIN, HIGH); vTaskDelay(pdMS_TO_TICKS(250));
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
  }
}

// ---------------------------------------------------------------- screens
static void paintMenu() {
  term.header("ROBCO TERMLINK :: ESP32-S3 HUB");
  term.footer("knob1: move / select    knob2: back");
  for (int i = 0; i < NITEMS; i++)
    term.line(i, String(i == sel ? "> " : "  ") + ITEMS[i], i == sel ? PipTerm::NORMAL | PipTerm::INV : PipTerm::NORMAL);
  term.line(NITEMS, "", PipTerm::NORMAL);
  term.line(NITEMS + 1, "------------------------------------------", PipTerm::DIM);
  term.line(NITEMS + 2, "last result: " + lastResult, PipTerm::BRIGHT);
  for (int r = NITEMS + 3; r < term.rows(); r++) term.line(r, "", PipTerm::NORMAL);
}

static void paintStats() {
  term.header("LINK STATS");
  term.footer("knob2: back    (updates every second)");
  term.line(0, "uptime        " + String(millis() / 1000) + " s", PipTerm::NORMAL);
  term.line(1, "free heap     " + String(ESP.getFreeHeap()) + " bytes", PipTerm::NORMAL);
  term.line(2, "bytes sent    " + String(pip.bytesSent), PipTerm::NORMAL);
  term.line(3, "acks          " + String(pip.acks), PipTerm::NORMAL);
  term.line(4, "ack timeouts  " + String(pip.ackTimeouts), pip.ackTimeouts ? PipTerm::BRIGHT : PipTerm::NORMAL);
  term.line(5, "device errors " + String(pip.deviceErrors), pip.deviceErrors ? PipTerm::BRIGHT : PipTerm::NORMAL);
  term.line(6, "keyboard uses " + String(kbCount), PipTerm::NORMAL);
  term.line(7, "grid          " + String(term.cols()) + " x " + String(term.rows()), PipTerm::NORMAL);
  term.line(8, "connections   " + String(pip.opens), PipTerm::NORMAL);
  for (int r = 9; r < term.rows(); r++) term.line(r, "", PipTerm::NORMAL);
}

static void runScrollTest() {
  term.cls();
  term.header("SCROLL TEST :: streaming text");
  term.footer("knob2: back to menu");
  for (int i = 0; i < 40; i++)
    term.print("line " + String(i) + "  the quick brown fox jumps over the lazy dog\n");
  term.print("-- end of stream --\n");
  pip.log("demo: scroll test streamed 41 lines");
}

static void goMenu() {
  screen = MENU;
  term.cls();
  paintMenu();
}

// ---------------------------------------------------------------- events
static void onKnob(int knob, int dir) {
  pip.log("demo: knob%d %s", knob, dir > 0 ? "+1" : dir < 0 ? "-1" : "click");
  if (knob == 2) { if (screen != MENU) goMenu(); return; }
  if (screen != MENU) return;
  if (dir != 0) {
    sel = (sel + dir + NITEMS) % NITEMS;
    term.sound("SCROLL");
    paintMenu();
    return;
  }
  switch (sel) {
    case 0: term.keyboard("demo", "TYPE SOMETHING, THEN SEND"); break;
    case 1: screen = SCROLL; runScrollTest(); break;
    case 2: screen = STATS; term.cls(); paintStats(); break;
    case 3: term.sound("SELECT"); lastResult = "played SELECT"; paintMenu(); break;
  }
}

static void onKeyboard(const String& id, bool ok, const String& text) {
  kbCount++;
  lastResult = ok ? "typed \"" + text + "\"" : "keyboard cancelled";
  pip.log("demo: keyboard %s -> %s", id.c_str(), lastResult.c_str());
  paintMenu();
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);                        // UART0 (D6/D7) if a TTL adapter is attached
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  term.begin();
  term.onKnob(onKnob);
  term.onKeyboard(onKeyboard);
  term.onReady([]() {
    pip.log("demo: terminal ready, painting menu");
    goMenu();
  });
  pip.begin();
}

void loop() {
  pip.poll();
  static uint32_t t = 0;
  if (screen == STATS && term.ready() && millis() - t > 1000) {
    t = millis();
    paintStats();
  }
  delay(5);
}
