# DECODER.md – decoder workstream (branch `decoder-m1090`)

Status as of 2026-11-10. This document records what changed in the Mode-S
decoder, the measurements behind each change, one finding that reorders the
roadmap, and what has **not** been verified. Everything here is reproducible
with `harness/` (see `harness/README.md`).

Origin: a review of Daniel Estévez's more1090 (v0.2.0, MIT OR Apache-2.0),
a high-sensitivity 8 Msps coherent ADS-B receiver. Its FFT detection front
end is out of reach for an ESP32-P4 fed by an RTL-SDR; its decode-side
ideas (exact syndrome correction, false-decode discipline) and its
simulator are not.

## 1. Summary

| | result |
| --- | --- |
| Error correction mechanism | brute force replaced by syndrome lookup; **identical output**, 51× (1-bit) and 1718× (2-bit) faster |
| DF18 | a length-table bug discarded every DF18 extended squitter; fixed, decoded, counted – delivery to the app is opt-in |
| Chunk boundaries | frames straddling a USB chunk were lost (0.17 % measured); streaming front end decodes them, proven identical to single-buffer processing |
| 2-bit correction (opt-in) | +5.4 % frames on a real capture, +19 % at 70–78 dB·Hz in simulation; 0 false decodes with discipline on |
| **Timing phase (new finding)** | the 2 Msps decoder is blind to ~35 % of arrival phases **at any signal strength**; a second pass on a half-sample-shifted grid recovers them: **+37 % to +43 % frames on a real capture** (experiment, not yet in firmware) |

| **2.4 Msps demodulator (new, now the default)** | built to fix the timing loss: 96 % of strong frames decode (was 68 %), **1018 → 1646 frames on a real capture**, at or above dump1090-fa on every test, about 1.8× the decode CPU |
| Coherent second look (2026-11-18; host-tested; on by default from the coh2 images, never run on a device) | frames that fail their CRC are sliced again from IQ on their own carrier: **1646 → 1918 frames on the real capture**, and 2062 with the soft rescue that is now on with it; weak-signal decode 37 % → 45 %, for 5–6 % more decoder instructions. Its stress test also found the 2-bit fix putting wrong messages on real tracks – section 5.7 |

| Gain loop (2026-11-18, coh3; simulated, never run on a device) | the first device run showed the error-rate loop taking 27 dB of gain away in half a second: at 2.4 Msps its error rate is 99 % at every gain. Replaced by a loop on headroom, the share of frames at full scale, and yield – section 5.8 |
| Decoder cost (coh3) | the same run showed the decoder filling a core. Measured in RISC-V instructions and cut from 288 M to 149 M per second of signal with identical output; 66 kB of internal RAM freed – section 5.9 |

The timing-phase and 2.4 Msps rows are the important ones. Sections 2–4
cover the earlier changes; section 5 covers the finding, the demodulator
built to fix it, in 5.7 the coherent second look, and in 5.8 and 5.9 what
the first run on a device found.

## 2. What changed in the firmware sources

All in `main/examples/adsb_receiver/`.

| change | file | default | effect at default |
| --- | --- | --- | --- |
| Byte-wise CRC-24 + syndrome tables for 1-bit and 2-bit correction | `mode-s.c`, `mode_s_syndromes.h` (generated) | on | none on output; less CPU on every CRC-failed frame |
| DF18 and DF24–31 are 112-bit frames (`MODE_S_DF18_FIX`) | `mode-s.c` | on | DF18 frames now pass CRC; see below |
| DF18 delivery to the callback (`pass_df18`) | `mode-s.c` | **off** | DF18 is decoded and counted (`stat_df18`), not passed to `on_msg` |
| DF18 counter, total and by source (TIS-B / ADS-R / non-transponder ADS-B) | `mode-s.c`, `class_driver.c`, `serial_console.c`, `main.cpp` | on | visible in four places, see below |
| Streaming detection (`mode_s_detect_stream`) | `mode-s.c`, `class_driver.c` | on | boundary frames decoded; per-frame absolute sample index available |
| 2.4 Msps demodulator, `CONFIG_ADSB_DEMOD_2400` (menuconfig → Example Configuration → ADS-B demodulator) | `mode_s_2400.c`, `class_driver.c`, `Kconfig.projbuild` | **on** | the RTL-SDR runs at 2.4 Msps; frames decode whatever their timing |
| Soft rescue (`soft_fix`) with strict CA/CF validation (`strict_es`) and the known-address gate (`fix2_known_icao`), 2.4 Msps only – `CONFIG_ADSB_DECODE_SOFT` | `mode_s_2400.c`, `mode-s.c` | **on** since coh2 (was off) | frames of aircraft already heard cleanly are repaired from their least confident bits |
| Coherent second look (`coh_fix`), 2.4 Msps only – `CONFIG_ADSB_DECODE_COH` | `mode_s_coh.c`, `mode_s_2400.c`, `mode-s.c` | **on** since coh2 | frames that fail their CRC are sliced again from IQ; section 5.7 |
| 2-bit correction (`fix2`), SNR gate (`fix2_min_delta`) – `CONFIG_ADSB_DECODE_FIX2` | `mode-s.c` | **off**, and since coh2 not recommended (section 5.7) | none |
| Decoder stages switchable while running: `adsb dec default\|soft\|fix2`, `adsb coh on\|off`; repair counters in `adsb status` and `diag`, a `DECODER:` log line once a minute, a second `REPLAY:` line | `class_driver.c`, `serial_console.c` | – | `UPLINK.md` section 9.4 |
| Gain loop on levels and yield, level statistics, squitter error rate, `gain sweep` (coh3) | `gain_ctl.c`, `mode-s.c`, `mode_s_2400.c`, `class_driver.c`, `serial_console.c` | on with the 2.4 Msps demodulator | section 5.8 |
| Faster exact forms of the magnitude table, checksum and chip sums; decoder files at `-O2`; decoder task on core 1 (coh3) | `mode-s.c`, `mode_s_2400.c`, `main/CMakeLists.txt`, `class_driver.c` | on | same output, half the instructions; section 5.9 |
| Scan-limit underflow guard, `<time.h>`, `#pragma once`, DF re-read after a correction | `mode-s.c`, `mode-s.h` | on | none in practice |

`+32,776` bytes of const flash (tables). No new RAM apart from 484 bytes
added to the magnitude buffer and ~80 bytes of state.

**DF18, precisely.** `mode_s_msg_len_by_type()` listed DF16/17/19/20/21 as
long. DF18 (TIS-B, ADS-R, ADS-B from non-transponder devices) is also 112
bits; it was truncated to 56, failed its CRC and vanished. It now decodes.
It is *not* delivered to `on_msg` by default, because `on_msg` creates an
aircraft entry for every address it sees and gates all position/velocity
handling on `msgtype == 17` – DF18 frames would become position-less
entries, and TIS-B addresses are not always ICAO addresses. Using DF18
needs an explicit design (address types by CF field); that is a separate
decision. One side effect to be aware of: DF18 frames now count as CRC-ok
in `stat_crc_ok` / `stat_signal_sum` / `stat_delta_sum`, which feed the
adaptive-gain loop; previously they counted as CRC failures. That is more
correct, but it is a behaviour change in the gain loop wherever DF18
traffic exists, and it has not been observed on hardware. (The gain loop
steers on CRC error rate, so this is not cosmetic.)

**Seeing how much DF18 arrives.** Until the application handles DF18, it
is counted, in total and by the CF field (RTCA DO-260B table 2-11):
CF 2/3/5 are TIS-B (radar targets relayed by ground stations: fine,
coarse, fine with a non-ICAO address), CF 6 is ADS-R (ground rebroadcast
of traffic from another link), CF 0/1 are ADS-B from non-transponder
devices (ICAO / other address), CF 4 is management, CF 7 is reserved.
The counts, since the receiver started, are shown in:

- the console: `adsb status` prints a `DF18:` line with the breakdown;
- the log: once a minute, if any DF18 arrived in that minute,
  `<timestamp>DF18: N in last 60 s, T total (TIS-B .., ADS-R .., ADS-B-NT .., other ..)`
  (it cannot match the web app's aircraft-line pattern);
- the on-device "ADS-B Receiver Status" page;
- the ADS-B app's stats panel, as `DF18: T  (N/min)`.

The two on-device additions add a line of text each; their layout has not
been seen on a screen.

## 3. Evidence

### 3.1 Parity – the mechanism swap changes nothing it should not

`harness/baseline/` holds the decoder exactly as it was at the vendor cut
(plus two instrumentation assignments). The current decoder, built with
`-DMODE_S_DF18_FIX=0` and all options off, produces **line-for-line
identical output** (sample index, DF, error bit, frame) on:

- a synthetic stream (3199 frames),
- the Madrid real capture (4107 frames),
- every simulated scenario (identical decode counts and counters).

Table generation proves, at emit time: the known vector
(`8D406B902015A678D4D220` → `AA4BDA`), equivalence with the historical
stored-vs-computed convention, and that all 112 single-bit and 6216
double-bit syndromes are jointly distinct and nonzero – so a table hit
identifies the error pattern exactly. `test_fix_c` then drives the real
fixers exhaustively: every 1-bit and every 2-bit pattern on 40 random
frames, every 1-bit pattern on 2000 short frames.

### 3.2 Speed of the fixers (host, includes call overhead)

| | 1-bit | 2-bit |
| --- | --- | --- |
| historical brute force | 30.6 µs | 1102 µs |
| syndrome lookup | 0.60 µs | 0.64 µs |

On the P4 the historical 2-bit path would have cost on the order of 10 ms
per failed frame, which is presumably why `aggressive` was never enabled.

### 3.3 Streaming front end

`mode_s_detect_stream()` at chunk sizes 262144, 65536, 4096, 1000, 482,
100 and **2 bytes** is identical, line for line, to processing the stream
as a single buffer – on the synthetic stream and on the Madrid capture.
The historical per-chunk path loses 7 of 4107 frames on Madrid (0.17 %,
the predicted 240/131072) and 11 of 3210 on the synthetic stream.

### 3.4 False decodes

20 s of pure noise at the simulated operating point (48.9 k noise-triggered
candidates):

| configuration | false frames |
| --- | --- |
| pristine / current (1-bit correction only) | 0 |
| fix2, no gates | 3 |
| fix2 + strict | 1 |
| fix2 + strict + `fix2_min_delta` 5760 | 0 |
| fix2 + known-address | 0 (3 refused) |

Across four simulated scenarios (26,343 Mode-S frames, 13,311 of them
extended squitter, including a 2000 msg/s collision-heavy case): **zero**
false DF17/18 frames in every configuration.

### 3.5 Sensitivity (simulation; 2.0 Msps, 8-bit)

P(decode) of extended squitter by truth CN0 (peak-power referenced, the
more1090 definition). ~300 msg/s of mixed traffic, so a few percent of the
losses are collisions.

| CN0 band, dB·Hz | 69–72 | 72–75 | 75–78 | 78–81 | 81–84 | 84–87 | 87–90 | 90–93 | 93–96 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| pristine | 0.4 % | 7.3 % | 21.8 % | 30.9 % | 32.9 % | 31.7 % | 30.8 % | 32.5 % | 34.6 % |
| current | 0.4 % | 15.9 % | 45.7 % | 59.5 % | 63.1 % | 66.1 % | 67.0 % | 67.7 % | 68.8 % |
| current + fix2 + strict | 1.9 % | 20.6 % | 48.4 % | 61.2 % | 65.1 % | 68.3 % | 68.5 % | 69.1 % | 69.3 % |

Reading this table:

- "pristine" is half of "current" because the simulator emits DF17 and
  DF18 in roughly equal numbers and pristine discards all DF18. On DF17
  alone the two are identical.
- 2-bit correction matters at the margin (+19 % frames at 70–78 dB·Hz) and
  barely at all for strong signals.
- **Nothing reaches 100 %.** The curve flattens near 67 % from 81 dB·Hz
  upward. That is section 5.

The absolute CN0 axis depends on front-end gain: the detector has fixed
magnitude thresholds (the `delta < 2550` floor in particular), so the same
signal decodes or not depending on scaling. These runs use a scale that
puts the noise at 7.8 LSB rms per rail below 88 dB·Hz. Compare rows, not
absolute thresholds. (On hardware this is the adaptive-gain loop's job.)

### 3.6 Real capture

Estévez's published recording (Madrid, 20 s, 8 Msps, Zenodo 23360412),
resampled to 2.0 Msps and quantised to 8 bits.

| configuration | ES frames | unique | aircraft |
| --- | --- | --- | --- |
| pristine | 1017 | 651 | 35 |
| current (streaming) | 1018 | 652 | 35 |
| + fix2 + strict | 1077 | 688 | 36 |
| + fix2 + strict + known-address | 1073 | 684 | 35 |

With the known-address gate, 71 two-bit repairs were accepted and 6
refused; one of the refused repairs was the 36th "aircraft". No DF18 is
present in this capture, so the DF18 fix shows no real-data benefit here –
its value depends on TIS-B/ADS-R ground infrastructure.

For scale, Estévez reports 941 good frames for readsb and 2582 for
more1090 on this recording (readsb at 2.4 Msps, 16-bit; not the same
front end as ours, so treat the comparison as rough).

## 4. Recommended configuration

**Since coh2 (2026-11-18)** the build default is the configuration
section 5.7 arrives at, decided by the project owner the same day:

1. the 2.4 Msps demodulator, 1-bit syndrome correction;
2. the soft rescue, with strict CA/CF validation and the known-address
   gate (`CONFIG_ADSB_DECODE_SOFT`);
3. the coherent second look, whose confidences give the soft rescue a
   second try (`CONFIG_ADSB_DECODE_COH`);
4. no 2-bit correction (`CONFIG_ADSB_DECODE_FIX2` stays off).

All of it is host-tested only. On a device each stage can be switched
from the console – `adsb dec default` and `adsb coh off` give exactly
what r10's default image did – and `UPLINK.md` section 9.4 has the test
procedure. The rest of this section is the earlier recommendation, kept
for the record.

*Until coh2:* ship as built: syndrome correction, DF18 length fix (held from the app),
streaming detection. After a soak on hardware, enable
`CONFIG_ADSB_DECODE_FIX2` (menuconfig → Example Configuration) – that
sets `fix2`, `strict_es` and `fix2_known_icao`, and with the 2.4 Msps
demodulator also the soft rescue (section 5.6). Leave `fix2_min_delta` at 0: with the known-address
gate it adds nothing measurable, and its right value depends on gain.

What the known-address gate means in practice: a 2-bit repair is accepted
only for an aircraft heard error-free within the last 60 s. Repairs
therefore extend tracks; they never create one.

*Measured 2026-11-18 (section 5.7): a 2-bit repair can still put a wrong
message on an existing track. In 400,000 simulated collisions the fix2
configuration delivered 124 frames that were never sent, 117 of them
under a real aircraft's address, against 12 for the same configuration
without the 2-bit fix. How often that happens on the air is not known.
Read 5.7 before enabling `CONFIG_ADSB_DECODE_FIX2`.*

The soak should watch `state.stat_fix1`, `stat_fix2`,
`stat_fix2_icao_rej` and `stat_strict_rej`. Since coh2 they are printed,
with the soft-rescue and coherent-look counters: the `Repairs:` and
`Coherent:` lines of `adsb status`.

## 5. The finding: timing-phase blindness

### 5.1 What was measured

At 2.0 Msps a Mode-S pulse (0.5 µs) is one sample, and the decoder decides
each bit by comparing two adjacent magnitude samples. If a frame arrives
about half a sample off-grid, each pulse straddles two samples and the
comparison is meaningless. `phase_diag.py`, on DF17 frames at
88–100 dB·Hz (12–25 dB above threshold, no clipping):

| arrival time modulo one sample | decoded | detector found a candidate |
| --- | --- | --- |
| 0.25 – 0.875 | 93–98 % | 95–99 % |
| 0.125 – 0.25 | 47 % | 98 % |
| 0.875 – 1.0 | 21 % | 33 % |
| 0.0 – 0.125 | 0.9 % | 22 % |

Five-eighths of arrival phases decode almost perfectly; the rest are lost
regardless of signal strength, and in the worst bins the preamble detector
does not even fire. This is a property of the sample rate and the
slicer, not of noise – so no amount of error correction recovers it.

### 5.2 The experiment

`experiments/shift_grid.py`: interpolate the complex IQ at half-sample
offsets, re-quantise, run the **same decoder** on that second stream, and
merge the two result sets (a frame found on both grids within ±3 samples
counts once). Each grid's dead zone is the other's best case.

| | single grid | + shifted grid | gain |
| --- | --- | --- | --- |
| simulated, 88–100 dB·Hz, vs truth | 67.6 % | 96.1 % | +42 % |
| simulated, 2000 msg/s collisions, vs truth | 20.9 % | 30.0 % (8-tap) | +43 % |
| **Madrid real capture, ES frames** | 1018 | 1437 (8-tap) | **+41 %** |
| Madrid, with fix2 + strict + known-address on both grids | 1073 | 1522 (8-tap) | +42 % |

Interpolator quality on the real capture: 2 taps +31 %, 4 taps
(−1, 9, 9, −1)/16 +37 %, 8 taps +41 %, 16 taps +43 %, 32 taps +43 %.
Eight taps captures nearly all of it. False frames from the shifted grid
in simulation: zero, in both scenarios.

End to end on the real capture: 1017 frames as shipped today → 1522 with
everything on (**+50 %**).

### 5.3 Why this reorders the roadmap

The approved plan's D2 was per-candidate coherent demodulation, expected
to be worth perhaps 1 dB at the decode margin. Timing-phase recovery is
worth a third more frames at *every* signal level, on real data, with a
mechanism that reuses the proven decoder unchanged and needs no retained
IQ history – the raw IQ chunk is already in hand in `demodulate()`.

Proposed D2 (replaces the coherent-demod D2; needs approval):

1. In `mode-s.c`, a dual-grid streaming entry point: compute magnitudes
   twice per chunk (as-is, and from IQ interpolated by an 8-tap symmetric
   FIR – 4 multiplies per rail per sample), run the existing detector over
   both with separate stream state, and suppress duplicates before the
   callback. Host-tested the same way the streaming front end was.
2. Measure CPU on the P4. The cost is one extra detect pass plus the FIR
   over 2 Msps; the host decodes 114 M samples/s single-pass, which does
   not translate directly. This is the open risk, and it is why the 4-tap
   adds-and-shifts interpolator (+37 %) is the fallback.
3. Only then revisit coherent demodulation, 2.4/3.2 Msps, and TOA.

Alternatives: a 2.4 Msps demodulator – measured in 5.4, and it changes
the recommendation – and a magnitude-domain sequence detector that models
the split pulses directly (no second pass, but a new slicer to validate).

### 5.4 Would a higher sample rate fix it? 2.0 vs 2.4 Msps, measured

Our decoder cannot run at 2.4 Msps: it assumes exactly two samples per
bit. So the real question is what a demodulator *built for* 2.4 Msps
achieves on the same input. `experiments/rate_24.py` resamples each
source to both rates through the same 8-bit model and runs our decoder at
2.0 Msps and dump1090-fa's demodulator at 2.4 Msps. dump1090-fa is a
measuring instrument here; none of its code is in the firmware.

Extended-squitter frames decoded:

| input | ours 2.0, single grid (today) | ours 2.0, dual grid (8-tap) | dual grid + 2-bit | dump1090-fa 2.4 |
| --- | --- | --- | --- | --- |
| simulated, 88–100 dB·Hz | 67.6 % | 95.9 % | 96.2 % | 94.8 % |
| simulated, 78.5–88 dB·Hz | 62.3 % | 93.1 % | 95.0 % | 94.3 % |
| simulated, 70.5–78.5 dB·Hz | 17.3 % | 22.8 % | 28.2 % | **36.5 %** |
| Madrid real capture, frames | 1018 | 1437 | 1522 | **1624** (1683 with its 2-bit fix) |

No configuration produced a false frame in simulation. The Madrid row
depends on gain: the auto-scaler picked a gain 10 dB lower for the 2.4
stream (its wider passband admits strong off-channel pulses). With the
gains swapped, dump1090-fa gets 1516 at our gain, while our decoder at
its gain falls to 648 (single) and 923 (dual).

Reading this:

- **The sample rate is not what helps; the demodulator is.** A 2.4 Msps
  demodulator removes the timing blindness as completely as the dual
  grid does – both sit at about 95 % for strong signals.
- **It is clearly better on weak signals**: 36.5 % against our best
  28.2 %, and twice what the firmware does today. That is where range
  comes from.
- **It is far less sensitive to gain.** Our decoder has fixed magnitude
  thresholds and lost a third of its frames when the gain dropped 10 dB;
  the reference lost 7 % going the other way. Today the adaptive-gain
  loop has to hold us in a narrow window.
- On the real capture it leads our best configuration by 7–11 % and
  today's firmware by about 60 %.

What moving to 2.4 Msps would take: a new demodulator (bits sliced by
correlating over fractional sample positions, thresholds relative to the
noise floor) written from the published idea – dump1090's implementation
is GPL-licensed, so it is a target to measure against, not code to port
– plus new timing constants and 20 % more USB data (4.8 MB/s). CPU is
unmeasured, but it is one pass with no interpolation filter, so plausibly
less than the dual grid.

**Recommendation.** The 2.4 Msps demodulator is the better end state.
The dual grid is the smaller change and already recovers most of the
loss for strong and medium signals. Either do the dual grid first as an
interim, or go straight to the new demodulator; that is the open
decision.

*Decided 2026-11-10: straight to the 2.4 Msps demodulator – section 5.6.*

### 5.5 CPU: what each option costs, and what is not known

Measured on host builds with an instruction counter
(`harness/results/cpu_cost.txt`), per input sample:

| | instructions / sample | per second | relative |
| --- | --- | --- | --- |
| our decoder today, 2.0 Msps | 48 | 96 M | 1.0× |
| reference 2.4 Msps demodulator (dump1090-fa) | 77 | 184 M | 1.9× |
| dual grid at 2.0 Msps (estimate: two scans, two conversions, 8-tap filter) | ~120 | ~240 M | ~2.5× |
| 2.4 Msps plus a continuous second pass (estimate) | – | – | ~3.8× |

Three things stand out. Half of today's cost is converting IQ bytes to
magnitudes, not decoding – the cheapest place to win time back. The
firmware's `-Og` setting costs this code only about 8 %. And these are
x86 instruction counts: they rank the options reliably but do not say
how much of a P4 core any of them takes.

**The budget itself is unmeasured.** Until now the firmware could not
report its CPU use at all. Two readouts were added:

- `adsb status` prints `Decode: N% of one core` – wall time inside the
  decoder over the last 5 s (an upper bound: it includes any preemption).
- the console command `top` lists every task's share of CPU over one
  second, with `IDLE0` / `IDLE1` showing the unused share of each core.

Those two numbers from a running device decide what fits.

**2.4 Msps and a second pass, both?** Not as a continuous second pass.
At 2.0 Msps the second grid has to run all the time because in the blind
timing phases the preamble detector never fires (22–33 % of frames get a
candidate there), so there is nothing to trigger a retry. A 2.4 Msps
demodulator detects at every phase and showed no plateau, so a blanket
second pass would double its cost for no measured gain. What does make
sense at 2.4 Msps is a second attempt *only for frames that were detected
but failed their CRC* – another timing hypothesis, 2-bit correction,
later a coherent re-slice from IQ. That runs on a small fraction of
frames and costs little.

### 5.6 The 2.4 Msps demodulator (`mode_s_2400.c`)

Written for this project from the signal model; dump1090-fa was the bar
to reach, not a source.

**How it works.** At 2.4 Msps a half-bit pulse is 1.2 samples wide. With
timing quantised to fifths of a sample, every pulse boundary of a frame
falls on that grid, so the energy of each half-bit is an exact two-sample
weighted sum and a bit is 1 when its first half outweighs its second.

1. *Gate, every sample.* The four preamble pulses, as two-sample sums
   that contain them at any sub-sample offset, must each – and then
   jointly – exceed the level of nine samples that are quiet at every
   offset. All thresholds are ratios to that level; nothing depends on
   absolute signal level.
2. *Timing.* The five sub-sample offsets are ranked by the energy in
   their pulse chips; the best two are then tested fully, four pulse
   chips against the twelve chips that must be quiet at that alignment.
3. *Slice and decode.* The frame is sliced at that offset and handed to
   `mode_s_decode()`, which keeps CRC checking, error correction and all
   their safeguards in one place for both demodulators.
4. *Soft rescue (optional).* A DF17/18 frame that still fails its CRC is
   repaired if flipping up to four of its eight least confident bits –
   and only those – accounts exactly for the CRC error.

**Results** on a fixed test set (`experiments/prep_2400.py`,
`eval_2400.py`): three simulated signal ranges, the Madrid capture at
two gains 10 dB apart, and 20 s of pure noise.

| | strong | medium | weak | Madrid, low gain | Madrid, high gain | false frames in noise |
| --- | --- | --- | --- | --- | --- | --- |
| 2.0 Msps demodulator (previous default) | 67.6 % | 62.3 % | 17.3 % | 648 | 1018 | 0 |
| dump1090-fa, 2.4 Msps (reference) | 94.8 % | 94.3 % | 36.5 % | 1623 | 1516 | 1 |
| **new 2.4 Msps demodulator** | 96.0 % | 96.1 % | 36.8 % | 1646 | 1571 | 0 |
| + soft rescue | 96.2 % | 96.4 % | 43.4 % | 1715 | 1685 | 0 |
| + soft rescue + 2-bit, known-address gate | * | * | * | 1755 | 1718 | 0 |

\* In simulation every frame carries a new random address, so the
known-address gate blocks every repair there; the Madrid columns show
its real effect, which is small – nearly all repairs are on aircraft
already being tracked. No row produced a false frame in any simulated set.

The frame counts on Madrid move by 5 % between the two gains (the
reference: 7 %; the 2.0 Msps demodulator: 36 %).

**Cost.** 70–77 instructions per sample, against 76–77 for the reference
and 47–50 for the 2.0 Msps demodulator; per second, about 1.8× the 2.0
Msps demodulator. The first version cost 118 because too many samples
reached a 16-chip timing correlation. Ranking and accepting on pulse
energy alone was cheaper still but cost about three points of
weak-signal decode at the same candidate rate, so the full
pulse-against-quiet test was kept for the best two offsets only.

**Checked.** `mode_s_detect_stream_2400()` is line-for-line identical to
single-buffer processing at chunk sizes down to 2 bytes (`make check`,
on a synthetic 2.4 Msps stream).

**Frame timing.** The demodulator also reports each frame's preamble
start in 12 MHz ticks of its own sample clock
(`mode_s_frame_ticks_12mhz()`), the unit Beast-format timestamps use.
Against simulator truth the spread is 42–59 ns rms (dump1090-fa: 41–50
ns) – `experiments/toa_error.py`, `results/toa_error.txt`. Nothing in
the firmware uses it yet; `docs/ROADMAP-AVIATION.md` section 4.1 says
what it is for.

### 5.7 Coherent second look (`mode_s_coh.c`) – host-tested, never run on a device

*Added 2026-11-18. First delivered off and unwired (coh1); the same day
the project owner chose to make it, with the soft rescue, the build
default (coh2, section 4) – ahead of roadmap rule 1, which would have
held decoder changes until the r10 images had reported from a device.
The r10 behaviour is one console command away (`adsb dec default`,
`adsb coh off`), and that is the baseline to take first.*

**The gap.** `mode_s_2400.c` decides a bit by comparing the magnitudes of
its two halves. Two things defeat that. When another transmission overlaps
the frame, its pulses land in the halves that should be empty. And for a
weak frame, the noise of both rails goes into every magnitude. How bad the
first is, measured with pairs of overlapping DF17 frames
(`experiments/collide.py`: 2,000 pairs per cell, the second frame starting
0–100 µs after the first, idealised pulses) – share of pairs in which the
stronger frame was decoded:

| the stronger frame is | equal | 3 dB up | 6 dB up | 10 dB up |
| --- | --- | --- | --- | --- |
| default image | 0.7 % | 10.3 % | 65.8 % | 100 % |
| default image + coherent look | 4.8 % | 28.9 % | 81.9 % | 100 % |
| soft rescue + 2-bit fix | 1.9 % | 20.4 % | 84.9 % | 100 % |
| soft rescue + 2-bit fix + coherent look | 19.4 % | 54.5 % | 91.0 % | 100 % |

**How it works.** For a frame the detector found and could not decode:

1. *Chips from IQ.* The 224 half-bit chips are matched-filtered from the
   IQ samples with the two-sample weights the magnitude slicer uses –
   integer arithmetic.
2. *Carrier.* The stronger chip of each bit is taken as its pulse.
   Products of neighbouring pulses 0.5 and 1.0 µs apart give the carrier
   offset coarsely; with that removed the pulses are summed in 4 µs
   blocks and a 64-point FFT refines it to a few hundred Hz.
3. *Is there one carrier?* For a clean frame the FFT peak holds nearly
   all the pulse energy. Under 30 %, the attempt stops: that is what a
   Mode A/C burst or a hopeless collision looks like, and stopping here
   is what keeps real traffic cheap.
4. *Slice in phase.* The chips are rotated to baseband, the phase is
   taken from the pulses, and each bit is decided on the in-phase
   difference of its two chips. What is not in phase with the frame's own
   carrier – half the noise, most of a signal on another carrier – drops
   out. Repeated one fifth of a sample either side; the widest eye wins.
5. *Checks.* CRC. If it fails and the soft rescue is on, the soft rescue
   with the new confidences, under the safeguards it already has (still
   DF17/18; a known address when asked). No 1-bit or 2-bit table fix.

**Gates.** Most failed candidates are noise, and a re-slice costs as much
as decoding 400–500 samples, so three tests decide who gets one. The first
slice must read DF17 or DF18 to within one bit. Its bit delta must be at
least twice the running lower quartile of the bit delta of failed
candidates – the noise reference; a quartile, so that traffic does not
pull it up. And a frame gets one attempt, not one per neighbouring
candidate. In 20 s of noise that leaves no attempts at all.

**Results** (`experiments/eval_coh.py`, `results/coh_eval.txt`): the test
set of section 5.6 plus two heavy-traffic scenarios, *busy* at 1,000 and
*dense* at 2,000 messages/s (`experiments/prep_dense.py`).

| | strong | medium | weak | busy | dense | Madrid, low gain | Madrid, high gain |
| --- | --- | --- | --- | --- | --- | --- | --- |
| default image | 96.0 % | 96.1 % | 36.8 % | 41.5 % | 35.7 % | 1646 | 1571 |
| **+ coherent look** | 96.4 % | 96.6 % | 45.4 % | 48.8 % | 42.3 % | **1918** | **1872** |
| soft rescue | 96.2 % | 96.4 % | 43.4 % | 46.2 % | 40.1 % | 1715 | 1685 |
| **+ coherent look** | 97.1 % | 97.8 % | 62.5 % | 62.0 % | 54.3 % | **2086** | **2063** |
| fix2 image (soft, 2-bit, known address) | * | * | * | * | * | 1755 | 1718 |
| **+ coherent look** | * | * | * | * | * | **2077** | **2056** |
| fix2 image without its 2-bit fix | * | * | * | * | * | 1708 | 1676 |
| **+ coherent look** | * | * | * | * | * | **2062** | **2043** |

\* As in 5.6: the known-address gate refuses every repair in simulation.
No row produced a false frame in any simulated set or in the noise.

- **+16 % to +22 % frames on the real capture**, depending on the
  configuration.
  The best row is 79 % of what more1090 decodes from the original
  8 Msps samples (2,633), through a 2.4 Msps 8-bit front end.
- **It is a weak-signal gain as much as a collision gain.** The weak set
  has 300 messages/s and little overlap; it goes from 36.8 % to 45.4 %,
  and with the soft rescue from 43.4 % to 62.5 %.
- **The soft rescue and the coherent look multiply.** The coherent slice
  gives better per-bit confidences, so the rescue finds its bits more
  often.
- About half the frames it recovers on Madrid would have been decoded
  from the next candidate position anyway; the table shows the net.

**Are the added frames real?** Madrid has no ground truth, so its frames
were compared with more1090's decodes of the 8 Msps original
(`experiments/xcheck_madrid.py`, `results/coh_madrid_xcheck.txt`). Of the
272 frames the look adds to the default image at low gain, 266 are frames
the reference decoded too – the same 112 bits – and the other 6 come from
aircraft it heard error-free. At high gain: 301 added, 295 and 6. For the
fix2 image: 322 added, 310 and 12; 338 added, 324 and 14. None of the
1,233 comes from an address the reference did not know, and the look
loses no frame the decoder had without it.

**False frames – and what the test found about the 2-bit fix.** Sets
without a single false frame say little, so `collide.py --stress` ran
400,000 collisions (stronger frame 0–10 dB up, 40 aircraft addresses per
2,000 pairs, each heard cleanly once first) and counted decoded frames
that were never sent (`results/coh_collide.txt`):

| | frames decoded | false | of those, under a real address |
| --- | --- | --- | --- |
| default image | 170,678 | 12 | 3 |
| + coherent look | 220,877 | 12 | 3 |
| soft rescue, no address gate | 198,886 | 17 | 5 |
| + coherent look | 291,396 | 18 | 5 |
| fix2 image | 206,940 | **124** | **117** |
| + coherent look | 290,621 | 110 | 103 |
| fix2 image without its 2-bit fix | 198,306 | 12 | 5 |
| + coherent look | 289,940 | 12 | 5 |

The coherent look adds 50,000–92,000 frames and no false ones. The fix2
image is the outlier: 113 of its 124 false frames are exact 2-bit
repairs, every one under a real aircraft's address. The frame's address
came through the collision, other bits did not, and one of the 6,216
two-bit patterns happened to match the CRC error – about 1 in 2,700
frames damaged beyond two bits. The known-address gate checks the
address, which is correct, so it passes them. Section 4 says repairs
"extend tracks; they never create one"; that holds, but a repair can put
a wrong message on an existing track. The soft rescue tries at most 162
patterns, all among the least confident bits; it accounts for 1 or 2 of
the false frames in the gated rows and 7 in the ungated one. The rest
are 1-bit repairs (7–9 per row) and 3 DF16 frames accepted through the
address/parity check, with or without the look.

With the coherent look in place the 2-bit fix has little left to add:
without it the fix2 image decodes 0.2 % fewer frames in this test and
0.7 % fewer on Madrid, with a ninth of the false frames. Without the
coherent look, dropping it costs 4 % and 2.7 %. These rates are per frame
decoded in a test where every frame is hit; on the air most are not, and
how often this happens there has not been measured.

One loose end, in r10 as well: a frame repaired by the soft rescue is
decoded as if it had arrived clean, so it renews its address's 60 s
"heard error-free" lifetime. With the known-address gate that cannot
admit a new aircraft, but an aircraft can stay known on repaired frames
alone.

**What it does not fix.**

- *Both frames of a collision.* Never, in any configuration. The look
  decides which of the two survives; a second frame would need the first
  subtracted, or two carriers estimated jointly. Not attempted in C.
- *Frames whose tail is overlapped* (the second starts 100–120 µs into
  the first). The decoder jumps a full frame length past every decoded
  frame, so decoding the first hides the second's preamble: with the
  default image both come out in 1.4–2.4 % of pairs, 2.1–2.9 % with the
  look. The jump is the limit there, not the slicer.
- *Frames the detector never found.* The preamble test assumes quiet
  samples around the pulses; under interference it does not fire, and
  there is nothing to take a second look at.

**Cost** (`experiments/cost_coh.py`, `results/coh_cost.txt`), x86-64
instructions per input sample:

| | Madrid | dense | noise |
| --- | --- | --- | --- |
| r10, default image | 95.39 | 80.76 | 70.48 |
| this tree, option off | 95.39 | 80.76 | 70.48 |
| default image + coherent look | 100.70 (+5.6 %) | 90.92 (+12.6 %) | 70.55 (+0.1 %) |
| r10, fix2 image | 99.10 | 84.95 | 71.54 |
| fix2 image, this tree | 97.15 | 82.78 | 70.99 |
| fix2 image + coherent look | 102.40 (+5.4 %) | 93.59 (+13.1 %) | 71.06 (+0.1 %) |

The fix2 image is cheaper than r10's because the soft rescue now walks
its bit subsets in Gray-code order – identical result, one XOR per subset
instead of eight. A re-slice that stops at step 3 costs about 29,000
instructions and a full one about 49,000; on Madrid 54 % stop early.
These are instruction counts on a PC. The decoder's share of a P4 core is
still unmeasured (`Decode` in `adsb status`), so the absolute cost on the
device is unknown.

**On the device** – built with ESP-IDF v5.4.1 since coh2 (no new
warnings), never run. Measured on the object files of a generic RISC-V
gcc for `rv32imafc`/`ilp32f`: about 4.7 kB of flash for the look itself
(the coh2 application image is about 12 kB larger than r10's, console
commands and counters included); no static RAM; 9,604 bytes of working
memory, allocated from PSRAM when the look is first switched on and
touched only during a re-slice; 64 bytes more in `mode_s_t`; 704 bytes of
stack below `mode_s_detect_2400()` on the deepest path – the decoder task
has 12 kB, and `adsb dec` prints how much of it has never been used.
Single-precision float throughout; per re-slice two `atan2f`, three
`cosf`, three `sinf` and one `roundf` from the C library.

**Checked.** With the option off, output is byte-identical to r10 on the
eight test sets in four configurations (and at 2.0 Msps), and the
instruction count is the same. With it on, output does not depend on the
chunk size – down to 2-byte chunks on the synthetic stream (`make check`)
and at five sizes on the dense, weak and Madrid sets. The Gray-code
rescue returns the same repair as the old loop on 20 million random
damaged frames.

**Turning it on and off.** On a PC: `host_mode_s --rate 2.4 --stream 1
--coh 1` (`--coh-gate`, `--coh-df`, `--coh-ts` for the gates and the
timing search; `--coh-on-at` / `--coh-off-at` to switch it in mid-stream;
five `coh_` counters on the `STATS` line). In the firmware since coh2:
on at boot (`CONFIG_ADSB_DECODE_COH`), and `adsb coh on|off` on the
console while the receiver runs. The application calls
`mode_s_coh_enable()`, which also discards what the look had kept from
before the switch; the firmware does that from the decoder task, between
two of its runs. `make check` switches the look on and off in mid-stream
and requires the output outside that interval to be exactly what it is
with the look off.

It only acts through `mode_s_detect_stream_2400()`, which keeps the IQ of
the carried samples for it.

**The first test on a device** is `UPLINK.md` section 9.4: a baseline
with the look off, then a replay of the same recording in each setting.
`experiments/replay_expect.py` gives the counts a PC gets for the same
file (`results/coh_replay_expect.txt` for the two files of the test
set); the replay's `decoder busy` figure is the first measurement of
what any of this costs on a P4.

### 5.8 Gain at 2.4 Msps: the error rate, and what replaced it (coh3)

**What the first device run showed (2026-11-18, coh2, a busy band).** Ten
seconds after the decoder started the adaptive gain loop wrote this, 40 ms
apart:

```
GAIN: [P1] Down: 49.6 → 43.4 dB (−6.2 dB, err=99%, pos=0.0/s, ok=3, sig=5168 delta=3577)
GAIN: [P1] Down: 43.4 → 37.2 dB (−6.2 dB, err=99%, pos=1.0/s, ok=3, sig=6120 delta=3314)
GAIN: [P1] Down: 37.2 → 29.7 dB (−7.5 dB, err=99%, pos=2.0/s, ok=3, sig=6721 delta=4843)
GAIN: [P1] Down: 29.7 → 22.9 dB (−6.8 dB, err=99%, pos=1.0/s, ok=3, sig=5087 delta=2864)
GAIN: Converged at 22.9 dB (err=99%, ...) → steady-state
```

27 dB of gain gone in half a second, and the message rate with it: about
thirty a second before, about three after.

**Two things were wired to the wrong counter.** The loop's error rate is
`stat_crc_fail / (stat_crc_ok + stat_crc_fail)`, and its phase 1 window
ends after 200 "messages", counted as `stat_crc_ok + stat_crc_fail`. With
the 2.0 Msps demodulator both are close to what the names say: its
preamble test lets few candidates through, so a failed candidate is nearly
always a damaged frame. The 2.4 Msps demodulator gates on ratios and passes
about 5,000 candidates a second on a busy band – Mode A/C replies, pulse
noise, the bodies of frames already failed – whatever the gain is. So:

- the "error rate" sat at 99 % and stays there at every gain, and
- the 200-"message" window was over in 40 ms, with between none and three
  decoded frames in it.

Phase 1 reads more than 90 % as "near-total garbage" and drops 6 dB a
window. The one thing that should have stopped it, the signal gate (three
decoded frames with a mean level over 5,000), passed whenever three frames
of a nearby aircraft fell into the same 40 ms.

**No error rate can do this job at 2.4 Msps.** Counting properly does not
rescue the idea. `harness/experiments/gain_sweep.py` puts the Madrid
capture (16-bit samples, 41 dB between its noise and its full scale)
through an 8-bit ADC at a series of gains; `results/gain_sweep.txt`,
abridged:

| ADC gain | noise, counts | headroom | msg/s | pos/s | loud | err (squitters lost) | old err |
| --- | --- | --- | --- | --- | --- | --- | --- |
| −12 dB | 0.60 | 46.6 dB | 320 | 33.6 | 0 % | 69.5 % | 99.4 % |
| −6 dB | 0.89 | 43.1 dB | 350 | 36.1 | 0 % | 64.2 % | 99.1 % |
| 0 dB | 1.46 | 38.8 dB | 357 | 37.4 | 7 % | 39.7 % | 94.3 % |
| +3 dB | 2.02 | 36.0 dB | 360 | 37.5 | 11 % | 35.8 % | 92.7 % |
| +6 dB | 2.76 | 33.3 dB | 356 | 37.0 | 20 % | 35.3 % | 92.1 % |
| +9 dB | 3.87 | 30.4 dB | 348 | 37.0 | 30 % | 33.9 % | 92.0 % |
| +12 dB | 5.43 | 27.4 dB | 335 | 36.2 | 39 % | 33.1 % | 92.2 % |
| +15 dB | 7.67 | 24.4 dB | 315 | 35.0 | 48 % | 31.9 % | 92.5 % |
| +18 dB | 10.8 | 21.4 dB | 275 | 33.5 | 57 % | 32.0 % | 93.3 % |
| +24 dB | 21.5 | 15.5 dB | 166 | 24.3 | 90 % | 32.2 % | 95.3 % |
| +30 dB | 42.8 | 9.5 dB | 28 | 6.8 | 100 % | 36.3 % | 98.7 % |

"Old err" is what the loop steered on: 92–99 % across 42 dB of gain. "Err"
is a properly counted one (below). From +6 to +24 dB the yield falls by
more than half and it does not move: it sits between 32 and 35 %. On this
band a third of the squitters that can be seen are lost to other
transmissions on top of them, which no gain setting changes; the frames
too much gain destroys leave the count on both sides. (It does rise at the
cold end, where the ADC's steps break up the level it is referred to.)

**The error rate that is shown now.** `stat_es_ok` counts extended
squitters decoded; `stat_es_lost` counts candidates whose slice reads DF17
or DF18 and whose bits stand at least twice as far apart as those of the
run of failed candidates, once per 120 µs, less any that decoded on the
neighbouring alignment. The rate is lost / (decoded + lost). Against the
simulator's truth (`experiments/es_lost_truth.py`,
`results/gain_es_lost_truth.txt`):

| set | squitters sent | really lost | shown |
| --- | --- | --- | --- |
| strong | 1378 | 3.6 % | 6.5 % |
| mid | 1428 | 3.4 % | 4.6 % |
| weak | 1380 | 54.6 % | 34.5 % |
| busy (1,000 messages/s) | 2234 | 51.2 % | 30.1 % |
| dense (2,000 messages/s) | 4468 | 57.7 % | 33.8 % |

It follows the truth in order and reads low when losses are heavy: a frame
too weak or too buried to show a DF17 header is not counted as lost,
because nothing says it was there. Read it as "at least this share". It
is shown in `adsb status`, `gain`, the `GAIN:` lines (`err=`) and the MQTT
status; the loop does not steer on it.

**What does move with gain** is in the same table:

- *headroom* – ADC full scale over the noise, from a histogram of one
  sample pair in 67 (`mode_s_level_count`; noise from the level half of
  the samples lie under, so traffic moves it little). Under about one
  count of noise the ADC's own steps start to cost weak frames: 3 % of
  the messages at 43 dB, 11 % at 47 dB.
- *loud* – the share of decoded frames whose pulses are within 3 dB of
  full scale. The frames that clip are the ones too much gain costs: at
  2.4 Msps a chip is 1.2 samples, each bit is decided from samples that
  carry both of its chips, and a clipped pulse loses the contrast between
  them. Messages decoded are within 3.5 % of the best from 3 % to 30 %
  loud, 7 % down at 39 %, a quarter down at 57 %.

**The loop (`gain_ctl.c`, used with the 2.4 Msps demodulator).**

- *Phase 1*, 10 s windows (or two seconds once 200 frames are **decoded**):
  down 3 dB at 35 % loud, 6 dB at 50 %; down to 26 dB of headroom if the
  noise alone leaves less than 20; up, 3 dB at a time (6 while the noise is
  under 0.8 count), while nothing is loud and more than 33 dB of headroom
  would remain. Three windows without a change end it.
- *Phase 2*, 60 s windows: one step down while more than 20 % is loud, one
  step up while under 5 % is loud and more than 31 dB of headroom remains.
  Every step is judged by the positions decoded per second two windows
  later and taken back if it cost more than 15 % – unless the signal level
  moved by 40 %, which means the traffic changed. Two failed steps fence
  that direction off for twenty windows. The per-gain history ("profile")
  can propose a neighbour. All of that is the older loop's phase 2,
  carried over; what changed is what proposes a step.
- A gain change is followed by half a second in which nothing is counted.
- Started from the top step it stays there unless something is loud:
  the loop does not give up gain to gain headroom nobody is using.

The 2.0 Msps build keeps the older loop unchanged; `gain band` belongs to
it and is not used by this one.

**Simulated** (`harness/gain/sim_gain.c`, in `make check`): the loop,
compiled from the firmware's file, against a model made of the decoder's
own counts on the capture above at every dB of gain, with traffic that
varies over the day, hours with none, a start from the lowest step, and
one aircraft in fifty or two hundred. Thirty runs of twelve hours
(`results/gain_sim.txt`): it ends at or next to the gain that would have
been the best fixed one and decodes 99–100 % of that gain's positions,
with 0–12 changes in twelve hours; where the top step is the best one it
never moves.

**What the model is not.** It is an ADC in front of a recording: its noise
follows the gain dB for dB. A tuner's noise figure also changes with the
gain setting, and its steps are not exactly what the table says. The
thresholds above come from one recording with many strong aircraft. So
the device measures itself: `gain sweep [seconds]` holds each step from
49.6 down to 19.7 dB for ten seconds (or as asked) and logs

```
GAIN: [Sweep] 49.6 dB: 28.4 msg/s, ES 9.8/s, pos=9.1/s, err=31%, sig=6120 delta=3314, loud=4% noise=3.2 headroom=32dB clip=0.02%
```

(example values), then returns to the gain it started from. Traffic
changes during the three minutes it takes, so run it twice. Those lines
are the evidence the thresholds should be tuned on.

**Checked.** `make check` (the loop on the model, thirty runs with
expectations); the firmware builds. **Not checked:** the loop on a device;
the sweep and the `[Manual]` line on a device; what a real R820T's noise
does across its steps.

### 5.9 What the decoder costs on the P4's processor (coh3)

**The same device run** printed the task watchdog every five seconds from
the moment the receiver was busy: the idle task of core 0 never ran, the
decoder task was on that core at four of five reports, core 1 was idle at
all five. The program counters, resolved against the flashed image, were
in the slicer, the checksum and the soft rescue – candidate work.

**Measured in the device's instructions.** `experiments/cost_rv32.py`
builds the decoder with the firmware's compiler and flags, runs it under
qemu-riscv32 on the start of the Madrid capture and counts instructions
(`results/cost_rv32.txt`). Per second of signal, the default decoder:

| | instructions / s | |
| --- | --- | --- |
| coh2 as flashed (`-Og`) | 288 M | 100 % |
| coh2 at `-O2` | 221 M | 77 % |
| coh3 at `-Og` | 190 M | 66 % |
| **coh3 as built (`-O2`)** | **149 M** | **52 %** |

The cores run at 360 MHz. How many instructions they finish per cycle on
this code is not known here; at 0.8 the flashed decoder needs a whole core
for a band like this one, which is what the device showed.

**What changed – all of it exact**, i.e. the same frames, bit deltas and
times out:

- *Magnitude table* indexed by the sample pair as it lies in the buffer
  (I + 256·Q): one load where there were two subtractions, two absolute
  values and a multiplication. 128 kB in PSRAM; the older table took
  66 kB of **internal** RAM (it was declared twice the size it used),
  which is free again.
- *Checksum* by the byte-wise CRC table over the data bytes instead of one
  step per bit.
- *Chip sums written out*: the preamble's pulse and quiet chips with
  positions the compiler folds, and each bit as a three- or four-tap
  filter with constant weights, instead of loops over position tables.
  `-DMODE_S_2400_PLAIN` builds the plain form.
- The three decoder files are compiled at `-O2` (`main/CMakeLists.txt`).

Evidence: against the coh2 decoder, identical output in fifty comparisons
(eight test sets, five configurations at 2.4 Msps, two at 2.0 Msps);
`make check` holds the written-out form to the plain one, the checksum to
the bit-by-bit one on two million frames, and the table to the square
root for all 65,536 sample pairs.

What is left, by share on RISC-V: the per-sample gate (about 19
instructions a sample, 30 %), the magnitude conversion (18 %), the later
gate stages and timing search (20 %), slicing (16 %), repairs and the
coherent look (the rest).

**A core of its own.** The decoder task is pinned to core 1. Core 0
carries the USB host, the timers, the network tasks and the interrupt
handlers registered at start-up; unpinned, the decoder settled there.

**Not known.** The decoder task rests for one tick every 200 ms when it
never has to wait, so that the idle task behind it runs; on the device that
did not happen for at least 25 s and reading the code has not shown why. If
the watchdog message returns, it will now name core 1, and `top` taken at
that moment is the evidence needed. And the real figure: `adsb status`
prints the decoder's share of a core and the share of air time decoded.

## 6. Not verified – read before relying on any of this

- **Compiled, not run.** The full firmware builds with ESP-IDF v5.4.1
  (with `patches/esp-idf-v5.4.1-sd-coexistence.patch` applied): 2175
  steps, no errors; `mode-s.c` / `mode-s.h` produce no warnings on the
  target toolchain, and the syndrome tables link into flash rodata, not
  RAM. That proves it compiles and links, nothing more.
- **Nothing has run on hardware.** No boot, no soak, no CPU figures, no
  adaptive-gain observation with DF18 present.
- **On the device the sample stream is not continuous (found 2026-11-13).**
  Every figure in this document is for a continuous stream. The firmware
  reads the dongle only between two runs of the decoder, so what arrives
  while the decoder runs – including the time it spends printing a log
  line per message – is never seen. The loss grows with traffic and has
  not been measured; from r7 `adsb status` reports it as `Capture`. See
  `docs/UPLINK.md` section 2.2. **Changed in r9:** samples are now read
  continuously into a ring buffer and decoded by a separate task
  (`docs/UPLINK.md` section 7); built and host-tested, not run.
- **The 2.4 Msps path is untested end to end on the device**: the RTL-SDR
  at 2.4 Msps through this USB host stack (4.8 MB/s instead of 4.0), and
  the decode load at that rate.
- **The adaptive-gain loop was tuned around the old detector.** It steers
  on the CRC error rate, which depends on how many noise candidates the
  detector lets through. The new detector's thresholds are ratios to the
  noise level, so that number no longer moves with gain the way it did.
  How the loop behaves with it is unknown; compare with a manual gain
  setting on first boot. **Found out on 2026-11-18** (the first device
  run): it took the gain from 49.6 to 22.9 dB in half a second. Section
  5.8; from coh3 the 2.4 Msps demodulator has a different loop, simulated
  and not yet run on a device.
- **The front end is modelled, not an RTL-SDR.** Recordings are resampled
  8 → 2.0 Msps with a generic anti-alias filter and quantised to 8 bits.
  The RTL2832U's own decimation filter and the tuner's IF response differ.
  The timing-phase effect is a consequence of the sample rate and should
  carry over; the exact percentages may not.
- **Madrid has no ground truth.** Frame counts there cannot distinguish a
  correct decode from a false one. The simulated runs say false decodes
  are at or near zero; cross-checking the Madrid output against
  more1090's own decodes of the same recording would settle it and has
  not been done. **Done 2026-11-18** (section 5.7,
  `harness/results/coh_madrid_xcheck.txt`): of the default image's 1646
  frames at low gain the reference decoded 1633 bit for bit; of the other 13, 6 come
  from addresses it never heard error-free. That bounds false frames; it
  does not prove the 13 wrong.
- **The coherent second look (section 5.7) has never run on a device.**
  Since coh2 it is built with ESP-IDF v5.4.1 and on by default. Its cost
  there is unknown: the figures are x86 instruction counts, and the P4's
  `atan2f`/`sinf`/`cosf` and its FPU timing are not in them. Whether the
  decode task's stack has 704 bytes to spare has not been checked
  (`adsb dec` reports it). The console switches, the counters, the
  `DECODER:` log line and the second `REPLAY:` line compile and have
  never been seen working.
- **The soft rescue is on by default since coh2 without the hardware
  soak section 4 once asked for.** The grounds are the stress test of
  5.7; the soak is still owed.
- **Its gates were tuned on this test set** – one real recording and six
  simulated ones. The noise reference adapts; the two fixed numbers (bit
  delta at twice the reference, carrier peak at 30 % of pulse energy)
  may want other values behind a real RTL-SDR.
- **The collision tests are idealised**: rectangular pulses, two frames,
  no multipath, carrier offsets as a normal distribution of 100 kHz
  sigma. Real transponders drift in frequency during a frame and an
  RTL-SDR adds a DC offset and IQ imbalance; none of that is modelled.
  The Madrid capture has real transponders but not an RTL-SDR front end.
- **How often the 2-bit fix puts a wrong message on a real track on the
  air is not measured** – only that it does so in the collision stress
  test, where the soft rescue and the coherent look do not.
- **One real recording, one location, 20 seconds.**
- **The DF18 counter's on-screen lines have not been seen on a display.**
  They compile; whether the extra line fits the stats panel at its
  default divider position is unknown (the divider is draggable).
- **Planned but not done:** settings/console wiring for the 2-bit options
  and their counters; DF18 application support; the Zenodo recording is
  not redistributed with the repo (the harness fetches it).

## 7. Provenance

Algorithms and table layout after more1090 v0.2.0 (commit `bcb0bccdf`),
© Daniel Estévez, MIT OR Apache-2.0. The syndrome tables are re-derived
by `harness/gen_syndrome_tables.py`, not copied. The simulator and the
Madrid recording are used as test tooling and are not part of the firmware.
