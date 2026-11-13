#!/usr/bin/env python3
"""toa_error.py - how well does the 2.4 Msps demodulator time a frame?

For each simulated set made by prep_2400.py, compares the demodulator's
preamble time (12 MHz ticks, 83.3 ns each) with the simulator's true
arrival time, which is recorded to a fraction of a sample. A constant
offset is a convention and is removed (the median); what matters for
timestamping is the spread. dump1090-fa's own 12 MHz timestamps are
analysed the same way for reference when DUMP1090 is set.

usage: toa_error.py DATADIR [--args "<host_mode_s options>"]
"""
import argparse, json, os, statistics, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "host_mode_s")
D1090 = os.environ.get("DUMP1090", "")
TICK_NS = 1e9 / 12e6

def truth_ticks(meta):
    out = {}
    for a in json.load(open(meta))["annotations"]:
        hx = a.get("more1090:mode_s_hex")
        if hx and len(hx) == 28:
            t8 = a["core:sample_start"] + a.get("more1090:sample_start_fractional", 0.0)
            out[hx.upper()] = (t8 * 1.5, a.get("more1090:cn0"))   # 8 Msps sample = 1.5 ticks
    return out

def ours(cu8, args):
    r = subprocess.run([BIN, cu8, "--rate", "2.4", "--stream", "1"] + args, capture_output=True, text=True)
    out = []
    for l in r.stdout.splitlines():
        if l.startswith("MSG "):
            d = dict(kv.split("=", 1) for kv in l[4:].split())
            out.append((d["hex"], int(d["tick"])))
    return out

def ref(cu8):
    r = subprocess.run([D1090, "--ifile", cu8, "--iformat", "UC8", "--mlat", "--fix"], capture_output=True, text=True)
    out = []
    for l in r.stdout.splitlines():
        l = l.strip()
        if l.startswith("@") and l.endswith(";") and len(l) > 14:
            out.append((l[13:-1].upper(), int(l[1:13], 16)))
    return out

def report(label, pairs, truth):
    err = [t - truth[h][0] for h, t in pairs if h in truth]
    if len(err) < 20:
        print(f"  {label:26s} too few matched frames ({len(err)})"); return
    med = statistics.median(err)
    dev = sorted(abs(e - med) for e in err)
    mad = statistics.median(dev) * 1.4826
    core = [e for e in err if abs(e - med) <= 3]          # ignore gross mis-timings for the spread
    sd = statistics.pstdev(core)
    out3 = 100.0 * (len(err) - len(core)) / len(err)
    print(f"  {label:26s} n={len(err):5d}  spread {sd * TICK_NS:5.1f} ns rms (robust {mad * TICK_NS:5.1f})  "
          f"95% within {dev[int(0.95 * len(dev))] * TICK_NS:6.1f} ns  off by >3 ticks: {out3:4.1f}%  "
          f"(constant offset {med:+.2f} ticks)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir"); ap.add_argument("--args", default="")
    a = ap.parse_args()
    if not os.path.exists(BIN):
        sys.exit("error: host_mode_s not built; run `make` in harness/")
    print(f"time-of-arrival error vs simulator truth; one 12 MHz tick = {TICK_NS:.1f} ns; "
          f"timing quantised to 1 tick has a floor of {TICK_NS / 12 ** 0.5:.1f} ns rms")
    for name in ("strong", "mid", "weak"):
        cu8 = os.path.join(a.dir, name + ".cu8"); meta = os.path.join(a.dir, name + ".sigmf-meta")
        if not (os.path.exists(cu8) and os.path.exists(meta)):
            continue
        truth = truth_ticks(meta)
        print(f"[{name}]")
        report("new 2.4 Msps demodulator", ours(cu8, a.args.split()), truth)
        if D1090 and os.path.exists(D1090):
            report("dump1090-fa (reference)", ref(cu8), truth)

if __name__ == "__main__":
    main()
