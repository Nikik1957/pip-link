// BleHub - the one place that owns NimBLE: init, security defaults, and a single scan
// that sorts what it hears into the device kinds the hub can use. The app sets how hard
// to scan (scanning competes with the radio links, so it is off unless something is looked for).
// Device drivers (MeshCore radio, keyboard, RadiaCode) look devices up here and
// pause the scan while they connect.
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>

namespace BleHub {

enum Kind : uint8_t { MESHCORE, KEYBOARD, RADIACODE, MESHTASTIC };

struct Found {
  String name;            // advertised name ("" if none)
  NimBLEAddress addr;
  int rssi;
  uint32_t seen;          // millis() of the last advert
  Kind kind;
};

void begin();
enum ScanMode : uint8_t { SCAN_OFF, SCAN_SLOW, SCAN_FAST };
void setScan(ScanMode m);                                       // FAST: pickers / reconnecting, SLOW: waiting for a keyboard
void poll();                                                    // keeps the scan in the requested mode unless paused
void pause(bool on);                                            // drivers pause scanning while connecting
std::vector<Found> found(Kind k, uint32_t maxAgeMs = 30000);    // strongest signal first
// only devices heard very recently: a stale entry (e.g. a device that has since connected
// elsewhere and stopped advertising) makes connect() block for its whole timeout
bool lookup(Kind k, const String& nameOrAddr, NimBLEAddress& out, uint32_t maxAgeMs = 4000);
uint32_t advertsHeard();
String summary();                                               // for the log: what the scan has heard
// troubleshooting: describe every unrecognised device heard (once each, nearby ones only)
void debugUnknown(bool on);
std::vector<String> takeDebug();
String label(const Found& f);                                   // display name for pickers

}  // namespace BleHub
