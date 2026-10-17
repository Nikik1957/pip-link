// MeshCoreBLE - MeshCore companion client over BLE (Nordic UART Service).
// Connects to the radio the app selects (by advertised name, with its pairing PIN), then:
// APP_START, contacts, channel names, the radio's clock, receiving (V3) messages and sending.
// Frame layouts: docs/meshcore_frames.md.
#pragma once
#include <Arduino.h>
#include <deque>
#include <vector>
#include "mesh_radio.h"

class MeshCoreBLE : public MeshRadio {
public:
  const char* kind() const override { return "MeshCore"; }
  void begin() override;
  void poll() override;
  void selectRadio(const String& advName, uint32_t passkey) override;   // MeshCore asks for the PIN up front
  void forgetRadio() override;
  void refreshPosition() override;                     // APP_START -> SELF_INFO
  void refreshContacts() override;                     // re-pull contacts (their positions change too)
  bool sendChannel(int idx, const String& text) override;   // prefixes "<selfName>: "
  bool sendDirect(const uint8_t prefix[6], const String& text) override;
  bool removeContact(const uint8_t prefix[6]) override;   // CMD_REMOVE_CONTACT (needs the full public key)
  bool advertise() override;                               // CMD_SEND_SELF_ADVERT, flood

private:
  enum ConnResult { CONN_OK, CONN_FAIL, CONN_BADPIN };
  ConnResult connectNow();
  void handleFrame(const uint8_t* d, size_t n);
  void queueCmd(const uint8_t* d, size_t n);

  struct Key { uint8_t k[32]; };
  std::vector<McContact> pending;
  std::vector<Key> keys, pendingKeys;                  // public keys, in step with contactList / pending
  std::deque<std::vector<uint8_t>> cmdq;
  uint32_t lastCmd = 0, retryAt = 0, lastDiag = 0;
  bool quietSelf = false;                              // SELF_INFO from refreshPosition(): position only
};
