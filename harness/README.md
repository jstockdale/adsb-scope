# harness/ – host-side measurement harness for the Mode-S decoder

Builds the firmware's `mode-s.c` on a PC, feeds it recorded or simulated
RF exactly the way `class_driver.c` does, and scores the output against
ground truth. Nothing here is compiled into the firmware.

`host_mode_s --cache-clock 1` ages the cache of recently seen addresses by
sample count instead of wall-clock time, as the device does when it
replays a recording (`adsb replay`, `../docs/UPLINK.md` 9.3); use it with
`--df18 0` to compare a count with the device's.

Results and their interpretation: `../docs/DECODER.md`; for the MQTT
feeder tests, `../docs/UPLINK.md`.

## What is here

| file | role |
| --- | --- |
| `gen_syndrome_tables.py` | generates `mode_s_syndromes.h`; refuses to emit unless its proofs pass |
| `host_mode_s.c` | drives the decoder: historical per-chunk path (`--stream 0`) or streaming path (`--stream 1`), all decoder options as flags |
| `baseline/` | the decoder exactly as of the vendor cut (commit `b8dd639`) + two instrumentation lines – the A/B reference |
| `Makefile` | `make` builds `host_mode_s`, `host_mode_s_df18off`, `host_mode_s_baseline`, `test_fix_c`, `test_batch`, `test_ring`, `test_fmt`, `test_link`, `sim_device` |
| `test_fix_c.c` | exhaustive 1-bit / 2-bit correction tests against the real fixers |
| `ring/test_ring.c` | the sample ring between USB reader and decoder (`sample_ring.c`): directed cases, a random run against a model, and a two-thread run that overflows it on purpose; part of `make check` |
| `feeder/test_batch.c`, `feeder/check_feeder.py` | MQTT feeder: per-aircraft batching and the JSON serialiser (`mqtt_batch.c`), every produced message validated with Python's JSON parser; part of `make check` |
| `feeder/coverage_sim.py` | model of how much traffic the old and new feeder uplink → `results/feeder_coverage.txt` |
| `feeder/old_batch_size.c` | size of the pre-r7 feeder's messages, built with ESP-IDF's own cJSON (`make old_batch_size IDF_PATH=...`) |
| `synth.py` | minimal Mode-S stream generator at 2.0 or 2.4 Msps (ideal pulses + noise) so the checks need no external tools |
| `check_stream.py` | proves streaming ≡ single buffer at many chunk sizes for both demodulators, pristine ≡ current with options off, and that samples lost mid-stream (`mode_s_stream_gap`) give exactly the frames of the two sides decoded separately |
| `bench_fix.py` | fixer micro-benchmark, historical vs syndrome |
| `dsp.py`, `decimate.py` | 8 Msps SigMF (cf32 / ci16) → 2.0 / 2.4 / 3.2 Msps cu8; block path self-tested against whole-file |
| `scenario_curve.py` | one simulated scenario → decode probability per CN0 bin, every decoder variant |
| `sweep.py` | isolated single-frame sweep (slow, no interference) – same CSV format |
| `plot_sweep.py` | merge CSVs, print 50 % / 90 % points, plot |
| `scenario_score.py` | score any decoder output against a SigMF truth file |
| `phase_diag.py` | decode rate vs sub-sample arrival phase |
| `count_es.py` | summarise decoder output when there is no truth (real captures) |
| `experiments/shift_grid.py` | the half-sample dual-grid experiment |
| `experiments/prep_2400.py`, `eval_2400.py` | build the fixed 2.4 Msps test set, and score the 2.4 Msps demodulator on it next to dump1090-fa |
| `experiments/toa_error.py` | frame time-of-arrival error of the 2.4 Msps demodulator against simulator truth (uses the `prep_2400.py` test set) |
| `experiments/rate_24.py` | 2.0 Msps (single / dual grid) vs a reference 2.4 Msps demodulator (dump1090-fa, set `DUMP1090`) on identical inputs |
| `experiments/prep_dense.py` | adds two heavy-traffic scenarios (1,000 and 2,000 messages/s) to the `prep_2400.py` test set |
| `experiments/eval_coh.py` | the coherent second look (`mode_s_coh.c`, `host_mode_s --coh 1`): decode with and without it in eight configurations, and how often it runs → `results/coh_eval.txt` |
| `experiments/collide.py` | pairs of overlapping frames at a chosen strength gap – which of the two comes out; `--stress N` counts false frames over N collisions → `results/coh_collide.txt` |
| `experiments/xcheck_madrid.py` | the Madrid capture has no ground truth: compares our frames with more1090's decodes of the 8 Msps original (set `MORE1090`) → `results/coh_madrid_xcheck.txt` |
| `experiments/cost_coh.py` | instruction counts with and without the coherent second look (needs valgrind) → `results/coh_cost.txt` |
| `experiments/replay_expect.py` | what `adsb replay <file>` should report on the device in each console setting (`adsb dec`, `adsb coh`): runs the host decoder on the same file the way the device replays it → `results/coh_replay_expect.txt` |
| `experiments/gain_sweep.py` | the Madrid capture through an 8-bit ADC at a series of gains: level statistics, yield, loud share and error rates at each → `results/gain_sweep.txt`; `--model` writes the per-window table `sim_gain` replays (`gain/madrid_model.txt`) |
| `experiments/es_lost_truth.py` | the decoder's squitter error rate (lost / seen) against what the simulator really sent → `results/gain_es_lost_truth.txt` |
| `gain/sim_gain.c`, `gain/check_gain.sh` | the firmware's gain loop (`gain_ctl.c`) on model receivers; part of `make check` → `results/gain_sim.txt` |
| `experiments/cost_rv32.py` | what the decoder costs in the device's own instructions: built with the firmware's compiler, run under qemu-riscv32 (needs both) → `results/cost_rv32.txt` |
| `fmt/test_fmt.c` | number text and great-circle distance without double arithmetic (`adsb_fmt.c`) against `printf` and the old `haversine()`; part of `make check` → `results/fmt.txt` |
| `link/test_link.c` | the console-speed state machine (`console_link.c`) through its cases; part of `make check` |
| `link/sim_device.c` | a stand-in for the device's console on a pseudo-terminal, running `console_link.c`: wire speed, garbage on a speed mismatch, reset on open, older firmware, download mode |
| `link/check_fastlog.py` | `../tools/adsb-fastlog.py` and the bare speed protocol against `sim_device`, ten cases; part of `make check` → `results/link.txt` |
| `link/check_page.js` | the web app's own script (`adsb_scope.htm`, run in Node) against `sim_device`, nine cases; part of `make check` when Node is installed → `results/link.txt` |
| `link/check_page_flash.js` | the web app's flashing (`doFlash()` as it is) with the **real esptool-js 0.5.4** against an emulated chip: "keep settings", the speeds, a loader that does not answer, the restart – thirteen cases (`IMAGE=<merged .bin>` for a real image). Needs the esptool-js bundle: `make esptool-js` (npm; checked against a pinned sha256), skipped with a notice without it; part of `make check`, its quick cases of `make check-quick` → `results/link.txt` |
| `link/fake_esp.js` | the emulated chip: reset and boot-mode lines, the ROM loader's and the flasher stub's protocol, a flash memory erased as the stub erases |
| `wifi/test_wifi_sup.c` | the WiFi supervisor (`wifi_sup.c`: which saved network, and when to try again) driven by scripted events – order of candidates, never giving up, loss, stalls, a dead link, a manual choice, edits of the list; part of `make check` and `make check-quick` → `results/wifi.txt` |
| `link/page_env.js` | loads the web app's whole script into Node with the browser's objects stubbed (and, for the profile, counted) |
| `page/profile_page.js` | the web app's own JavaScript under a made-up device log, in Node: time per function, and what it asks of the DOM and the map, per second → `results/page.txt` |
| `page/browser_profile.js` | the same load on the page in a real headless Chrome: script, style, layout, layouts and frames per second, and the page's own meter next to them; fails on a script error at load. Needs a Chrome and `make page-deps` (npm). `PAGE=<file>` measures another version → `results/page.txt` |
| `page/fake_log.js` | the made-up device log both use |
| `experiments/cost_app_rv32.py` | what a decoded message and a list refresh cost in the device's instructions, before and after coh4 (needs the ESP-IDF toolchain and qemu-riscv32) → `results/cost_app_rv32.txt` |
| `results/` | CSVs and plot from the runs reported in `docs/DECODER.md` |

## Checks that need nothing but gcc and Python

```
cd harness
make check     # table proofs, exhaustive fixer tests, stream equivalence, parity, the gain loop on model receivers
make bench     # fixer micro-benchmark
```

If you have sourced ESP-IDF's `export.sh` in this shell, `python3` is the
IDF environment's interpreter, which normally has no numpy, and the check
stops with a missing-dependency message. Use a fresh shell, or
`make check PYTHON=/usr/bin/python3`.

## Full reproduction

Needs: gcc, make, git, curl, Python 3 with numpy + scipy (+ matplotlib for
the plot), a Rust toolchain (to build the more1090 simulator), ~3 GB of
scratch disk, and about 15 minutes on one core. The block below checks all
of that before doing anything, and runs in a subshell so a failure cannot
close your terminal. Edit the two variables at the top, then paste.

```sh
(
REPO="$HOME/adsb-scope"            # your checkout, on branch decoder-m1090
WORK="$HOME/adsb-harness-work"     # scratch space, ~3 GB

fail() { echo "STOP: $1"; exit 1; }   # leaves only this subshell
for c in gcc make python3 git curl; do
  command -v "$c" >/dev/null 2>&1 || fail "missing '$c'  (Debian/Ubuntu: sudo apt install build-essential python3 git curl)"
done
python3 -c "import numpy, scipy" 2>/dev/null || fail "missing Python packages  (pip install numpy scipy matplotlib)"
command -v cargo >/dev/null 2>&1 || fail "missing Rust toolchain  (curl -sSf https://sh.rustup.rs | sh ; then open a new shell)"
[ -f "$REPO/harness/Makefile" ] || fail "REPO=$REPO does not contain harness/Makefile"
mkdir -p "$WORK/results" || fail "cannot create $WORK"
set -e

# 1. decoder builds + self-checks
cd "$REPO/harness"
make
make check

# 2. more1090 v0.2.0 (simulator)
[ -d "$WORK/more1090" ] || git clone https://github.com/daniestevez/more1090.git "$WORK/more1090"
( cd "$WORK/more1090" && git checkout -q v0.2.0 && cargo build --release )
export MORE1090_SIM="$WORK/more1090/target/release/more1090-simulator"

# 3. sensitivity curve: three distance rings = three CN0 ranges
CSV="$WORK/results/curve.csv"; rm -f "$CSV"
python3 scenario_curve.py --name weak   --min-distance 120 --max-distance 300 --max-cn0 78.5 --k 8000 --seed 13 --csv "$CSV"
python3 scenario_curve.py --name mid    --min-distance 40  --max-distance 120 --max-cn0 88   --k 8000 --seed 12 --csv "$CSV"
python3 scenario_curve.py --name strong --min-distance 10  --max-distance 40  --max-cn0 100  --k 2000 --seed 11 --csv "$CSV" --keep
python3 plot_sweep.py "$CSV" --png "$WORK/results/curve.png"

# 4. timing-phase diagnosis and the dual-grid experiment (simulated, strong signals)
./host_mode_s "$WORK/results/work/strong.cu8" --stream 1 --check-crc 0 > "$WORK/results/strong_all.txt" 2>/dev/null
python3 phase_diag.py "$WORK/results/work/strong.sigmf-meta" "$WORK/results/strong_all.txt"
python3 experiments/shift_grid.py "$WORK/results/work/strong.cu8" --truth "$WORK/results/work/strong.sigmf-meta"

# 5. real capture (Madrid, 640 MB download, Zenodo record 23360412)
Z="https://zenodo.org/api/records/23360412/files/ADS-B_8Msps_2026-08-27T08:36:39"
[ -f "$WORK/madrid.sigmf-meta" ] || curl -L -o "$WORK/madrid.sigmf-meta" "$Z.sigmf-meta/content"
[ -f "$WORK/madrid.sigmf-data" ] || curl -L -o "$WORK/madrid.sigmf-data" "$Z.sigmf-data/content"
python3 decimate.py "$WORK/madrid.sigmf-meta" "$WORK/madrid_2M.cu8" --rate 2.0 --scale auto
python3 check_stream.py "$WORK/madrid_2M.cu8"
./host_mode_s_baseline "$WORK/madrid_2M.cu8"            > "$WORK/m0.txt" 2>/dev/null; python3 count_es.py "$WORK/m0.txt" "pristine"
./host_mode_s "$WORK/madrid_2M.cu8" --stream 1          > "$WORK/m1.txt" 2>/dev/null; python3 count_es.py "$WORK/m1.txt" "current"
./host_mode_s "$WORK/madrid_2M.cu8" --stream 1 --fix2 1 --strict 1 --known-icao 1 > "$WORK/m2.txt" 2>/dev/null; python3 count_es.py "$WORK/m2.txt" "fix2+strict+known-icao"
python3 experiments/shift_grid.py "$WORK/madrid_2M.cu8" --methods linear,cubic,sinc8,sinc16,sinc
python3 experiments/shift_grid.py "$WORK/madrid_2M.cu8" --methods cubic,sinc8 --decoder-args "--fix2 1 --strict 1 --known-icao 1"
echo "done – results in $WORK/results"
)
```

`scenario_curve.py` and `sweep.py` *append* to their CSV, so remove the
file (as step 3 does) before repeating a run with the same seeds.

## The coherent second look (`../docs/DECODER.md` section 5.7)

Everything in `results/coh_*.txt`, from nothing. Needs what the block above
needs, plus valgrind; about 4 GB of scratch disk and 25 minutes on one
core. It reuses more1090 and the Madrid download if `WORK` is the same
directory as above. Checks first, runs in a subshell; edit the two
variables, then paste.

```sh
(
REPO="$HOME/adsb-scope"            # your checkout, on a branch that has mode_s_coh.c
WORK="$HOME/adsb-harness-work"     # scratch space, ~4 GB

fail() { echo "STOP: $1"; exit 1; }   # leaves only this subshell
for c in gcc make python3 git curl valgrind; do
  command -v "$c" >/dev/null 2>&1 || fail "missing '$c'  (Debian/Ubuntu: sudo apt install build-essential python3 git curl valgrind)"
done
python3 -c "import numpy, scipy" 2>/dev/null || fail "missing Python packages  (pip install numpy scipy)"
command -v cargo >/dev/null 2>&1 || fail "missing Rust toolchain  (curl -sSf https://sh.rustup.rs | sh ; then open a new shell)"
[ -f "$REPO/harness/experiments/eval_coh.py" ] || fail "REPO=$REPO has no harness/experiments/eval_coh.py - wrong checkout or branch"
[ -f "$REPO/main/examples/adsb_receiver/mode_s_coh.c" ] || fail "REPO=$REPO has no mode_s_coh.c - wrong branch"
mkdir -p "$WORK/set" "$WORK/results" || fail "cannot create $WORK"
set -e
R="$WORK/results"

# 1. decoder builds + self-checks (includes the coherent path at every chunk size)
cd "$REPO/harness"
make
make check

# 2. more1090 v0.2.0: its simulator makes the test data, its receiver is the reference.
#    Some stable Rust releases refuse one library feature it uses; the second form builds there.
[ -d "$WORK/more1090" ] || git clone https://github.com/daniestevez/more1090.git "$WORK/more1090"
( cd "$WORK/more1090" && git checkout -q v0.2.0 &&
  { cargo build --release ||
    RUSTC_BOOTSTRAP=1 RUSTFLAGS="-Zcrate-attr=feature(float_algebraic)" cargo build --release; } )
export MORE1090_SIM="$WORK/more1090/target/release/more1090-simulator"
export MORE1090="$WORK/more1090/target/release/more1090"

# 3. the Madrid capture (640 MB, Zenodo record 23360412) and the test set
Z="https://zenodo.org/api/records/23360412/files/ADS-B_8Msps_2026-08-27T08:36:39"
[ -f "$WORK/madrid.sigmf-meta" ] || curl -L -o "$WORK/madrid.sigmf-meta" "$Z.sigmf-meta/content"
[ -f "$WORK/madrid.sigmf-data" ] || curl -L -o "$WORK/madrid.sigmf-data" "$Z.sigmf-data/content"
python3 experiments/prep_2400.py "$WORK/set" --madrid "$WORK/madrid.sigmf-meta"
python3 experiments/prep_dense.py "$WORK/set"

# 4. the measurements
python3 experiments/eval_coh.py "$WORK/set"                                > "$R/coh_eval.txt";          cat "$R/coh_eval.txt"
python3 experiments/xcheck_madrid.py "$WORK/madrid.sigmf-meta" "$WORK/set" > "$R/coh_madrid_xcheck.txt"; cat "$R/coh_madrid_xcheck.txt"
python3 experiments/collide.py                   --out "$R/coh_collide_full.txt"
python3 experiments/collide.py --tau-us 100,120  --out "$R/coh_collide_tail.txt"
python3 experiments/collide.py --stress 400000   --out "$R/coh_collide_stress.txt"
python3 experiments/cost_coh.py "$WORK/set"                                > "$R/coh_cost.txt";          cat "$R/coh_cost.txt"
python3 experiments/replay_expect.py "$WORK/set/dense.cu8" "$WORK/set/madrid_lo.cu8" > "$R/coh_replay_expect.txt"; cat "$R/coh_replay_expect.txt"
echo "done - results in $R"
)
```

`results/coh_collide.txt` is the three `collide.py` outputs in one file.
To set the cost of this build against an older one, build that one's
`host_mode_s` elsewhere and add `--old path/to/it` to `cost_coh.py`.

## Gain and cost (`../docs/DECODER.md` sections 5.8 and 5.9)

Everything in `results/gain_*.txt` and `results/cost_rv32.txt`. Needs the
test set and the Madrid download of the block above (`WORK` the same
directory); the last step also needs ESP-IDF v5.4.1's toolchain and
qemu-riscv32, and is skipped with a note if either is missing. About 15
minutes. Checks first, runs in a subshell; edit the three variables, then
paste.

```sh
(
REPO="$HOME/adsb-scope"            # your checkout, on a branch that has gain_ctl.c
WORK="$HOME/adsb-harness-work"     # where the block above left the test set and the capture
IDF_EXPORT="$HOME/esp/esp-idf/export.sh"   # ESP-IDF v5.4.1's export.sh (only for the last step)

fail() { echo "STOP: $1"; exit 1; }   # leaves only this subshell
for c in gcc make python3; do
  command -v "$c" >/dev/null 2>&1 || fail "missing '$c'  (Debian/Ubuntu: sudo apt install build-essential python3)"
done
python3 -c "import numpy, scipy" 2>/dev/null || fail "missing Python packages  (pip install numpy scipy) - or this shell has ESP-IDF's python: use a fresh one"
[ -f "$REPO/main/examples/adsb_receiver/gain_ctl.c" ] || fail "REPO=$REPO has no gain_ctl.c - wrong checkout or branch"
[ -f "$WORK/madrid.sigmf-meta" ] && [ -f "$WORK/madrid.sigmf-data" ] || fail "no Madrid capture in $WORK - run the block above first"
[ -f "$WORK/set/dense.cu8" ] && [ -f "$WORK/set/madrid_lo.cu8" ] || fail "no test set in $WORK/set - run the block above first"
mkdir -p "$WORK/results" || fail "cannot create $WORK/results"
set -e
R="$WORK/results"
cd "$REPO/harness"
make                                                                       # host_mode_s, sim_gain
python3 experiments/gain_sweep.py "$WORK/madrid.sigmf-meta"               > "$R/gain_sweep.txt";         cat "$R/gain_sweep.txt"
python3 experiments/es_lost_truth.py "$WORK/set"                           > "$R/gain_es_lost_truth.txt"; cat "$R/gain_es_lost_truth.txt"
sh gain/check_gain.sh                                                      > "$R/gain_sim.txt";           tail -1 "$R/gain_sim.txt"
# the model sim_gain replays (in the repository already; this remakes it, 64 decodes of the capture):
#   python3 experiments/gain_sweep.py "$WORK/madrid.sigmf-meta" --model gain/madrid_model.txt
if [ -f "$IDF_EXPORT" ] && command -v qemu-riscv32 >/dev/null 2>&1; then
  ( . "$IDF_EXPORT" >/dev/null 2>&1 && python3 experiments/cost_rv32.py "$WORK/set/madrid_lo.cu8" ) > "$R/cost_rv32.txt"; cat "$R/cost_rv32.txt"
else
  echo "note: skipped cost_rv32.py - needs $IDF_EXPORT and qemu-riscv32 (Debian/Ubuntu: sudo apt install qemu-user)"
fi
echo "done - results in $R"
)
```

To watch the loop work: `./sim_gain gain/madrid_model.txt --site 21 --hours 1 --log`
prints its log lines on a model site whose top gain step is 21 dB too hot.
`--site 0` is a site where the top step is right.

## Around the decoder, the console link and the web app (`../docs/UPLINK.md` sections 9.7 to 9.9)

What coh4 added, and how each number in `results/fmt.txt`,
`results/link.txt` and `results/cost_app_rv32.txt` is made. No recordings
needed. Three commands, each complete in itself; the first two need gcc,
make and Python 3, the page test in the second also Node 18 or later
(without it that one test is skipped, and says so).

```sh
cd ~/adsb-scope/harness && make check-quick
```

Half a minute. Everything `make check` runs except the two below,
including `test_fmt` (number text against `printf`, distance and bearing
against the old double-precision `haversine()`), `test_link` (the
console-speed state machine) and, with Node and the esptool-js bundle
(`make esptool-js`, once), the cases of `link/check_page_flash.js` that
need no waiting.
`./test_fmt --report` prints the figures.

```sh
cd ~/adsb-scope/harness && make check
```

About six minutes more: `link/check_fastlog.py` runs
`../tools/adsb-fastlog.py` and the bare protocol against `sim_device` – a
stand-in for the device on a pseudo-terminal, running the firmware's own
`console_link.c` – and `link/check_page.js` runs the web app's own script
against the same stand-in. If your Python is ESP-IDF's (after `export.sh`),
add `PYTHON=/usr/bin/python3`.

```sh
. ~/esp/esp-idf/export.sh && cd ~/adsb-scope/harness && python3 experiments/cost_app_rv32.py
```

A few minutes. What a decoded message and a list refresh cost in RISC-V
instructions, before and after; needs ESP-IDF v5.4.1's toolchain and
qemu-riscv32 (`sudo apt install qemu-user`), and says so if either is
missing.

```sh
cd ~/adsb-scope/harness && make esptool-js && make page-deps      # once; npm and the network
cd ~/adsb-scope/harness && make page-profile
```

The first two fetch what is not in the repository: the esptool-js bundle
the web app loads (for `link/check_page_flash.js`, which `make check` then
runs in full: a minute and a half), and the browser driver with local
copies of the page's libraries. `make page-profile` then measures the web
app under a made-up device log, first its own JavaScript in Node, then
the whole page in a headless Chrome (`CHROME=<path>` if it is not on the
PATH) – `../docs/UPLINK.md` section 9.9, `results/page.txt`.

To play with the stand-in by hand: `./sim_device --rate 300 --reset-on-open`
prints the path of a terminal (`/dev/pts/N`); `python3
../tools/adsb-fastlog.py --port /dev/pts/N --seconds 10` logs from it.

## Things to know when reading the numbers

- **CN0** is peak-power referenced, as in more1090. It is not SNR in any
  particular bandwidth.
- **`--k`** is the gain from the simulator's floating-point amplitude to
  8-bit counts. The decoder has fixed magnitude thresholds, so the
  absolute CN0 at which it starts decoding moves with `--k`. `--k 8000`
  puts simulated noise at 7.8 LSB rms per rail and clips above about
  88 dB·Hz; the strong-signal run uses `--k 2000` to stay unclipped.
  Compare decoder variants at the same `--k`.
- **`host_mode_s` delivers DF18 by default** (`--df18 1`) so it can be
  scored; the firmware default is off.
- **The front end is a model.** A generic anti-alias filter and 8-bit
  quantiser stand in for the RTL2832U. See the caveats in
  `../docs/DECODER.md`.
- **Regenerating the tables:** `python3 gen_syndrome_tables.py` rewrites
  `../main/examples/adsb_receiver/mode_s_syndromes.h`. `make check`
  verifies the committed header is byte-identical to a fresh generation.
