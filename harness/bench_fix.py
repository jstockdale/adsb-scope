#!/usr/bin/env python3
"""bench_fix.py - isolated cost of the error-correction functions:
pristine brute force (baseline/) vs syndrome lookup (current)."""
import ctypes, os, random, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..", "main", "examples", "adsb_receiver")
def build(name, args):
    so = os.path.join(HERE, name)
    subprocess.run(["gcc", "-std=gnu17", "-O2", "-shared", "-fPIC", *args, "-o", so, "-lm"], check=True)
    return ctypes.CDLL(so)
libs = [("historical brute force", build("libbaseline.so", ["-include", "time.h", "-I" + os.path.join(HERE, "baseline"), os.path.join(HERE, "baseline", "mode-s.c")])),
        ("syndrome lookup", build("libcurrent.so", ["-I" + APP, os.path.join(APP, "mode-s.c"), os.path.join(APP, "mode_s_2400.c")]))]
random.seed(7)
frames = [bytes(random.randrange(256) for _ in range(14)) for _ in range(1500)]
res = {}
for name, L in libs:
    for fn in ("fix_single_bit_errors", "fix_two_bits_errors"):
        f = getattr(L, fn); f.argtypes = [ctypes.c_char_p, ctypes.c_int]
        bufs = [ctypes.create_string_buffer(x, 14) for x in frames]
        t0 = time.perf_counter()
        for b in bufs: f(b, 112)
        res[(name, fn)] = 1e6 * (time.perf_counter() - t0) / len(bufs)
print(f"{'':24s} {'1-bit us/call':>14s} {'2-bit us/call':>14s}   (uncorrectable 112-bit frames; includes ctypes call overhead)")
for name, _ in libs:
    print(f"{name:24s} {res[(name,'fix_single_bit_errors')]:14.2f} {res[(name,'fix_two_bits_errors')]:14.2f}")
a, b = libs[0][0], libs[1][0]
print(f"speedup: 1-bit {res[(a,'fix_single_bit_errors')]/res[(b,'fix_single_bit_errors')]:.0f}x, "
      f"2-bit {res[(a,'fix_two_bits_errors')]/res[(b,'fix_two_bits_errors')]:.0f}x")
