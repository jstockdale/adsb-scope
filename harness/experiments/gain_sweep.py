#!/usr/bin/env python3
"""gain_sweep.py - what the receiver's gain does to the decoder, measured.

Takes a recording with more dynamic range than the dongle's ADC (the Madrid
capture: 16-bit samples, 41 dB between its noise and its full scale) and
puts it through an 8-bit ADC at a series of gains, 3 dB apart. For each
gain: the level statistics the firmware keeps (noise, headroom, share of
samples on a rail), the yield (frames, extended squitters, position
messages per second), the share of decoded frames at full scale ("loud"),
the squitter error rate (lost / seen), and - for comparison - the figure the
older gain loop steered on: candidates that failed over all candidates.

This models the ADC only. A real tuner's noise figure also changes with its
gain setting, which a recording made at one setting cannot show; the
device's own sweep (`adsb gain sweep`) measures that.

usage: gain_sweep.py madrid.sigmf-meta [--model OUT.txt] [--from DB] [--to DB]
  --model OUT   instead of the table, write the per-window statistics at
                1 dB steps that harness/gain/sim_gain replays (slow: one
                decode of the recording per dB)
needs: numpy, scipy; `make` in harness/ first
"""
import argparse, os, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
BIN = os.path.join(HERE, "..", "host_mode_s")
DEFAULT = ["--soft", "8", "--strict", "1", "--known-icao", "1", "--coh", "1"]   # the firmware's default decoder
REF_COUNTS = 1.32   # noise, in ADC counts per rail, at 0 dB of this script's scale (the test set's madrid_lo)


def stats(line):
    return dict(kv.split("=") for kv in line.split() if "=" in kv)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("meta")
    ap.add_argument("--model", default="")
    ap.add_argument("--from", dest="lo", type=int, default=None)
    ap.add_argument("--to", dest="hi", type=int, default=None)
    a = ap.parse_args()
    # Everything this run needs, checked before the first (slow) step.
    if not os.access(BIN, os.X_OK):
        sys.exit(f"error: {BIN} not built; run `make` in {os.path.dirname(BIN)} first")
    from dsp import np, RATES            # exits with an install hint if numpy/scipy are missing
    from decimate import open_input, blocks, resample_blocks
    data, dt = open_input(a.meta)        # exits if the recording is not there or not usable
    lo = a.lo if a.lo is not None else (-30 if a.model else -18)
    hi = a.hi if a.hi is not None else 33
    step = 1 if a.model else 3
    if a.model:
        try:
            open(a.model, "a").close()
        except OSError as e:
            sys.exit(f"error: cannot write {a.model}: {e}")

    up, down = RATES["2.4"]
    y = np.concatenate([b.astype(np.complex64) for b in resample_blocks(blocks(data, dt), up, down)])
    iq = np.empty(2 * len(y), np.float32)
    iq[0::2] = y.real
    iq[1::2] = y.imag
    secs = len(y) / 2.4e6
    # Noise of the recording itself: half of the rail samples lie under 0.6745 sigma.
    sigma0 = float(np.median(np.abs(iq[: 8_000_000]))) / 0.6745
    k0 = REF_COUNTS / sigma0
    del y

    out = open(a.model, "w") if a.model else None
    if out:
        out.write("# Gain model for harness/gain/sim_gain: the Madrid capture through an 8-bit ADC at 1 dB steps,\n"
                  "# decoded with the firmware's default decoder, in 2.5 s windows. Made by experiments/gain_sweep.py.\n"
                  f"# 0 dB puts the recording's noise at {REF_COUNTS} counts rms per rail.\n"
                  "# db win secs ok es_ok es_lost loud pos noise clip sig delta\n")
    else:
        print(f"Recording: {secs:.0f} s, noise {20 * np.log10(sigma0):.1f} dB below its full scale. "
              f"Decoder: {' '.join(DEFAULT)}")
        print("  dB = ADC gain relative to the test set's madrid_lo; noise in ADC counts rms per rail;")
        print("  err = squitters lost / seen; old err = failed candidates / all candidates\n")
        print("   dB  noise headroom  clip%   msg/s  ES/s  pos/s  loud%   err%  old err%")
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "g.cu8")
        for d in range(lo, hi + 1, step):
            k = k0 * 10 ** (d / 20)
            np.clip(np.round(iq * k) + 127.0, 0, 255).astype(np.uint8).tofile(path)
            r = subprocess.run([BIN, path, "--rate", "2.4", "--stream", "1"] + DEFAULT + (["--win", "2.5"] if out else []),
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"error: host_mode_s failed at {d:+d} dB: {r.stderr.strip()[-300:]}")
            if out:
                wins = [stats(l) for l in r.stderr.splitlines() if l.startswith("WIN ")]
                for i, w in enumerate(wins):
                    out.write(f"{d} {i} {w['secs']} {w['ok']} {w['es_ok']} {w['es_lost']} {w['loud']} {w['pos']} "
                              f"{w['noise']} {w['clip']} {w['sig']} {w['delta']}\n")
                out.flush()
                print(f"{d:+d} dB: {len(wins)} windows", file=sys.stderr, flush=True)
                continue
            st = stats([l for l in r.stderr.splitlines() if l.startswith("STATS")][0])
            ok, fail = int(st["crc_ok"]), int(st["crc_fail"])
            eo, el = int(st["es_ok"]), int(st["es_lost"])
            noise = float(st["noise"])
            print(f"  {d:+3d} {noise:6.2f} {20 * np.log10(127.5 / noise):7.1f}  {100 * float(st['clip']):6.3f} "
                  f"{ok / secs:7.1f} {eo / secs:5.1f} {int(st['pos']) / secs:6.1f} {100 * int(st['loud']) / max(ok, 1):6.1f} "
                  f"{100 * el / max(eo + el, 1):6.1f} {100 * fail / max(ok + fail, 1):8.1f}", flush=True)
    if out:
        out.close()


if __name__ == "__main__":
    main()
