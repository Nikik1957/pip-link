#include "telnet_codec.h"
#include <string.h>

void TelnetCodec::begin(int cols, int rows, const char* ttype) {
  nc = cols; nr = rows;
  termType = ttype;
  st = DATA;
  sub.clear();
  echo = nawsOn = false;
  memset(told, 0, sizeof(told));
}

static void put3(std::string& o, uint8_t a, uint8_t b, uint8_t c) { o += (char)a; o += (char)b; o += (char)c; }

std::string TelnetCodec::windowSize() const {
  std::string o;
  if (!nawsOn) return o;
  put3(o, IAC, SB, OPT_NAWS);
  uint8_t v[4] = { (uint8_t)(nc >> 8), (uint8_t)nc, (uint8_t)(nr >> 8), (uint8_t)nr };
  for (uint8_t b : v) { o += (char)b; if (b == IAC) o += (char)IAC; }
  o += (char)IAC; o += (char)SE;
  return o;
}

void TelnetCodec::option(uint8_t v, uint8_t opt, std::string& reply) {
  switch (v) {
    case DO:                                                  // server asks us to do something
      if (opt == OPT_TTYPE || opt == OPT_SGA || opt == OPT_NAWS) {
        if (!told[opt]) put3(reply, IAC, WILL, opt);
        told[opt] = true;
        if (opt == OPT_NAWS) { nawsOn = true; reply += windowSize(); }
      } else put3(reply, IAC, WONT, opt);
      break;
    case DONT:
      if (opt == OPT_NAWS) nawsOn = false;
      put3(reply, IAC, WONT, opt);
      break;
    case WILL:                                                // server offers to do something
      if (opt == OPT_ECHO || opt == OPT_SGA) {
        if (opt == OPT_ECHO) echo = true;
        if (!told[128 + opt % 128]) put3(reply, IAC, DO, opt);
        told[128 + opt % 128] = true;
      } else put3(reply, IAC, DONT, opt);
      break;
    case WONT:
      if (opt == OPT_ECHO) echo = false;
      put3(reply, IAC, DONT, opt);
      break;
  }
}

void TelnetCodec::subneg(std::string& reply) {
  // terminal type: IAC SB TTYPE SEND(1) IAC SE  ->  IAC SB TTYPE IS(0) "VT100" (or "ANSI") IAC SE
  if (sub.size() >= 2 && (uint8_t)sub[0] == OPT_TTYPE && sub[1] == 1) {
    put3(reply, IAC, SB, OPT_TTYPE);
    reply += (char)0;
    reply += termType;
    reply += (char)IAC; reply += (char)SE;
  }
}

void TelnetCodec::receive(const uint8_t* d, size_t n, std::string& data, std::string& reply) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = d[i];
    switch (st) {
      case DATA:
        if (c == IAC) st = IAC_SEEN;
        else if (c == '\r') { data += '\r'; st = CR_SEEN; }
        else data += (char)c;
        break;
      case CR_SEEN:                                           // CR NUL = bare CR
        st = DATA;
        if (c == 0) break;
        if (c == IAC) { st = IAC_SEEN; break; }
        data += (char)c;
        break;
      case IAC_SEEN:
        if (c == IAC) { data += (char)IAC; st = DATA; }      // escaped 0xFF
        else if (c >= WILL && c <= DONT) { verb = c; st = VERB; }
        else if (c == SB) { sub.clear(); st = SUB; }
        else st = DATA;                                       // NOP, GA, AYT, ...
        break;
      case VERB:
        option(verb, c, reply);
        st = DATA;
        break;
      case SUB:
        if (c == IAC) st = SUB_IAC;
        else if (sub.size() < 64) sub += (char)c;
        break;
      case SUB_IAC:
        if (c == SE) { subneg(reply); st = DATA; }
        else { if (c == IAC && sub.size() < 64) sub += (char)IAC; st = SUB; }
        break;
    }
  }
}

std::string TelnetCodec::encode(const std::string& keys) const {
  std::string o;
  for (char ch : keys) {
    uint8_t c = (uint8_t)ch;
    if (c == IAC) o += (char)IAC;
    o += (char)c;
    if (c == '\r') o += (char)0;                              // Enter: CR NUL (NVT)
  }
  return o;
}
