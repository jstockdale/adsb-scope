#!/usr/bin/env python3
"""prep_dense.py - add two heavy-traffic scenarios to the 2.4 Msps test set.

Creates, in the directory prep_2400.py filled:
  busy.cu8    1,000 messages per second, 5 s  (+ .sigmf-meta ground truth)
  dense.cu8   2,000 messages per second, 5 s

Transmitters at 10-300 km with CN0 up to 100 dB.Hz, so frames of every
strength overlap one another - the case the coherent second look
(mode_s_coh.c) is for. The three prep_2400.py scenarios carry 300 messages
per second and say little about overlap.

usage: prep_dense.py DATADIR
needs: MORE1090_SIM (more1090-simulator), numpy, scipy
"""
import argparse, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..")); sys.path.insert(0, HERE)
try:
    from dsp import np  # noqa: F401  (dsp.py checks numpy / scipy itself)
    from rate_24 import to_rate, SIM, need
except ImportError as e:
    sys.exit(f"error: {e}\n  install: pip install numpy scipy")

#       name     msg/s  seed
SCEN = [("busy", 1000, 22),
        ("dense", 2000, 21)]
SECONDS, MAX_CN0, DMIN, DMAX, K = 5, 100.0, 10, 300, 2000.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    a = ap.parse_args()
    need(SIM, "more1090-simulator", "build more1090 v0.2.0 and set MORE1090_SIM")
    os.makedirs(a.out, exist_ok=True)
    for name, mps, seed in SCEN:
        cu8 = os.path.join(a.out, name + ".cu8")
        if os.path.exists(cu8):
            continue
        meta = os.path.join(a.out, name + ".sigmf-meta")
        data = os.path.join(a.out, name + ".sigmf-data")
        subprocess.run([SIM, "scenario", "--duration", str(SECONDS), "--messages-per-second", str(mps),
                        "--max-cn0", str(MAX_CN0), "--min-distance", str(DMIN), "--max-distance", str(DMAX),
                        "--seed", str(seed), meta], check=True, capture_output=True)
        _, clipped = to_rate(data, "cf32_le", "2.4", K, cu8)
        os.unlink(data)
        print(f"made {cu8}  ({clipped:.3f} % of samples clipped)", flush=True)


if __name__ == "__main__":
    main()
