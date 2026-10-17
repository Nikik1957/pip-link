# TERM tab: SSH and Telnet over Wi-Fi (v1.1)

The TERM tab connects to a computer on your Wi-Fi network, such as a Raspberry Pi, with SSH or Telnet, and shows its screen.
Text arrives through a VT100 terminal emulator (`lib/remote_term`). It shows plain text only: colours and bold are dropped.
The text size is chosen with SIZE: LARGE 58×12 (Monofonto16), MEDIUM 66×16 (Monofonto14), or SMALL 78×24 (the 6×8 font). BBS mode always uses 80×24 in the 6×8 font, which fills the whole 480 px width.

SETTINGS > Text margin (NVS `term_margin`, 0–12 px, default 6) sets how far from the left edge the text starts (`T.vt(size, margin)`). The Pip-Boy's bezel hides the first few pixels, and v1.1 drew 80×24 from x = 0, so the first column was cut off. In BBS mode a margin pushes the last column off the right edge instead. With 6 px, that's only column 80, which BBS screens rarely use: writing there makes many terminals wrap.

## Using it
- **WI-FI NETWORK:** scans for networks; pick one and type its password.
  - The password is stored in the PIP LINK unit (NVS `wifi_ssid` / `wifi_pass`), never on the SD card. With a BLE keyboard the password prompt shows `*`; the on-screen keyboard shows what you type.
  - Wi-Fi is only on while the TERM tab is open or a session is live. It switches off 30 s after you leave, because Wi-Fi and Bluetooth share one radio.
- **NEW CONNECTION:**
  - `user@computer[:port]` connects with **SSH** (port 22), for example `pi@raspberrypi.local`.
  - A bare `computer[:port]` uses **Telnet** (port 23).
  - Add `:port` for anything else, e.g. a BBS on `bbs.example.com:2323`. The on-screen keyboard has `@` and `:` keys.
  - `.local` names are looked up with mDNS.
  - The last 8 computers are remembered (NVS `hosts`). Pick one to CONNECT, DELETE it, switch its MODE, or (SSH) FORGET KEY.
- **MODE**, per computer (saved as ` bbs` / ` std` after the address):
  - **BBS** (the default for Telnet): announces the terminal type `ANSI`, reads every byte ≥ 0x80 as IBM PC code page 437 (no UTF-8 guessing, which could merge two art characters into one and shift the rest of the line), always runs at 80×24, and `ESC[2J` homes the cursor the way ANSI.SYS does. SIZE is fixed in this mode.
  - **STANDARD** (the default for SSH): `VT100`, UTF-8 with the CP437 fallback, and your chosen SIZE. Use it for Linux machines such as a Pi.
- **SSH login:**
  - **First visit:** PIP LINK shows the computer's key fingerprint (`SHA256:...`). Choose ACCEPT (knob 2 then click, or `y`) to remember it in the unit's flash (`/known_hosts`), or REJECT. To check it, run `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` on the Pi.
  - **Key changed:** if a computer's key ever changes, PIP LINK refuses to connect and shows a warning. If the computer was reinstalled, use FORGET KEY.
  - **Password:** asked every time, masked on the BLE keyboard. 3 tries. It is never stored or logged. Password and keyboard-interactive logins both work; SSH keys are a later step.
- **In a session:**
  - Knob 1 scrolls back through the last 500 lines that went off the top: turn up for older text, down to return. A click jumps back to the live screen. The view holds still while new text arrives, and typing on the keyboard returns to live.
  - A click opens the footer menu (knob 2 moves the highlight):
    - `TYPE`: on-screen keyboard; what you type is sent followed by Enter.
    - `CTRL-C`, `ESC`, `TAB`.
    - `SIZE`: cycles LARGE / MEDIUM / SMALL; click again to keep cycling. It's remembered (NVS `term_size`), and the computer is told the new size (Telnet NAWS / SSH pty).
    - `WHEEL`: switches knob 1 between scrollback and sending Up/Down keys (for menus or `nano` without a keyboard). Remembered (NVS `term_wheel`).
  - With a keyboard, while scrolled back: arrows and PgUp/PgDn move, Esc returns to live.
    - `DISCONNECT`.
  - Knob 2 changes tabs; the session keeps running in the background.
  - With a BLE keyboard every key goes to the computer, including Ctrl-letter, Alt, the arrows, Home/End, PgUp/PgDn and Del. That means the keyboard's Tab/Left/Right no longer change tabs here; use knob 2.
- The on-screen keyboard now has an `abc`/`ABC` key for lower case.

## Raspberry Pi setup
SSH: Raspberry Pi OS has it, but it may be off. Turn it on with `sudo raspi-config` → Interface Options → SSH, or `sudo systemctl enable --now ssh`.

Telnet (testing only) isn't encrypted, so only use it on your own network:
```
sudo apt update
sudo apt install telnetd
hostname -I        # the Pi's address
```
On current Raspberry Pi OS the `telnetd` package installs `inetd` along with it, and the service starts by itself. Remove it again with `sudo apt remove telnetd openbsd-inetd`.

## How it works
- `TelnetCodec` (lib/remote_term):
  - Answers DO TTYPE with `VT100`, DO NAWS with 58×12, WILL ECHO/SGA with DO, and refuses everything else.
  - Sends Enter as CR NUL and doubles 0xFF.
- `VtScreen`:
  - Handles cursor movement, erase, insert/delete line and character, scroll regions, the alternate screen (`?1049`), DECCKM application cursor keys, DSR/DA replies, UTF-8 (box drawing and quotes become ASCII look-alikes) and DEC line drawing (`ESC ( 0`).
  - SGR (colour/bold/inverse) is parsed and ignored.
  - Bytes that aren't valid UTF-8, or that decode to characters the Pip-Boy can't show, are read as IBM PC code page 437 (BBS box drawing → `+ - | =`, shading → `#`, accents → plain letters).
  - BBSes often assume an 80×24 screen and ignore the 58×12 size PIP LINK reports, so wide menus wrap.
- The PIP LINK unit keeps the screen and redraws changed rows at most about 12 times a second. That way a flood of output (`dmesg`, `cat`) can't swamp the Pip-Boy.
- Holotape additions:
  - `T.cur(row, col)` draws an underline cursor (row -1 hides it).
  - `T.vt(size)` switches the grid area to the TERM canvas (its own font and grid; -1 = off) and replies `<<T VT cols rows>>`.
  - `T.vl(row, b64)` fills one canvas line.
- The installed holotape is minified (`make_update.py`, rjsmin): 27 KB down to 16 KB. Reading the file at launch needs that much RAM at once. Reloading the holotape on top of a running copy runs out of memory (`LOW_MEMORY` in LOGS/LOG.TXT), so it's always launched from the menu.
- Connecting runs in its own FreeRTOS task, so the mesh link and the UI keep running while DNS/mDNS and the TCP connect wait.
- Unit tests on the PC: `pip install ziglang`, then `python tools/vt_test.py`.
- SSH:
  - `lib/ssh_link` wraps LibSSH-ESP32 5.9.0 (libssh, LGPL-2.1) in a FreeRTOS task with a 32 KB internal-RAM stack. libssh's large allocations land in PSRAM automatically (`CONFIG_SPIRAM_USE_MALLOC`, over 4 KB).
  - The task asks the UI two questions through its state (ASK_HOSTKEY, ASK_PASSWORD) and moves data through FreeRTOS stream buffers (8 KB in, 1 KB out).
  - It asks for a `vt100` pty the size of the grid.
  - The log reports the task's unused stack when a session ends, and free heap/PSRAM when the terminal starts.
