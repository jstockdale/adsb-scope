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
| `Makefile` | `make` builds `host_mode_s`, `host_mode_s_df18off`, `host_mode_s_baseline`, `test_fix_c`, `test_batch`, `test_ring` |
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
| `results/` | CSVs and plot from the runs reported in `docs/DECODER.md` |

## Checks that need nothing but gcc and Python

```
cd harness
make check     # table proofs, exhaustive fixer tests, stream equivalence, parity
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
echo "done - results in $R"
)
```

`results/coh_collide.txt` is the three `collide.py` outputs in one file.
To set the cost of this build against an older one, build that one's
`host_mode_s` elsewhere and add `--old path/to/it` to `cost_coh.py`.

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
