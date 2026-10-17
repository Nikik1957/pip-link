"""Geiger-counter crackle WAVs for the PIP LINK radiation alarm (16 kHz mono 16-bit, Pip-Boy format).

    python gen_geiger.py ../pipboy/TERM

Writes GEIGER1.WAV (a little over the alarm level), GEIGER2.WAV (well over) and GEIGER3.WAV (way over):
random (Poisson) clicks, each a sharp noise burst plus a short ring, like a classic tube counter.
"""
import math
import os
import random
import struct
import sys
import wave

RATE = 16000
SECONDS = 1.5
LEVELS = {"GEIGER1.WAV": 9, "GEIGER2.WAV": 28, "GEIGER3.WAV": 80}     # clicks per second


def click(rng):
    n = int(RATE * 0.004)                      # 4 ms
    f = rng.uniform(2600, 3600)                # the "tick" ring of the counter's speaker
    amp = rng.uniform(0.55, 1.0)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * 1400)
        noise = rng.uniform(-1, 1) * math.exp(-t * 3000)
        ring = math.sin(2 * math.pi * f * t) * env
        out.append(amp * (0.65 * noise + 0.5 * ring))
    return out


def make(path, cps, seed):
    rng = random.Random(seed)
    buf = [0.0] * int(RATE * SECONDS)
    t = rng.expovariate(cps)
    while t < SECONDS - 0.005:
        start = int(t * RATE)
        for i, v in enumerate(click(rng)):
            if start + i < len(buf):
                buf[start + i] += v
        t += max(0.0012, rng.expovariate(cps))  # dead time: a tube can't click twice that fast
    peak = max(1e-9, max(abs(v) for v in buf))
    gain = 0.85 / peak
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v * gain)) * 32767)) for v in buf))
    print(f"wrote {path}: {cps}/s, {os.path.getsize(path)} bytes")


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    for i, (name, cps) in enumerate(LEVELS.items()):
        make(os.path.join(out, name), cps, 1234 + i)
