#!/usr/bin/env python3
"""cost_rv32.py - what the decoder costs on the device's processor, in
RISC-V instructions per second of signal.

Builds the decoder with the firmware's own compiler and flags
(riscv32-esp-elf-gcc, rv32imafc / ilp32f, as ESP-IDF v5.4.1 compiles for the
ESP32-P4), links the start of a recording into it, runs it under
qemu-riscv32 and counts the instructions executed, from qemu's own trace of
translated blocks. Start-up (the magnitude table) is measured separately
and subtracted. Two optimisation levels: -Og, which the project is built
at, and -O2, which main/CMakeLists.txt gives the decoder files.

These are instruction counts, not time: the ESP32-P4's cores run at 360 MHz
and how many instructions they finish per cycle on this code is not known
here (cache misses on PSRAM, branches). `adsb status` on the device has
the measured share of a core.

usage: cost_rv32.py FILE.cu8 [--seconds S] [--old DIR ...]
  FILE.cu8     2.4 Msps recording (e.g. DATADIR/madrid_lo.cu8)
  --seconds S  how much of it to decode (default 0.2; the trace is slow)
  --old DIR    also measure the decoder sources in DIR (a checkout of an
               older main/examples/adsb_receiver), for a before / after
needs: riscv32-esp-elf-gcc on PATH (source ESP-IDF's export.sh), qemu-riscv32
       (Debian/Ubuntu: sudo apt install qemu-user)
"""
import argparse, os, shutil, subprocess, sys, tempfile
from collections import Counter
HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.normpath(os.path.join(HERE, "..", "..", "main", "examples", "adsb_receiver"))
FLAGS = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f", "-fno-shrink-wrap", "-fstrict-volatile-bitfields",
         "-fno-jump-tables", "-fno-tree-switch-conversion", "-std=gnu17", "--specs=sim.specs"]
SRCS = ["mode-s.c", "mode_s_2400.c", "mode_s_coh.c"]


def trace(elf, *args):
    """(instructions executed, Counter by function) for one run."""
    p = subprocess.Popen(["qemu-riscv32", "-d", "in_asm,exec,nochain", "-D", "/dev/stdout", elf, *args],
                         stdout=subprocess.PIPE, text=True, errors="replace")
    size, by, cur, total = {}, Counter(), None, 0
    for line in p.stdout:
        if line.startswith("Trace"):
            i = line.find("/")
            pc = int(line[i + 1:line.find("/", i + 1)], 16)
            n = size.get(pc, 0)
            total += n
            by[line.rsplit("]", 1)[1].strip()] += n
        elif line.startswith("IN:"):
            cur = "new"
        elif cur is not None and line.startswith("0x"):
            if cur == "new":
                cur = int(line[2:10], 16)
                size[cur] = 0
            size[cur] += 1
        elif not line.strip():
            cur = None
    if p.wait() != 0:
        sys.exit(f"error: qemu-riscv32 failed on {elf}")
    return total, by


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cu8")
    ap.add_argument("--seconds", type=float, default=0.2)
    ap.add_argument("--old", action="append", default=[])
    a = ap.parse_args()
    # Everything this run needs, checked before the first build.
    for tool, hint in (("riscv32-esp-elf-gcc", "source ESP-IDF v5.4.1's export.sh first"),
                       ("qemu-riscv32", "Debian/Ubuntu: sudo apt install qemu-user")):
        if not shutil.which(tool):
            sys.exit(f"error: {tool} not found  ({hint})")
    if not os.path.isfile(a.cu8):
        sys.exit(f"error: {a.cu8} not found")
    trees = [("this tree", APP)] + [(d, d) for d in a.old]
    for label, d in trees:
        for s in SRCS + ["mode-s.h"]:
            if not os.path.isfile(os.path.join(d, s)):
                sys.exit(f"error: {os.path.join(d, s)} not found")
    nbytes = int(a.seconds * 2.4e6) * 2
    if os.path.getsize(a.cu8) < nbytes:
        sys.exit(f"error: {a.cu8} is shorter than {a.seconds} s")

    with tempfile.TemporaryDirectory() as td:
        sig = os.path.join(td, "sig.cu8")
        with open(a.cu8, "rb") as f, open(sig, "wb") as g:
            g.write(f.read(nbytes))
        print(f"{a.seconds} s of {os.path.basename(a.cu8)}, the firmware's default decoder; "
              "RISC-V instructions per second of signal (millions)\n")
        base = None
        for label, d in trees:
            for opt in ("-Og", "-O2"):
                elf = os.path.join(td, "dec.elf")
                r = subprocess.run(["riscv32-esp-elf-gcc", *FLAGS, opt, f'-DSIGFILE="{sig}"', f"-I{d}", "-o", elf,
                                    os.path.join(HERE, "rv32", "rv_main.c"), *[os.path.join(d, s) for s in SRCS], "-lm"],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    sys.exit(f"error: build failed ({label}, {opt}):\n{r.stderr[-600:]}")
                out = subprocess.run(["qemu-riscv32", elf], capture_output=True, text=True).stdout.strip()
                init, _ = trace(elf, "init")
                total, by = trace(elf)
                init_by = trace(elf, "init")[1]
                per_s = (total - init) / a.seconds / 1e6
                if base is None:
                    base = per_s
                print(f"{label}, {opt}: {per_s:6.0f} M/s  ({100 * per_s / base:.0f}% of the first row); start-up "
                      f"{init / 1e6:.0f} M once; frames/CRC-ok/candidates: {out}")
                dec = Counter({k: v - init_by.get(k, 0) for k, v in by.items()})
                top = ", ".join(f"{k} {100 * v / (total - init):.0f}%" for k, v in dec.most_common(5))
                print(f"    {top}")


if __name__ == "__main__":
    main()
