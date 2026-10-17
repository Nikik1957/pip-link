# Meshtastic over BLE (as used by `lib/meshtastic_ble`)

Sources:
- [meshtastic/protobufs](https://github.com/meshtastic/protobufs) (`mesh.proto`, `channel.proto`, `config.proto`, `portnums.proto`)
- [meshtastic/firmware](https://github.com/meshtastic/firmware) (`BluetoothCommon.h`, `nimble/NimbleBluetooth.cpp`, `mesh/PhoneAPI.*`), read 2026-10

The byte layouts below were checked against the official Python protobuf classes (`meshtastic` package). They have not been tested on a real Meshtastic radio yet.

## Transport
| | UUID | use |
|---|---|---|
| service | `6ba1b218-15a8-461f-9fa8-5dcae273eafd` | advertised; this is how BleHub spots Meshtastic radios |
| ToRadio | `f75c76d2-129e-4dad-a1dd-7866124401e7` | write one `ToRadio` protobuf |
| FromRadio | `2c55e69e-4993-11ed-b878-0242ac120002` | read one `FromRadio` per read; an empty read means the queue is empty |
| FromNum | `ed9da18c-a800-4f66-a670-aa7547e34453` | notify: new FromRadio data is waiting |

- **Advertised name:** `SHORTNAME_abcd`, or `Meshtastic_abcd` before an owner is set.
- **Pairing:** the radio is DISPLAY_ONLY, so we type the PIN (our IO capability is KEYBOARD_ONLY).
  - FIXED_PIN mode uses a fixed PIN, 123456 by default.
  - RANDOM_PIN mode shows a new PIN on the radio's screen for each pairing.
  - NO_PIN mode pairs with no PIN (just works).
  - Pairing therefore runs asynchronously (`secureConnection(true)`). `onPassKeyEntry` puts the driver in the `NEED_PIN` state, the app asks the user, and `providePin()` injects the passkey. Bonds are kept.

## Session
1. Subscribe to FromNum, then write `ToRadio { want_config_id(3) = nonce }`.
2. Read FromRadio until `config_complete_id(7) == nonce`. Along the way you get `my_info(3)`, `node_info(4)` for every node, `channel(10)`, `config(5)`, and so on.
3. After that, each FromNum notify means: read FromRadio until it's empty.
4. Refreshes use `want_config_id = 69421` (`SPECIAL_NONCE_ONLY_NODES`, firmware 2.6+), which sends just the node DB. Older firmware sends everything.

## Messages used
| message | fields |
|---|---|
| `FromRadio` | packet 2, my_info 3, node_info 4, config 5, config_complete_id 7, rebooted 8, channel 10 |
| `MyNodeInfo` | my_node_num 1 |
| `NodeInfo` | num 1, user 2, position 3, snr 4 (float), last_heard 5 (fixed32) |
| `User` | long_name 2, short_name 3, role 7 |
| `Position` | latitude_i 1 / longitude_i 2 (sfixed32, ×1e-7), time 4 |
| `MeshPacket` | from 1, to 2 (fixed32), channel 3, decoded 4, id 6, rx_time 7, rx_snr 8, want_ack 10 |
| `Data` | portnum 1 (TEXT_MESSAGE_APP 1, POSITION_APP 3, NODEINFO_APP 4), payload 2 |
| `Channel` | index 1, settings 2 (`ChannelSettings.name` 3), role 3 (0 disabled, 1 primary, 2 secondary) |
| `Config` | lora 6 → `use_preset` 1, `modem_preset` 2. An empty primary channel name is shown as the preset name (LongFast, …). |
| `ToRadio` | packet 1 (text: `to` = node or 0xFFFFFFFF, `channel`, `decoded{portnum 1, payload}`, `id`, `want_ack`), want_config_id 3 |

## PIP LINK mapping
- **Contacts:** contact key = node number (little-endian) in `prefix[0..3]`.
- **Contact types** (from the node's role):
  - Router, ROUTER_CLIENT, REPEATER and ROUTER_LATE map to type 2, the RADIO icon.
  - SENSOR maps to type 4.
  - Everything else is a chat node.
- **Node list:** nodes heard in the last 7 days, at most 120, most recent first.
- **Clock:** the freshest `last_heard`, position `time` or packet `rx_time`. The phone API has no "get time" call.
- **Channel messages:** sent without a name prefix, because Meshtastic shows the sender itself.
