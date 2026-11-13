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

The last two rows are the important ones. Sections 2–4 cover the earlier
changes; section 5 covers the finding and the demodulator built to fix it.

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
| Soft rescue (`soft_fix`), 2.4 Msps only | `mode_s_2400.c` | **off** (enabled by `CONFIG_ADSB_DECODE_FIX2`) | none |
| 2-bit correction (`fix2`), strict CA/CF validation (`strict_es`), known-address gate (`fix2_known_icao`), SNR gate (`fix2_min_delta`) | `mode-s.c` | **off** | none |
| `CONFIG_ADSB_DECODE_FIX2` (menuconfig → Example Configuration; enables fix2 + strict + known-address) | `Kconfig.projbuild`, `class_driver.c` | **off** | none |
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

Ship as built: syndrome correction, DF18 length fix (held from the app),
streaming detection. After a soak on hardware, enable
`CONFIG_ADSB_DECODE_FIX2` (menuconfig → Example Configuration) – that
sets `fix2`, `strict_es` and `fix2_known_icao`, and with the 2.4 Msps
demodulator also the soft rescue (section 5.6). Leave `fix2_min_delta` at 0: with the known-address
gate it adds nothing measurable, and its right value depends on gain.

What the known-address gate means in practice: a 2-bit repair is accepted
only for an aircraft heard error-free within the last 60 s. Repairs
therefore extend tracks; they never create one.

The soak should watch `state.stat_fix1`, `stat_fix2`,
`stat_fix2_icao_rej` and `stat_strict_rej`. **Those four are not printed
anywhere yet.** (The DF18 counters are – see section 2.)

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
  setting on first boot.
- **The front end is modelled, not an RTL-SDR.** Recordings are resampled
  8 → 2.0 Msps with a generic anti-alias filter and quantised to 8 bits.
  The RTL2832U's own decimation filter and the tuner's IF response differ.
  The timing-phase effect is a consequence of the sample rate and should
  carry over; the exact percentages may not.
- **Madrid has no ground truth.** Frame counts there cannot distinguish a
  correct decode from a false one. The simulated runs say false decodes
  are at or near zero; cross-checking the Madrid output against
  more1090's own decodes of the same recording would settle it and has
  not been done.
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
