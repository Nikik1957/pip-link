# RadiaCode BLE protocol (as used by `lib/ble_radiacode`)

Source: the open-source [cdump/radiacode](https://github.com/cdump/radiacode) Python library
(`transports/bluetooth.py`, `radiacode.py`, `decoders/databuf.py`, `types.py`), read 2026-10-05.
Not yet checked byte-for-byte against our own captures. Compare the live readings with the RadiaCode's
own display.

## Transport
- Service `e63215e5-7003-49d8-96b0-b024798fb901`. The device needs no pairing.
- Requests go to `e63215e6-…`, written **without response** in **18-byte chunks**.
- Responses come back as notifications on `e63215e7-…`. The first chunk starts with `i32 LE` = the number of bytes that follow. Keep appending chunks until that many bytes have arrived.

## Request / response frame
```
request : u32 len | u16 cmd | u8 0 | u8 seq (0x80 + n, n = 0..31) | args
response: (after the length prefix) the same 4-byte header echoed | payload
```

## Session start (what the library does)
1. `SET_EXCHANGE 0x0007` with args `01 ff 12 ff`.
2. `SET_TIME 0x0A04` with args `day, month, year-2000, 0, sec, min, hour, 0`. We skip this when the hub doesn't know the time yet.
3. `WR_VIRT_SFR 0x0825` with args `u32 DEVICE_TIME (0x0504)`, `u32 0`. The reply is `u32 retcode` (1 = ok).

## Live data: `RD_VIRT_STRING 0x0826`, args `u32 VS_DATA_BUF (0x100)`
The reply is `u32 retcode (1)`, `u32 len`, then `len` bytes. Newer firmware sometimes adds one trailing `00`.

The data is a run of records:
```
u8 seq | u8 eid | u8 gid | i32 ts (x10 ms) | payload
```
Stop at a seq jump or an unknown type.

| eid/gid | name | payload |
|---|---|---|
| 0/0 | RealTimeData | `f32 cps, f32 dose_rate, u16 cps_err(/10 %), u16 dr_err(/10 %), u16 flags, u8 rt_flags` (15 B) |
| 0/1 | RawData | `f32 cps, f32 dose_rate` (8) |
| 0/2 | DoseRateDB | `u32 count, f32 cps, f32 dose_rate, u16 dr_err, u16 flags` (16) |
| 0/3 | RareData | `u32 duration s, f32 dose, u16 temp ((t-2000)/100 C), u16 charge (/100 %), u16 flags` (14) |
| 0/4, 0/5 | User/Schedule | 16 B |
| 0/6 | Accel | 6 B |
| 0/7 | Event | `u8 event, u8 param, u16 flags` (4) |
| 0/8, 0/9 | RawCountRate / RawDoseRate | `f32, u16` (6) |
| 1/1, 1/2, 1/3 | sample blocks | `u16 n, u32 ms` + n × 8 / 16 / 14 B |

**Units:** dose_rate and dose are in R-based protocol units. Multiply by **10000** to get uSv/h and uSv. The library's Prometheus exporter does `10000 * dose_rate`, and its narodmon example does `1e6 * dose_rate` to get uR/h.
