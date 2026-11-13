#!/usr/bin/env python3
"""plot_sweep.py - summarise and plot sweep CSVs written by sweep.py.

usage: plot_sweep.py results/sweep.csv [more.csv ...] [--png out.png]
Prints, per variant, the CN0 at which 50% / 90% of DF17 frames decode
(linear interpolation) and total wrong decodes; writes a PNG if
matplotlib is available.
"""
import argparse, csv, sys
from collections import defaultdict


def crossing(xs, ys, level):
    for (x0, y0), (x1, y1) in zip(zip(xs, ys), zip(xs[1:], ys[1:])):
        if y0 < level <= y1:
            return x0 + (level - y0) * (x1 - x0) / (y1 - y0)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+")
    ap.add_argument("--png")
    a = ap.parse_args()
    acc = defaultdict(lambda: defaultdict(lambda: [0, 0, 0, 0, 0]))  # variant -> cn0 -> sums
    order = []
    for p in a.csvs:
        for r in csv.DictReader(open(p)):
            v = r["variant"]
            if v not in order:
                order.append(v)
            s = acc[v][float(r["cn0"])]
            s[0] += int(r["n"]); s[1] += int(r["n_df17"]); s[2] += int(r["decode"])
            s[3] += int(r["decode_df17"]); s[4] += int(r["wrong"])
    print(f"{'variant':12s} {'DF17 50%':>9s} {'DF17 90%':>9s} {'all-ES 50%':>11s} {'frames':>7s} {'wrong':>6s}")
    curves = {}
    for v in order:
        xs = sorted(acc[v])
        y17 = [acc[v][x][3] / max(acc[v][x][1], 1) for x in xs]
        yall = [acc[v][x][2] / max(acc[v][x][0], 1) for x in xs]
        curves[v] = (xs, y17, yall)
        f = lambda z: f"{z:9.2f}" if z is not None else f"{'-':>9s}"
        print(f"{v:12s} {f(crossing(xs, y17, 0.5))} {f(crossing(xs, y17, 0.9))} "
              f"{f(crossing(xs, yall, 0.5)):>11s} {sum(acc[v][x][0] for x in xs):7d} "
              f"{sum(acc[v][x][4] for x in xs):6d}")
    if a.png:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            print("matplotlib missing; skipping plot "
                  "(pip install --break-system-packages matplotlib)", file=sys.stderr)
            return
        fig, axes = plt.subplots(1, 2, figsize=(13, 5), sharey=True)
        for v in order:
            xs, y17, yall = curves[v]
            axes[0].plot(xs, y17, marker="o", ms=3, label=v)
            axes[1].plot(xs, yall, marker="o", ms=3, label=v)
        axes[0].set_title("DF17 frames only")
        axes[1].set_title("all extended squitter (DF17 + DF18)")
        for ax in axes:
            ax.set_xlabel("CN0, dB-Hz (peak-power referenced)")
            ax.grid(True, alpha=0.3)
        axes[0].set_ylabel("P(decode)")
        axes[1].legend(fontsize=8)
        fig.suptitle("adsb-scope decoder: decode probability vs CN0 (2.0 Msps, cu8)")
        fig.savefig(a.png, dpi=130, bbox_inches="tight")
        print(f"wrote {a.png}")


if __name__ == "__main__":
    main()
