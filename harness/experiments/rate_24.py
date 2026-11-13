#!/usr/bin/env python3
"""rate_24.py - would sampling at 2.4 Msps help?

Runs identical inputs through
  - our decoder at 2.0 Msps, single grid (what the firmware does today),
  - our decoder at 2.0 Msps, dual grid (experiments/shift_grid.py),
  - dump1090-fa's 2.4 Msps demodulator, as a reference for what a
    purpose-built 2.4 Msps demodulator achieves. dump1090-fa is used here
    only as a measuring instrument; none of its code is in the firmware.

Both rates are produced from the same 8 Msps source by the same resampler
and 8-bit quantiser (decimate.py / dsp.py).

usage:
  rate_24.py scenario --name N --min-distance A --max-distance B --max-cn0 C --k K [--seed S]
  rate_24.py recording path/to/file.sigmf-meta
environment:
  MORE1090_SIM  path to more1090-simulator (scenario mode)
  DUMP1090      path to a dump1090-fa binary built without SDR support:
                make RTLSDR=no BLADERF=no HACKRF=no LIMESDR=no SOAPYSDR=no dump1090
"""
import argparse, json, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
from dsp import np, RATES, to_cu8
from decimate import blocks, resample_blocks
from shift_grid import shift_file, decode as decode_ours

SIM = os.environ.get("MORE1090_SIM", "/tmp/more1090/target/release/more1090-simulator")
D1090 = os.environ.get("DUMP1090", "/tmp/dump1090/dump1090")


def need(path, what, hint):
    if not os.path.exists(path):
        sys.exit(f"error: {what} not found at {path}\n  {hint}")


def to_rate(data, dtype, rate, k, out):
    """Resample an 8 Msps file to `rate` and quantise. k=None -> auto scale."""
    up, down = RATES[rate]
    n = clipped = 0
    with open(out, "wb") as fo:
        for y in resample_blocks(blocks(data, dtype), up, down):
            if k is None:
                rails = np.abs(np.concatenate([y.real, y.imag]))
                k = 110.0 / max(float(np.percentile(rails, 99.99)), 1e-12)
            u8, c = to_cu8(y, k)
            u8.tofile(fo)
            n += len(y); clipped += c
    return k, 100.0 * clipped / max(2 * n, 1)


def dump1090(cu8, extra):
    r = subprocess.run([D1090, "--ifile", cu8, "--iformat", "UC8", "--raw"] + extra,
                       capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("*") and line.endswith(";"):
            hx = line[1:-1].upper()
            if len(hx) == 28 and (int(hx[:2], 16) >> 3) in (17, 18):
                out.append(hx)
    return out


def merge_count(a, b):
    """Instances in the union of two grids (same frame within +-3 samples counts once)."""
    byhex = {}
    for s, hx, *_ in a:
        byhex.setdefault(hx, []).append(s)
    dup = sum(1 for s, hx, *_ in b if any(abs(s + 0.5 - p) <= 3 for p in byhex.get(hx, ())))
    return len(a) + len(b) - dup


def run(label, cu8_20, cu8_24, truth_hex, all_hex, fix2_args):
    tmp = tempfile.mkdtemp(prefix="r24_")
    rows = []

    def add(name, hexes, instances):
        if truth_hex is not None:
            got = set(hexes)
            rows.append((name, f"{100.0 * len(truth_hex & got) / len(truth_hex):5.1f}%",
                         f"{len(truth_hex & got):5d}", f"false {len(got - all_hex)}"))
        else:
            rows.append((name, f"{instances:6d} frames", f"{len(set(hexes)):5d} unique", ""))

    for tag, args in (("", []), (" +2-bit fix", fix2_args)):
        a = decode_ours(cu8_20, args)
        add(f"ours 2.0 Msps, single grid{tag}", [m[1] for m in a], len(a))
        for method in ("cubic", "sinc8"):
            dst = os.path.join(tmp, f"{method}.cu8")
            shift_file(cu8_20, dst, method)
            b = decode_ours(dst, args)
            os.unlink(dst)
            add(f"ours 2.0 Msps, dual grid ({method}){tag}",
                [m[1] for m in a] + [m[1] for m in b], merge_count(a, b))
    for name, extra in (("dump1090-fa 2.4 Msps (1-bit fix)", ["--fix"]),
                        ("dump1090-fa 2.4 Msps (2-bit fix)", ["--fix", "--fix-2bit"])):
        h = dump1090(cu8_24, extra)
        add(name, h, len(h))
    os.rmdir(tmp)
    print(f"[{label}]" + (f" {len(truth_hex)} ES truth frames" if truth_hex is not None else " (no ground truth)"))
    for r in rows:
        print(f"  {r[0]:42s} {r[1]:>14s} {r[2]:>13s}  {r[3]}")
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    s = sub.add_parser("scenario")
    s.add_argument("--name", required=True)
    s.add_argument("--min-distance", type=float, required=True)
    s.add_argument("--max-distance", type=float, required=True)
    s.add_argument("--max-cn0", type=float, required=True)
    s.add_argument("--k", type=float, default=8000.0)
    s.add_argument("--mps", type=float, default=300.0)
    s.add_argument("--duration", type=float, default=10.0)
    s.add_argument("--seed", type=int, default=1090)
    r = sub.add_parser("recording")
    r.add_argument("meta")
    a = ap.parse_args()

    need(D1090, "dump1090-fa", "build it (see module docstring) and set DUMP1090")
    need(os.path.join(HERE, "..", "host_mode_s"), "host_mode_s", "run `make` in harness/")
    work = tempfile.mkdtemp(prefix="rate24_")
    c20, c24 = os.path.join(work, "x20.cu8"), os.path.join(work, "x24.cu8")

    if a.mode == "scenario":
        need(SIM, "more1090-simulator", "build more1090 v0.2.0 and set MORE1090_SIM")
        meta = os.path.join(work, "s.sigmf-meta")
        data = os.path.join(work, "s.sigmf-data")
        subprocess.run([SIM, "scenario", "--duration", str(a.duration),
                        "--messages-per-second", str(a.mps), "--max-cn0", str(a.max_cn0),
                        "--min-distance", str(a.min_distance), "--max-distance", str(a.max_distance),
                        "--seed", str(a.seed), meta], check=True, capture_output=True)
        to_rate(data, "cf32_le", "2.0", a.k, c20)
        to_rate(data, "cf32_le", "2.4", a.k, c24)
        os.unlink(data)
        anns = json.load(open(meta))["annotations"]
        all_hex = {x["more1090:mode_s_hex"].upper() for x in anns if x.get("more1090:mode_s_hex")}
        truth = {h for h in all_hex if len(h) == 28 and (int(h[:2], 16) >> 3) in (17, 18)}
        os.unlink(meta)
        run(a.name, c20, c24, truth, all_hex, ["--fix2", "1", "--strict", "1"])
    else:
        if not a.meta.endswith(".sigmf-meta"):
            sys.exit("error: pass the .sigmf-meta file")
        data = a.meta[:-len(".sigmf-meta")] + ".sigmf-data"
        need(data, "recording data", "the .sigmf-data file must sit next to the .sigmf-meta")
        dt = json.load(open(a.meta))["global"]["core:datatype"]
        k20, _ = to_rate(data, dt, "2.0", None, c20)
        k24, _ = to_rate(data, dt, "2.4", None, c24)
        print(f"auto scale: K={k20:.1f} at 2.0 Msps, K={k24:.1f} at 2.4 Msps")
        run(os.path.basename(a.meta), c20, c24, None, None,
            ["--fix2", "1", "--strict", "1", "--known-icao", "1"])
    for f in (c20, c24):
        os.unlink(f)
    os.rmdir(work)


if __name__ == "__main__":
    main()
