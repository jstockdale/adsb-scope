#!/usr/bin/env python3
"""phase_diag.py - why are frames lost? Decode rate vs sub-sample timing.

For every DF17 truth frame of a simulated scenario, computes the frame's
arrival time modulo one output sample (its timing phase at the decoder's
sample rate) and reports, per phase bin:
  decode   - fraction decoded (CRC-ok, exact frame)
  cand     - fraction for which the detector produced any candidate at the
             right position (run the decoder with --check-crc 0)
  errs     - median bit errors of the best candidate vs the truth

usage: phase_diag.py scenario.sigmf-meta decoded_checkcrc0.txt [--rate 2.0] [--min-cn0 X]
"""
import argparse, bisect, json, statistics, sys
from scenario_score import RATE

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("meta"); ap.add_argument("decoded")
    ap.add_argument("--rate", choices=RATE, default="2.0")
    ap.add_argument("--min-cn0", type=float, default=0.0)
    ap.add_argument("--bins", type=int, default=8)
    a = ap.parse_args()
    f = RATE[a.rate]
    truth = []
    for an in json.load(open(a.meta))["annotations"]:
        hx = an.get("more1090:mode_s_hex", "").upper()
        if len(hx) == 28 and int(hx[:2], 16) >> 3 == 17 and an["more1090:cn0"] >= a.min_cn0:
            t = (an["core:sample_start"] + an.get("more1090:sample_start_fractional", 0.0)) * f
            truth.append((t, hx))
    msgs = []
    for line in open(a.decoded):
        if line.startswith("MSG "):
            d = dict(kv.split("=", 1) for kv in line[4:].split())
            msgs.append((int(d["samp"]), d["hex"], int(d["crcok"])))
    msgs.sort()
    pos = [m[0] for m in msgs]
    def bits(hx): return bin(int(hx, 16))[2:].zfill(4 * len(hx))
    rows = [[0, 0, 0, []] for _ in range(a.bins)]
    for t, hx in truth:
        b = int((t % 1.0) * a.bins) % a.bins
        lo = bisect.bisect_left(pos, t - 3); hi = bisect.bisect_right(pos, t + 3)
        cands = msgs[lo:hi]
        rows[b][0] += 1
        rows[b][1] += any(c[1] == hx and c[2] for c in cands)
        if cands:
            rows[b][2] += 1
            tb = bits(hx)
            best = min(sum(x != y for x, y in zip(bits(c[1]), tb)) + (len(tb) - 4 * len(c[1]) if 4 * len(c[1]) < len(tb) else 0)
                       for c in cands)
            rows[b][3].append(best)
    print(f"DF17 truth frames: {len(truth)} (CN0 >= {a.min_cn0:g})")
    print(f"{'timing phase':>14s} {'n':>5s} {'decode':>7s} {'cand':>6s} {'median bit errs (best candidate)':>34s}")
    for i, (n, d, c, e) in enumerate(rows):
        if n:
            print(f"  {i / a.bins:4.3f}-{(i + 1) / a.bins:4.3f} {n:5d} {100.0 * d / n:6.1f}% {100.0 * c / n:5.1f}% "
                  f"{statistics.median(e) if e else float('nan'):>20.0f}")
    n = sum(r[0] for r in rows); d = sum(r[1] for r in rows); c = sum(r[2] for r in rows)
    print(f"  {'all':>11s} {n:5d} {100.0 * d / n:6.1f}% {100.0 * c / n:5.1f}%")

if __name__ == "__main__":
    main()
