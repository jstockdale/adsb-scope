#!/usr/bin/env python3
"""eval_coh.py - what the coherent second look (mode_s_coh.c) adds, and how
often it runs.

Scores eight configurations of the 2.4 Msps demodulator on the fixed test set
(prep_2400.py: strong / mid / weak, Madrid at two gains, noise) plus the
two heavy-traffic scenarios of prep_dense.py (busy / dense).

usage: eval_coh.py DATADIR [--configs "label=options;label=options"]
Simulated sets: % of extended squitters decoded / frames matching no truth.
Madrid: extended-squitter frame count (no ground truth - see
xcheck_madrid.py). Noise: frames decoded from 20 s of noise. Last column:
re-slices per second, and frames they recovered per second.
"""
import argparse, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..")); sys.path.insert(0, HERE)
from scenario_score import load_truth

BIN = os.path.join(HERE, "..", "host_mode_s")
SIM = [("strong", 10), ("mid", 10), ("weak", 10), ("busy", 5), ("dense", 5)]   # name, seconds
REAL = [("madrid_lo", 20), ("madrid_hi", 20)]
NOISE = ("noise", 20)
FIX2 = ["--soft", "8", "--fix2", "1", "--strict", "1", "--known-icao", "1"]
GATED = ["--soft", "8", "--strict", "1", "--known-icao", "1"]
CONFIGS = [
    ("default image", []),
    ("default image + coherent", ["--coh", "1"]),
    ("soft rescue", ["--soft", "8"]),
    ("soft rescue + coherent", ["--soft", "8", "--coh", "1"]),
    ("fix2 image (soft, 2-bit, known addr)", FIX2),
    ("fix2 image + coherent", FIX2 + ["--coh", "1"]),
    ("fix2 image minus 2-bit fix", GATED),
    ("fix2 image minus 2-bit + coherent", GATED + ["--coh", "1"]),
]


def es(hexes):
    return [h for h in hexes if len(h) == 28 and (int(h[:2], 16) >> 3) in (17, 18)]


def run(d, name, args):
    r = subprocess.run([BIN, os.path.join(d, name + ".cu8"), "--rate", "2.4", "--stream", "1"] + args,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: host_mode_s failed on {name}: {r.stderr.strip()}")
    hx = [l.rsplit("hex=", 1)[1].strip() for l in r.stdout.splitlines() if l.startswith("MSG ")]
    st = dict(kv.split("=") for kv in r.stderr.replace("STATS ", "").split() if "=" in kv)
    return es(hx), st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--configs", default="", help='instead of the built-in eight: "label=options;label=options"')
    a = ap.parse_args()
    # Everything this run needs, checked before the first decode.
    if not os.access(BIN, os.X_OK):
        sys.exit(f"error: {BIN} not built; run `make` in {os.path.dirname(BIN)} first")
    missing = [n + ext for n, _ in SIM for ext in (".cu8", ".sigmf-meta")
               if not os.path.exists(os.path.join(a.dir, n + ext))]
    missing += [n + ".cu8" for n, _ in REAL + [NOISE] if not os.path.exists(os.path.join(a.dir, n + ".cu8"))]
    if missing:
        sys.exit(f"error: missing in {a.dir}: {', '.join(missing)}\n"
                 "  build them with experiments/prep_2400.py DATADIR --madrid <madrid.sigmf-meta>\n"
                 "  and experiments/prep_dense.py DATADIR")
    configs = CONFIGS
    if a.configs:
        configs = [(c.split("=", 1)[0], c.split("=", 1)[1].split()) for c in a.configs.split(";")]

    truth = {}
    for name, _ in SIM:
        t = load_truth(os.path.join(a.dir, name + ".sigmf-meta"), 0.3)
        allhex = {x[1] for x in t}
        truth[name] = ({h for h in allhex if len(h) == 28 and (int(h[:2], 16) >> 3) in (17, 18)}, allhex)

    print(f"{'configuration':<38}" + "".join(f"{n:>11}" for n, _ in SIM) + "".join(f"{n:>10}" for n, _ in REAL)
          + f"{'noise':>7}  re-slices/s (recovered/s): madrid_lo, dense, noise", flush=True)
    for label, args in configs:
        cells, rate = [], {}
        for name, secs in SIM:
            tr, allhex = truth[name]
            hx, st = run(a.dir, name, args)
            hs = set(hx)
            cells.append(f"{100.0 * len(tr & hs) / len(tr):6.1f}%/{len(hs - allhex):<3d}".rjust(11))
            rate[name] = (int(st.get("coh_try", 0)) / secs, int(st.get("coh_ok", 0)) / secs)
        for name, secs in REAL + [NOISE]:
            hx, st = run(a.dir, name, args)
            cells.append(f"{len(hx):>7d}" if name == "noise" else f"{len(hx):>10d}")
            rate[name] = (int(st.get("coh_try", 0)) / secs, int(st.get("coh_ok", 0)) / secs)
        tail = ", ".join(f"{rate[k][0]:.0f} ({rate[k][1]:.1f})" for k in ("madrid_lo", "dense", "noise"))
        print(f"{label:<38}" + "".join(cells) + "  " + tail, flush=True)


if __name__ == "__main__":
    main()
