#!/usr/bin/env python3
"""collide.py - what does the 2.4 Msps demodulator do with two overlapping frames?

Generates pairs of DF17 frames in complex baseband (each with its own
amplitude, carrier offset and phase), overlapped by a random time offset,
renders them at 12 Msps, box-averages to 2.4 Msps, adds noise, quantises to
cu8, runs the host build of the firmware decoder in four configurations
(with and without the coherent second look, mode_s_coh.c) and scores which
of the two frames came out.

Idealised: rectangular pulses, no multipath, a box filter for the front
end, exactly two frames per collision, carrier offsets drawn from a normal
distribution of 100 kHz sigma. It measures the decoder's logic, not a
channel.

usage: collide.py [--binary path/to/host_mode_s] [--pairs N] [--seed K]
                  [--tau-us LO,HI] [--out results.txt]
  --tau-us 0,100     (default) the second frame starts 0-100 us after the
                     first: at least 20 us of the two frames overlap
  --tau-us 100,120   only the tail of the first frame is overlapped
  --stress N         instead of the tables: N pairs with the stronger frame
                     0-10 dB up, to count frames that match neither input
                     (false frames) with enough trials to see any
needs: numpy; `make` in harness/ first
"""
import argparse, os, subprocess, sys, tempfile
try:
    import numpy as np
except ImportError:
    sys.exit("error: numpy missing; install: pip install numpy")
HERE = os.path.dirname(os.path.abspath(__file__))

POLY = 0xFFF409
FS12 = 12_000_000


def crc24(data):
    crc = 0
    for b in data:
        crc ^= b << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= POLY
        crc &= 0xFFFFFF
    return crc


def make_frame(rng, addr=None):
    body = bytes([0x8D]) + bytes(rng.integers(0, 256, 10, dtype=np.uint8))
    if addr is not None:
        body = body[:1] + addr + body[4:]
    c = crc24(body)
    return body + bytes([(c >> 16) & 255, (c >> 8) & 255, c & 255])


def envelope12(msg):
    """On/off envelope at 12 Msps: 8 us preamble + 112 bits = 1440 samples."""
    env = np.zeros(1440, np.float32)
    for p in (0, 12, 42, 54):
        env[p:p + 6] = 1.0
    for i in range(112):
        bit = (msg[i // 8] >> (7 - (i % 8))) & 1
        s = 96 + 12 * i + (0 if bit else 6)
        env[s:s + 6] = 1.0
    return env


def build(rng, pairs, ratio_db, isolated, a_weak=10.0, sigma=1.0, tau_lo=0, tau_hi=1200, with_ticks=False,
          pool=None):
    """Returns (cu8 bytes, truth list, clipped samples). Each trial has a 720 us
    window. pool: a list of 3-byte addresses to draw from (default: a new
    random address for every frame)."""
    W = 8640                                    # 12 Msps samples per trial
    y = np.zeros(pairs * W, np.complex64)
    a_strong = a_weak * 10 ** (ratio_db / 20)
    truth = []
    n = np.arange(1440)
    for t in range(pairs):
        base = t * W
        s_first = 1800 + int(rng.integers(0, 60))           # 150 us + sub-sample spread
        if isolated:
            tau = 3600                                         # 300 us apart: no overlap
        else:
            tau = int(rng.integers(tau_lo, tau_hi))            # default 0..100 us: at least 20 us of overlap
        strong_first = bool(rng.integers(0, 2))
        starts = (s_first, s_first + tau)
        frames = []
        for k in range(2):
            is_strong = (k == 0) == strong_first
            msg = make_frame(rng, pool[int(rng.integers(0, len(pool)))] if pool else None)
            f = float(np.clip(rng.normal(0, 100e3), -400e3, 400e3))
            ph = rng.uniform(0, 2 * np.pi)
            amp = a_strong if is_strong else a_weak
            sig = amp * envelope12(msg) * np.exp(1j * (2 * np.pi * f * n / FS12 + ph))
            y[base + starts[k]: base + starts[k] + 1440] += sig.astype(np.complex64)
            frames.append((is_strong, msg.hex().upper(), base + starts[k]))
        strong_hex, strong_tick = [(h, tk) for s, h, tk in frames if s][0]
        weak_hex, weak_tick = [(h, tk) for s, h, tk in frames if not s][0]
        if with_ticks:
            truth.append((strong_hex, weak_hex, strong_first, tau / 12.0, strong_tick, weak_tick))
        else:
            truth.append((strong_hex, weak_hex, strong_first, tau / 12.0))
    data, clipped = quantise(y, rng, sigma)
    return data, truth, clipped


def quantise(y, rng, sigma):
    """12 Msps complex -> 2.4 Msps cu8 with noise. Returns (bytes, clipped samples)."""
    y24 = y.reshape(-1, 5).mean(axis=1)
    y24 = y24 + (rng.normal(0, sigma, len(y24)) + 1j * rng.normal(0, sigma, len(y24))).astype(np.complex64)
    iq = np.empty(2 * len(y24), np.float32)
    iq[0::2] = y24.real
    iq[1::2] = y24.imag
    v = np.round(iq) + 127.0
    clipped = int(((v < 0) | (v > 255)).sum())
    return np.clip(v, 0, 255).astype(np.uint8).tobytes(), clipped


def warmup(rng, pool, amp=30.0, sigma=1.0):
    """One clean, isolated frame from every address in the pool, so that a
    known-address gate is open before the collisions start - as it is on
    the air, where most frames of an aircraft arrive undamaged."""
    W = 8640
    y = np.zeros(len(pool) * W, np.complex64)
    n = np.arange(1440)
    sent = []
    for t, addr in enumerate(pool):
        msg = make_frame(rng, addr)
        f = float(np.clip(rng.normal(0, 100e3), -400e3, 400e3))
        ph = rng.uniform(0, 2 * np.pi)
        s = t * W + 1800 + int(rng.integers(0, 60))
        y[s:s + 1440] += (amp * envelope12(msg) * np.exp(1j * (2 * np.pi * f * n / FS12 + ph))).astype(np.complex64)
        sent.append(msg.hex().upper())
    return quantise(y, rng, sigma)[0], sent


def decode(binary, path, extra):
    out = subprocess.run([binary, path, "--rate", "2.4", "--stream", "1"] + extra,
                         capture_output=True, text=True, check=True).stdout
    got = set()
    for line in out.splitlines():
        if line.startswith("MSG") and "crcok=1" in line:
            got.add(line.rsplit("hex=", 1)[1].strip())
    return got


VARIANTS = [
    ("default image (1-bit fix)", []),
    ("default image + coherent second look", ["--coh", "1"]),
    ("soft rescue + 2-bit fix", ["--soft", "8", "--fix2", "1", "--strict", "1"]),
    ("soft rescue + 2-bit fix + coherent second look", ["--soft", "8", "--fix2", "1", "--strict", "1", "--coh", "1"]),
]


FIX2 = ["--soft", "8", "--fix2", "1", "--strict", "1", "--known-icao", "1"]
GATED = ["--soft", "8", "--strict", "1", "--known-icao", "1"]      # the fix2 image without its 2-bit fix
STRESS_VARIANTS = [
    ("default image", []),
    ("default image + coherent", ["--coh", "1"]),
    ("soft rescue, no address gate", ["--soft", "8"]),
    ("soft rescue + coherent, no gate", ["--soft", "8", "--coh", "1"]),
    ("fix2 image", FIX2),
    ("fix2 image + coherent", FIX2 + ["--coh", "1"]),
    ("fix2 image minus 2-bit fix", GATED),
    ("... + coherent", GATED + ["--coh", "1"]),
]
STRESS_AIRCRAFT = 40    # addresses per batch, so the known-address gate has something to know


def stress(a, tau_lo, tau_hi, say):
    """Many collisions, to put a number on false frames. Every batch of 2000
    pairs draws its frames from 40 aircraft addresses and opens with one
    clean frame from each, so the known-address gate of the fix2 image works
    as it does on the air. A frame counts as false when it matches no frame
    sent in its batch; 'real address' counts those carrying the address of
    one of the batch's aircraft - a wrong message attributed to a real
    aircraft, which no address gate can stop."""
    batch = 2000
    batches = max(1, a.stress // batch)
    tot = {name: [0, 0, 0] for name, _ in STRESS_VARIANTS}
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "x.cu8")
        for b in range(batches):
            rng = np.random.default_rng(a.seed * 100003 + b)
            pool = [bytes(rng.integers(0, 256, 3, dtype=np.uint8)) for _ in range(STRESS_AIRCRAFT)]
            data, truth, _ = build(rng, batch, float(rng.uniform(0, 10)), False, tau_lo=tau_lo, tau_hi=tau_hi,
                                   pool=pool)
            lead, lead_sent = warmup(rng, pool)
            with open(path, "wb") as f:
                f.write(lead)
                f.write(data)
            sent = {h for t in truth for h in t[:2]} | set(lead_sent)
            addrs = {h[2:8] for h in sent}
            for name, extra in STRESS_VARIANTS:
                got = decode(a.binary, path, extra)
                bad = got - sent
                tot[name][0] += len((got & sent) - set(lead_sent))
                tot[name][1] += len(bad)
                tot[name][2] += sum(h[2:8] in addrs for h in bad)
            if (b + 1) % 10 == 0:
                print(f"  ... {(b + 1) * batch} pairs", file=sys.stderr, flush=True)
    n = batches * batch
    say(f"{n} pairs of overlapping DF17 frames, start offset {a.tau_us} us, stronger frame 0-10 dB up "
        f"(one gap per batch of {batch}),")
    say(f"{STRESS_AIRCRAFT} aircraft addresses per batch, each heard once cleanly first; seed {a.seed}")
    say("false = decoded frames matching nothing that was sent; real address = of those, frames carrying an aircraft's address")
    say()
    say(f"{'configuration':<34}{'frames decoded':>15}{'false':>7}{'per million decoded':>21}{'real address':>14}")
    for name, _ in STRESS_VARIANTS:
        ok, bad, known = tot[name]
        say(f"{name:<34}{ok:>15}{bad:>7}{1e6 * bad / max(ok, 1):>21.0f}{known:>14}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=os.path.join(HERE, "..", "host_mode_s"))
    ap.add_argument("--pairs", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="")
    ap.add_argument("--tau-us", default="0,100", help="range of the start offset between the two frames, microseconds")
    ap.add_argument("--stress", type=int, default=0, help="count false frames over this many pairs instead")
    a = ap.parse_args()
    if not os.access(a.binary, os.X_OK):
        sys.exit(f"error: decoder binary not found or not executable: {a.binary}\n  run `make` in harness/ first, or pass --binary")
    try:
        tau_lo, tau_hi = [int(round(float(v) * 12)) for v in a.tau_us.split(',')]
    except ValueError:
        sys.exit("error: --tau-us wants two numbers, e.g. 0,100")
    if not 0 <= tau_lo < tau_hi <= 1440:
        sys.exit("error: --tau-us must satisfy 0 <= LO < HI <= 120 (a frame is 120 us long)")
    lines = []

    def say(s=""):
        print(s, flush=True)
        lines.append(s)

    if a.stress:
        stress(a, tau_lo, tau_hi, say)
        if a.out:
            with open(a.out, "w") as f:
                f.write("\n".join(lines) + "\n")
        return
    say(f"Two overlapping DF17 frames, start offset {a.tau_us} us, {a.pairs} pairs per row, weak frame at amplitude 10 LSB, noise 1 LSB per rail")
    say("pct of frames decoded: S = stronger, W = weaker, both = both from one pair, false = decoded frames matching neither")
    say()
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "x.cu8")
        for name, extra in VARIANTS:
            say(f"[{name}]")
            say(f"{'case':<26}{'S':>7}{'W':>7}{'both':>7}{'false':>7}   {'W if S first':>13}{'W if W first':>13}")
            for label, ratio, iso in [("no overlap (control)", 6.0, True)] + [(f"overlap, S {r:g} dB up", float(r), False) for r in (0, 3, 6, 10, 15, 20)]:
                rng = np.random.default_rng(a.seed * 1000 + int(ratio * 10) + (500 if iso else 0))
                data, truth, clipped = build(rng, a.pairs, ratio, iso, tau_lo=tau_lo, tau_hi=tau_hi)
                with open(path, "wb") as f:
                    f.write(data)
                got = decode(a.binary, path, extra)
                all_truth = set()
                s = w = both = 0
                w_sf = n_sf = w_wf = n_wf = 0
                for sh, wh, sfirst, tau in truth:
                    all_truth.add(sh)
                    all_truth.add(wh)
                    ds, dw = sh in got, wh in got
                    s += ds
                    w += dw
                    both += ds and dw
                    if sfirst:
                        n_sf += 1
                        w_sf += dw
                    else:
                        n_wf += 1
                        w_wf += dw
                false = len(got - all_truth)
                n = len(truth)
                say(f"{label:<26}{100*s/n:6.1f}%{100*w/n:6.1f}%{100*both/n:6.1f}%{false:>7}   {100*w_sf/max(n_sf,1):12.1f}%{100*w_wf/max(n_wf,1):12.1f}%" + (f"   ({clipped} clipped)" if clipped else ""))
            say()
    if a.out:
        with open(a.out, "w") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
