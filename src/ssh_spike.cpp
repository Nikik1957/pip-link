// SSH spike: link LibSSH-ESP32 and open a session object (no network needed to prove it builds).
#include <Arduino.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

void setup() {
  Serial.begin(115200);
  libssh_begin();
  ssh_session s = ssh_new();
  Serial.printf("ssh_new -> %p, libssh %s\n", (void*)s, ssh_version(0));
  if (s) ssh_free(s);
}
void loop() { delay(1000); }
