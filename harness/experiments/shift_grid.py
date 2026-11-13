#!/usr/bin/env python3
"""shift_grid.py - experiment: does a half-sample-shifted second decode pass
recover the frames the 2 Msps decoder loses to sample-timing phase?

The decoder slices each bit by comparing two adjacent magnitude samples, so
frames arriving about half a sample off-grid are lost at any SNR (see
phase_diag.py). Here the complex IQ stream is interpolated at half-sample
offsets, re-quantised, and decoded a second time by the SAME decoder; the
two result sets are merged.

Interpolators compared:
  sincN   N-tap windowed sinc (sinc = 32 taps, the reference)
  cubic   4 taps, (-1, 9, 9, -1)/16   (adds and shifts only)
  linear  2 taps, (1, 1)/2

usage: shift_grid.py stream.cu8 [--truth scenario.sigmf-meta] [--decoder-args "..."]
"""
import argparse, bisect, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from dsp import np
from scenario_score import load_truth, load_msgs

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

def taps(method):
    if method == "linear":
        return np.array([0.5, 0.5], np.float32)
    if method == "cubic":
        return np.array([-1, 9, 9, -1], np.float32) / 16.0
    L = int(method[4:] or 32)          # "sinc", "sinc8", "sinc16", ...
    n = np.arange(L) - (L - 1) / 2.0
    h = np.sinc(n) * np.kaiser(L, 6.0 if L >= 16 else 3.0)
    return (h / h.sum()).astype(np.float32)

def shift_file(src, dst, method, chunk=4_000_000):
    """dst[n] = interpolated src(n + 0.5), re-quantised to cu8."""
    h = taps(method)
    L = len(h)
    ctx = 64
    n_total = os.path.getsize(src) // 2
    with open(src, "rb") as fi, open(dst, "wb") as fo:
        for start in range(0, n_total, chunk):
            a = max(start - ctx, 0)
            b = min(start + chunk + ctx, n_total)
            fi.seek(2 * a)
            raw = np.frombuffer(fi.read(2 * (b - a)), dtype=np.uint8).astype(np.float32) - 127.0
            out = np.empty(2 * (min(start + chunk, n_total) - start), np.float32)
            for rail in (0, 1):
                y = np.convolve(raw[rail::2], h, mode="full")[L // 2:]
                seg = y[start - a: start - a + len(out) // 2]
                out[rail::2][:len(seg)] = seg
                out[rail::2][len(seg):] = 0
            np.clip(np.round(out) + 127, 0, 255).astype(np.uint8).tofile(fo)

def decode(cu8, args):
    out = cu8 + ".txt"
    with open(out, "w") as fo:
        subprocess.run([os.path.join(HERE, "host_mode_s"), cu8, "--stream", "1"] + args,
                       stdout=fo, stderr=subprocess.DEVNULL, check=True)
    m = [x for x in load_msgs(out) if x[3] == 112 and x[2] in (17, 18)]
    os.unlink(out)
    return m

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cu8")
    ap.add_argument("--truth")
    ap.add_argument("--decoder-args", default="")
    ap.add_argument("--methods", default="sinc,cubic,linear")
    a = ap.parse_args()
    if not os.path.exists(os.path.join(HERE, "host_mode_s")):
        sys.exit("error: host_mode_s not built; run `make` in harness/")
    dargs = a.decoder_args.split()
    A = decode(a.cu8, dargs)
    tmp = tempfile.mkdtemp(prefix="shift_")
    truth = es = None
    if a.truth:
        truth = load_truth(a.truth, 0.25)
        es = [t for t in truth if len(t[1]) == 28 and int(t[1][:2], 16) >> 3 in (17, 18)]
        all_hex = {t[1] for t in truth}

    def hits(msgs, shift):
        pos = [m[0] + shift for m in msgs]
        h = set()
        for i, (start, hx, _) in enumerate(es):
            lo = bisect.bisect_left(pos, start - 600); hi = bisect.bisect_right(pos, start + 600)
            if any(msgs[j][1] == hx for j in range(lo, hi)):
                h.add(i)
        return h

    def report(name, msgs_b):
        # merge: a B frame duplicates an A frame if same hex within +-3 samples
        byhex = {}
        for s, hx, *_ in A:
            byhex.setdefault(hx, []).append(s)
        dup = sum(1 for s, hx, *_ in msgs_b if any(abs(s + 0.5 - p) <= 3 for p in byhex.get(hx, ())))
        union = len(A) + len(msgs_b) - dup
        line = (f"{name:8s} grid B alone {len(msgs_b):6d}   union {union:6d} "
                f"({100.0 * (union - len(A)) / max(len(A), 1):+6.1f}% vs single grid)")
        if es:
            hb = hits(msgs_b, 0.5)
            u = ha | hb
            fb = sum(1 for m in msgs_b if m[1] not in all_hex)
            line += (f"   truth: B {100.0 * len(hb) / len(es):5.1f}%  union {100.0 * len(u) / len(es):5.1f}%"
                     f"  false(B) {fb}")
        print(line)

    line = f"single grid (as shipped): {len(A)} ES frames"
    if es:
        ha = hits(A, 0.0)
        line += f"   truth: {100.0 * len(ha) / len(es):5.1f}% of {len(es)} ES decoded, false {sum(1 for m in A if m[1] not in all_hex)}"
    print(line)
    for method in a.methods.split(","):
        dst = os.path.join(tmp, f"{method}.cu8")
        shift_file(a.cu8, dst, method)
        report(method, decode(dst, dargs))
        os.unlink(dst)
    os.rmdir(tmp)

if __name__ == "__main__":
    main()
