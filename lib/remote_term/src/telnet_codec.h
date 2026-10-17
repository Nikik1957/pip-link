// TelnetCodec - the Telnet protocol (RFC 854) between the TCP socket and the terminal emulator.
// Incoming: strips IAC commands and answers option negotiation; the rest is screen data.
// We offer: terminal type (VT100), window size (NAWS), suppress-go-ahead; the server echoes.
// Outgoing: keystrokes with 0xFF doubled. Plain C++ (no Arduino) so it can be unit-tested on the PC.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

class TelnetCodec {
public:
  void begin(int cols, int rows, const char* ttype = "VT100");   // a new connection; ttype "ANSI" for BBSes
  // bytes from the server: screen data is appended to `data`, protocol replies to `reply`
  void receive(const uint8_t* d, size_t n, std::string& data, std::string& reply);
  std::string encode(const std::string& keys) const;   // keystrokes -> wire bytes
  std::string windowSize() const;               // NAWS report (when the size changes)
  std::string setSize(int cols, int rows) { nc = cols; nr = rows; return windowSize(); }   // "" if NAWS is off
  bool serverEchoes() const { return echo; }

  static const uint8_t IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240;
  static const uint8_t OPT_ECHO = 1, OPT_SGA = 3, OPT_TTYPE = 24, OPT_NAWS = 31;

private:
  void option(uint8_t verb, uint8_t opt, std::string& reply);
  void subneg(std::string& reply);

  enum State { DATA, IAC_SEEN, VERB, SUB, SUB_IAC, CR_SEEN };
  State st = DATA;
  uint8_t verb = 0;
  std::string sub;
  int nc = 80, nr = 24;
  std::string termType = "VT100";
  bool echo = false, nawsOn = false;
  bool told[256];                               // options we already answered (avoids negotiation loops)
};
