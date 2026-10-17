// MeshRadio - what PIP LINK needs from a mesh radio's phone-companion link, whatever the mesh.
// One radio is used at a time: MeshCoreBLE (MeshCore companion) or MeshtasticBLE (Meshtastic).
// The UI only talks to this interface: contacts, channels, text messages, our position, the clock.
#pragma once
#include <Arduino.h>
#include <functional>
#include <vector>

struct McContact {
  String name;
  uint8_t prefix[6];       // DM address: MeshCore = first 6 bytes of the public key,
                           //             Meshtastic = node number (little-endian) + 2 zero bytes
  uint8_t type;            // 1 chat, 2 repeater/router, 3 room, 4 sensor
  float lat = 0, lon = 0;  // advertised position (0,0 = none)
  uint32_t lastAdvert = 0; // Unix time it was last heard
};

struct McChannel {
  int idx;
  String name;
};

struct McMessage {
  bool isChannel;
  int chan;                // channel index (isChannel)
  uint8_t prefix[6];       // sender (DM)
  String from;             // resolved sender name
  String text;
  float snr;
  uint32_t ts;             // sender's Unix time (0 if unknown)
};

class MeshRadio {
public:
  enum State {
    IDLE,          // no radio selected
    SEARCHING,     // radio selected, not heard yet (or reconnecting)
    CONNECTING,    // connecting + pairing
    NEED_PIN,      // the radio is asking for the PIN shown on its screen -> providePin()
    ONLINE,        // companion link up
    PAIR_FAILED    // the radio rejected the PIN; waits for selectRadio() / providePin() with a new one
  };
  virtual ~MeshRadio() {}

  virtual const char* kind() const = 0;                  // "MeshCore" / "Meshtastic"
  virtual void begin() = 0;
  virtual void poll() = 0;                               // call often from loop()
  // key = how the radio is found again (advertised name or BLE address); pin 0 = ask when needed
  virtual void selectRadio(const String& key, uint32_t pin) = 0;
  virtual void forgetRadio() = 0;
  virtual void providePin(uint32_t pin) { (void)pin; }

  State state() const { return st; }
  const String& radioName() const { return target; }
  const String& selfName() const { return self; }
  const std::vector<McContact>& contacts() const { return contactList; }
  const std::vector<McChannel>& channels() const { return channelList; }
  String contactName(const uint8_t prefix[6]) const;
  uint32_t now() const { return radioTime ? radioTime + (millis() - radioTimeAt) / 1000 : 0; }   // Unix time, 0 = unknown

  // our own position as the radio reports it (GPS, or the manually set location); 0,0 = none
  bool hasPosition() const { return selfLat != 0 || selfLon != 0; }
  float lat() const { return selfLat; }
  float lon() const { return selfLon; }
  uint32_t positionAt() const { return posAt; }        // millis() of the last report
  virtual void refreshPosition() = 0;
  virtual void refreshContacts() = 0;

  virtual bool sendChannel(int idx, const String& text) = 0;
  virtual bool sendDirect(const uint8_t prefix[6], const String& text) = 0;
  // delete a contact from the radio's own contact list (it drops out of contacts() at once)
  virtual bool removeContact(const uint8_t prefix[6]) { (void)prefix; return false; }
  // tell the mesh we're here now (MeshCore: a flood advert; Meshtastic: our node info, broadcast)
  virtual bool advertise() { return false; }

  void onLog(std::function<void(const String&)> h) { logH = h; }
  void onRoster(std::function<void()> h) { rosterH = h; }          // contacts / channels changed
  void onMessage(std::function<void(const McMessage&)> h) { msgH = h; }
  void onState(std::function<void(State)> h) { stateH = h; }

protected:
  void setState(State s) {
    if (s == st) return;
    st = s;
    if (stateH) stateH(s);
  }
  void log(const String& s) { if (logH) logH(s); }
  void setClock(uint32_t unix) { if (unix > 1700000000UL) { radioTime = unix; radioTimeAt = millis(); } }

  String target, self;
  State st = IDLE;
  std::vector<McContact> contactList;
  std::vector<McChannel> channelList;
  uint32_t radioTime = 0, radioTimeAt = 0;
  float selfLat = 0, selfLon = 0;
  uint32_t posAt = 0;

  std::function<void(const String&)> logH;
  std::function<void()> rosterH;
  std::function<void(const McMessage&)> msgH;
  std::function<void(State)> stateH;
};

inline String MeshRadio::contactName(const uint8_t prefix[6]) const {
  for (const McContact& c : contactList)
    if (memcmp(c.prefix, prefix, 6) == 0) return c.name;
  char hex[13];
  snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", prefix[0], prefix[1], prefix[2], prefix[3]);
  return String(hex);
}
