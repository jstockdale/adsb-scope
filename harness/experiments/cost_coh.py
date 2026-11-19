#!/usr/bin/env python3
"""cost_coh.py - what the coherent second look costs, in instructions.

Runs the host decoder under valgrind's instruction counter on the first
two seconds of three inputs from the test set - the Madrid capture (real
traffic), the 2,000 messages/s scenario (worst case) and noise (quiet
band) - and prints instructions per input sample for each configuration,
with start-up subtracted, plus the average cost of one re-slice.

These are x86-64 instruction counts. They rank configurations and give
relative overhead; they are not ESP32-P4 cycles.

usage: cost_coh.py DATADIR [--old path/to/older/host_mode_s]
  --old   also measure an older build (e.g. of the r10 tip) on the two
          configurations it has, to show what changed with coherent off
needs: valgrind; `make` in harness/ first; DATADIR from prep_2400.py
       (--madrid) and prep_dense.py
"""
import argparse, os, re, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "host_mode_s")
SETS = [("madrid_lo", "Madrid"), ("dense", "dense"), ("noise", "noise")]
SECONDS = 2
NBYTES = SECONDS * 2_400_000 * 2
FIX2 = ["--soft", "8", "--fix2", "1", "--strict", "1", "--known-icao", "1"]
GATED = ["--soft", "8", "--strict", "1", "--known-icao", "1"]
CONFIGS = [
    ("default image", [], None),
    ("default image + coherent", ["--coh", "1"], "default image"),
    ("fix2 image", FIX2, None),
    ("fix2 image + coherent", FIX2 + ["--coh", "1"], "fix2 image"),
    ("fix2 image minus 2-bit fix", GATED, None),
    ("fix2 image minus 2-bit + coherent", GATED + ["--coh", "1"], "fix2 image minus 2-bit fix"),
]
OLD = [("older build, default image", []), ("older build, fix2 image", FIX2)]


def count(binary, path, args, td):
    """(instructions, stats dict) for one run."""
    out = os.path.join(td, "cg.out")
    r = subprocess.run(["valgrind", "--tool=callgrind", f"--callgrind-out-file={out}", binary, path,
                        "--rate", "2.4", "--stream", "1"] + args, capture_output=True, text=True)
    m = re.search(r"Collected : (\d+)", r.stderr)
    if r.returncode != 0 or not m:
        sys.exit(f"error: valgrind run failed for {binary} {' '.join(args)}:\n{r.stderr[-400:]}")
    st = dict(kv.split("=") for l in r.stderr.splitlines() if "STATS" in l
              for kv in l.split("STATS", 1)[1].split() if "=" in kv)
    return int(m.group(1)), st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--old", default="")
    a = ap.parse_args()
    # Everything this run needs, checked before the first (slow) measurement.
    if not shutil.which("valgrind"):
        sys.exit("error: valgrind not found  (Debian/Ubuntu: sudo apt install valgrind)")
    for b in [BIN] + ([a.old] if a.old else []):
        if not os.access(b, os.X_OK):
            sys.exit(f"error: {b} not found or not executable; run `make` in harness/ first")
    for name, _ in SETS:
        p = os.path.join(a.dir, name + ".cu8")
        if not os.path.exists(p) or os.path.getsize(p) < NBYTES:
            sys.exit(f"error: {p} missing or shorter than {SECONDS} s; build the test set with "
                     "experiments/prep_2400.py (--madrid) and prep_dense.py")

    samples = NBYTES // 2
    with tempfile.TemporaryDirectory() as td:
        empty = os.path.join(td, "empty.cu8")
        open(empty, "wb").write(b"\x7f\x7f")
        cut = {}
        for name, _ in SETS:
            cut[name] = os.path.join(td, name + ".cu8")
            with open(os.path.join(a.dir, name + ".cu8"), "rb") as f, open(cut[name], "wb") as g:
                g.write(f.read(NBYTES))
        print(f"x86-64 instructions per input sample, first {SECONDS} s of each input ({samples} samples), start-up subtracted")
        print()
        print(f"{'configuration':<36}" + "".join(f"{label:>10}" for _, label in SETS)
              + "   re-slices in Madrid / dense (stopped early), instructions per re-slice")
        res = {}
        rows = [(n, a.old, g, None) for n, g in OLD] if a.old else []
        rows += [(n, BIN, g, base) for n, g, base in CONFIGS]
        for label, binary, args, base in rows:
            start, _ = count(binary, empty, args, td)
            cells, extra = [], []
            for name, _ in SETS:
                ir, st = count(binary, cut[name], args, td)
                res[(label, name)] = ir - start
                cells.append(f"{(ir - start) / samples:>10.2f}")
                tries = int(st.get("coh_try", 0))
                if base and tries and name != "noise":
                    per = (res[(label, name)] - res[(base, name)]) / tries
                    extra.append(f"{tries} ({st.get('coh_abort', '0')}), {per:,.0f}")
            print(f"{label:<36}" + "".join(cells) + ("   " + " / ".join(extra) if extra else ""), flush=True)
        print()
        print("overhead of the coherent look, percent of decoder instructions:")
        for label, _, _, base in rows:
            if base:
                print(f"  {label:<34}" + "   ".join(
                    f"{lab} {100.0 * (res[(label, n)] - res[(base, n)]) / res[(base, n)]:+.1f} %" for n, lab in SETS))


if __name__ == "__main__":
    main()
