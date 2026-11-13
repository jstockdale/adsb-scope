#!/usr/bin/env python3
"""scenario_curve.py - decode probability vs CN0 from one simulated scenario.

Generates a more1090-simulator scenario whose transmitters are spread over
a distance ring (so truth CN0 spans max_cn0 down to
max_cn0 - 20*log10(max_distance/min_distance)), resamples it to the target
rate, runs every decoder variant, and appends per-1-dB-bin rows to a CSV in
the same format sweep.py writes (plot with plot_sweep.py).

Compared with sweep.py (one isolated frame per simulator run) this yields
thousands of frames per run and includes realistic interference: other
Mode-S replies and Mode A/C at --mps messages per second.

usage example:
  scenario_curve.py --name mid --min-distance 40 --max-distance 120 \
      --max-cn0 88 --k 8000 --csv results/curve.csv
"""
import argparse, bisect, csv, os, subprocess, sys
from collections import defaultdict
from dsp import np, RATES, to_cu8
from decimate import blocks, resample_blocks
from scenario_score import load_truth, load_msgs, RATE as RATE_FACTOR

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.environ.get("MORE1090_SIM", "/tmp/more1090/target/release/more1090-simulator")


def variants(k):
    lo = str(int(0.72 * k))
    return [
        ("pristine",  "host_mode_s_baseline", []),
        ("df18off",   "host_mode_s_df18off",  []),
        ("current",   "host_mode_s",          ["--stream", "1"]),
        ("fix2",      "host_mode_s",          ["--stream", "1", "--fix2", "1"]),
        ("fix2_s",    "host_mode_s",          ["--stream", "1", "--fix2", "1", "--strict", "1"]),
        ("fix2_s_lo", "host_mode_s",          ["--stream", "1", "--fix2", "1", "--strict", "1", "--fix2-delta", lo]),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--min-distance", type=float, required=True)
    ap.add_argument("--max-distance", type=float, required=True)
    ap.add_argument("--max-cn0", type=float, required=True)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--mps", type=float, default=300.0)
    ap.add_argument("--k", type=float, default=8000.0)
    ap.add_argument("--rate", choices=RATES, default="2.0")
    ap.add_argument("--seed", type=int, default=1090)
    ap.add_argument("--csv", default=os.path.join(HERE, "results", "curve.csv"))
    ap.add_argument("--keep", action="store_true", help="keep the cu8 and decoder outputs")
    a = ap.parse_args()

    if not os.path.exists(SIM):
        sys.exit(f"error: simulator not found at {SIM}\n"
                 f"  build more1090 v0.2.0 (cargo build --release) or set MORE1090_SIM")
    vs = variants(a.k)
    for _, b, _ in vs:
        if not os.path.exists(os.path.join(HERE, b)):
            sys.exit(f"error: {b} not built; run `make` in {HERE}")
    outdir = os.path.dirname(os.path.abspath(a.csv))
    work = os.path.join(outdir, "work")
    os.makedirs(work, exist_ok=True)
    meta = os.path.join(work, f"{a.name}.sigmf-meta")
    data = meta[:-len(".sigmf-meta")] + ".sigmf-data"
    cu8 = os.path.join(work, f"{a.name}.cu8")

    subprocess.run([SIM, "scenario", "--duration", str(a.duration),
                    "--messages-per-second", str(a.mps), "--max-cn0", str(a.max_cn0),
                    "--min-distance", str(a.min_distance), "--max-distance", str(a.max_distance),
                    "--seed", str(a.seed), meta], check=True, capture_output=True)
    up, down = RATES[a.rate]
    total = clipped = 0
    with open(cu8, "wb") as fo:
        for y in resample_blocks(blocks(data, "cf32_le"), up, down):
            u8, c = to_cu8(y, a.k)
            u8.tofile(fo)
            total += len(y); clipped += c
    os.unlink(data)

    truth = load_truth(meta, RATE_FACTOR[a.rate])
    all_hex = {t[1] for t in truth}
    es = [t for t in truth if len(t[1]) == 28 and int(t[1][:2], 16) >> 3 in (17, 18)]
    print(f"[{a.name}] {total} samples, K={a.k:g}, clipped {100.0 * clipped / (2 * total):.3f}%, "
          f"{len(truth)} Mode-S truth frames, {len(es)} ES "
          f"(CN0 {min(t[2] for t in es):.1f}..{max(t[2] for t in es):.1f})")

    fields = ["rate", "k", "cn0", "variant", "n", "n_df17", "decode", "decode_df17", "wrong"]
    new = not os.path.exists(a.csv)
    with open(a.csv, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        if new:
            w.writeheader()
        for name, binary, extra in vs:
            out = os.path.join(work, f"{a.name}.{name}.txt")
            with open(out, "w") as fo:
                r = subprocess.run([os.path.join(HERE, binary), cu8] + extra,
                                   stdout=fo, stderr=subprocess.PIPE, text=True, check=True)
            msgs = load_msgs(out)
            es_out = [m for m in msgs if m[3] == 112 and m[2] in (17, 18)]
            pos = [m[0] for m in es_out]
            bins = defaultdict(lambda: [0, 0, 0, 0])
            dec = 0
            for start, hx, cn0 in es:
                lo = bisect.bisect_left(pos, start - 600)
                hi = bisect.bisect_right(pos, start + 600)
                hit = any(es_out[i][1] == hx for i in range(lo, hi))
                df17 = (int(hx[:2], 16) >> 3) == 17
                b = bins[int(cn0 // 1)]
                b[0] += 1; b[1] += df17; b[2] += hit; b[3] += hit and df17
                dec += hit
            false = sum(1 for m in es_out if m[1] not in all_hex)
            for c in sorted(bins):
                n, n17, d, d17 = bins[c]
                w.writerow({"rate": a.rate, "k": a.k, "cn0": c + 0.5, "variant": name, "n": n,
                            "n_df17": n17, "decode": d, "decode_df17": d17, "wrong": 0})
            print(f"  {name:10s} decoded {dec:5d}/{len(es)} ({100.0 * dec / len(es):5.1f}%)  "
                  f"false {false:3d}   {r.stderr.strip().replace('STATS ', '')[:150]}")
            if not a.keep:
                os.unlink(out)
    if not a.keep:
        os.unlink(cu8)


if __name__ == "__main__":
    main()
