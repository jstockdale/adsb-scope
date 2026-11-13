#!/usr/bin/env python3
"""eval_2400.py - score the 2.4 Msps demodulator on the fixed test set made
by prep_2400.py, next to dump1090-fa's 2.4 Msps demodulator.

usage: eval_2400.py DATADIR [--args "<host_mode_s options>"] [--label NAME] [--ref]
  --ref   also (re)run dump1090-fa (DUMP1090) and cache its results in DATADIR
Prints one line per configuration: decode % for strong / mid / weak, false
frames in each, Madrid frame counts at two gains, false frames in 20 s of
noise, candidates per second on noise, and wall time for the whole set.
"""
import argparse, json, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..")); sys.path.insert(0, HERE)
from scenario_score import load_truth

BIN = os.path.join(HERE, "..", "host_mode_s")
D1090 = os.environ.get("DUMP1090", "/tmp/dump1090/dump1090")
SETS = ("strong", "mid", "weak")

def es(hexes):
    return [h for h in hexes if len(h) == 28 and (int(h[:2], 16) >> 3) in (17, 18)]

def ours(cu8, args):
    r = subprocess.run([BIN, cu8, "--rate", "2.4", "--stream", "1"] + args, capture_output=True, text=True)
    hx = [l.rsplit("hex=", 1)[1].strip() for l in r.stdout.splitlines() if l.startswith("MSG ")]
    st = dict(kv.split("=") for kv in r.stderr.replace("STATS ", "").split() if "=" in kv)
    return es(hx), st

def ref(cu8):
    r = subprocess.run([D1090, "--ifile", cu8, "--iformat", "UC8", "--raw", "--fix"], capture_output=True, text=True)
    return es([l.strip()[1:-1].upper() for l in r.stdout.splitlines() if l.startswith("*")])

def truth(d, name):
    t = load_truth(os.path.join(d, name + ".sigmf-meta"), 0.3)
    allhex = {x[1] for x in t}
    return {h for h in allhex if len(h) == 28 and (int(h[:2], 16) >> 3) in (17, 18)}, allhex

def row(label, d, fn):
    t0 = time.perf_counter()
    cells = []
    for name in SETS:
        if not os.path.exists(os.path.join(d, name + ".cu8")):
            cells.append("   -      "); continue
        tr, allhex = truth(d, name)
        hx = set(fn(os.path.join(d, name + ".cu8"))[0])
        cells.append(f"{100.0 * len(tr & hx) / len(tr):5.1f}%/{len(hx - allhex):<3d}")
    for name in ("madrid_lo", "madrid_hi"):
        p = os.path.join(d, name + ".cu8")
        cells.append(f"{len(fn(p)[0]):5d}" if os.path.exists(p) else "    -")
    p = os.path.join(d, "noise.cu8")
    if os.path.exists(p):
        hx, st = fn(p)
        cand = int(st.get("preambles", 0)) / 20.0 if st else float("nan")
        cells.append(f"{len(hx):3d}  {cand:7.0f}/s")
    else:
        cells.append("  -")
    print(f"{label:34s} " + "  ".join(cells) + f"   {time.perf_counter() - t0:5.1f}s", flush=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir"); ap.add_argument("--args", default=""); ap.add_argument("--label")
    ap.add_argument("--ref", action="store_true")
    a = ap.parse_args()
    print(f"{'':34s} strong      mid         weak        madLo  madHi  noise-false  candidates   time")
    if a.ref:
        if not os.path.exists(D1090):
            sys.exit(f"error: dump1090-fa not found at {D1090}; set DUMP1090")
        row("dump1090-fa 2.4 (reference)", a.dir, lambda p: (ref(p), None))
    args = a.args.split()
    row(a.label or ("ours 2.4 " + a.args), a.dir, lambda p: ours(p, args))

if __name__ == "__main__":
    main()
