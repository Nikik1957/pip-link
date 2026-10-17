#include "ssh_link.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

namespace {
const uint32_t STACK = 32768;                    // internal RAM; big allocations go to PSRAM by themselves
const size_t RX_BUF = 8192, TX_BUF = 1024;
bool libsshReady = false;

struct Lock {
  SemaphoreHandle_t m;
  explicit Lock(void* p) : m((SemaphoreHandle_t)p) { xSemaphoreTake(m, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m); }
};
}  // namespace

void SshLink::begin() {
  if (rx) return;
  rx = xStreamBufferCreate(RX_BUF, 1);
  tx = xStreamBufferCreate(TX_BUF, 1);
  mx = xSemaphoreCreateMutex();
}

bool SshLink::start(const String& u, const String& address, uint16_t p, int c, int r, const String& known, const char* tt) {
  if (started || !rx) return false;
  if (!libsshReady) { libssh_begin(); libsshReady = true; }
  {
    Lock l(mx);
    user = u; ip = address; knownFp = known; fp = ""; why = "";
  }
  port = p; cols = c; rows = r; term = tt;
  cancelReq = keyAnswered = keyOk = pwGiven = fpChanged = resizeReq = false;
  tries = 3;
  memset(pw, 0, sizeof(pw));
  xStreamBufferReset((StreamBufferHandle_t)rx);
  xStreamBufferReset((StreamBufferHandle_t)tx);
  st = CONNECTING;
  started = true;
  if (xTaskCreatePinnedToCore(taskEntry, "ssh", STACK, this, 1, nullptr, 1) != pdPASS) {
    started = false;
    closeWith("not enough memory for SSH");
    return false;
  }
  return true;
}

void SshLink::cancel() {
  if (started) cancelReq = true;
  else if (st != IDLE && st != CLOSED) st = IDLE;
}

void SshLink::reset() { if (!started) st = IDLE; }

String SshLink::fingerprint() { Lock l(mx); return fp; }
String SshLink::reason() { Lock l(mx); return why; }

void SshLink::acceptHostKey(bool yes) { keyOk = yes; keyAnswered = true; }

void SshLink::password(const String& p) {
  strlcpy(pw, p.c_str(), sizeof(pw));
  pwGiven = true;
}

size_t SshLink::read(uint8_t* buf, size_t n) {
  if (!rx) return 0;
  return xStreamBufferReceive((StreamBufferHandle_t)rx, buf, n, 0);
}

void SshLink::write(const uint8_t* d, size_t n) {
  if (st != ONLINE || !tx) return;
  xStreamBufferSend((StreamBufferHandle_t)tx, d, n, pdMS_TO_TICKS(50));
}

void SshLink::closeWith(const String& w) {
  { Lock l(mx); why = w; }
  st = CLOSED;
}

bool SshLink::waitFor(volatile bool& flag) {
  while (!flag) {
    if (cancelReq) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

void SshLink::taskEntry(void* self) {
  SshLink* s = (SshLink*)self;
  s->run();
  s->stackFree = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  s->started = false;
  vTaskDelete(nullptr);
}

void SshLink::run() {
  String u, host, known;
  { Lock l(mx); u = user; host = ip; known = knownFp; }
  ssh_session s = ssh_new();
  if (!s) { closeWith("not enough memory for SSH"); return; }
  long timeout = 10;
  int verbosity = SSH_LOG_NOLOG;
  int no = 0;
  unsigned int pt = port;
  ssh_options_set(s, SSH_OPTIONS_HOST, host.c_str());
  ssh_options_set(s, SSH_OPTIONS_PORT, &pt);
  ssh_options_set(s, SSH_OPTIONS_USER, u.c_str());
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
  ssh_options_set(s, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
  ssh_options_set(s, SSH_OPTIONS_PROCESS_CONFIG, &no);    // no ~/.ssh/config on a microcontroller

  ssh_channel ch = nullptr;
  String fail;
  do {
    if (ssh_connect(s) != SSH_OK) { fail = String("can't connect: ") + ssh_get_error(s); break; }
    if (cancelReq) break;

    // host key: compare with what we saw last time (trust on first use)
    ssh_key key = nullptr;
    unsigned char* hash = nullptr;
    size_t hlen = 0;
    if (ssh_get_server_publickey(s, &key) != SSH_OK ||
        ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != 0) {
      ssh_key_free(key);
      fail = "couldn't read the computer's key";
      break;
    }
    char* f = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(key);
    { Lock l(mx); fp = f ? f : ""; }
    if (f) ssh_string_free_char(f);
    String got = fingerprint();
    if (known.length() && known != got) { fpChanged = true; fail = "HOST KEY CHANGED - not connecting"; break; }
    if (!known.length()) {
      st = ASK_HOSTKEY;
      if (!waitFor(keyAnswered)) break;
      if (!keyOk) { fail = "host key not accepted"; break; }
    }

    // login: password (or keyboard-interactive with the same password), 3 tries
    bool in = false;
    ssh_userauth_none(s, nullptr);
    int methods = ssh_userauth_list(s, nullptr);
    if (!(methods & (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_INTERACTIVE))) {
      fail = "this computer doesn't allow password logins";
      break;
    }
    while (!in && tries > 0 && !cancelReq) {
      pwGiven = false;
      st = ASK_PASSWORD;
      if (!waitFor(pwGiven)) break;
      if (!pw[0]) { cancelReq = true; break; }
      st = LOGGING_IN;
      int rc = SSH_AUTH_DENIED;
      if (methods & SSH_AUTH_METHOD_PASSWORD) rc = ssh_userauth_password(s, nullptr, pw);
      if (rc != SSH_AUTH_SUCCESS && (methods & SSH_AUTH_METHOD_INTERACTIVE)) {
        rc = ssh_userauth_kbdint(s, nullptr, nullptr);
        while (rc == SSH_AUTH_INFO) {
          int n = ssh_userauth_kbdint_getnprompts(s);
          for (int i = 0; i < n; i++) ssh_userauth_kbdint_setanswer(s, i, pw);
          rc = ssh_userauth_kbdint(s, nullptr, nullptr);
        }
      }
      memset(pw, 0, sizeof(pw));
      if (rc == SSH_AUTH_SUCCESS) in = true;
      else if (rc == SSH_AUTH_ERROR) { fail = String("login error: ") + ssh_get_error(s); tries = 0; }
      else tries = tries - 1;
    }
    if (!in) { if (!fail.length() && !cancelReq) fail = "wrong password"; break; }

    // a shell on a pseudo-terminal the size of the Pip-Boy grid
    ch = ssh_channel_new(s);
    if (!ch || ssh_channel_open_session(ch) != SSH_OK ||
        ssh_channel_request_pty_size(ch, term, cols, rows) != SSH_OK || ssh_channel_request_shell(ch) != SSH_OK) {
      fail = String("couldn't open a shell: ") + ssh_get_error(s);
      break;
    }
    st = ONLINE;

    uint8_t buf[512];
    while (!cancelReq) {
      bool busy = false;
      for (int err = 0; err < 2; err++) {
        int n = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), err);
        if (n == SSH_ERROR) { fail = "connection lost"; cancelReq = true; break; }
        if (n > 0) {
          busy = true;
          size_t off = 0;
          while (off < (size_t)n && !cancelReq)               // the UI is slower than the network: wait for room
            off += xStreamBufferSend((StreamBufferHandle_t)rx, buf + off, n - off, pdMS_TO_TICKS(100));
        }
      }
      if (cancelReq) break;
      if (resizeReq) { resizeReq = false; ssh_channel_change_pty_size(ch, newCols, newRows); }
      if (ssh_channel_is_eof(ch) || !ssh_channel_is_open(ch)) { fail = "the computer closed the session"; break; }
      size_t k = xStreamBufferReceive((StreamBufferHandle_t)tx, buf, sizeof(buf), 0);
      if (k) { busy = true; if (ssh_channel_write(ch, buf, k) == SSH_ERROR) { fail = "connection lost"; break; } }
      if (!busy) vTaskDelay(pdMS_TO_TICKS(10));
    }
  } while (false);

  memset(pw, 0, sizeof(pw));
  if (ch) {
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);
    ssh_channel_free(ch);
  }
  ssh_disconnect(s);
  ssh_free(s);
  closeWith(fail.length() ? fail : String("disconnected"));
}
