#!/usr/bin/env python3
"""synth.py - tiny self-contained Mode-S test-stream generator (2.0 or 2.4 Msps cu8).

Not a channel model (use more1090-simulator for that): ideal sample
alignment, rectangular pulses, AWGN. It exists so `make check` can
exercise the decoder's framing/stream logic with no external tools.

usage: synth.py out.cu8 [--seconds S] [--frames N] [--seed K] [--rate 2.0|2.4]
At 2.4 Msps frames are rendered on a 12 Msps grid and box-averaged by 5, so
they start at every multiple of 1/5 sample.
Writes out.cu8 and out.truth (one line per frame: sample_index hex).
"""
import argparse, sys
try:
    import numpy as np
except ImportError:
    sys.exit("error: numpy missing; install: pip install --break-system-packages numpy")

POLY = 0xFFF409
def crc24(data):
    crc = 0
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000: crc ^= POLY
        crc &= 0xFFFFFF
    return crc

def frame_samples(msg):
    """480-sample on/off envelope at 2 Msps: 16 preamble + 224 payload... (240 total)."""
    env = np.zeros(240, np.float32)
    env[[0, 2, 7, 9]] = 1.0
    for i in range(112):
        bit = (msg[i // 8] >> (7 - (i % 8))) & 1
        env[16 + 2 * i + (0 if bit else 1)] = 1.0
    return env

def frame_samples_24(msg, sub):
    """Envelope at 2.4 Msps for a frame starting 'sub'/5 of a sample late (290 samples)."""
    hi = np.zeros(5 * 290, np.float32)           # 12 Msps grid: 1 us = 12, a chip = 6
    def pulse(us12):
        hi[sub + us12: sub + us12 + 6] = 1.0
    for p in (0, 12, 42, 54):
        pulse(p)
    for i in range(112):
        bit = (msg[i // 8] >> (7 - (i % 8))) & 1
        pulse(96 + 12 * i + (0 if bit else 6))
    return hi.reshape(290, 5).mean(axis=1)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--frames", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=1090)
    ap.add_argument("--sigma", type=float, default=4.0, help="noise sigma per rail, LSB")
    ap.add_argument("--rate", choices=("2.0", "2.4"), default="2.0")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    r24 = a.rate == "2.4"
    span = 290 if r24 else 240
    n = int(a.seconds * (2_400_000 if r24 else 2_000_000))
    i = rng.normal(0, a.sigma, n).astype(np.float32)
    q = rng.normal(0, a.sigma, n).astype(np.float32)
    starts = np.sort(rng.choice(np.arange(10, n - 300), size=a.frames, replace=False))
    truth = []
    last_end = -1
    for s in starts:
        if s < last_end + 20:      # no overlapping frames in this logic test
            continue
        body = bytes([0x8D]) + bytes(rng.integers(0, 256, 10, dtype=np.uint8))
        c = crc24(body)
        msg = body + bytes([(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF])
        amp = rng.uniform(12, 90)  # LSB; spans marginal to strong
        ph = rng.uniform(0, 2 * np.pi)
        env = (frame_samples_24(msg, int(rng.integers(0, 5))) if r24 else frame_samples(msg)) * amp
        i[s:s + span] += env * np.cos(ph)
        q[s:s + span] += env * np.sin(ph)
        truth.append((int(s), msg.hex().upper()))
        last_end = s + span
    iq = np.empty(2 * n, np.float32)
    iq[0::2] = i; iq[1::2] = q
    np.clip(np.round(iq) + 127, 0, 255).astype(np.uint8).tofile(a.out)
    with open(a.out.rsplit(".", 1)[0] + ".truth", "w") as f:
        for s, hx in truth:
            f.write(f"{s} {hx}\n")
    print(f"wrote {a.out}: {n} samples, {len(truth)} frames")

if __name__ == "__main__":
    main()
