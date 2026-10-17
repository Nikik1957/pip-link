// pb_lite - the few protobuf wire-format operations the Meshtastic client needs (no code generation).
// Wire types: 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32.
#pragma once
#include <Arduino.h>
#include <vector>

struct PbReader {
  const uint8_t* p;
  const uint8_t* end;
  PbReader(const uint8_t* d, size_t n) : p(d), end(d + n) {}

  bool next(uint32_t& field, uint8_t& wt) {           // false at the end (or on garbage)
    if (p >= end) return false;
    uint64_t key = varint();
    field = key >> 3;
    wt = key & 7;
    return p <= end && field != 0;
  }
  uint64_t varint() {
    uint64_t v = 0;
    for (int s = 0; p < end && s < 64; s += 7) {
      uint8_t b = *p++;
      v |= (uint64_t)(b & 0x7F) << s;
      if (!(b & 0x80)) break;
    }
    return v;
  }
  uint32_t fixed32() {
    if (end - p < 4) { p = end; return 0; }
    uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    return v;
  }
  bool bytes(const uint8_t*& d, size_t& n) {
    n = (size_t)varint();
    if ((size_t)(end - p) < n) { p = end; return false; }
    d = p;
    p += n;
    return true;
  }
  PbReader sub() {                                       // a nested message
    const uint8_t* d; size_t n;
    if (!bytes(d, n)) return PbReader(p, 0);
    return PbReader(d, n);
  }
  String str() {
    const uint8_t* d; size_t n;
    if (!bytes(d, n)) return String();
    String s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) {                     // ASCII only: the Pip-Boy font has no UTF-8 / emoji
      if (d[i] >= 32 && d[i] < 127) s += (char)d[i];
      else if (d[i] < 0x80) s += '?';
    }
    return s;
  }
  void skip(uint8_t wt) {
    if (wt == 0) varint();
    else if (wt == 1) p += 8;
    else if (wt == 5) p += 4;
    else if (wt == 2) { const uint8_t* d; size_t n; bytes(d, n); }
    else p = end;                                        // groups (3/4) aren't used by Meshtastic
    if (p > end) p = end;
  }
};

struct PbWriter {
  std::vector<uint8_t> b;
  void varint(uint64_t v) {
    do { uint8_t c = v & 0x7F; v >>= 7; b.push_back(v ? c | 0x80 : c); } while (v);
  }
  void tag(uint32_t field, uint8_t wt) { varint((field << 3) | wt); }
  void u32(uint32_t field, uint64_t v) { tag(field, 0); varint(v); }
  void fixed32(uint32_t field, uint32_t v) {
    tag(field, 5);
    for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xFF);
  }
  void bytes(uint32_t field, const uint8_t* d, size_t n) {
    tag(field, 2);
    varint(n);
    b.insert(b.end(), d, d + n);
  }
  void str(uint32_t field, const String& s) { bytes(field, (const uint8_t*)s.c_str(), s.length()); }
  void msg(uint32_t field, const PbWriter& m) { bytes(field, m.b.data(), m.b.size()); }
};
