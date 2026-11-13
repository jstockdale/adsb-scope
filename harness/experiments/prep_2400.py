#!/usr/bin/env python3
"""prep_2400.py - build the fixed 2.4 Msps test set used by eval_2400.py.

Creates, in the given directory:
  strong.cu8 / mid.cu8 / weak.cu8 (+ .sigmf-meta ground truth)  simulated, 10 s each
  noise.cu8                                                       20 s of pure noise
  madrid_lo.cu8 / madrid_hi.cu8   the Madrid capture at two gains (if --madrid given)

usage: prep_2400.py OUTDIR [--madrid path/to/madrid.sigmf-meta]
needs: MORE1090_SIM (more1090-simulator)
"""
import argparse, os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..")); sys.path.insert(0, HERE)
from dsp import np
from rate_24 import to_rate, SIM, need

SCEN = [("strong", 10, 40, 100.0, 2000.0, 11),
        ("mid", 40, 120, 88.0, 8000.0, 12),
        ("weak", 120, 300, 78.5, 8000.0, 13)]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out"); ap.add_argument("--madrid")
    a = ap.parse_args()
    need(SIM, "more1090-simulator", "build more1090 v0.2.0 and set MORE1090_SIM")
    os.makedirs(a.out, exist_ok=True)
    for name, dmin, dmax, cn0, k, seed in SCEN:
        cu8 = os.path.join(a.out, name + ".cu8")
        if os.path.exists(cu8):
            continue
        meta = os.path.join(a.out, name + ".sigmf-meta")
        data = os.path.join(a.out, name + ".sigmf-data")
        subprocess.run([SIM, "scenario", "--duration", "10", "--messages-per-second", "300",
                        "--max-cn0", str(cn0), "--min-distance", str(dmin), "--max-distance", str(dmax),
                        "--seed", str(seed), meta], check=True, capture_output=True)
        to_rate(data, "cf32_le", "2.4", k, cu8)
        os.unlink(data)
        print("made", cu8, flush=True)
    noise = os.path.join(a.out, "noise.cu8")
    if not os.path.exists(noise):
        rng = np.random.default_rng(42)
        with open(noise, "wb") as f:
            for _ in range(12):
                np.clip(np.round(rng.normal(0, 7.8, 8_000_000)) + 127, 0, 255).astype(np.uint8).tofile(f)
        print("made", noise, flush=True)
    if a.madrid:
        import json
        data = a.madrid[:-len(".sigmf-meta")] + ".sigmf-data"
        dt = json.load(open(a.madrid))["global"]["core:datatype"]
        for tag, k in (("lo", 145.9), ("hi", 452.9)):
            cu8 = os.path.join(a.out, f"madrid_{tag}.cu8")
            if not os.path.exists(cu8):
                to_rate(data, dt, "2.4", k, cu8)
                print("made", cu8, flush=True)

if __name__ == "__main__":
    main()
