#!/usr/bin/env python3
"""check_stream.py - proves the streaming front end is exact.

1. mode_s_detect_stream() at many chunk sizes (down to chunks far smaller
   than one frame) must produce output IDENTICAL, line for line including
   sample indices, to processing the whole stream as a single buffer.
2. The current decoder with the DF18 length fix compiled out and all new
   options off must be identical to the pristine historical decoder.
3. Reports what the historical per-chunk path loses at the firmware's
   chunk size.
Uses synth.py, so it needs no external tools. Exits non-zero on failure.
"""
import os, subprocess, sys, tempfile
import re
HERE = os.path.dirname(os.path.abspath(__file__))

def run(binary, path, *args):
    r = subprocess.run([os.path.join(HERE, binary), path, *args],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{binary} failed: {r.stderr}")
    return [l for l in r.stdout.splitlines() if l.startswith("MSG ")]

def main():
    for b in ("host_mode_s", "host_mode_s_df18off", "host_mode_s_baseline"):
        if not os.path.exists(os.path.join(HERE, b)):
            sys.exit(f"error: {b} not built; run `make` in {HERE} first")
    path = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = None
    if path is None:
        tmp = tempfile.mkdtemp(prefix="adsb_check_")
        path = os.path.join(tmp, "synth.cu8")
        subprocess.run([sys.executable, os.path.join(HERE, "synth.py"), path],
                       check=True, capture_output=True)
    big = str(1 << 30)
    ref = run("host_mode_s", path, "--chunk", big, "--stream", "0")
    print(f"reference (single buffer): {len(ref)} frames")
    ok = True
    for chunk in ("262144", "65536", "4096", "1000", "482", "100", "2"):
        got = run("host_mode_s", path, "--chunk", chunk, "--stream", "1")
        same = got == ref
        ok &= same
        print(f"  stream chunk={chunk:>7s}: {len(got):6d} frames  {'IDENTICAL' if same else 'MISMATCH'}")
    legacy = run("host_mode_s", path, "--chunk", "262144", "--stream", "0")
    lost = len(set(ref) - set(legacy))
    extra = len(set(legacy) - set(ref))
    print(f"  historical per-chunk path, chunk=262144: {len(legacy)} frames "
          f"({lost} lost at chunk boundaries, {extra} spurious)")
    base = run("host_mode_s_baseline", path, "--chunk", "262144")
    new = run("host_mode_s_df18off", path, "--chunk", "262144")
    same = base == new
    ok &= same
    print(f"  pristine decoder vs current (DF18 fix off, options off): "
          f"{'IDENTICAL' if same else 'MISMATCH'} ({len(base)} vs {len(new)} frames)")
    # ---- the 2.4 Msps demodulator: same equivalence, on a 2.4 Msps stream
    if tmp:
        p24 = os.path.join(tmp, "synth24.cu8")
        subprocess.run([sys.executable, os.path.join(HERE, "synth.py"), p24, "--rate", "2.4"],
                       check=True, capture_output=True)
        truth24 = {l.split()[1] for l in open(os.path.join(tmp, "synth24.truth"))}
        ref24 = run("host_mode_s", p24, "--rate", "2.4", "--chunk", big, "--stream", "0")
        got_hex = {l.rsplit("hex=", 1)[1] for l in ref24}
        print(f"2.4 Msps reference (single buffer): {len(ref24)} frames, "
              f"{100.0 * len(truth24 & got_hex) / len(truth24):.1f}% of {len(truth24)} synthetic frames")
        for chunk in ("262144", "65536", "4096", "1000", "596", "100", "2"):
            got = run("host_mode_s", p24, "--rate", "2.4", "--chunk", chunk, "--stream", "1")
            same = got == ref24
            ok &= same
            print(f"  stream chunk={chunk:>7s}: {len(got):6d} frames  {'IDENTICAL' if same else 'MISMATCH'}")
    # ---- a loss of samples in the stream (mode_s_stream_gap): the frames
    # decoded must be exactly those of the two sides decoded as separate
    # streams, with the second side's sample times moved on by the first
    # side's length plus the number of samples declared lost.
    def shifted(lines, by):
        out = []
        for l in lines:
            l = re.sub(r"samp=(\d+)", lambda m: f"samp={int(m.group(1)) + by}", l)
            l = re.sub(r"tick=(\d+)", lambda m: f"tick={int(m.group(1)) + 5 * by}", l)
            out.append(l)
        return out
    gap_tmp = tmp or tempfile.mkdtemp(prefix="adsb_gap_")
    for label, p, rate in (("2.0", path, []), ("2.4", p24 if tmp else None, ["--rate", "2.4"])):
        if p is None:
            continue
        raw = open(p, "rb").read()
        size = len(raw) & ~1
        print(f"{label} Msps, samples lost mid-stream:")
        for frac, skip, lost in ((0.25, 2000, 1000), (0.5, 2, 1), (0.777, 48000, 24000), (0.4, 0, 0), (0.6, 6000, 0)):
            at = int(size * frac) & ~1
            seg1, seg2 = os.path.join(gap_tmp, "seg1.cu8"), os.path.join(gap_tmp, "seg2.cu8")
            open(seg1, "wb").write(raw[:at])
            open(seg2, "wb").write(raw[at + skip:size])
            want = (run("host_mode_s", seg1, *rate, "--chunk", big, "--stream", "1")
                    + shifted(run("host_mode_s", seg2, *rate, "--chunk", big, "--stream", "1"), at // 2 + lost))
            for chunk in ("131072", "5632", "1000"):
                got = run("host_mode_s", p, *rate, "--chunk", chunk, "--stream", "1",
                          "--gap-at", str(at), "--gap-skip", str(skip), "--gap-lost", str(lost))
                same = got == want
                ok &= same
                print(f"  at {frac:5.1%}, {skip // 2:5d} samples dropped, {lost:5d} declared, chunk={chunk:>6s}: "
                      f"{len(got):6d} frames  {'IDENTICAL to separate streams' if same else 'MISMATCH'}")
            os.unlink(seg1)
            os.unlink(seg2)
    if not tmp:
        os.rmdir(gap_tmp)
    if tmp:
        for f in os.listdir(tmp):
            os.unlink(os.path.join(tmp, f))
        os.rmdir(tmp)
    if not ok:
        sys.exit("FAIL")
    print("OK")

if __name__ == "__main__":
    main()
