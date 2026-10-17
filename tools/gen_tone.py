#!/usr/bin/env python3
"""
gen_tone.py - make short tone sequences as WAVs the Pip-Boy can play
(16-bit mono 16 kHz, what Pip.audioStart expects).

  python gen_tone.py out.wav 1046:80 gap:30 1568:120      # freq_hz:ms or gap:ms
Options: --amp 0.5 (0..1)   --fade 5 (ms, avoids clicks)
"""
import argparse, math, struct, wave

RATE = 16000


def tone(freq, ms, amp, fade_ms):
    n = int(RATE * ms / 1000)
    fade = max(1, int(RATE * fade_ms / 1000))
    out = []
    for i in range(n):
        env = 1.0
        if i < fade:
            env = 0.5 - 0.5 * math.cos(math.pi * i / fade)
        elif i >= n - fade:
            env = 0.5 - 0.5 * math.cos(math.pi * (n - 1 - i) / fade)
        out.append(int(32767 * amp * env * math.sin(2 * math.pi * freq * i / RATE)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("parts", nargs="+", help="freq:ms or gap:ms")
    ap.add_argument("--amp", type=float, default=0.5)
    ap.add_argument("--fade", type=float, default=5)
    a = ap.parse_args()
    samples = []
    for p in a.parts:
        what, ms = p.split(":")
        if what == "gap":
            samples += [0] * int(RATE * float(ms) / 1000)
        else:
            samples += tone(float(what), float(ms), a.amp, a.fade)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))
    print("wrote %s: %d ms, %d bytes" % (a.out, len(samples) * 1000 // RATE, 44 + 2 * len(samples)))


if __name__ == "__main__":
    main()
