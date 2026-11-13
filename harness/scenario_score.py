#!/usr/bin/env python3
"""scenario_score.py - score decoder output against the ground truth of a
more1090-simulator scenario (or any SigMF meta carrying
more1090:mode_s_hex annotations, e.g. more1090's own receiver output).

usage: scenario_score.py truth.sigmf-meta decoded.txt [--rate 2.0] [--win 600]

For every 112-bit DF17/18 truth frame: decoded if the exact frame was
output within +-win samples of its true position.
Outputs that are CRC-ok 112-bit DF17/18 frames but equal to no truth
frame anywhere are counted as false decodes.
"""
import argparse, bisect, json, sys
from collections import defaultdict

RATE = {"2.0": 0.25, "2.4": 0.3, "3.2": 0.4}


def load_truth(meta, f):
    out = []
    for a in json.load(open(meta)).get("annotations", []):
        hx = a.get("more1090:mode_s_hex")
        if hx:
            hx = hx.upper()
            out.append((a["core:sample_start"] * f, hx, a.get("more1090:cn0")))
    return sorted(out)


def load_msgs(path):
    out = []
    for line in open(path):
        if line.startswith("MSG "):
            d = dict(kv.split("=", 1) for kv in line[4:].split())
            out.append((int(d["samp"]), d["hex"], int(d["df"]), int(d["bits"]), int(d["err"])))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("meta"); ap.add_argument("decoded")
    ap.add_argument("--rate", choices=RATE, default="2.0")
    ap.add_argument("--win", type=int, default=600)
    a = ap.parse_args()
    truth = load_truth(a.meta, RATE[a.rate])
    if not truth:
        sys.exit("error: no more1090:mode_s_hex annotations in the meta file")
    msgs = load_msgs(a.decoded)
    es_out = [m for m in msgs if m[3] == 112 and m[2] in (17, 18)]
    pos = [m[0] for m in es_out]
    all_hex = {t[1] for t in truth}
    es_truth = [t for t in truth if len(t[1]) == 28 and int(t[1][:2], 16) >> 3 in (17, 18)]
    dec = 0
    bucket = defaultdict(lambda: [0, 0])
    for start, hx, cn0 in es_truth:
        lo = bisect.bisect_left(pos, start - a.win)
        hi = bisect.bisect_right(pos, start + a.win)
        hit = any(es_out[i][1] == hx for i in range(lo, hi))
        dec += hit
        if cn0 is not None:
            b = int(cn0 // 2) * 2
            bucket[b][0] += hit; bucket[b][1] += 1
    false = [m for m in es_out if m[1] not in all_hex]
    f1 = sum(1 for m in false if m[4] != -1 and m[4] < 256)
    f2 = sum(1 for m in false if m[4] >= 256)
    print(f"ES truth {len(es_truth)}  decoded {dec} ({100.0 * dec / max(len(es_truth), 1):.1f}%)  "
          f"ES output {len(es_out)}  false {len(false)} (unfixed {len(false) - f1 - f2}, 1-bit-fixed {f1}, 2-bit-fixed {f2})")
    if bucket:
        print("  by truth CN0: " + "  ".join(f"{b}-{b + 2}:{d}/{n}" for b, (d, n) in sorted(bucket.items())))


if __name__ == "__main__":
    main()
