# MeshCore companion frames over BLE (verified)

Verified against the `meshcore` Python library 2.3.7 (`reader.py`, `commands/*.py`) and live
traffic from BYRD 650 (RAK4631). BLE uses the Nordic UART Service; each write/notify is one
raw frame (no `<`/`>`+length wrapper â€” that's USB only). Byte 0 is the command/response code.
Multi-byte integers are little-endian. Offsets below are from the start of the frame.

## Pairing
MITM + bonding, fixed passkey **123456** (screenless node). Call `secureConnection()` before
subscribing to the TX characteristic or the CCCD write is refused.

## Commands (ESP -> radio, write to RX `6E400002-â€¦`)
| Cmd | Bytes | Reply |
|---|---|---|
| APP_START | `01 01 00Ã—6 <appname>` | SELF_INFO `0x05` |
| GET_CONTACTS | `04 <since:u32>` (0 = all) | `0x02`, `0x03`Ã—N, `0x04` |
| GET_TIME | `05` | CURR_TIME `0x09` |
| SYNC_NEXT_MSG | `0A` | `0x10`/`0x11` message, or `0x0A` none left |
| GET_CHANNEL | `1F <idx>` | CHANNEL_INFO `0x12` (or ERR `0x01`) |
| SEND_DM | `02 00 <attempt> <ts:u32> <dst prefix:6> <utf8>` | SENT `0x06` |
| SEND_CHANNEL | `03 00 <chan> <ts:u32> <utf8>` | OK `0x00` |

Timestamps are Unix seconds; the ESP has no clock, so it borrows the radio's via GET_TIME.
Channel text convention: senders prefix their own name, `"NAME: message"` â€” so outgoing
channel messages must be sent as `"<selfName>: text"` or they look anonymous.

## Responses / pushes (radio -> ESP, notify on TX `6E400003-â€¦`)
| Code | Name | Layout |
|---|---|---|
| `0x05` | SELF_INFO | 1 adv_type, 2 tx_pwr, 3 max_tx, 4â€“35 pubkey, 36 lat, 40 lon, 44 multi_acks, 45 loc_policy, 46 telem, 47 manual_add, 48 freq, 52 bw, 56 sf, 57 cr, **58.. name** |
| `0x03` | CONTACT | **1â€“32 pubkey** (first 6 = DM prefix), **33 type** (1 chat, 2 repeater, 3 room, 4 sensor), 34 flags, 35 out_path_len, 36â€“99 out_path, **100â€“131 name** (NUL-padded), 132 last_advert |
| `0x09` | CURR_TIME | 1â€“4 time u32 |
| `0x12` | CHANNEL_INFO | 1 idx, **2â€“33 name** (NUL-terminated), 34â€“49 secret. idx 0 is the unnamed public channel |
| `0x10` | CONTACT_MSG_V3 | 1 SNR (int8 Ã·4), 2â€“3 reserved, **4â€“9 sender prefix**, 10 path_len, 11 txt_type, 12â€“15 ts, **16.. text** (20.. if txt_type==2, signed) |
| `0x11` | CHANNEL_MSG_V3 | 1 SNR (int8 Ã·4), 2â€“3 reserved, **4 channel idx**, 5 path_len, 6 txt_type, 7â€“10 ts, **11.. text** |
| `0x07` | CONTACT_MSG (old) | 1â€“6 prefix, 7 path_len, 8 txt_type, 9â€“12 ts, 13.. text |
| `0x08` | CHANNEL_MSG (old) | 1 chan, 2 path_len, 3 txt_type, 4â€“7 ts, 8.. text |
| `0x83` | MSG_WAITING | (push) -> send SYNC_NEXT_MSG until `0x0A` |
| `0x88` | (push) | binary RX-log/telemetry; ignore |
| `0x00`/`0x01`/`0x06` | OK / ERR / SENT | |

BYRD 650's firmware sends the **V3** message variants (`0x10`/`0x11`).
