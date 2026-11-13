#!/usr/bin/env python3
"""decimate.py - resample an 8 Msps SigMF recording to an RTL-style cu8 stream.

Reads cf32_le or ci16_le (from the .sigmf-meta), polyphase-resamples to
2.0 / 2.4 / 3.2 Msps and writes interleaved uint8 I,Q (offset 127).

  --scale K     amplitude -> LSB gain. For more1090-simulator output
                (noise sigma 1/1024 per rail) K=8000 puts the noise at
                ~7.8 LSB rms per rail.
  --scale auto  (default) pick K so the 99.99th percentile rail magnitude
                of the first block maps to 110 counts.
  --gain-steps 1,2,4   additionally write <out>_gN.cu8 at N times the
                chosen K (to study sensitivity to front-end gain).

Processed in blocks with on-grid padding; --selftest proves the block
path equals a whole-file resample.
"""
import argparse, json, os, sys
from dsp import np, RATES, design, resample, to_cu8

BLOCK = 4_000_000  # input samples per block; multiple of every 'down'


def open_input(meta_path):
    if not meta_path.endswith(".sigmf-meta"):
        sys.exit("error: first argument must be a .sigmf-meta file")
    data_path = meta_path[:-len(".sigmf-meta")] + ".sigmf-data"
    for p in (meta_path, data_path):
        if not os.path.exists(p):
            sys.exit(f"error: {p} not found")
    g = json.load(open(meta_path))["global"]
    if abs(g["core:sample_rate"] - 8e6) > 1:
        sys.exit("error: input must be 8 Msps")
    dt = g["core:datatype"]
    if dt not in ("cf32_le", "ci16_le"):
        sys.exit(f"error: unsupported datatype {dt}")
    return data_path, dt


def blocks(data_path, dt):
    bpc = 8 if dt == "cf32_le" else 4
    with open(data_path, "rb") as f:
        while True:
            raw = f.read(BLOCK * bpc)
            if not raw:
                return
            if dt == "cf32_le":
                yield np.frombuffer(raw, dtype=np.complex64)
            else:
                a = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
                yield (a[0::2] + 1j * a[1::2]).astype(np.complex64)


def resample_blocks(block_iter, up, down):
    """Yield resampled blocks equal to a whole-stream resample (except within
    half a filter length of the very ends, which see zeros either way)."""
    h = design(up, down)
    pad = 64 * down            # on the output grid: pad*up/down is an integer
    lead_out = pad * up // down
    tail = np.zeros(0, np.complex64)
    prev = None
    for blk in block_iter:
        if prev is not None:
            ctx = np.concatenate([tail, prev, blk[:pad]])
            y = resample(ctx, up, down, h)
            lead = len(tail) * up // down
            yield y[lead: lead + len(prev) * up // down]
            tail = prev[-pad:]
        prev = blk
    if prev is not None:
        y = resample(np.concatenate([tail, prev]), up, down, h)
        yield y[len(tail) * up // down:]


def selftest():
    rng = np.random.default_rng(1)
    x = (rng.normal(size=1_000_003) + 1j * rng.normal(size=1_000_003)).astype(np.complex64)
    global BLOCK
    saved, BLOCK = BLOCK, 100_000
    for rate, (up, down) in RATES.items():
        whole = resample(x, up, down)
        it = (x[i:i + BLOCK] for i in range(0, len(x), BLOCK))
        blk = np.concatenate(list(resample_blocks(it, up, down)))
        n = min(len(whole), len(blk))
        err = np.abs(whole[:n] - blk[:n]).max()
        print(f"selftest rate={rate}: len whole={len(whole)} block={len(blk)} max|diff|={err:.2e}")
        if len(whole) != len(blk) or err > 1e-5:
            sys.exit("SELFTEST FAIL")
    BLOCK = saved
    print("selftest OK")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("meta")
    ap.add_argument("out")
    ap.add_argument("--rate", choices=RATES, default="2.0")
    ap.add_argument("--scale", default="auto")
    ap.add_argument("--gain-steps", default="")
    a = ap.parse_args()
    up, down = RATES[a.rate]
    data_path, dt = open_input(a.meta)
    steps = [float(s) for s in a.gain_steps.split(",") if s] or [None]
    base = a.out[:-4] if a.out.endswith(".cu8") else a.out
    outs = [(a.out if s is None else f"{base}_g{s:g}.cu8", 1.0 if s is None else s) for s in steps]
    files = [open(p, "wb") for p, _ in outs]
    k = None if a.scale == "auto" else float(a.scale)
    total, clipped = 0, [0] * len(outs)
    for y in resample_blocks(blocks(data_path, dt), up, down):
        if k is None:
            rails = np.abs(np.concatenate([y.real, y.imag]))
            k = 110.0 / max(float(np.percentile(rails, 99.99)), 1e-12)
        for i, (f, (_, g)) in enumerate(zip(files, outs)):
            u8, c = to_cu8(y, k * g)
            u8.tofile(f)
            clipped[i] += c
        total += len(y)
    for f in files:
        f.close()
    for (p, g), c in zip(outs, clipped):
        print(f"{p}: {total} samples @ {a.rate} Msps, K={k * g:.1f}, "
              f"clipped {100.0 * c / max(2 * total, 1):.4f}% of rail samples")


if __name__ == "__main__":
    main()
