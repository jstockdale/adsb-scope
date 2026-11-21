# ROADMAP-AVIATION.md – from plane-watching tool to aviation instrument

Draft 5.1, 2026-11-13. Since draft 5 the sequence in 5.0 was revised at
John's direction: the instrumentation (diagnostic dump, pass-criteria
checks, replay mode) is delivered as r10; update slots, crash dumps and
sealed commands move up to r11; WiFi follows as r12. Changes from draft 4: **this is now the single
plan.** Section 5.0 holds the whole sequence from r10 on, including the
receiver and uplink work that had its own list in `UPLINK.md`; it adds a
rule that nothing is built on work nobody has measured, pass criteria per
release, a replay mode and a one-command diagnostic dump, and moves crash
capture, update slots and command authentication forward. The audit
(1.2) records what r7 to r9 changed. Section 6 has five new decisions.
Earlier drafts added the use case (section 0), time (4.1), cross-checks
(4.2), the barometers (4.3) and the validation plan (5.1).

Three parts: a brief audit of where the system is, an assessment of the
ideas on the table (plus some that were not), and a phased plan. Section
6 lists the decisions that are yours.

Of the instrument plan, only the groundwork exists in code: frame times
on the sample clock (4.1) and, since r9, a continuous sample stream with
a time stamp on every USB transfer. Numbers marked *measured* come from a
firmware build or the host harness; everything else is an estimate and
says so. **No firmware from r5 on has been reported on from a device.**

---

## 0. What this is for

A low-cost, accurate, useful **digital instrument cluster** for people
in or around aircraft:

- a **backup** for a pilot, standing in for several individual
  instruments that can fail;
- an **advisory** instrument in an aircraft that has no ADS-B
  equipment of its own;
- a **tool** for paramotor pilots, skydivers, passengers and others who
  fly in aircraft or work around them.

One core serves all of them: where am I and how sure, what is near me,
what is under me, and is any of that stale. The users differ in what
they look at, so they get display profiles over the same core, not
separate products.

**What it can stand in for**

| Instrument | Source | Limits |
| --- | --- | --- |
| Attitude indicator | IMU aided by GPS velocity | wrong without GPS aiding in sustained turns; must flag itself |
| Turn coordinator, slip ball | IMU | none particular |
| Heading indicator | GPS track, gyro-stabilised | track, not heading: differs by the wind correction angle; magnetometer is unreliable in a cockpit |
| Altimeter | two barometers of different makes, cross-checked, + GPS height (4.2, 4.3) | cabin pressure is not static-port pressure; GPS height is not pressure altitude |
| Vertical speed | the quieter barometer, cross-checked against GPS vertical speed | lags and misreads while cabin pressure is changing (door, vents) |
| Ground speed, position, map | GPS | – |
| Traffic display | ADS-B In | only what broadcasts (below) |
| Clock, G-meter | GPS, IMU | – |

**What it cannot stand in for – and must say so**

- **Airspeed.** There is no pitot. It knows ground speed only, and
  airspeed is what keeps a wing flying. An add-on pressure probe could
  serve paramotors and ultralights (decision 9).
- **The altitude ATC means.** Close, in an unpressurised cabin; not the
  same thing.
- **Aircraft that do not broadcast.** And in the US, traffic relayed
  from the ground (TIS-B, ADS-R) is uplinked only around aircraft that
  themselves transmit ADS-B Out – so in an aircraft without it the
  picture is partial. An empty display is not an empty sky.
- **Being seen.** The device does not make its carrier visible to
  anyone.

**What "backup" demands that features do not.** A backup is consulted at
the worst moment, so:

- it is already running, **fixed to the airframe** (attitude from a
  hand-held device is meaningless), on power, readable in sunlight,
  working in the cold;
- it knows and shows when it is wrong;
- it does not reboot. The reliability work in Phase 1 is the product,
  not housekeeping;
- "accurate" is demonstrated in flight against a reference before it is
  claimed (5.1).

---

## 1. Audit

### 1.1 What is solid

- **The receive chain is measured, not assumed.** The decoder has a host
  harness with ground truth, proofs that streaming equals single-buffer
  processing, and a reference to compare against. On a real capture the
  2.4 Msps demodulator decodes 1646 frames where the original got 1018.
- **The hard integration problems were solved once already**: USB host
  with an RTL-SDR on an ESP32-P4, SD and WiFi coexisting, two display
  types detected at run time, GPS/NTP time.
- **The firmware builds reproducibly** from a pinned tree, and now
  reports its own CPU use.

### 1.2 What stands between this and an instrument

| # | Finding | Evidence |
| --- | --- | --- |
| A1 | **No data model.** There is no "ownship" and no "traffic" object. The aircraft table is private to `class_driver.c`, which also holds the USB host, RTL-SDR control, CPR decoding, the gain loop and statistics. The UI receives formatted strings. Every instrument below needs to ask "where am I, how sure" and "what is near me". | `class_driver.c` 2750 lines; `on_msg` alone 387 lines; `main.cpp` 5129; `lvgl_ui.cpp` 10617 |
| A2 | **Demo heritage.** About 24 tasks, among them NFC, microphone, ethernet, an AT task and a music player. The application is 3.46 MB of the 5.85 MB image; it carries a 224 kB audio sample and music-player artwork. | *measured*: `idf.py size-components`, symbol table |
| A3 | **Internal RAM is 73 % committed before anything allocates** (323.6 kB of 445.4 kB). 66.5 kB of that is the IQ-to-magnitude table. New features must live in PSRAM. | *measured*: `idf.py size` |
| A4 | **Time is coarse where it is used.** Frames are stamped when consumed, so their time is only as good as a USB chunk (about 55 ms) – although the demodulator itself places each frame to about 50 ns in its own sample clock (section 4.1). GPS runs at its default rate; no rate is configured. Alerting and attitude both need better. | code; *measured* frame timing: `harness/experiments/toa_error.py` |
| A5 | **Faults are neither contained nor explained.** The task watchdog only prints a warning. Core dumps are off. The known USB-host assertion after about 98 minutes therefore reboots the device and leaves no trace. There is no notion of "this value is stale" beyond aircraft expiring at 60 s. | *measured*: `sdkconfig`; known issue |
| A6 | **Performance is unmeasured where it matters.** Decode is about 75 instructions per sample at 2.4 Msps, half of the old decoder's cost was the magnitude table, the build is `-Og`. What that is in percent of a core, and what is left, nobody knows yet. | *measured* on host; `top` and the decode-load readout exist since r4 and await a boot |
| A7 | **Only the decoder is testable off the device.** Tracking, position decoding, the UI and everything planned below can only be exercised by flashing. The serial log format doubles as the web app's API. | harness covers `mode-s*.c` only |
| A8 | **Sensors are present but not instruments.** The IMU is read by a demo task: tilt from the accelerometer alone, heading from the raw magnetometer without tilt compensation or calibration. There is no barometer on the board. GPS PPS is not routed on hardware v1.0; hardware v2.0 (planned, shipping date open) routes it to the processor, and this plan uses it (section 4.1). | `device_imu_task` in `main.cpp`; no pressure sensor anywhere in the tree |
| A9 | **No field update.** The partition table has a single application slot, so over-the-air update cannot work. | `partitions.csv` |

**Since draft 4** (details in `UPLINK.md`):

- A4, in part: the sample stream is continuous from r9 and every USB
  transfer is stamped on arrival; the time is not yet carried to the
  application.
- A6: the readouts exist (`adsb status`: decode load, capture, ring,
  log) and have not been read on a device.
- A7, in part: the MQTT batching and the sample ring are pure C and
  tested on a PC, as the decoder is. Tracking and the UI are not.
- New, **A10: the receiver was deaf while it decoded** – one task read
  samples and then decoded them. Fixed in r9, unverified.
- New, **A11: three behaviour-changing releases (r7, r8, r9) are stacked
  without a boot report between them.** The plan's first rule (5.0)
  exists because of this.
- New, **A12: the admin command topic.** The device acts on MQTT
  commands, including a firmware update trigger. How those commands are
  authenticated has not been reviewed.

None of these is surprising for a project that grew from a vendor demo
into a receiver. All of them matter once someone in the air looks at
the screen.

---

## 2. What "first-rate instrument" has to mean

Features do not make an instrument; trust does. Proposed rules, to be
held to in design review:

1. **No value without an age and a validity.** Anything stale or
   unverified is flagged or removed, never shown as live.
2. **Uncertainty is displayed, not hidden.** Position, attitude and
   traffic position each carry an error estimate.
3. **Latency budgets are written down and measured.** Proposed: attitude
   display under 50 ms at 30 Hz or better; traffic alert under 2 s from
   reception to annunciation.
4. **Failure is obvious.** A reset is fast and announced; degraded modes
   (no GPS, no SDR, low battery, overheating) are annunciated.
5. **It says what it is.** Uncertified and advisory. It never issues an
   avoidance instruction, only information.
6. **Tested before trusted.** Each instrument gets a host-side replay
   harness fed by recorded sensor logs, the way the decoder did.
7. **Receive-only by default.** Any transmission is opt-in.
8. **One timebase.** Every sample from every source is stamped as close
   to the hardware event as the hardware allows, on one clock, with a
   stated uncertainty. Time is measured where data enters, not where it
   is consumed.
9. **Cross-check everything that can be.** A displayed value is checked
   against an independent source wherever one exists. Two sources can
   show that something is wrong; only a third, of a different kind, can
   say which. A disagreement is shown on the instrument, never averaged
   away (4.2).

Rule 5 deserves a plain statement: a pilot who trusts a traffic display
that silently missed an aircraft is worse off than one with no display.
The honest product is one that is explicit about what it cannot see.

---

## 3. The ideas, assessed

### 3.1 Yours

**Artificial horizon.** Feasible with the ICM-20948 already on the
board. The naive version – tilt from the accelerometer, which is what
the demo task does – is wrong exactly when it matters: in a coordinated
turn the accelerometer sees "down" through the floor and the horizon
stays level. A usable one integrates the gyros and corrects them with
gravity only when the aircraft is not accelerating, using GPS velocity
to tell the difference. That needs GPS at 5 Hz or more and a mounting
calibration. The same estimator yields turn rate, a slip ball and a
G-meter for free. It must show when it does not trust itself.

**Collision and proximity warnings.** Feasible, and the centre of the
whole plan: own position and velocity against each target's, closest
point of approach, relative bearing and altitude, a spoken callout. Two
honest limits. First, without a barometer, relative altitude rests on
GPS height, which is the weakest axis. Second – the bigger one – **this
receiver only sees 1090 MHz ADS-B.** Most paragliders and many gliders
are not on it. For the people you named, the lever is not a better
alert algorithm but more ways of seeing:

| Who is nearby | How they are visible | Do we have the hardware |
| --- | --- | --- |
| Airliners, most powered aircraft | ADS-B on 1090 MHz | yes |
| Radar targets and UAT traffic relayed from the ground (US) | TIS-B / ADS-R, the DF18 frames now being counted | yes |
| US light aircraft below 18,000 ft using UAT | 978 MHz | second RTL-SDR |
| Gliders, paragliders, hang gliders | FLARM (proprietary), FANET, ADS-L (open) on 860/900 MHz bands | **the SX1262 radio already on the board** (to be confirmed against each specification) |
| Aircraft with a Mode S transponder but no ADS-B | signal strength only: "something close", no bearing | yes |
| Aircraft with no transponder | nothing | no |

**Aviation radio on a second RTL-SDR.** Feasible; needs a USB hub and a
power budget. Candidates, most valuable first: 978 MHz UAT in the US
(traffic plus free in-flight weather), airband AM voice (monitor, scan,
record and replay ATIS – the board has a speaker), ACARS and VDL2 data.
Voice needs little CPU; a second ADS-B-class decoder needs the headroom
measured first.

**LORAN and nav beacons.** LORAN: no. The US and Canada switched
Loran-C off in 2010 and north-west Europe in 2015; what remains is
eLoran in a few countries and test transmissions, and 100 kHz is in any
case not practical for this receiver. NDB: no (low frequency, and no
bearing without a direction-finding antenna). DME: no (it answers
interrogations; listening gives nothing). **VOR: yes.** The bearing from
a VOR station can be decoded from an RTL-SDR, and with a station
database that is a line of position that does not depend on GPS; two
stations give a fix, to roughly 1–3° of bearing. ILS localiser and
glideslope are decodable too, as a display. VOR is the one real
GPS-independent radio aid available, which makes it the natural partner
for the dead-reckoning idea.

**Built-in vector map.** Feasible. Not general vector tiles – too heavy
to parse – but a compact purpose-built format generated on a PC from
open data, stored on the SD card and drawn on the LVGL canvas the scope
already uses: coastlines and borders, airports and runways, navaids,
then airspace and terrain. Licensing needs care for airspace: the
popular open airspace database is non-commercial only; government
sources vary by country.

**Inertial navigation when GPS is lost – "right?"** Partly. The physics
of a consumer MEMS unit, after good calibration (assumed: 2 mg
accelerometer bias, 0.01 °/s gyro bias; an estimate, not a measurement
of this part):

| Time without GPS | Pure inertial position error, order of magnitude |
| --- | --- |
| 10 s | about 1 m |
| 30 s | 10–20 m |
| 1 min | 70–100 m |
| 2 min | 500–650 m |
| 5 min | several km |

So pure inertial navigation bridges short outages well and is useless
after a few minutes. Past the first minute, plain dead reckoning –
hold the last track and speed, follow turns from the gyro – does better,
growing roughly linearly (a 5-knot wind error is about 150 m per
minute). The honest feature is therefore: **coast through GPS loss with
a visibly growing error ellipse**, switching from inertial to dead
reckoning as the first overtakes the second, and tightening again if a
VOR bearing is available. Quantified error is the right instinct; it is
also what makes this safe to show.

### 3.2 Additions worth considering

| Idea | Why | Cost |
| --- | --- | --- |
| **Feed the pilot's existing app** (GDL90 over WiFi for ForeFlight and others; FLARM-style NMEA for XCSoar, XCTrack, SeeYou) | The fastest route to being useful in a cockpit: traffic, GPS and attitude appear in software pilots already trust | small |
| **Variometer with audio** | The primary instrument for gliders and paragliders | needs a barometer (see 6) |
| **Flight instruments page** – ground speed, track, altitude, vertical speed, G | Falls out of the ownship estimator | small once that exists |
| **GNSS interference monitor** | ADS-B frames carry each aircraft's own position-integrity figures; when many nearby degrade at once, someone is jamming. A real-time local version of what public jamming maps do | small; decode two more message fields |
| **Nearest airfield and glide range** | An engine-out or sink-out aid; needs the map data and terrain | medium |
| **Drop-zone / airfield ground mode** | A fixed display answering "is the airspace over us clear right now", with an alert on inbound traffic. Ground-based, so the traffic-alert limits matter less | small once alerts exist |
| **Flight recorder** – IGC / GPX logs; automatic jump or flight detection | Expected by glider, paraglider and skydiving users | small |
| **Distress beacon monitor** – 121.5 MHz sweep tone, 406 MHz beacon bursts | Uses the airband SDR; genuinely useful to search-and-rescue volunteers | medium |
| **Point-and-identify** – aim the device at an aircraft, it names it (IMU + GPS + ADS-B, optionally the camera) | For the plane-watching side | medium |
| **Standard Beast-format output** over TCP | Makes the device a drop-in receiver for the existing tools (readsb, tar1090, Virtual Radar and the like) | small |
| **Joining existing multilateration networks** | Draft 1 said this needs PPS. It does not: those networks synchronise receivers on aircraft whose positions are known, and need only good *relative* frame timestamps plus Beast output. Ours are already at dump1090's level (4.1) | small to medium; it sends frames and the receiver's position to a third party, so opt-in |
| **Our own time-difference experiments** between two devices | The "DX" ambition; PPS helps here (4.1) | later, with hardware v2.0 |

---

## 4. Architecture this implies

Five services, each small enough to test on a PC:

```
 sensors ──► OWNSHIP estimator ──┐
 (GNSS, IMU,  position, velocity, │
  baro)       attitude, each with │
              error + validity    ├──► ALERTS ──► audio, haptics
 radios ───► TRAFFIC table ───────┘      │
 (1090, 978,  one entry per target,      ├──► UI apps (horizon, scope, map, instruments)
  SX1262)     tagged by source and       │
              address type, with age     └──► outputs (GDL90, NMEA, logs, MQTT)
 SD card ──► NAV DATA (airports, navaids, airspace, terrain) ──► MAP renderer
```

- **Ownship** is one estimator, not three features: the artificial
  horizon, the flight instruments and GPS-loss coasting are views of it.
- **Traffic** absorbs the DF18 decision: TIS-B, ADS-R, UAT and FANET
  targets are entries with a source tag, shown distinctly – your
  "separate but equal".
- **Integrity** sits across all of it: validity flags, the cross-checks
  of 4.2, a health monitor, a watchdog policy, crash dumps kept in
  flash.
- **A sensor recorder** writes raw GNSS, IMU and (later) baro to SD.
  Replayed on a PC, it is to the estimator and the alert engine what the
  simulator was to the decoder.
- **A timebase** underlies all of it.

### 4.1 Time

There are three clocks, and most timing confusion comes from treating
them as one:

1. the **sample clock** – the RTL-SDR's own oscillator, visible only as
   the count of samples it has delivered;
2. the **device clock** – the processor's monotonic timer;
3. **UTC**.

**Stamp at ingress.** Each source is stamped at the earliest point the
hardware allows, on the device clock:

| Source | Where it is stamped | Today |
| --- | --- | --- |
| Mode S frame | in the sample clock, by the demodulator: sample index plus sub-sample timing | implemented; not yet carried through to the application |
| USB transfer from the dongle | on completion of each transfer, not each 262 kB buffer | not done; this is what ties the sample clock to the device clock |
| GPS fix | at the PPS edge, which is the instant the fix is valid | not possible on v1.0; today the fix time is inferred from serial arrival, which the code's own notes put at 1–2 s late |
| IMU, barometer | at the sensor's data-ready signal, by hardware capture | not done; read on a task schedule |

**Frame timestamps: Beast-compatible, and a little better.** At 2.4 Msps
the demodulator's timing resolution is a fifth of a sample, which is
exactly one tick of the 12 MHz clock that Beast-format timestamps use.
So the native frame time is that count, and the Beast output is a
formatting step. *Measured* against simulator truth, constant offset
removed:

| | new 2.4 Msps demodulator | dump1090-fa |
| --- | --- | --- |
| timing spread, rms | 42–59 ns | 41–50 ns |
| 95 % of frames within | 84–131 ns | 80–106 ns |
| frames mis-timed by more than 3 ticks | none | none |

The floor for timing quantised to one tick is 24 ns rms. An interpolated
estimate (more1090 reports about 7.5 ns at 8 Msps) could go below it,
which is the main room for "better". A simpler gain may come first: the
demodulator keeps the first alignment that decodes, and on a synthetic
stream whose true sub-sample timings are uniform its reported timings
are visibly not. Choosing the best-scoring alignment among the
neighbours afterwards should tighten the spread (untested). The other
improvements are things Beast timestamps lack and an instrument needs:

- an **epoch number** that changes whenever the sample count stops being
  continuous (stream restart, detected sample loss), so nobody
  subtracts timestamps across a gap;
- a **UTC estimate with its uncertainty**, and a flag for whether it is
  PPS-disciplined;
- room for **sub-tick resolution** later.

**What PPS on hardware v2.0 buys**, in order of value to an instrument:

1. *Knowing when each GPS fix was true.* The fix is valid at the PPS
   edge. That removes the serial-delay ambiguity from ownship position –
   at 100 knots, one second is 51 m – and from GPS-aided attitude.
2. *UTC on the device clock* to well under a microsecond with hardware
   capture of the edge, instead of milliseconds from network time or
   worse from serial GPS.
3. *Calibrating the dongle.* Counting samples between PPS edges
   measures its oscillator error; that corrects the tuner frequency and
   the conversion from ticks to seconds.
4. *Integrity.* A sample count per second that comes up short means
   samples were lost: the epoch increments and timestamps across the gap
   are marked.
5. *A shared reference between two of our devices* for time-difference
   experiments.

**What PPS does not do by itself**: put nanosecond-accurate UTC on a
frame. The frame is timed in the dongle's sample clock, and that clock
reaches the device only through USB, whose delivery jitters. Expected
accuracy of each link:

| Link | v1.0 | v2.0 with PPS | Basis |
| --- | --- | --- | --- |
| frame to frame, one receiver | about 50 ns rms | same | *measured* |
| sample clock to device clock | tens of µs, once every transfer is stamped and filtered | same; plus oscillator error known and sample loss detected | estimate |
| device clock to UTC | milliseconds (network time); 1–2 s (serial GPS alone) | under 1 µs | code notes; estimate |
| frame to UTC | milliseconds | tens of µs, limited by USB, not by PPS | estimate |

That is ample for sensor fusion, alert latency and logs. It is not
enough for multilateration on absolute time – which is why the networks
do not rely on it. Nanosecond-level absolute frame time would need the
PPS edge to appear *in the sample stream* (a marker injected at the
antenna input). That is a hardware option beyond v2.0 as described;
noted, not planned.

### 4.2 Cross-checks

Rule 9 in practice. For each quantity the instrument shows: what it is
taken from, what independently checks it, and what settles a
disagreement.

| Quantity | Primary | Checked against | Third source (says which is wrong) |
| --- | --- | --- | --- |
| Altitude | barometer A | barometer B – another maker, another sensing principle | GPS height: slow and noisy, but it fails in entirely different ways |
| Vertical speed | rate of barometric altitude | GPS vertical speed | vertical acceleration from the IMU |
| Attitude | integrated gyros | gravity from the accelerometer, when not accelerating | bank implied by GPS turn rate and speed (coordinated flight) |
| Track | GPS track | turn integrated from the gyro | magnetometer – weak in a cockpit, last resort |
| Position | GPS | the estimator's own prediction (inertial, then dead reckoning) | the carrying aircraft's own ADS-B position, when it transmits; later a VOR bearing |
| Time | PPS (hardware v2.0) | network time | the real-time clock |

Three things this table is for:

- **Detect, then isolate.** With two sources the instrument can only
  flag. The third column is what lets it keep showing a value, and say
  which sensor it stopped believing.
- **Common-mode errors.** Sensors of the same kind in the same place
  share their errors: both barometers read the same wrong cabin pressure
  when the door opens. They will agree with each other and drift
  together from GPS height. That pattern is a *condition* (cabin
  pressure is not outside pressure), not a sensor fault, and is
  annunciated as such. Only a source of a different kind catches it.
- **A reference we get for free.** An aircraft with ADS-B Out
  broadcasts its pressure altitude from its own static system. Received
  from a few metres away, the difference from our barometers *is* the
  cabin-pressure error, measured live. To be logged on the test flights
  first; whether it is ever used to correct the display is a later
  decision. It depends on recognising our own aircraft (5.1).

Thresholds for "disagree" come from bench and flight data (5.1), not
from datasheets.

### 4.3 Barometers

Decided: **two sensors of different makes on one cable**, so that a
fault or drift in one is unlikely to be shared by the other.

| | Bosch BMP581 | ST LPS22DF |
| --- | --- | --- |
| Sensing principle | capacitive | piezoresistive |
| Range | 300–1250 hPa | 260–1260 hPa |
| Noise (best mode) | 0.08 Pa | 0.34 Pa |
| Absolute accuracy, as specified | ±0.3 hPa, for -5 to 65 °C and 300–1100 hPa | ±0.45 hPa over the full range, -20 to +85 °C; ±0.2 hPa in a narrower window |
| Temperature coefficient | ±0.5 Pa/K | ±0.45 Pa/°C |
| Role | vertical speed (quietest) | altitude (its specification reaches the cold, low-pressure corner) |

Each also checks the other. If no plug-in board for the LPS22DF can be
found, the Infineon DPS310 is the second sensor (±1 hPa absolute,
-40 to 85 °C; plug-in boards are common). Its waterproof sibling, the
DPS368, is the choice for an installation exposed to weather, such as a
paramotor.

**Connection.** The two 4-pin Qwiic connectors on the side of the board
are on GPIO 45/46 and 47/48; the firmware uses neither. Both sensors go
on one of them, daisy-chained, at different addresses. Both of the
processor's hardware I2C controllers already serve on-board chips, so
the barometers get their own bus in software on those two pins – ample
for 25–50 readings a second from each. GPIO 54 on the 16-pin header
stays untouched: this build uses it as the reset line of the WiFi chip.

**Placement** matters more than the choice of part: on a short cable
away from the device's own heat, out of direct airflow and sunlight,
open-cell foam over the ports, fixed to the same mount as the device,
not sealed in a case.

**What no sensor fixes:** the cabin is not the static port. That is
handled by annunciating it (4.2), not by a better chip. These are
consumer parts; their makers do not offer them for safety-critical use,
which is consistent with an advisory instrument and one more reason it
stays one.

---

## 5. Phased plan

### 5.0 Status, rules and the sequence from here

**Where things stand**

| Item | State |
| --- | --- |
| r5: 2.4 Msps demodulator, DF18 counter, CPU readouts | built; no boot report |
| r7: MQTT feeder sends every aircraft once per batch; link and feeder counters | built; no boot report |
| r8: one limit of 256 aircraft for tracking, scope, list, uplink | built; no boot report |
| r9: continuous capture – USB reader task, sample ring, decoder task, log writer task | built; no boot report |
| r10: `diag`, pass-criteria checks, replay mode | built; no boot report |
| A field test on some firmware: uplink "dropping" above 60 to 70 aircraft | reported; which firmware, and what dropping looked like, not yet known |
| Everything in Phases 1 to 5 below, except the groundwork named in the preamble | not started |

**Rules for the sequence**

1. **Nothing is built on work nobody has measured.** No further change to
   the sample path (USB, ring, decoder) until r9 has numbers from a
   device. Work that does not touch it may proceed.
2. **Order of precedence** when items compete: seeing what the device
   does (diagnostics, crash capture), then the correctness of the data
   path, then staying connected, then updating and commanding it safely,
   then instruments.
3. **Every release has pass criteria**, agreed before it is flashed
   (proposals below; the numbers are yours – decision 13).
4. **One theme per release**, so a regression points at one change.
5. Every release is built and tested on a PC as far as it can be, and
   says plainly what could not be.

**Yours** – roughly in order; the first unblocks the most:

1. Flash r10 (r8 is the fallback). After a few minutes in traffic, run
   `diag` on the console and send the block it prints, with the session
   log (`UPLINK.md` sections 4 and 9 say how to read them). The two
   numbers that decide r13: `Decode` and `Capture`.
2. What "dropping" looked like in the field test, and on which firmware.
3. A sixty-second raw capture from the dongle (block in 5.1), copied to
   the SD card: r10's replay mode needs it.
4. Check that the MQTT consumer copes with several messages per batch.
5. Order the two barometer boards (4.3); check with a meter that the
   Qwiic connector's power pin is 3.3 V before connecting anything.
6. Confirm the jump plane transmits ADS-B Out; raise the mount with the
   pilot.
7. The open decisions in section 6; 10 and 11 are needed for r12.

**The sequence**

| Release | Theme | Contents | Needs |
| --- | --- | --- | --- |
| **r10** – built | Instrumentation | `diag`: one console command that prints everything a field report needs, with the pass criteria below evaluated as PASS/FAIL; `diag mark` restarts the observation. Reset reason and a count of task-watchdog firings. Replay mode: `adsb replay <file>` feeds a recorded sample file from the SD card through ring, decoder and tracker on the device and reports the message count and what the decoding cost. | a boot; the raw capture for replay |
| **r11** | Field readiness | Update slots in the partition table (A9), with room for a crash dump that survives the reset. The update path reviewed end to end. **Seals**: MQTT commands and firmware updates validated against John's seal specification; unsealed commands accepted unless the device is in strict mode. Feeder counters in the MQTT status message. | the seal specification |
| **r12** | Staying connected | WiFi self-healing: periodic reconnection instead of giving up; several saved networks. A WiFi drop no longer stalls the event task while MQTT stops. | decisions 10, 11 |
| **r13** | Capture follow-up | Whatever r9's numbers call for, and nothing else: the decoder split over both cores or made cheaper if `Decode` is near 100 %; the scope's trail recorder moved out of the timer task and a fourth USB transfer if the endpoint runs dry often. Possibly nothing. Moves ahead of r11 if the numbers are bad. | r9 numbers |
| **r14** | Console | A faster console with the matching web app change. Its own release because it breaks compatibility for anything reading the port. | decision 14 |
| then | Phase 1 onward | The instrument platform, below, starting with the sensor recorder, the barometers and the timebase. | parts; decisions 3 to 9 |

Notes on the table:

- r10 touches the sample path in three places only, all inert unless a
  replay is running: the USB callback discards live samples during one,
  the decoder switches itself over when its ring is empty, and replayed
  messages are kept out of MQTT, the SD log and the serial log.
- A watchdog that resets the device, and a full crash dump, wait for r11:
  the dump needs a partition the table does not have, and turning the
  watchdog from a warning into a reset is safer once a reset leaves
  evidence.
- r11 is ahead of the WiFi work because units in the field that cannot
  be updated, or that take commands from anyone who can publish to a
  topic, are a liability everything else inherits.
- Replay mode is the first half of the sensor recorder's replay reader
  (Phase 1).

**Pass criteria – proposals**

| Release | Passes when |
| --- | --- |
| r9 | Boots and decodes. Over an hour in traffic: `Capture` at or above 99 %, `Decode` at or below 80 %, no transfers dropped by the ring. Display stays responsive. Unplugging and replugging the dongle, `adsb off`/`adsb on` and mass-storage mode work. |
| r7 uplink | On a stable network for 24 h: no MQTT disconnects. The server shows at least 95 % of the aircraft the device shows. |
| r10 | `diag` prints one complete block. Replaying the same file twice gives the same message count, with no transfers dropped, and the count agrees with the PC decoder on the same file (`UPLINK.md` 9.3). Live reception resumes afterwards. |
| r11 | An update delivered over the air boots, and a bad one rolls back. A crash leaves a dump the next boot reports. In strict mode an unsealed or wrongly sealed command or update is refused; outside it, unsealed commands still work. |
| r12 | With the access point switched off and on, the device is back on MQTT within 60 s of the network returning, 20 times out of 20. With two saved networks, it moves to the second when the first disappears. |

**Mine** – in this order:

1. r11, starting with the partition table and the update path, which
   need no decision; the seals when the specification arrives.
2. The sensor recorder and its replay reader (Phase 1), built on r10's
   replay mode.
3. The software I2C bus, both barometer drivers and the altitude
   cross-check, tested against register-level models of the two parts
   until the boards arrive.
4. The timebase carried through to the application (r9 supplies the
   per-transfer time stamps).

r13 is slotted in when r9's numbers arrive; r12 and r14 when their
decisions are made.

### The phases

Each phase ends in something flashable. "Host" means I can build and
verify it on a PC; "device" means it needs your hands and hardware.

**Phase 0 – Validate what is built** (device; no new code)
r9 on the bench: boot, `adsb status`, `mqtt status`, `top`, 2.4 Msps
stability, the gain loop, DF18 counts, against the pass criteria in 5.0.
Gate: 2.4 Msps stays the default or not; the measured CPU headroom sets
the budget for everything below, and decides r13.

**Phase 1 – Make it an instrument platform** (host-heavy; little visible)
Items 1 and 8 are pulled forward into r11 (5.0); what remains of
item 1 here is the recovery path and the validity model.
1. Fault handling: watchdog that resets, crash dumps to flash, a
   recovery path for the USB assertion, a health/validity model.
2. Board support extraction (already planned), which is also what lets
   a barometer or a second board in cleanly.
3. Remove the demo ballast; reclaim flash, RAM and tasks.
4. Split tracking out of `class_driver.c` into the traffic table; add
   the ownship object.
5. Timebase (section 4.1): carry the demodulator's 12 MHz frame time,
   an epoch number and a UTC estimate with uncertainty on every frame;
   stamp every USB transfer on arrival and keep the sample-clock model;
   stamp GPS and IMU at the event. Build the PPS input into the board
   layer now – the hooks exist (`board_gps_pps_gpio`, the reserved
   stratum-0 source in `time_manager`) – so v2.0 is a wiring change, not
   a redesign.
6. GPS at 5–10 Hz; sensor recorder; host replay harness.
7. Barometers (4.3): the software I2C bus on a Qwiic port, drivers for
   both sensors, the altitude cross-check of 4.2; both logged raw by the
   recorder.
8. Real update slots in the partition table.

The sensor recorder is built first within this phase: test flights
(5.1) are only useful once it exists.

**Phase 2 – The instrument panel** (the first visible deliverable)
Ownship estimator developed host-first against recorded drives and
flights. Then one page: attitude, turn and slip, track, altitude,
vertical speed, ground speed – each with its validity flag and, where
4.2 provides one, its cross-check – and
GPS-loss coasting with an error ellipse. Display profiles (pilot
backup, paramotor, skydiver, passenger) are arrangements of the same
data, added as each is wanted.

**Phase 3 – Traffic awareness**
Alert engine (closest approach, relative bearing and height);
heading-up traffic scope; spoken callouts; DF18 targets integrated and
visually distinct; signal-strength proximity for non-ADS-B Mode S;
GDL90, NMEA and Beast-format outputs; drop-zone ground mode.

**Phase 4 – Map**
Base map, airports and navaids; then airspace and terrain; nearest
airfield and glide range.

**Phase 5 – More ways of seeing**
SX1262: receive FANET and ADS-L, then opt-in transmit. Second SDR:
978 MHz UAT or airband voice. VOR bearing as an experiment feeding the
coasting estimator. GNSS interference monitor.

**When hardware v2.0 arrives** (shipping date open) – a sibling board
package; PPS captured in hardware and made the top time source;
GPS fixes stamped at the edge; dongle oscillator calibrated from it;
sample-loss detection switched on. Then time-difference experiments
between two devices.

**Later** – second board (Waveshare 7B, which has no IMU),
distress-beacon monitor, point-and-identify, interpolated sub-tick
frame timing.

The existing items map in as follows: board support extraction,
capability gating and update partitions are Phase 1; the Waveshare
bring-up and wide layout move to "later" unless you want them sooner;
the DF18 decision lands in Phase 3; frame timing is carried through in
Phase 1, exported in Phase 3, and disciplined by PPS when v2.0 exists.

### 5.1 Validation: bench, road, then the jump plane

The rule that worked for the decoder applies here: **record first, build
on the PC, then fly the result.** Flights are record-only until the
estimator has been proven on recorded data, which also means the device
needs no attention in the air.

**On the ground – no aircraft needed**

- Boot r5; `adsb status`, `top`, 2.4 Msps stability (Phase 0).
- **A raw capture from the real dongle.** Everything measured so far
  went through a modelled front end. Sixty seconds from the actual
  RTL-SDR runs through the harness unchanged and settles that (block
  below).
- **The IMU at rest for an hour, and through a temperature swing**
  (freezer to room). Measured bias and noise replace the assumed figures
  in the dead-reckoning table of 3.1.
- Known tilt angles; a turntable for gyro scale.
- **Both barometers side by side**: an hour at rest for noise, drift
  and their offset from each other; the same freezer swing; a known
  height change (a stairwell or a lift) for scale. This is where the
  disagreement threshold of 4.2 comes from.
- **Car drives with the recorder running**: turns and braking are the
  first real data for GPS-aided attitude, and cost nothing. A drive
  over a hill is also the first three-way comparison of the two
  barometers and GPS height.

**Why the jump plane is a good test aircraft**

| What each load provides | What it tests |
| --- | --- |
| A climb to 13,000 or 18,000 ft and a fast descent | altitude and vertical speed across half the atmosphere's pressure range, and the cold |
| The door opening at altitude | how wrong cabin pressure gets, and how fast it recovers |
| Sustained climbing turns and steep descending turns | exactly the case in which unaided attitude fails |
| **The aircraft's own ADS-B Out** | a certified position, pressure altitude and velocity for the aircraft we are sitting in, received by our own radio from a few metres away – a reference we do not have to bring |
| A sky full of traffic seen from altitude | decode load and range at the worst case; real targets for the alert logic |
| Several loads in a day | quick iteration, same route each time |

Two caveats. That the aircraft has ADS-B Out is an assumption to
confirm – in US airspace above 10,000 ft it is required. And the flight
gives **no attitude reference**: a small camera fixed beside the device
and aimed at the horizon does, to about a degree of roll and pitch
(estimate). There is no airspeed reference to log either.

One requirement falls out immediately: our own aircraft will be the
strongest target on the scope, and recognising and suppressing it is
part of the traffic logic, not an afterthought.

**Flights, in order**

1. **Record only.** Raw IMU, GPS, barometer and every received frame,
   own aircraft included, all time-stamped; the horizon camera running.
2. **On the PC.** The estimator is built against those logs. GPS loss is
   tested in replay – delete GPS for ten seconds to five minutes,
   compare the coasted position with the truth – which turns the error
   table in 3.1 from physics into measurement, at no risk.
3. **Fly the estimator live**, still recording; compare with the horizon
   video and with the aircraft's own ADS-B.
4. **Watch the traffic alerts live.**

A jump with the device is its own later test, after the ride-alongs.

**Needed before flight 1**

- r5 booted and stable for a full day on the bench;
- the recorder build;
- both barometers fitted and logging;
- a mount fixed to the airframe that cannot snag, or come loose with the
  door open; the pilot's agreement;
- a battery sized for a day of loads in the cold.

**Capturing from the real dongle.** Run from `harness/` on a computer
with the dongle attached (after `make`). Edit the variables, then paste;
it runs in a subshell, so a failure cannot close your terminal.

```sh
(
OUT="$HOME/adsb-capture-2400.cu8"   # output file: about 290 MB per minute
SECS=60                             # length of the capture
GAIN=40                             # tuner gain in dB; use what the device settles on

fail() { echo "STOP: $1"; exit 1; }
command -v rtl_sdr >/dev/null 2>&1 || fail "rtl_sdr not found (Debian/Ubuntu: sudo apt install rtl-sdr; macOS: brew install librtlsdr)"
[ -x ./host_mode_s ] || fail "run this from harness/ after 'make' (host_mode_s not found)"
command -v python3 >/dev/null 2>&1 || fail "python3 not found"
rtl_sdr -f 1090000000 -s 2400000 -g "$GAIN" -n $((SECS * 2400000)) "$OUT" || fail "capture failed (is the dongle on this computer and not in use by another program?)"
./host_mode_s "$OUT" --rate 2.4 --stream 1 > "$OUT.txt" 2> "$OUT.stats" || fail "decoder failed"
python3 count_es.py "$OUT.txt" "new 2.4 Msps demodulator"
cat "$OUT.stats"
if [ -n "$DUMP1090" ] && [ -x "$DUMP1090" ]; then
  echo "dump1090-fa on the same file: $("$DUMP1090" --ifile "$OUT" --iformat UC8 --raw --fix 2>/dev/null | grep -c '^\*') frames (all types)"
else
  echo "(set DUMP1090=/path/to/dump1090 to compare with dump1090-fa on the same file)"
fi
)
```

---

## 6. Decisions that are yours

1. ~~Who is the first user?~~ **Answered:** the instrument cluster
   itself (section 0); user types are display profiles.
2. ~~Barometer~~ **Answered:** two, of different makes, cross-checked
   (4.2, 4.3). Still open only: which boards actually arrive, and where
   on the mount they sit.
3. **Region.** UAT and TIS-B are US-only; ADS-L is European; FLARM and
   FANET frequencies differ by region. Which comes first?
4. **Transmitting.** Stay receive-only, or allow an opt-in FANET/ADS-L
   position beacon so the user is seen by gliders and paragliders? It
   is the single biggest safety gain for that group and the only item
   here with a privacy cost and regulatory limits.
5. **Demo features.** May the music player, NFC, microphone and
   ethernet tasks go? (You wanted camera support eventually; that would
   stay.)
6. **Second SDR's first job**: 978 MHz UAT or airband voice.
7. **How the device describes itself.** I recommend a fixed "advisory,
   not certified" statement in the UI and documentation, and no feature
   that tells a pilot what manoeuvre to fly.
8. **Feeding third parties.** Beast output on the local network is
   harmless. Joining a multilateration or tracking network sends your
   frames and the receiver's position to someone else. Opt-in only, or
   not at all?
9. **Airspeed probe.** A differential-pressure sensor and a small pitot
   tube would give paramotors and ultralights a real airspeed. Worth a
   hardware variant, or out of scope?
10. **Which saved WiFi network wins** (r12) when several are in range:
    the strongest signal, or a fixed order of preference? I would use
    your order of preference among those in range, and fall back to
    signal strength only between networks you have not ranked.
11. **How WiFi credentials are stored** (r12). Today's single network
    and its password sit in plain text in flash with the other settings;
    neither flash nor settings encryption is enabled. Several networks
    make a lost or resold device worth more to whoever reads its flash.
    Keep plain storage, or move to encrypted storage tied to the chip?
12. ~~Who may command the device over MQTT~~ **Answered:** commands
    and firmware updates carry a seal the device validates, to John's
    seal specification (r11). Unsealed commands are accepted unless the
    device is in strict mode.
13. **Pass criteria** (5.0). The proposed numbers are starting points.
14. ~~Console speed~~ **Answered** (coh4): the console stays at 115200
    for anything that opens the port, and a host that wants more
    negotiates it for its session and loses it when it goes away
    (`UPLINK.md` section 9.8). Nothing else that reads the port breaks.
    On this board the web app pays for it with a second boot at connect,
    because opening the port resets the processor.

## 7. What I am unsure of

- CPU and memory headroom on the device: unmeasured until Phase 0.
- Whether the SX1262 as wired on this board meets the FANET and ADS-L
  physical layers: likely, not verified against the specifications.
- The inertial error table: order-of-magnitude physics with assumed
  sensor figures, not a characterisation of this IMU in a vibrating
  cockpit.
- GPS module limits (maximum update rate) and whether an outdoor
  handheld antenna position is good enough for attitude aiding.
- Map data licensing per country.
- The Beast timestamp's reference point (which instant of the frame it
  marks) – to be checked against the format before claiming
  compatibility; ours marks the start of the preamble.
- USB delivery jitter on this host stack, and therefore the "tens of µs"
  figures: estimates until measured on the device.
- Which hardware-capture peripheral to use for PPS and sensor
  data-ready on the ESP32-P4 under ESP-IDF 5.4.1, and the PPS accuracy
  of the GPS module on v2.0.
- Frame timing was measured on simulated signals through the modelled
  front end, not on a real dongle.
- That the jump plane carries ADS-B Out, and how well we receive it
  from inside the cabin at that range (it may overload the front end).
- How far cabin pressure departs from outside pressure in that aircraft,
  door closed and door open.
- How well a horizon camera works as an attitude reference in haze or
  over featureless terrain.
- The Qwiic connectors on this board: their supply voltage and which
  pin is data and which is clock. I have not seen the schematic; a
  meter settles the first, a continuity check or the schematic the
  second.
- Barometer specifications at 18,000 ft with the door open: the BMP581's
  accuracy figure covers -5 to 65 °C and its relative accuracy 700 to
  1100 hPa. Outside those windows the flights are the measurement.
- The LPS22DF's bus address (from memory: 0x5C or 0x5D; the BMP581 is
  0x47 or 0x46) and whether a plug-in board for it can be bought.
- The barometer disagreement threshold: unknown until bench data.
- Software I2C timing while the decoder is at full load: unmeasured.
- Whether ESP-IDF 5.4.1 has a usable software I2C driver; I am assuming
  not and would write a small one.
