#!/usr/bin/env python3
"""sweep.py - decode probability vs CN0 for every decoder variant.

Per CN0 point: N single extended-squitter snippets from
more1090-simulator (seeded, 1 ms each, random frequency offset / phase /
clock error), resampled to the target rate, quantised to cu8 and
concatenated; every decoder variant then runs over the same stream.

Per snippet:  decode = the exact truth frame was output;
              wrong  = some other CRC-ok 112-bit DF17/18 frame was output.

Rows are appended to the CSV as each point completes, so an interrupted
run loses nothing. Plot / summarise with plot_sweep.py.

CN0 is peak-power referenced (more1090's definition). The absolute axis
depends on the front-end scale K, because the detector has fixed
magnitude thresholds - compare variants at equal K.
"""
import argparse, csv, json, os, subprocess, sys
from dsp import np, RATES, resample, to_cu8

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.environ.get("MORE1090_SIM", "/tmp/more1090/target/release/more1090-simulator")


def variants(k):
    lo, hi = str(int(0.72 * k)), str(int(0.85 * k))
    return [
        ("pristine",   "host_mode_s_baseline", []),
        ("df18off",    "host_mode_s_df18off",  []),
        ("current",    "host_mode_s",          []),
        ("fix2",       "host_mode_s",          ["--fix2", "1"]),
        ("fix2_s",     "host_mode_s",          ["--fix2", "1", "--strict", "1"]),
        ("fix2_s_lo",  "host_mode_s",          ["--fix2", "1", "--strict", "1", "--fix2-delta", lo]),
        ("fix2_s_hi",  "host_mode_s",          ["--fix2", "1", "--strict", "1", "--fix2-delta", hi]),
    ]


def gen_point(cn0, n, rate, k, work, seed_base):
    up, down = RATES[rate]
    stream = os.path.join(work, f"cn0_{cn0:g}.cu8")
    meta = os.path.join(work, f"snip_{cn0:g}.sigmf-meta")
    data = meta[:-len(".sigmf-meta")] + ".sigmf-data"
    manifest, offset = [], 0
    with open(stream, "wb") as fo:
        for t in range(n):
            subprocess.run([SIM, "single-message", "--cn0", str(cn0),
                            "--seed", str(seed_base + t), meta],
                           check=True, capture_output=True)
            ann = json.load(open(meta))["annotations"][0]
            y = resample(np.fromfile(data, dtype=np.complex64), up, down)
            u8, _ = to_cu8(y, k)
            u8.tofile(fo)
            truth = ann["more1090:mode_s_hex"].upper()
            manifest.append({"offset": offset, "len": len(y), "truth": truth,
                             "df": int(truth[:2], 16) >> 3})
            offset += len(y)
    os.unlink(meta)
    os.unlink(data)
    return stream, manifest


def decode(binary, stream, extra):
    r = subprocess.run([os.path.join(HERE, binary), stream, "--chunk", str(1 << 28)] + extra,
                       capture_output=True, text=True, check=True)
    out = []
    for line in r.stdout.splitlines():
        if line.startswith("MSG "):
            d = dict(kv.split("=", 1) for kv in line[4:].split())
            out.append((int(d["samp"]), int(d["df"]), int(d["bits"]), d["hex"]))
    return out


def score(manifest, msgs):
    msgs = sorted(msgs)
    i = 0
    res = {"decode": 0, "decode_df17": 0, "wrong": 0}
    for s in manifest:
        lo, hi = s["offset"], s["offset"] + s["len"]
        while i < len(msgs) and msgs[i][0] < lo:
            i += 1
        j = i
        hit = bad = False
        while j < len(msgs) and msgs[j][0] < hi:
            _, df, bits, hx = msgs[j]
            if hx == s["truth"]:
                hit = True
            elif bits == 112 and df in (17, 18):
                bad = True
            j += 1
        res["decode"] += hit
        res["decode_df17"] += hit and s["df"] == 17
        res["wrong"] += bad
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cn0", default="71:87:1", help="start:stop:step (stop exclusive), dB-Hz")
    ap.add_argument("--trials", type=int, default=150)
    ap.add_argument("--rate", choices=RATES, default="2.0")
    ap.add_argument("--k", type=float, default=8000.0, help="cu8 scale (see module docstring)")
    ap.add_argument("--csv", default=os.path.join(HERE, "results", "sweep.csv"))
    ap.add_argument("--seed", type=int, default=77000)
    a = ap.parse_args()

    if not os.path.exists(SIM):
        sys.exit(f"error: simulator not found at {SIM}\n"
                 f"  build more1090 v0.2.0 (cargo build --release) or set MORE1090_SIM")
    vs = variants(a.k)
    for _, b, _ in vs:
        if not os.path.exists(os.path.join(HERE, b)):
            sys.exit(f"error: {b} not built; run `make` in {HERE}")
    os.makedirs(os.path.dirname(os.path.abspath(a.csv)), exist_ok=True)
    work = os.path.join(os.path.dirname(os.path.abspath(a.csv)), "work")
    os.makedirs(work, exist_ok=True)

    start, stop, step = (float(x) for x in a.cn0.split(":"))
    fields = ["rate", "k", "cn0", "variant", "n", "n_df17", "decode", "decode_df17", "wrong"]
    new = not os.path.exists(a.csv)
    with open(a.csv, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        if new:
            w.writeheader()
        for cn0 in np.arange(start, stop, step):
            stream, manifest = gen_point(float(cn0), a.trials, a.rate, a.k, work,
                                         a.seed + int(round(cn0 * 10)) * 1000)
            n17 = sum(1 for s in manifest if s["df"] == 17)
            line = [f"cn0={cn0:5.1f}"]
            for name, binary, extra in vs:
                r = score(manifest, decode(binary, stream, extra))
                w.writerow({"rate": a.rate, "k": a.k, "cn0": float(cn0), "variant": name,
                            "n": len(manifest), "n_df17": n17, **r})
                line.append(f"{name}={r['decode']}/{r['decode_df17']}" + (f"!{r['wrong']}" if r["wrong"] else ""))
            f.flush()
            os.unlink(stream)
            print(" ".join(line) + f"   (of {len(manifest)}, {n17} DF17; all/DF17-only, !wrong)", flush=True)


if __name__ == "__main__":
    main()
