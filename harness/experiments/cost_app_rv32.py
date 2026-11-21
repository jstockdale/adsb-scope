#!/usr/bin/env python3
"""cost_app_rv32.py - what the work around the decoder costs on the
device's processor, before and after coh4, in RISC-V instructions.

"Around the decoder" is what the firmware does with a decoded message and
with the aircraft table: the console line, the SD card row, the distance
for MQTT, and the list and statistics that the display refreshes. Up to
coh3 that was double-precision arithmetic, which the ESP32-P4 does in
software, and most of it ran while the aircraft table was locked.

Builds rv32/app_main.c (copies of the old and the new code, the new one
calling main/examples/adsb_receiver/adsb_fmt.c) with the firmware's
compiler and flags at -Og, as the project is built, runs it under
qemu-riscv32 and counts instructions from qemu's trace. It also checks
that the two versions write the same text, apart from distances and
bearings that single precision rounds differently.

These are instruction counts, not time (see cost_rv32.py).

usage: cost_app_rv32.py
needs: riscv32-esp-elf-gcc on PATH (source ESP-IDF's export.sh), qemu-riscv32
"""
import os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from cost_rv32 import trace, FLAGS
APP = os.path.normpath(os.path.join(HERE, "..", "..", "main", "examples", "adsb_receiver"))


def main():
    for tool, hint in (("riscv32-esp-elf-gcc", "source ESP-IDF v5.4.1's export.sh first"),
                       ("qemu-riscv32", "Debian/Ubuntu: sudo apt install qemu-user")):
        if not shutil.which(tool):
            sys.exit(f"error: {tool} not found  ({hint})")
    tmp = tempfile.mkdtemp(prefix="cost_app_")
    elf = os.path.join(tmp, "app.elf")
    subprocess.run(["riscv32-esp-elf-gcc", *FLAGS, "-Og", "-I", APP, "-o", elf,
                    os.path.join(HERE, "rv32", "app_main.c"), os.path.join(APP, "adsb_fmt.c"), "-lm"], check=True)

    def cost(what, n, reps):
        base, _ = trace(elf, "none", str(n))
        tot, _ = trace(elf, what, str(n), str(reps))
        return (tot - base) / reps

    old = subprocess.run(["qemu-riscv32", elf, "show_old", "40"], capture_output=True, text=True).stdout.splitlines()
    new = subprocess.run(["qemu-riscv32", elf, "show_new", "40"], capture_output=True, text=True).stdout.splitlines()
    differ = [(a, b) for a, b in zip(old, new) if a != b]
    print(f"text: {len(old)} lines from each version (a list of 40 aircraft, a console line, an SD row); "
          f"{len(differ) + abs(len(old) - len(new))} differ")
    for a, b in differ[:5]:
        print("  old: " + a + "\n  new: " + b)

    print("\nRISC-V instructions (-Og, firmware flags), 80 % of the aircraft with a position")
    print("\nthe aircraft list, refreshed twice a second while the ADS-B app is open:")
    print(f"  {'aircraft':>8}  {'coh3, all under the lock':>26}  {'coh4 under the lock':>20}  {'coh4 in all':>12}")
    for n in (25, 50, 100, 150, 200, 250):
        o = cost("old_list", n, 2); c = cost("new_list_copy", n, 20); t = cost("new_list", n, 2)
        print(f"  {n:8d}  {o / 1e6:24.2f} M  {c / 1e3:18.1f} k  {t / 1e6:10.2f} M")
    print("\nnearest and farthest (status bar and ADS-B app, once a second or more):")
    print(f"  {'aircraft':>8}  {'coh3, under the lock':>22}  {'coh4 (two distances of it after the lock)':>42}")
    for n in (25, 50, 100, 150, 200, 250):
        o = cost("old_pass", n, 4); c = cost("new_pass", n, 8)
        print(f"  {n:8d}  {o / 1e6:20.2f} M  {c / 1e3:40.1f} k")
    print("\none decoded message: console line, SD card row, distance for MQTT:")
    o = cost("old_msg", 100, 200); c = cost("new_msg", 100, 200)
    print(f"  aircraft with a position     coh3 {o / 1e3:6.1f} k   coh4 {c / 1e3:5.1f} k   ({o / c:.1f} times less)")
    o2 = cost("old_msg_nopos", 100, 200); c2 = cost("new_msg_nopos", 100, 200)
    print(f"  aircraft without one         coh3 {o2 / 1e3:6.1f} k   coh4 {c2 / 1e3:5.1f} k")
    print(f"\nat 300 messages a second, all from aircraft with a position: coh3 {300 * o / 1e6:.1f} M a second, coh4 {300 * c / 1e6:.1f} M")
    print("(the demodulator itself: 149 M per second of signal, results/cost_rv32.txt)")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
