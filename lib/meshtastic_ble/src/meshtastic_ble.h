// MeshtasticBLE - Meshtastic "phone API" client over BLE (see docs/meshtastic_protocol.md).
// Service 6ba1b218-..., ToRadio (write) / FromRadio (read) / FromNum (notify), protobuf messages.
// Pairing: the radio shows a PIN (fixed, default 123456, or random per pairing) that we type; because a
// random PIN only appears once pairing starts, pairing runs async and the state goes NEED_PIN until the
// app calls providePin(). Bonds are kept, so later reconnects need no PIN.
#pragma once
#include <Arduino.h>
#include <deque>
#include <vector>
#include "mesh_radio.h"

class MeshtasticBLE : public MeshRadio {
public:
  const char* kind() const override { return "Meshtastic"; }
  void begin() override;
  void poll() override;
  void selectRadio(const String& advName, uint32_t pin) override;   // pin 0: ask (NEED_PIN) if the radio wants one
  void forgetRadio() override;
  void providePin(uint32_t pin) override;
  void refreshPosition() override;                     // nodes-only config pull (rate-limited)
  void refreshContacts() override;
  bool sendChannel(int idx, const String& text) override;
  bool sendDirect(const uint8_t prefix[6], const String& text) override;
  bool removeContact(const uint8_t prefix[6]) override;   // AdminMessage remove_by_nodenum to our own node
  bool advertise() override;                               // our own User record as a NODEINFO_APP broadcast

private:
  enum Phase { P_IDLE, P_PAIRING, P_CONFIG, P_RUN };
  bool connectNow();
  void startSession();                                 // subscribe + want_config
  void requestConfig(uint32_t nonce);
  bool readFromRadio();                                // one FromRadio; false when the queue is empty
  void handleFromRadio(const uint8_t* d, size_t n);
  void handlePacket(const uint8_t* d, size_t n);
  void handleNodeInfo(const uint8_t* d, size_t n, bool configuring);
  void handleChannel(const uint8_t* d, size_t n);
  McContact* contactFor(uint32_t num, bool create);
  void commitNodes();
  bool sendText(uint32_t to, int channel, const String& text);

  Phase phase = P_IDLE;
  uint32_t pin = 0;
  uint32_t myNum = 0;
  std::vector<uint8_t> selfUser;                           // our node's User protobuf, as the radio reported it
  uint32_t nonce = 0;
  uint32_t retryAt = 0, lastDiag = 0, lastRefresh = 0, pairStart = 0;
  String presetName = "LongFast";
  std::vector<McContact> pendingNodes;
  std::vector<McChannel> pendingChannels;
  bool sawConfigNodes = false;
};
