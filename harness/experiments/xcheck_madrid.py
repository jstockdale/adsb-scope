#!/usr/bin/env python3
"""xcheck_madrid.py - are the frames we decode from the Madrid capture real?

The capture has no ground truth. This compares our decodes (2.4 Msps,
8-bit) with those of more1090 - a different receiver, working on the
original 8 Msps samples - and reports, per configuration:

  frames      extended squitters we decoded
  in ref      of those, frames whose 112 bits the reference decoded too
  not in ref  the rest (the reference misses frames as well, so these are
              not all wrong)
  new addr    of the rest, frames from an address the reference never
              decoded error-free; a false frame almost always lands here

A frame the reference also decoded is as good as confirmed: two receivers
agreeing on 112 bits by accident does not happen. A frame it did not
decode, from an aircraft it did hear, is very probably real.

usage: xcheck_madrid.py MADRID.sigmf-meta DATADIR
  DATADIR holds madrid_lo.cu8 / madrid_hi.cu8 (prep_2400.py --madrid);
  the reference's decodes are cached there as madrid_more1090_ec{0,2}.txt
needs: MORE1090 (the more1090 receiver binary, v0.2.0), numpy, scipy
"""
import argparse, json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..")); sys.path.insert(0, HERE)
try:
    from dsp import np
    from decimate import blocks
except ImportError as e:
    sys.exit(f"error: {e}\n  install: pip install numpy scipy")

BIN = os.path.join(HERE, "..", "host_mode_s")
REF = os.environ.get("MORE1090", "/tmp/more1090/target/release/more1090")
FIX2 = ["--soft", "8", "--fix2", "1", "--strict", "1", "--known-icao", "1"]
GATED = ["--soft", "8", "--strict", "1", "--known-icao", "1"]
CONFIGS = [
    ("default image", []),
    ("default image + coherent", ["--coh", "1"]),
    ("fix2 image", FIX2),
    ("fix2 image + coherent", FIX2 + ["--coh", "1"]),
    ("fix2 image minus 2-bit fix", GATED),
    ("fix2 image minus 2-bit + coherent", GATED + ["--coh", "1"]),
    ("soft rescue + coherent, no address gate", ["--soft", "8", "--coh", "1"]),
]


def is_es(h):
    return len(h) == 28 and (int(h[:2], 16) >> 3) in (17, 18)


def reference(meta, level, cache):
    """more1090's decodes of the 8 Msps recording, all formats, upper-case hex."""
    if os.path.exists(cache):
        return [l.strip() for l in open(cache) if l.strip()]
    data = meta[:-len(".sigmf-meta")] + ".sigmf-data"
    dt = json.load(open(meta))["global"]["core:datatype"]
    p = subprocess.Popen([REF, "--stdin", "--error-correction-level", str(level)],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    import threading
    out = []
    t = threading.Thread(target=lambda: out.extend(p.stdout.read().decode().splitlines()))
    t.start()
    for y in blocks(data, dt):
        p.stdin.write(np.ascontiguousarray(y, dtype=np.complex64).tobytes())
    p.stdin.close()
    t.join()
    if p.wait() != 0:
        sys.exit(f"error: {REF} exited with status {p.returncode}")
    hexes = [l.strip()[1:-1].upper() for l in out if l.startswith("*") and l.strip().endswith(";")]
    with open(cache, "w") as f:
        f.write("\n".join(hexes) + "\n")
    return hexes


def ours(cu8, args):
    r = subprocess.run([BIN, cu8, "--rate", "2.4", "--stream", "1"] + args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"error: host_mode_s failed: {r.stderr.strip()}")
    return [h for h in (l.rsplit("hex=", 1)[1].strip() for l in r.stdout.splitlines() if l.startswith("MSG "))
            if is_es(h)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("meta"); ap.add_argument("dir")
    a = ap.parse_args()
    # Everything this run needs, checked before the first decode.
    if not os.access(BIN, os.X_OK):
        sys.exit(f"error: {BIN} not built; run `make` in {os.path.dirname(BIN)} first")
    if not a.meta.endswith(".sigmf-meta") or not os.path.exists(a.meta):
        sys.exit(f"error: {a.meta} is not a .sigmf-meta file (the harness README has the download)")
    if not os.path.exists(a.meta[:-len(".sigmf-meta")] + ".sigmf-data"):
        sys.exit(f"error: no .sigmf-data next to {a.meta}")
    cu8 = {g: os.path.join(a.dir, f"madrid_{g}.cu8") for g in ("lo", "hi")}
    for p in cu8.values():
        if not os.path.exists(p):
            sys.exit(f"error: {p} missing; build it with experiments/prep_2400.py DATADIR --madrid {a.meta}")
    caches = {lv: os.path.join(a.dir, f"madrid_more1090_ec{lv}.txt") for lv in (0, 2)}
    if not all(os.path.exists(c) for c in caches.values()) and not os.access(REF, os.X_OK):
        sys.exit(f"error: more1090 not found at {REF}\n  build more1090 v0.2.0 (cargo build --release) and set MORE1090")

    ref = [h for h in reference(a.meta, 2, caches[2]) if is_es(h)]
    clean = [h for h in reference(a.meta, 0, caches[0]) if is_es(h)]
    ref_set, heard = set(ref), {h[2:8] for h in clean}
    print(f"reference (more1090 v0.2.0 at 8 Msps, up to 2 bits corrected): {len(ref)} extended squitters, "
          f"{len(ref_set)} distinct; error-free: {len(clean)} from {len(heard)} addresses")
    print()
    print(f"{'':<42}{'low gain':^36}  {'high gain':^36}".rstrip())
    print((f"{'configuration':<42}" + f"{'frames':>8}{'in ref':>8}{'not in ref':>11}{'new addr':>9}  " * 2).rstrip())
    for label, args in CONFIGS:
        row = f"{label:<42}"
        for g in ("lo", "hi"):
            hx = ours(cu8[g], args)
            miss = [h for h in hx if h not in ref_set]
            row += f"{len(hx):>8}{len(hx) - len(miss):>8}{len(miss):>11}{sum(h[2:8] not in heard for h in miss):>9}  "
        print(row.rstrip(), flush=True)


if __name__ == "__main__":
    main()
