// BleKeyboard - Bluetooth LE (HID over GATT) keyboard for the hub.
// Pairing: the hub *displays* a 6-digit code (shown on the Pip-Boy) that the user types on
// the keyboard, then the bond is stored so later reconnects are automatic.
// Only BLE keyboards work: the ESP32-S3 has no Bluetooth Classic.
#pragma once
#include <Arduino.h>
#include <functional>

struct KeyEvent {
  enum Type : uint8_t { CHAR, ENTER, ESC, BACKSPACE, TAB, UP, DOWN, LEFT, RIGHT, DEL, PGUP, PGDN, HOME, END };
  Type type;
  char ch;                 // for CHAR
  bool ctrl;               // Ctrl / Alt held (the TERM tab sends control codes / ESC prefixes)
  bool alt;
};

class BleKeyboard {
public:
  enum State { NONE, SEARCHING, CONNECTING, ONLINE, FAILED };

  void begin();
  void poll();                                             // call often from loop()

  // pair(): fresh pairing with a new on-screen code.  use(): reconnect to a bonded keyboard.
  void pair(const String& addr, uint8_t addrType, const String& name);
  void use(const String& addr, uint8_t addrType, const String& name);
  void forget();
  // forget() + drop the stored bond, so this unit stops recognising the keyboard's reconnect adverts
  void unpair();

  State state() const { return st; }
  const String& name() const { return kbName; }
  uint32_t passkey() const { return code; }                // the code to type while pairing
  bool pairingNow() const { return pairing && (st == SEARCHING || st == CONNECTING); }

  void onKey(std::function<void(const KeyEvent&)> h) { keyH = h; }
  void onState(std::function<void(State)> h) { stateH = h; }
  void onLog(std::function<void(const String&)> h) { logH = h; }

private:
  bool connectNow();
  void handleReport(const uint8_t* d, size_t n);
  void emit(uint8_t usage, bool shift, bool ctrl, bool alt);
  void setState(State s);
  void log(const String& s) { if (logH) logH(s); }

  State st = NONE;
  String addrStr, kbName;
  uint8_t addrType = 0;
  bool pairing = false;
  uint32_t code = 0;
  uint32_t retryAt = 0;
  uint8_t prevKeys[6] = { 0 };
  bool capsLock = false;

  std::function<void(const KeyEvent&)> keyH;
  std::function<void(State)> stateH;
  std::function<void(const String&)> logH;
};
