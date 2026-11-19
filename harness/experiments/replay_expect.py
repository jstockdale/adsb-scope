#!/usr/bin/env python3
"""replay_expect.py - what `adsb replay <file>` should report on the device.

Runs the host build of the firmware decoder on a recording the way the
device replays it (2.4 Msps, streaming, DF18 not delivered, the cache of
known addresses aged by the recording's own clock) in each of the console's
decoder settings, and prints the message count and repair counters to set
against the device's two REPLAY: lines.

The device should report these counts (less any frames with an all-zero
address). The coherent look works in single-precision float, and the P4's
compiler and C library need not round every operation as a PC's do, so
with the look on a difference of a frame or two would not be alarming;
more would be a finding. (Two PC builds that round differently - with and
without fused multiply-add - give identical output on the three files
tried: results/coh_replay_expect.txt.)

usage: replay_expect.py FILE.cu8 [FILE.cu8 ...]
  FILE: raw unsigned 8-bit I/Q at 2.4 Msps - what `rtl_sdr -s 2400000`
        writes, and what prep_2400.py / prep_dense.py make
needs: `make` in harness/ first
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("HOST_MODE_S", os.path.join(HERE, "..", "host_mode_s"))
DEVICE = ["--rate", "2.4", "--stream", "1", "--df18", "0", "--cache-clock", "1"]
SOFT = ["--soft", "8", "--strict", "1", "--known-icao", "1"]
PRESETS = [
    ("adsb dec default ; adsb coh off", []),
    ("adsb dec default ; adsb coh on", ["--coh", "1"]),
    ("adsb dec soft    ; adsb coh off", SOFT),
    ("adsb dec soft    ; adsb coh on   (boot default)", SOFT + ["--coh", "1"]),
    ("adsb dec fix2    ; adsb coh off", SOFT + ["--fix2", "1"]),
    ("adsb dec fix2    ; adsb coh on", SOFT + ["--fix2", "1", "--coh", "1"]),
]


def main():
    files = sys.argv[1:]
    if not files or files[0] in ("-h", "--help"):
        sys.exit(__doc__)
    if not os.access(BIN, os.X_OK):
        sys.exit(f"error: {BIN} not built; run `make` in {os.path.dirname(os.path.abspath(BIN))} first")
    for f in files:
        if not os.path.isfile(f):
            sys.exit(f"error: no such file: {f}")
        if os.path.getsize(f) < 4800:
            sys.exit(f"error: {f} is shorter than a millisecond of samples")
    for f in files:
        secs = os.path.getsize(f) / 2 / 2.4e6
        print(f"{f}: {os.path.getsize(f)} bytes, {secs:.1f} s of signal")
        print(f"  {'console setting':<50}{'messages':>9}{'1-bit':>7}{'soft':>6}{'2-bit':>7}"
              f"{'re-slices':>11}{'stopped early':>15}{'recovered':>11}")
        for label, args in PRESETS:
            r = subprocess.run([BIN, f] + DEVICE + args, capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"error: host_mode_s failed: {r.stderr.strip()}")
            msgs = sum(1 for l in r.stdout.splitlines() if l.startswith("MSG "))
            st = dict(kv.split("=") for l in r.stderr.splitlines() if "STATS" in l
                      for kv in l.split("STATS", 1)[1].split() if "=" in kv)
            print(f"  {label:<50}{msgs:>9}{st.get('fix1', '?'):>7}{st.get('soft', '?'):>6}{st.get('fix2', '?'):>7}"
                  f"{st.get('coh_try', '?'):>11}{st.get('coh_abort', '?'):>15}{st.get('coh_ok', '?'):>11}", flush=True)
        print()


if __name__ == "__main__":
    main()
