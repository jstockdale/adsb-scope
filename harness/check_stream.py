#!/usr/bin/env python3
"""check_stream.py - proves the streaming front end is exact.

1. mode_s_detect_stream() at many chunk sizes (down to chunks far smaller
   than one frame) must produce output IDENTICAL, line for line including
   sample indices, to processing the whole stream as a single buffer.
2. The current decoder with the DF18 length fix compiled out and all new
   options off must be identical to the pristine historical decoder.
3. Reports what the historical per-chunk path loses at the firmware's
   chunk size.
4. The coherent second look (2.4 Msps, streaming only) must give the same
   output at every chunk size, add frames, and add none that was not sent.
   Switched on and off in mid-stream it must leave the output outside that
   interval exactly as it is with the look off.
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
    for b in ("host_mode_s", "host_mode_s_plain", "host_mode_s_df18off", "host_mode_s_baseline"):
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
        # ---- the demodulator's written-out chip sums (mode_s_2400.c) against
        # the plain form they stand for (host_mode_s_plain, -DMODE_S_2400_PLAIN):
        # the same frames, bit deltas and times, with and without the repairs.
        for label, opts in (("defaults", []), ("soft rescue + coherent look", ["--soft", "8", "--coh", "1"])):
            a = run("host_mode_s", p24, "--rate", "2.4", *opts, "--chunk", "5632", "--stream", "1")
            b = run("host_mode_s_plain", p24, "--rate", "2.4", *opts, "--chunk", "5632", "--stream", "1")
            same = a == b and len(a) > 0
            ok &= same
            print(f"  written-out chip sums vs plain form, {label}: {len(a)} frames  {'IDENTICAL' if same else 'MISMATCH'}")
        # ---- the coherent second look (mode_s_coh.c). It needs the IQ behind
        # the magnitudes, so it only runs on the streaming path, where the IQ
        # of the carried tail is kept between calls: its output must not
        # depend on the chunk size, and no frame it adds may be one that was
        # never sent.
        soft = ["--rate", "2.4", "--soft", "8"]
        base = {l.rsplit("hex=", 1)[1] for l in run("host_mode_s", p24, *soft, "--chunk", big, "--stream", "1")}
        refc = run("host_mode_s", p24, *soft, "--coh", "1", "--chunk", big, "--stream", "1")
        hexc = {l.rsplit("hex=", 1)[1] for l in refc}
        stray = (hexc - base) - truth24
        ok &= not stray and len(hexc) > len(base)
        print(f"2.4 Msps with the coherent second look: {len(refc)} frames, {len(hexc - base)} more than without, "
              f"{len(stray)} of those never sent  {'OK' if not stray and len(hexc) > len(base) else 'MISMATCH'}")
        for chunk in ("262144", "65536", "4096", "1000", "596", "100", "2"):
            got = run("host_mode_s", p24, *soft, "--coh", "1", "--chunk", chunk, "--stream", "1")
            same = got == refc
            ok &= same
            print(f"  stream chunk={chunk:>7s}: {len(got):6d} frames  {'IDENTICAL' if same else 'MISMATCH'}")
        off = run("host_mode_s", p24, "--rate", "2.4", "--coh", "1", "--chunk", big, "--stream", "0")
        same = off == ref24
        ok &= same
        print(f"  single buffer (no IQ kept), option on: {'IDENTICAL to option off' if same else 'MISMATCH'}")
        # ---- switching the look while the stream runs, as the firmware's
        # console commands do (between two runs of the decoder). Before the
        # switch-on and well after the switch-off the output must be exactly
        # what it is with the look off; in between it must add frames, and
        # none that was never sent.
        def samp(l):
            return int(re.search(r"samp=(\d+)", l).group(1))
        nbytes = os.path.getsize(p24) & ~1
        on_at, off_at = (nbytes // 3) & ~1, (2 * nbytes // 3) & ~1
        plain = run("host_mode_s", p24, *soft, "--chunk", "5632", "--stream", "1")
        for chunk in ("5632", "45056", "1000"):
            sw = run("host_mode_s", p24, *soft, "--chunk", chunk, "--stream", "1",
                     "--coh-on-at", str(on_at), "--coh-off-at", str(off_at))
            lo, hi = on_at // 2, off_at // 2 + int(chunk) // 2 + 2000   # a chunk and a few frames of slack
            before = [l for l in sw if samp(l) < lo - 400] == [l for l in plain if samp(l) < lo - 400]
            after = [l for l in sw if samp(l) >= hi] == [l for l in plain if samp(l) >= hi]
            mid_sw = {l.rsplit("hex=", 1)[1] for l in sw if lo <= samp(l) < off_at // 2}
            mid_plain = {l.rsplit("hex=", 1)[1] for l in plain if lo <= samp(l) < off_at // 2}
            stray = ({l.rsplit("hex=", 1)[1] for l in sw} - {l.rsplit("hex=", 1)[1] for l in plain}) - truth24
            good = before and after and len(mid_sw) > len(mid_plain) and not stray
            ok &= good
            print(f"  switched on at 1/3, off at 2/3, chunk={chunk:>6s}: before {'IDENTICAL' if before else 'MISMATCH'}, "
                  f"after {'IDENTICAL' if after else 'MISMATCH'}, between +{len(mid_sw) - len(mid_plain)} frames, "
                  f"{len(stray)} never sent  {'OK' if good else 'MISMATCH'}")
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
