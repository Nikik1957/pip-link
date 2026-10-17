// SshLink - an interactive SSH shell for the TERM tab (LibSSH-ESP32 / libssh).
// libssh blocks for seconds at a time (connect, key exchange, login), so it runs in its own FreeRTOS task.
// The UI (loop()) polls state(), answers the two questions it asks (accept the host key? the password?),
// and moves bytes through two stream buffers. Nothing here is logged or stored: the password is wiped
// after use, and remembering host keys is the caller's job (fingerprint() / fingerprintChanged()).
#pragma once
#include <Arduino.h>

class SshLink {
public:
  enum State {
    IDLE,
    CONNECTING,       // TCP + key exchange
    ASK_HOSTKEY,      // first visit: show fingerprint(), then acceptHostKey()
    ASK_PASSWORD,     // password(); triesLeft() tells how many are left
    LOGGING_IN,
    ONLINE,           // shell open: read() / write()
    CLOSED            // reason() says why; fingerprintChanged() if the host key didn't match
  };

  void begin();
  // ip: an address (resolve names first). knownFp: the saved "SHA256:..." for this host, "" if none.
  bool start(const String& user, const String& ip, uint16_t port, int cols, int rows, const String& knownFp,
             const char* termType = "vt100");
  void cancel();                                  // from any state; ends in CLOSED (or IDLE if not running)
  void reset();                                   // CLOSED -> IDLE once the UI is done with it

  State state() const { return st; }
  String fingerprint();
  bool fingerprintChanged() const { return fpChanged; }
  String reason();
  int triesLeft() const { return tries; }

  void acceptHostKey(bool yes);
  void password(const String& pw);                // empty = give up

  size_t read(uint8_t* buf, size_t n);            // remote output, non-blocking
  void write(const uint8_t* d, size_t n);         // keystrokes
  void resize(int c, int r) { newCols = c; newRows = r; resizeReq = true; }   // the pty follows the screen
  uint32_t stackLeft() const { return stackFree; }   // bytes of the task's stack never used (tuning)

private:
  static void taskEntry(void* self);
  void run();
  void closeWith(const String& why);
  bool waitFor(volatile bool& flag);              // false if cancelled

  volatile State st = IDLE;
  volatile bool cancelReq = false, keyAnswered = false, keyOk = false, pwGiven = false, fpChanged = false;
  volatile int tries = 0;
  volatile bool resizeReq = false;
  volatile int newCols = 0, newRows = 0;
  volatile uint32_t stackFree = 0;
  char pw[96];
  String user, ip, knownFp, fp, why;
  uint16_t port = 22;
  int cols = 80, rows = 24;
  const char* term = "vt100";
  void* rx = nullptr;                             // StreamBufferHandle_t: remote -> UI
  void* tx = nullptr;                             //                       UI -> remote
  void* mx = nullptr;                             // guards the Strings above
  bool started = false;
};
