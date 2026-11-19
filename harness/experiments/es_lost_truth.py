#!/usr/bin/env python3
"""es_lost_truth.py - the decoder's squitter error rate against the truth.

The 2.4 Msps demodulator counts extended squitters it decoded (es_ok) and
ones that were clearly there and could not be read (es_lost); the firmware
shows es_lost / (es_ok + es_lost) as the error rate. On the simulated
sets every transmitted frame is known, so the real loss can be put next to
it: how many of the squitters sent were not decoded.

usage: es_lost_truth.py DATADIR      (DATADIR from prep_2400.py and prep_dense.py)
needs: `make` in harness/ first
"""
import bisect, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from scenario_score import load_truth
BIN = os.path.join(HERE, "..", "host_mode_s")
SETS = ("strong", "mid", "weak", "busy", "dense")
DEFAULT = ["--soft", "8", "--strict", "1", "--known-icao", "1", "--coh", "1"]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    # Everything this run needs, checked before the first decode.
    if not os.access(BIN, os.X_OK):
        sys.exit(f"error: {BIN} not built; run `make` in {os.path.dirname(BIN)} first")
    missing = [n + e for n in SETS for e in (".cu8", ".sigmf-meta") if not os.path.exists(os.path.join(d, n + e))]
    if missing:
        sys.exit(f"error: missing in {d}: {', '.join(missing)}  (make them with prep_2400.py and prep_dense.py)")
    print("Extended squitters, the firmware's default decoder at 2.4 Msps\n")
    print("set       sent  decoded     lost   true loss | es_ok  es_lost  shown as error rate")
    for name in SETS:
        truth = load_truth(os.path.join(d, name + ".sigmf-meta"), 0.3)
        es = [t for t in truth if len(t[1]) == 28 and int(t[1][:2], 16) >> 3 in (17, 18)]
        r = subprocess.run([BIN, os.path.join(d, name + ".cu8"), "--rate", "2.4", "--stream", "1"] + DEFAULT,
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"error: host_mode_s failed on {name}: {r.stderr.strip()[-300:]}")
        st = dict(kv.split("=") for l in r.stderr.splitlines() if l.startswith("STATS")
                  for kv in l.split() if "=" in kv)
        out = sorted((int(m["samp"]), m["hex"]) for m in
                     (dict(kv.split("=", 1) for kv in l[4:].split()) for l in r.stdout.splitlines() if l.startswith("MSG ")))
        pos = [o[0] for o in out]
        dec = 0
        for start, hx, _ in es:
            lo, hi = bisect.bisect_left(pos, start - 600), bisect.bisect_right(pos, start + 600)
            dec += any(out[i][1] == hx for i in range(lo, hi))
        eo, el = int(st["es_ok"]), int(st["es_lost"])
        print(f"{name:8s} {len(es):5d} {dec:8d} {len(es) - dec:8d} {100 * (len(es) - dec) / len(es):9.1f}% | "
              f"{eo:5d} {el:8d} {100 * el / max(eo + el, 1):12.1f}%")


if __name__ == "__main__":
    main()
