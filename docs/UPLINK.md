# UPLINK.md – the receiver and its uplink under heavy traffic (r7 to r9)

Status as of 2026-11-13. Sections 1 to 4 were written with r7, section 6
with r8; section 7 describes r9, which fixes finding 2.2, and section 8
lists what is planned next. Written in answer to a field report; records what
the code showed, what r7 changes, what is still not explained, and what to
capture in the next test. Nothing here has run on a device.

## 1. The report

The unit was receiving more than 100 aircraft, uplinking by MQTT over a 5G
hotspot. It worked well until the aircraft count passed about 60 to 70;
above that the uplink was "dropping".

## 2. Findings

Three, in decreasing order of certainty.

### 2.1 The feeder sent 64 messages every 2 s, not 64 aircraft (certain; fixed in r7)

`on_msg()` in `class_driver.c` calls `mqtt_feeder_update_aircraft()` once
for every decoded message. Up to r6 the feeder appended each call to a
64-entry buffer (`MAX_AIRCRAFT`), flushed it every 2 s, and dropped
everything that arrived while it was full. Nothing merged two messages
from the same aircraft.

So the uplink carried at most 32 messages a second. Past that, each batch
held the first 64 messages decoded after the previous flush – a short
slice of the 2 s – and the strong, frequently heard aircraft took most of
the slots, several times each, while others were absent. The limit is on
messages, so it is reached well below 64 aircraft; but 64 entries per 2 s
is also a hard ceiling on how many aircraft can be kept current at that
rate, and it sits where the report puts the trouble.

`harness/feeder/coverage_sim.py` puts numbers on it
(`harness/results/feeder_coverage.txt`). It is a model with assumed
message rates, not a measurement:

| aircraft | old feeder: share of them in an average batch | aircraft with a gap over 30 s between uplinked updates in 10 min (old / what the decoded stream allows) |
| ---: | ---: | ---: |
| 40 | 63 % | 6 / 1 |
| 60 | 49 % | 21 / 7 |
| 80 | 43 % | 32 / 8 |
| 100 | 37 % | 48 / 14 |
| 150 | 30 % | 82 / 18 |

(Rates spread from 0.1 to 6 decoded messages per second per aircraft. With
every aircraft at 2 per second the shares are 81 %, 66 %, 55 %, 47 %, 35 %
and no aircraft has a 30 s gap until 150.) Decline is steady, not a cliff;
whether it looks like one depends on how quickly the consumer drops an
aircraft it has not heard about.

Once the buffer saturates the feeder's traffic stops growing: 64 entries,
10.7 kB as one MQTT message every 2 s, measured with ESP-IDF's own cJSON
for aircraft with every field known (`make old_batch_size` in `harness/`).
That matters for 2.3.

### 2.2 The receiver is deaf while the decoder works, and the log line is most of that work under load (certain from the code; size not measured)

Not in the MQTT code, but it limits everything the uplink can carry.

- Samples are fetched with one synchronous USB transfer at a time
  (`esp_libusb_bulk_transfer()`): eight transfers of 16,384 bytes fill a
  buffer – 27.3 ms of signal at 2.4 Msps – and then `demodulate()` runs
  in the same task. No transfer is pending while it runs, so the dongle is
  not read, and what it produces in that time is lost apart from whatever
  its own small buffer holds. **Time inside the decoder is air time not
  received.** The `Decode: N% of one core` figure added in r4 is therefore
  also, roughly, the share of traffic the receiver never sees. The harness
  figures in `DECODER.md` are for a continuous sample stream.
- Inside the decoder, `on_msg()` prints one log line per decoded message.
  The console is UART0 at 115200 baud with no UART driver installed, and
  in that configuration ESP-IDF writes each character with a busy-wait
  when the 128-byte transmit FIFO is full (`uart_tx_char()` in
  `esp_driver_uart/src/uart_vfs.c`, v5.4.1). 115200 baud is 11,520 bytes
  a second. A line for an aircraft with its full state is 135 to 150
  bytes: about 12 ms each, about 80 lines a second at most.
- All lines of one buffer are printed back to back and the FIFO holds less
  than one line, so every message beyond the first costs about 12 ms of
  deafness, in addition to the decoding itself, the SD log line and two
  great-circle calculations.

Worked example, with one assumed number (15 ms of decode time per buffer;
the real figure is not known) and 140-byte lines:

| decodable messages per second on air | share of air time received | messages decoded per second |
| ---: | ---: | ---: |
| 0 | 65 % | – |
| 100 | 42 % | 42 |
| 200 | 28 % | 56 |
| 400 | 17 % | 67 |
| 800 | 9 % | 74 |

The decoded rate levels off near what the UART can print, whatever the
traffic. This is progressive too. It is an estimate until r7's `Capture:`
figure (section 3.3) is read on a device.

In command mode (after Ctrl+C on the console) log lines are held in
memory instead of printed, so the decoder is not slowed by them. A unit
left in command mode should receive visibly more than one streaming its
log.

### 2.3 Not found: a reason for the connection itself to drop

If "dropping" meant the MQTT or WiFi link going down on the device, the
code does not explain it:

- The feeder's traffic is constant above 32 decoded messages a second
  (2.1), so nothing about the MQTT connection changes between 20 aircraft
  and 100.
- esp-mqtt at the commit ESP-IDF v5.4.1 pins (`cac1552`) fragments large
  publishes and handles partial writes correctly. It drops the connection
  only when a write fails, when a write makes no progress for 10 s, or
  when a PINGRESP is missed.
- WiFi up/down handling is driven by events only; nothing in it depends on
  load.

Things that do grow with traffic and that the code cannot rule out: the SD
log, written per message on SPI beside the SDIO WiFi link (the contention
`PATCHES.md` describes); internal DMA-capable memory, which the hardware
AES in TLS needs; the hotspot or the broker. r7 logs enough to tell these
apart (3.3).

### 2.4 Seen in passing, not changed

- When WiFi drops, `mqtt_feeder_pause()` stops the MQTT client from the
  system event task. If the feeder is inside a publish that is waiting on
  the dead link, that call – and every other event handler behind it –
  waits for the client's 10 s network timeout. A WiFi blip can therefore
  cost up to 10 s more than it needs to.
- `aircraft_expire()` in `class_driver.c` is defined and never called;
  stale table entries are filtered where the table is read.

## 3. What r7 changes

### 3.1 The feeder (`mqtt_batch.c`, `mqtt_feeder.c`)

- **One slot per aircraft.** 256 slots (the size of the decoder's aircraft
  table), keyed by ICAO address; a later message replaces the aircraft's
  pending state. A batch carries every aircraft heard since the previous
  batch exactly once, with its latest state.
- **Bounded messages.** A batch is serialised into as many MQTT messages
  as it needs, each a complete JSON document of at most 3,800 bytes: it
  fits the client's 4,096-byte output buffer, one TLS record and the TCP
  send buffer in one piece. 100 aircraft are four messages (14 kB), 256
  are ten (36 kB).
- No allocation per batch (the old code allocated a copy and a cJSON tree
  every 2 s); the receiver position is read under its lock; a batch
  collected before a disconnect is discarded on reconnect instead of being
  sent stale; if a publish fails the rest of that batch is dropped and
  counted.
- `mqtt_feeder_stop()` asks the task to finish and frees its PSRAM stack.
  It used to delete the task wherever it was – in the middle of a publish
  that would have left the MQTT client locked – and leaked the stack on
  every `mqtt` settings change from the console.

`mqtt_batch.c` has no ESP-IDF dependencies and is tested on a PC
(`make check` in `harness/`): 417 batches, 8,663 messages and 53,103
aircraft entries – typical, adversarial and random inputs – each parsed
with Python's JSON parser and checked for one entry per aircraft, key
order, value ranges and size.

### 3.2 What changes on the wire

Topic, envelope, key names, key order and the rules for when a key is
present are unchanged. Three things differ, and the consumer should be
checked against them:

1. **Each aircraft appears at most once per batch.** Before, a busy
   aircraft could appear several times and others not at all.
2. **A batch can be several messages**, published back to back on
   `adsb/<id>/aircraft`, each with its own `rx_lat`/`rx_lon`. A consumer
   that updates aircraft by `icao` needs no change. One that treats each
   message as the complete picture would now see partial pictures – though
   it never received complete ones.
3. **Numbers are shorter.** `lat`, `lon`, `rx_lat`, `rx_lon` have 6
   decimals (0.11 m) instead of 15 to 17 significant digits; `dist` has 2.
   A position that is not a finite coordinate in range is omitted, and so
   is a non-finite distance.

Kept as it was, though arguably wrong: `hdg` and `brg` are sent as 0 when
unknown, because the tracker's "unknown" is 0 and the feeder sends
anything that is not negative.

### 3.3 Counters, so the next test is conclusive

`mqtt status` (console) now also prints:

```
  Link:      up 512 s; since boot 3 connects, 2 disconnects, 0 failed attempts, 0 WiFi drops
             last drop 512 s ago, after 1307 s up
             last error 514 s ago: type 1, errno 113 (...), esp-tls 0x8008, TLS -0x0000, CONNACK 0
  Updates:   184220 from the decoder, 171003 merged into a pending one, dropped: 0 (slots full) 0 (busy)
  Batches:   610 sent as 1830 messages, 6582100 bytes; aircraft entries sent 46930, not sent 0
             last batch: 97 aircraft in 4 messages, 13650 bytes
  Publish:   0 failed, 0 slow (over 1 s), longest 212 ms; longest gap between batches 2250 ms (nominal 2000)
```

(Example values.) `disconnects` are broker or network losses with WiFi
still up; `WiFi drops` are the WiFi layer reporting a disconnect. A long
`longest` publish or a large gap between batches means the link or the CPU
was holding the feeder up. On a disconnect the log gains one line with how
long the link had been up and the state of these counters, and transport
errors are logged with their errno and TLS codes.

`adsb status` (console) gained a `Capture` figure in r7 and a fuller set
of lines in r9; they are described in section 7.3. Once a minute the same
figures go to the log as a `LOAD:` line, so a recorded session shows them
over time.

## 4. What to capture in the next test

With r9 flashed, in traffic like the reported session:

1. Let it run for a few minutes, then on the console: `adsb status`,
   `mqtt status`, `top`. Keep the output.
2. Keep the session log. The lines that matter: `LOAD:` (once a minute),
   `MQTT: Disconnected from broker` and the `Link was up` line after it,
   `Transport error` / `Transport detail`, `Slow publish`, `Publish
   failed`, the `MEM:` heartbeat, and any `WRITE_DIAG` from the SD path.
3. Note what "dropping" looks like if it happens: aircraft missing or
   stale on the server while the device shows them, or the device
   reporting the link down.

Reading the result:

| observation | meaning |
| --- | --- |
| `disconnects` stays 0, server tracks the device | 2.1 was the cause |
| `Capture` near 100 %, `Decode` well under 100 % | r9 works: the receiver hears everything it can decode |
| `Decode` near 100 %, `Ring` dropping transfers | the decoder cannot keep up on one core: section 8, item 1 |
| `USB delivered` clearly under 100 %, `endpoint ran dry` climbing | the reader is being held up: section 7.4 |
| `Log` dropping lines | expected in heavy traffic: the console is full (7.2) |
| `disconnects` climbing, `WiFi drops` 0 | broker or network: read the error line and `longest` publish |
| `WiFi drops` climbing | below MQTT: WiFi link, ESP-Hosted, SD/SDIO contention |
| `dropped (slots full)` not 0 | more than 256 aircraft in 2 s |

## 5. What was agreed after r7

Samples go through a ring buffer in PSRAM, filled by a reader task that
does nothing else; the log line leaves the decoder task; and internal
DMA-capable RAM, which is short on this board, is not to grow. r9
(section 7) does this. A faster console (921,600 baud or more) was left
for later: it needs the web app to open the port at the same speed.
(Done in coh4, as a speed negotiated for one session: section 9.8.)

## 6. Aircraft limits (r8)

The 64 in section 2.1 was the only limit of that size, and r7 removed it.
r8 brings the remaining limits to one number, `ADSB_MAX_AIRCRAFT` = 256 in
`class_driver.h`:

| stage | before r8 | from r8 | sized by |
| --- | ---: | ---: | --- |
| decoder aircraft table, CPR cache | 256 | 256 | `AIRCRAFT_TABLE_SIZE`, `CPR_CACHE_SIZE` = `ADSB_MAX_AIRCRAFT` |
| scope: aircraft drawn | 128 | 256 | `MAX_SCOPE_AIRCRAFT` = `ADSB_MAX_AIRCRAFT` |
| scope: trails kept | 128, never reused | 256, reused | `SCOPE_TRAIL_MAX` |
| ADS-B app list | about 108 rows | 256 rows | `ADSB_LIST_BUF_SIZE` |
| MQTT batch | 64 messages (r6), 256 aircraft (r7) | 256 aircraft | `MQTT_BATCH_SLOTS`, checked at compile time |

Beyond 256 live aircraft the decoder evicts the one heard longest ago.

Also fixed: a trail slot, once given to an aircraft, was never released,
so after 128 different aircraft with a position since boot no new aircraft
got a trail on the scope. A slot is now reused once its trail has had no
new point for 180 s, the age at which the scope stops drawing the
aircraft.

The scope's "Max Aircraft" setting gains 192; "All" now means 256.

Cost. PSRAM: trails 3.7 MB (was 1.85 MB), everything else about 45 kB
more. `ARCHITECTURE.md` gives about 16 MB of PSRAM free in steady state;
the `MEM:` heartbeat line shows the real figure. Internal RAM: 10.7 kB
**less**, because the list buffer and the scope's per-aircraft work arrays
moved from internal `.bss` to PSRAM (`EXT_RAM_BSS_ATTR`; placement checked
in the linked image). This is the first use of PSRAM-placed static data in
this firmware. It is a standard ESP-IDF feature that the configuration
already enabled, and it has not run on the device.

Not measured: how long the scope takes to draw 256 aircraft. Each one is a
dot, a heading line and a text label, and trail drawing is budgeted, so
the cost should grow roughly in proportion; the scope lowers its own frame
rate when a frame takes long (down to 1 per second), and the info line
shows the rate. If it is too slow at full load, "Max Aircraft" caps it.

## 7. Continuous capture (r9)

Fixes finding 2.2. Up to r8 one task read 27 ms of samples and then
decoded them, and the dongle was not read while it decoded or printed.

### 7.1 The sample path

```
dongle --USB--> 3 transfers, always queued --> reader task --> ring (PSRAM, 1.08 MB) --> decoder task
```

- **USB (`usb_stream.h`, `esp_libusb.c`).** Three transfers of 5,632
  bytes are kept queued on the bulk endpoint and each is queued again
  from its completion callback. The host controller driver starts the
  next queued transfer from its interrupt handler (`hcd_dwc.c`, ESP-IDF
  v5.4.1), so the dongle is read without a break as long as the reader
  task answers within two transfer periods, 2.3 ms. The three use 16.9 kB
  of DMA-capable internal RAM where the single transfer before used
  17.4 kB.
- **Reader task** (the former read-and-decode task, now priority 11). It
  copies each finished transfer into the ring and queues the transfer
  again, about 850 times a second, and makes the tuner and bias-T writes
  other tasks ask for. Nothing else. The USB host library task, which
  hands it the transfers, moves from priority 4 to 12 so that MQTT (5) and
  audio (6) cannot hold either of them up.
- **Ring (`sample_ring.c`).** 192 slots, one transfer each: 225 ms of
  signal. One writer, one reader, no lock. The decoder works on the
  samples in place, so there is one copy between the USB buffer and the
  decoder where there were two. When the ring is full the newest transfer
  is dropped and counted.
- **Decoder task** (new, priority 3, PSRAM stack). Takes runs of up to 23
  transfers from the ring and decodes them; runs the adaptive gain loop.
  If it cannot keep up it still rests for one tick every 200 ms, so it
  never holds a core for good.
- **Losses are declared, not hidden.** A dropped transfer, or the endpoint
  running dry, is passed to the decoder as a gap with the number of
  samples lost (exact for the ring, an estimate for USB). The decoder then
  restarts cleanly – it does not build a frame from samples that were not
  adjacent on air – and its sample count moves on by that number, so frame
  times stay on the sample clock (`mode_s_stream_gap()`). Before r9 the
  gaps between reads were invisible to it.

### 7.2 The log line

`on_msg()` no longer writes to the console. Its lines – one per message,
plus the `ICAO:`, `CPR:`, `DF18:` and `LOAD:` lines – go into a 16 kB
queue and a task of their own (priority 2) writes them. The line format
and time stamps are unchanged; the stamp is taken when the message is
decoded. The console UART is polled, so a writer that simply printed
would spin on it and take a core from the display whenever the log is
busy; this one works out when the UART will have room for the next line
and sleeps until then.

**What changes for anything reading the log:** when messages arrive
faster than the console carries them (about 80 lines a second at 115200
baud), the queue fills and lines are dropped and counted. Before r9 every
decoded message was printed, because the decoder slowed down to the
console's speed and lost the radio samples instead. The SD log, the
display and the MQTT feeder get every message.

**What changes for the SD log:** it now records every decoded message, so
in heavy traffic it grows several times faster than before.

### 7.3 `adsb status`

```
  Decode:    43.0% of one core, peak 61.2% (last 5 s; must stay under 100% to keep up; 'top' shows all tasks)
             of which SD log 1.9%
  Capture:   99.8% of air time decoded, lowest 97.3% (USB delivered 99.8%)
  USB:       3 transfers queued, endpoint ran dry 14 times
  Ring:      4% full now, 31% at most; 0 transfers dropped; 14 gaps passed to the decoder
  Log:       79 lines/s, 11020 B/s written (console carries 11520 B/s); dropped 18230, 1240 in the last 5 s
```

(Example values, not measurements.) `Capture` is measured: sample bytes
that reached the decoder against what the dongle produces at the
configured rate. `USB delivered` is the same ratio taken where the
transfers arrive. The `LOAD:` log line, once a minute, carries the same
figures.

### 7.4 Not verified

Nothing here has run on the device. What the code cannot tell:

- **Whether the decoder keeps up.** It needs less than one second of one
  core per second of signal. The instruction counts in `DECODER.md` 5.5
  suggest it does, with unknown margin; `Decode` shows it. If it does not,
  the ring overflows steadily and `Capture` settles at whatever the
  decoder can manage – still far more than before r9 in traffic.
- **Whether 2.3 ms is enough for the reader.** Tasks above it can still
  delay it: the network stack, the ESP-Hosted tasks, and timer callbacks
  (the scope's once-a-second trail recorder runs there and takes the
  aircraft lock). Flash writes stop everything for their duration. Each
  such delay costs a millisecond or two and is counted as `endpoint ran
  dry`. If the count is high, a fourth transfer (5.6 kB more DMA RAM) buys
  another 1.2 ms.
- **The three-transfer allocation at boot** fits the 20 kB hole the
  firmware reserves for it; with fewer than three the firmware runs and
  says so in the boot log (`only N of 3 bulk transfers allocated`).
- **Behaviour on unplugging the dongle, on `adsb off`, and in USB mass
  storage mode**: the stream is cancelled, the decoder task is stopped and
  the device closed, in that order. Reviewed, not exercised.
- **Gain changes on the RTL-SDR Blog V4**, whose control requests stall
  and are retried with a 50 ms wait: the stream is kept fed during the
  wait, but each gain step may still cost a few milliseconds.

Tested on a PC (`make check` in `harness/`): the ring – directed cases, a
random run against a model, and a two-thread run in which the writer
outruns the reader on purpose, checking that every byte delivered is the
right one and every byte not delivered is covered by a declared gap of
exactly that size; and `mode_s_stream_gap()` – at both sample rates, five
loss patterns and three chunk sizes the frames decoded are identical to
decoding the two sides of the loss as separate streams.

## 8. Planned next

The plan is in `ROADMAP-AVIATION.md` section 5.0, which is the single
plan: r11 update slots, crash dumps and sealed commands; r12 WiFi
self-healing and several saved networks; r13 whatever r9's numbers call
for on the capture path; r14 console speed; then the instrument work.

## 9. Instruments for testing (r10)

### 9.1 `diag`

One console command that prints, between `===== DIAG BEGIN =====` and
`===== DIAG END =====`: firmware version, the reason for the last reset,
how often the task watchdog fired, memory, GPS, `adsb status`, the last
replay, `mqtt status`, `wifi status` (no passwords), `top`, and the
checks below. A field report is that block and the session log.

### 9.2 The checks

`diag` ends with the pass criteria proposed in `ROADMAP-AVIATION.md` 5.0,
evaluated on the device:

```
  Checks over the last 01:02:11 – proposed pass criteria (docs/ROADMAP-AVIATION.md 5.0)
  [PASS] capture never below 99.0% (lowest 99.6%)
  [FAIL] decode never above 80.0% of one core (peak 91.2%)
  [PASS] no transfers dropped by the ring (0)
  [INFO] endpoint ran dry 14 times; 1820 log lines dropped
  [PASS] no MQTT disconnects (0; WiFi drops 0, failed publishes 0)
  [PASS] task watchdog has not fired since boot (0)
```

(Example values.) They cover the time since `diag mark`, since the last
replay, or since boot, whichever is latest; run `diag mark` when the test
conditions are in place. The thresholds are constants at the top of the
`diag` code in `serial_console.c`.

### 9.3 Replay: `adsb replay <file>`

Feeds a recording from the SD card through the sample ring, the decoder
and the tracker in place of the dongle. The file is raw unsigned 8-bit
I/Q at 2.4 Msps – what the capture block in `ROADMAP-AVIATION.md` 5.1
writes (`rtl_sdr -s 2400000`). `adsb replay` with no argument shows
progress, `adsb replay stop` ends it, and the result is logged:

```
REPLAY: /sdcard/adsb-capture-2400.cu8: 288000000 bytes, 60.0 s of signal, 61234 messages, decoder busy 23400 ms = 39.0% of one core at real time, 0 transfers dropped, took 183 s
```

(Example values.) What it gives:

- **What decoding costs**, at a known input: decoder time against the
  length of the recording. Under 100 % of one core means the decoder
  keeps up with a live dongle in that traffic; the margin is how much
  busier it can get.
- **A repeatable test of the r9 path.** The file is fed only as fast as
  the decoder takes it, so nothing is dropped, and the cache of recently
  seen addresses is aged by the recording's sample count instead of the
  wall clock. The same file gives the same message count every time.
- **A cross-check against the PC.** In `harness/`:
  `./host_mode_s FILE --rate 2.4 --stream 1 --df18 0 --cache-clock 1 | grep -c '^MSG'`
  runs the same decoder on the same samples. The device's count should
  equal it, less any frames with an all-zero address. I expect equality
  and have not seen it demonstrated; a difference would be a finding.

While a replay runs: live samples are discarded (the dongle keeps being
read); nothing is sent to MQTT, written to the SD log or printed per
message; the adaptive gain loop is paused; the aircraft on the display are
the recording's. The aircraft table is cleared when it starts and when it
ends, and the checks of 9.2 restart when it ends. It needs the receiver
running – the dongle attached – because it borrows its decoder. The card
delivers samples more slowly than the dongle does, so a minute of
recording takes several minutes to replay.

Not verified: any of it on a device, as before. In particular how fast
the card reads, and that live reception resumes cleanly afterwards.

### 9.4 Decoder stages on the console (coh2): `adsb dec`, `adsb coh`

The decoder's repair stages can be switched while the receiver runs, so
one flash can compare them on the same antenna or the same recording.
`docs/DECODER.md` sections 4 and 5.7 say what each stage is and why the
default is what it is.

| command | effect |
| --- | --- |
| `adsb dec` | show the setting, the repair counters and the decoder task's unused stack |
| `adsb dec default` | 1-bit correction only – with `adsb coh off`, exactly r10's default image |
| `adsb dec soft` | + soft rescue, known-address gate, strict validation (**boot default**) |
| `adsb dec fix2` | + exact 2-bit correction (r10's `fix2` image; not recommended) |
| `adsb coh on` / `adsb coh off` | the coherent second look (**on at boot**) |

Nothing is stored: a restart returns to the build's settings
(`CONFIG_ADSB_DECODE_SOFT`, `_COH`, `_FIX2`). A change is taken over by
the decoder task between two of its runs.

Where the stages report:

- `adsb status`, and so `diag`: a `Decoder:` line with the setting, a
  `Repairs:` line (frames repaired by each kind since the receiver
  started, and repairs refused by the two gates) and a `Coherent:` line
  (re-slices, how many stopped early, frames recovered).
- the log, once a minute after the `LOAD:` line:
  `DECODER: 1-bit fix, soft rescue (known address, strict), coherent look; in the last 60 s: repaired 1-bit 412, soft 96, 2-bit 0; coherent look 20100 re-slices, 11300 stopped early, 1830 frames recovered`
  (example values).
- a replay: a second line,
  `REPLAY: decoder: <setting>; repaired 1-bit N, soft N, 2-bit N; coherent look N re-slices, N stopped early, N frames recovered`.
  At the start and end of a replay the coherent look's noise reference is
  reset, so a replay does not depend on what was on the air before it.

**Reading the live counters.** "Frames recovered" overstates the gain:
on the recorded capture about half of them would have been decoded from
the next candidate position a sample later. The honest comparison is a
replay of one file in two settings.

**A first session with coh2**, with the dongle attached and aircraft
showing:

1. *Baseline, as r10:* `adsb dec default`, `adsb coh off`, `diag mark`,
   a few minutes of traffic, `diag`. This is the measurement roadmap
   rule 1 was waiting for.
2. *The default:* `adsb dec soft`, `adsb coh on`, `diag mark`, the same
   few minutes, `diag`. Compare `Decode` and `Capture` with step 1; the
   message rates cannot be compared, the traffic has changed.
3. *The same samples in both settings:* with a recording on the card,
   `adsb dec default`, `adsb coh off`, `adsb replay <file>`; then
   `adsb dec soft`, `adsb coh on`, `adsb replay <file>`. Keep the four
   `REPLAY:` lines. The message counts give the gain; the two `decoder
   busy` figures give what it costs on a P4, which nothing so far has
   measured.
4. *Against the PC:* `python3 harness/experiments/replay_expect.py
   <the same file>` prints the counts the host build gets in each
   setting. They should match the device's; with the coherent look on a
   frame or two of difference would not be alarming (it works in
   single-precision float), more would be a finding.

Not verified: any of this on a device. The switches and counters compile
and are exercised on a PC only through the decoder functions they call.

### 9.5 Gain on the console (coh3): `gain`, `gain sweep`

With the 2.4 Msps demodulator the adaptive gain steers on signal levels
and yield, not on the error rate (`docs/DECODER.md` section 5.8 has the
reasons and the rules).

| command | effect |
| --- | --- |
| `gain` | the gain, the phase, and what the loop sees: noise in ADC counts and the headroom above it, the share of decoded frames at full scale ("loud"), the share of clipped samples, squitters lost |
| `gain auto` | adaptive, from the top step |
| `gain restart` | adaptive, phase 1 again from the gain in use |
| `gain <dB>` | fixed |
| `gain sweep [seconds]` | hold each step from 49.6 down to 19.7 dB for that long (default 10 s, 3–600) and log what it yields; then back to the gain in use before. About three minutes at the default |
| `gain sweep stop` | end a sweep early |
| `gain band ...` | the 2.0 Msps loop's error-rate band; stored, not used by this loop |

In the log:

```
GAIN: [P1] Holding 49.6 dB (err=31%, pos=9.4/s, ok=283, sig=6120 delta=3314, loud=4% noise=3.2 headroom=32dB clip=0.02%)
GAIN: Converged at 49.6 dB (err=31%, pos=9.2/s, range=85nm, sig=6093 delta=3301, loud=4% noise=3.2 headroom=32dB clip=0.02%) → steady-state
GAIN: [P2] Steady: 49.6 dB, EMA err=32%, pos=9.3/s, range=85nm, sig=6101 delta=3290, loud=4% noise=3.2 headroom=32dB clip=0.02%
GAIN: [Manual] 44.5 dB, err=30%, pos=8.8/s, range=85nm, sig=3400 delta=2100, loud=1% noise=1.8 headroom=37dB clip=0.00%
GAIN: [Sweep] 49.6 dB: 28.4 msg/s, ES 9.8/s, pos=9.1/s, err=31%, sig=6120 delta=3314, loud=4% noise=3.2 headroom=32dB clip=0.02%
```

(example values). The lines keep the older ones' beginnings and the
`err=`, `pos=`, `range=`, `sig=` and `delta=` fields in their old order;
the level fields are added at the end. Two differences a parser may meet:
`band=` is gone from the `[P2] Steady` line, and `err=` now means
squitters lost over squitters seen, a figure around 30 % on a busy band
at any gain, where it used to read 99 %. The lines go through the log
queue like the aircraft lines; the decoder no longer waits for the console
to print them.

`adsb status` (and `diag`) carry a `Levels:` line and `Squitters:` in
place of `CRC error:`.

### 9.6 The first run on a device (2026-11-18, coh2): what it found

The log of that run (a busy band, a T-Display-P4 that had been running a
build of 2026-04-18) is the first evidence from hardware for anything
since r7. In order of weight:

1. **More traffic** than the build it replaced – about ten aircraft and
   thirty messages a second in the first ten seconds, at 49.6 dB.
2. **The gain loop then removed 27 dB of gain in half a second** and the
   rate fell to about three a second. `docs/DECODER.md` section 5.8.
3. **The decoder filled core 0**: task watchdog reports every five
   seconds, I2C transfers timing out. Section 5.9.
4. **The device had been running `5b1dbb7-dirty`**, built nine days after
   the tip of the fork this repository was cut from (`6fd3692`,
   2026-04-09), from a commit that is not on GitHub. Three differences
   show in the two boot logs, and there may be more:
   - it seeded the clock from the RTC as UTC; this tree reads the RTC as
     local time and landed eight hours out (`RTC seed: … 13:13:05 UTC`
     for an RTC at 05:13:05). The seed only serves TLS before GPS or NTP
     arrive, and this tree writes local time back to the RTC once they
     do, so the two builds disagree about what the RTC holds;
   - it printed `[SD] LDO VO4 set to 3300mV (VDDPST_5 for GPIO39-48)`;
     this tree asks ESP-IDF's SD power control for the same LDO and gets
     `ldo: The voltage value 0 is out of the recommended range`. The card
     mounted and logged in both. Not touched in coh3;
   - it did not print the bus driver's info lines. Off in coh3
     (`PATCHES.md` section 2).
5. **Flashing the merged image replaced the partition table** (two OTA
   slots → one factory app) **and with it the NVS**: settings back to
   defaults, a new Meshtastic node identity and key pair. `README-FLASH.md`
   in the firmware kit now says so, and how to keep the NVS.
6. The I/O expander did not answer the first probe after the reset that
   followed flashing (it had been mid-transfer); the start-up now waits
   for it.

**Wanted from the next run** (coh3), about ten minutes of work:

1. The boot log up to and including the first `GAIN: Converged` line, and
   the first `LOAD:` line a minute later.
2. `diag` once it has run for a few minutes – `Decode:` (share of a core)
   and `Capture:` (share of air time decoded) are the two numbers section
   5.9 could not measure.
3. `gain sweep`, twice, and the `GAIN: [Sweep]` lines.
4. If `task_wdt` appears again: `top`, right then.
5. A replay, as in section 9.4 step 3, if a recording is on the card.

### 9.7 Around the decoder: what a message costs, and the aircraft table's lock (coh4)

**The report.** With coh3, near 300 messages a second the ADS-B app (the
list) on the device stopped updating; the Scope app, in the same traffic,
did not. So the decoder kept up, and what gave way was around it.

**What the two apps do.** The Scope redraws inside the display task,
times each frame and stretches its own period to match (33 ms to 500 ms
and beyond): it slows itself under load. The ADS-B app had its own task,
at priority 2 – above the display task's 1 – on a fixed half-second
cycle, and on every cycle it took the aircraft table's lock three times:
twice in `adsb_get_stats()` and once in `adsb_format_aircraft_list()`,
which held it through a great-circle distance for every aircraft, a sort
and six `snprintf()` calls per row. The decoder takes the same lock for
every message.

**What that cost.** The ESP32-P4's FPU is single precision; `double` is
done in software. One `haversine()` was about 38,000 instructions, and
`printf("%f")` several thousand more per number. Counted on the device's
instruction set with the firmware's compiler flags
(`harness/experiments/cost_app_rv32.py`, `harness/results/cost_app_rv32.txt`;
instructions, not time – see `DECODER.md` 5.9):

| | coh3 | coh4 |
| --- | ---: | ---: |
| list refresh, 100 aircraft, under the lock | 4.17 M | 7.5 k |
| list refresh, 100 aircraft, in all | 4.17 M | 0.72 M |
| list refresh, 250 aircraft, under the lock | 11.06 M | 15.2 k |
| nearest and farthest, 100 aircraft, under the lock | 3.03 M | 56 k |
| one message from an aircraft with a position: console line, SD row, MQTT distance | 199.7 k | 23.1 k |
| the same at 300 messages a second | 59.9 M/s | 6.9 M/s |

The last row is in the decoder's own task. The demodulator is 149 M
instructions per second of signal (`DECODER.md` 5.9), so at 300 messages a
second the three haversines and the number formatting of each message
added 40 % to it. That is the larger finding: it is load that grows with
the message rate, in the task that must keep up, and it is gone.

**What changed in coh4.**

1. `adsb_fmt.c` (new): great-circle distance and bearing in single
   precision, with the coordinate differences taken in double first, in
   forms without a difference of nearly equal terms; and decimal text by
   scaling and rounding. Against the old code (`harness/fmt/test_fmt.c`,
   `harness/results/fmt.txt`): distance within 0.52 m and bearing within
   0.00003° over two million pairs out to 450 nm; number text identical to
   `printf` on 3.8 million values, including exact ties. What is printed
   changes only where single precision rounds a distance or a bearing the
   other way: 0.02 % of distances to 0.1 km, 0.001 % of bearings.
2. `on_msg()` updates the table, takes a copy of the aircraft, and
   **releases the lock** before it builds the console line, the SD row and
   the MQTT update. It was held to the end: through three haversines, two
   lines of text, any SD card write the row triggered, and – on the first
   message of an aircraft – a lookup in the aircraft database that reads
   the SD card when it is not in the cache.
3. One distance per message instead of three, and the console line is not
   built at all when the log queue has no room for it (it was built and
   then dropped: two lines in three at 300 messages a second on a 115200
   baud console).
4. `adsb_format_aircraft_list()` copies the table under the lock and does
   everything else after it; `adsb_get_stats()` takes the lock once
   instead of twice and computes only a ranking value under it;
   `adsb_get_aircraft_for_scope()` computes range and bearing after it.
5. Seen on the way: the nearest aircraft, if it had no callsign, was shown
   with the callsign of an earlier candidate. Fixed.

**A deadlock that was waiting.** Leaving the ADS-B app called
`vTaskSuspend()` on its task from the display task, wherever the task
was – including inside the lock. Then the decoder stopped at its next
message, the status bar's task stopped at its next refresh, and opening
the Scope would have stopped the display task too; opening the ADS-B app
again released it. The chance per exit was the share of time the lock was
held, so it grew with traffic: a few percent at 100 aircraft, 10 to 20 %
at 250. In coh4 the ADS-B and Meshy app tasks are told through a flag and
a task notification and park themselves at the top of their loop, where
they hold nothing. (The factory-test windows inherited from LilyGO's base
– microphone, IMU, Ethernet, AT – still suspend their tasks from outside.
They are not part of normal use and were left alone.)

**Not established.** Why the list stopped updating. At the 50 to 100
aircraft that 300 messages a second corresponds to, the list's own work
was a few percent of a core twice a second: too little to stall a display
by itself. The per-message cost (above) and the lock held through SD
writes are better candidates, and both are removed, but that is an
argument, not a measurement. If it happens again on coh4: is the touch
screen still alive, did it follow a change of app, how many aircraft –
and `top`.

### 9.8 The console's speed (coh4)

**What it is.** The console always starts at 115200 baud. A host that
wants more asks, and keeps it only while it is there
(`console_link.h`, `console_link.c`; the commands are in
`serial_console.c`):

    host    baud 921600                      at 115200, in command mode
    device  BAUD: switching to 921600 – …    then the prompt, then its UART changes
    host    changes its own speed; Ctrl+C, baud ok
    device  BAUD: up 921600
    host    the byte 0x05 every two seconds  (answered: BAUD: ok 921600)
    …
    host    the byte 0x04 when it leaves – or nothing

The device goes back to 115200 if `baud ok` does not arrive at the new
speed within 5 s (a speed the bridge chip or the cable cannot do costs
five seconds and nothing else), at once on 0x04, and after 10 s without
hearing the host. A command counts as hearing the host, at its start and
at its end, and so does upload data. Both bytes work in log mode and in
command mode; `BAUD: ok <speed>` is also the answer at 115200, which is
how a host tells this firmware from an older one without entering command
mode.

`baud` alone shows the state and the speeds offered: 115200, 230400,
460800, 921600, 1000000, 1500000, 2000000, 3000000. From the console
UART's 40 MHz crystal each is within 0.2 % (`harness/link/test_link.c`).
`baud <speed> stay` is for a person at a terminal: no confirmation and no
keepalive, until `baud 115200` or a reset. Before the first change the
firmware reads the UART's divider back and refuses if it is not what
115200 from the crystal gives, rather than set a speed from a wrong
assumption about its clock.

**The catch on this board: opening the port resets it.** The USB-C port
is a UART behind a bridge chip, and its RTS line is wired to the
processor's reset, DTR to the boot-mode pin. The evidence: in the first
log from a device (9.6) both plain connects from the web app are followed
by `rst:0x1 (POWERON)` – the reset pin, not a software or USB reset – one
of them with `boot:0x307 (DOWNLOAD…)`, and LilyGO's own notes say to
disable RTS "because the RTS line can reset the P4". (The project's
README calls the port "USB Serial/JTAG". It cannot be: console input
works, and in this configuration ESP-IDF takes input from UART0 only.)

Web Serial can change a port's speed only by closing and reopening it.
So three kinds of host:

- **A program that can change the speed of an open port** (a terminal,
  `tools/adsb-fastlog.py`): the exchange above, exactly. No reset beyond
  the one its own open causes.
- **The web app, on a device that resets when the port is opened:**
  `baud 921600 next` stores the request for the next start only (NVS,
  namespace `console`, removed as it is read; dropped after 15 s if no
  reset follows). The page then reopens at 921600, which is that reset;
  the firmware starts its console at 921600 and waits 10 s for `baud ok`.
  One more boot, about 15 s, at every connect that asks for a faster
  speed. The bootloader's and the early application's text is at 115200
  and unreadable to the page during that boot; the page keeps the version
  and device ID it read from the first one.
- **The web app, where opening does not reset the device:** ask, reopen,
  confirm. The page tells the two apart by whether a boot banner followed
  its own connect.

**`tools/adsb-fastlog.py`** (Python 3, nothing else): connects at 115200,
waits for the console, negotiates (921600 by default, lower speeds if that
is not confirmed), logs every line to a file, keeps the link alive, finds
the device again if it restarts, and on the way out appends the device's
own `adsb status` and `baud` and says goodbye. Against the simulated
device with 300 lines a second: none missing at 921600, 73 % never sent at
115200 (`harness/results/link.txt`).

**The web app** (`adsb_scope.htm`, v1.0.9; v1.0.10 with coh5): "Serial speed" in the
firmware panel (default 921600; 115200 is the old behaviour), shown in the
session panel. It also now: queues its writes (a keepalive can no longer
collide with a command's write on the port), hides the garbage of a boot
log at the wrong speed, and – when a connect leaves the device in the
ROM's download mode, which the DTR/RTS race on open does now and then –
pulses RTS with DTR released to restart it, instead of waiting there.

**Flashing from the page.** esptool-js changes speed by closing and
reopening the port, which on this board would reset the processor out of
the loader, so the page had been pinned to 115200. It now opens the port
at the flashing speed from the start and lets the ROM loader take its
speed from the sync packets (esptool's protocol notes: "the sync packets
can be sent at any baud rate, and the UART peripheral will detect this").
Default 460800, where one cycle of the 40 MHz clock is 1.2 % of a bit;
if the loader does not answer there, or a write fails, the page does the
whole thing again at 115200 and does not try the faster speed again until
the setting is chosen anew. **Whether the ESP32-P4's ROM does this is not
confirmed** – Espressif's page for the P4 only recommends 115200 – so the
first flash from the page will show which. (As delivered in coh4 the
fall-back for a loader that does not answer could not work – next
paragraph.)

**Three things coh4's flashing got wrong (coh5).** coh4's flashing was
tested against a stand-in for esptool-js that I wrote from the page's
calls. Run against esptool-js 0.5.4 itself (below), it showed:

- *The fall-back to 115200 could not happen.* `ESPLoader.main()` hands the
  baud rate to `connect()` in the place of the number of attempts –
  460800 attempts, each one to four seconds. With a loader that does not
  answer at the faster speed the page would have stayed at "Connecting"
  for as long as it was left there. The page now does `main()`'s steps
  itself (`connect()` with three attempts at a speed that has 115200
  behind it and ten at the last, then `runStub()`), so it comes back: ten
  seconds at 460800 before it goes on to 115200 in the test, 45 s in all
  before "Bootloader connection failed" on a device that never enters its
  loader. A faster speed is now only written off when the loader then
  answers at a slower one.
- *The restart after a flash never ran.* The page called
  `loader.hardReset()`, which esptool-js 0.5 no longer has; every flash
  ended with "Auto-reset failed", and the device started the new firmware
  because the reconnect three seconds later resets it (opening the port
  does, on this board). `after('hard_reset')`, its replacement, releases
  RTS without having asserted it. The page now pulses RTS on the port
  itself, with DTR released.
- *Every byte was being traced.* The page created the transport with
  esptool-js's tracing on: a hex dump of everything written and read, to
  the console and into a string kept in memory – 2.3 MB of text for a
  365 kB image in the test, some 40 MB for this firmware. Off now
  (`localStorage.setItem('flash_trace','1')` turns it on).

**Keeping the settings when flashing from the page (coh5).** The merged
image is written from 0x0, so it covered the settings partition with 0xFF:
every flash from the page reset the settings, the WiFi credentials and the
Meshtastic identity. With "Settings: keep when flashing" ticked – the
default; the page remembers the choice – the same image is written in two
pieces, around the NVS partition, which the page finds in the image's own
partition table (0x9000, 24 kB). It first reads the device's partition
table through the loader. If the device has its settings partition in the
same place – true of the April layout as well as the current one – they
are kept. If it has them somewhere else, the page says where and asks
before it writes the whole image and resets them; answered with no, it
writes nothing and restarts the device. A device without a table has
nothing to keep. A file that is not a merged image is refused (with the
box unticked: questioned), where it used to be written at 0x0 whatever it
was.

**How the page's flashing is tested (coh5).** `harness/link/check_page_flash.js`
runs the page's own `doFlash()` with the real esptool-js 0.5.4 bundle –
the file the page loads from unpkg, fetched by `make esptool-js` and
checked against a pinned sha256 – against an emulated chip
(`harness/link/fake_esp.js`): the reset and boot-mode lines, the ROM
loader's SYNC, READ_REG and MEM commands, the stub's READ_FLASH (with the
MD5 frame esptool-js leaves unread) and FLASH_DEFL commands, and a flash
memory that is erased as the stub erases – `handle_flash_begin` in its
`stub_write_flash.c`: the sectors from the first to the last of the size
it is given. Thirteen cases, also run on the real merged images: the
settings partition byte for byte as it was and everything else the new
image, on the current and the April layout; the two writes the stub is
given (0x0 + 0x9000, 0xF000 + the rest); the box unticked; settings
elsewhere with yes and with no; a blank device; a table that cannot be
read; a ROM loader that does not answer above 115200; a write that stalls
at 460800; a device that never enters its loader; files that are not
merged images; and that the device is restarted at the end.

What that leaves: the emulated chip answers at once and at any speed
unless told not to, so nothing here says whether the ESP32-P4's ROM
takes 460800 – only that the page copes either way. The stub's erasing
is taken from its current source; the stub binary inside esptool-js
0.5.4 was not taken apart. **Not tested: any of it on a device.**

**`baud probe [ms]`** is a diagnostic for the next step: it measures the
speed of what arrives with the UART's pulse counters, whatever the UART is
set to. If that works on the running device, a page could open the port at
the fast speed in the first place and be recognised, which would save the
second boot (at the price of the boot log). `adsb-fastlog.py --probe
921600` runs it.

**What is tested, and what is not.** The state machine has a unit test
(`harness/link/test_link.c`, 41 checks). `harness/link/sim_device.c` is a
stand-in for the device on a pseudo-terminal that runs the same state
machine, carries baud/10 bytes a second, turns both directions to garbage
when the two ends' speeds differ, and can reset on open, refuse speeds
above a limit, play older firmware, or land in download mode. Against it:
the logger through ten cases (`link/check_fastlog.py`) and **the web app's
own code** – the page's whole script run in Node, its serial port replaced
by the pseudo-terminal – through nine (`link/check_page.js`): both ways
up, older firmware, a speed that does not work with and without reset on
open, a device that restarts by itself, download mode, writes that meet.
Results in `harness/results/link.txt`.

None of it has run on hardware. In particular, untested: that the
console UART is clocked as assumed (the read-back check is there for
that); that the bridge chip does 921600 and above; what a real reopen does
to the device; the ROM loader's speed detection; uploads to the device at
the higher speed (the UART is polled and has a 128-byte receive FIFO,
which fills in 1.4 ms at 921600; the upload code reads it in a tight loop
at raised priority, which should do, but has not been tried).

**Limits.** The console UART has no driver: the log writer estimates when
the transmit FIFO has room and sleeps in whole ticks. At 921600, where
the FIFO empties in 1.4 ms, that is 450 to 500 lines a second; the wire
itself would carry about 600.

**Wanted from a first run:** `python3 adsb-fastlog.py` for a few minutes
in traffic and the file it writes (the status block at its end says what
the device sent and dropped at 921600); a connect from the page with the
browser console's lines that begin `[scope] Serial`; a flash from the
page, with its log; and `python3 adsb-fastlog.py --probe 921600`.

### 9.9 The web app under load (coh5)

At 115200 the page never saw more than about 115 lines a second. With the
console at 921600 the device can send 450 to 500, and at 2 Mbaud more –
so what the page does per line now matters four times as much or more.

**How it was looked at.** Two instruments, both in `harness/page/`, both
fed the same made-up device log (`fake_log.js`: N aircraft, R lines a
second, 45 % of them with a position) through the page's `handleLine()`
in 10 ms portions, as a serial port delivers it:

- `profile_page.js` runs the page's whole script in Node with the
  browser's objects stubbed, times every top-level function and counts,
  by name, everything the page asks of the DOM and of Leaflet.
- `browser_profile.js` loads the page in a real Chrome (headless), its
  libraries from local copies, and reads Chrome's own counters: script,
  style and layout time, number of layouts, all main-thread work; frames
  and long tasks from the page side. It is also the only place where a
  real browser loads the page: a script error on load fails the run.

`make page-profile` runs both (`make page-deps` once: npm).
Results: `harness/results/page.txt`.

**What they showed.** The page's own JavaScript is not the problem: 34 to
74 µs a line, 3 % of a core at 600 lines a second (Node). What it asked
of the browser per line was: three `getElementById`, two marker moves, a
log line (one node moved, four properties set) – and
`el.scrollTop = el.scrollHeight`. Reading `scrollHeight` straight after
changing the page makes the browser lay the page out there and then. One
layout per line:

| in Chrome, ms of main-thread work per second | v1.0.9 (coh4) | v1.0.10 |
| --- | --- | --- |
| 60 aircraft, 300 lines/s, log closed | 167 (134 layouts/s) | 162 (38) |
| 60 aircraft, 300 lines/s, log open | 874 (300 layouts/s, 566 ms of layout, 15 long tasks in 10 s) | 281 (20, 51 ms, none) |
| 150 aircraft, 1000 lines/s, log closed | 364 (453 layouts/s) | 266 (22) |
| 150 aircraft, 1000 lines/s, log open | **1881** (1000 layouts/s; 1 frame a second, worst frame 3.3 s) | 456 (19; 20 frames a second) |

With the log panel open, v1.0.9 used 87 % of the main thread at 300 lines
a second and could not keep up at 1000: the page stood still. (Headless
Chrome renders in software here, so frames per second are lower than on
a machine with a GPU – 58 with the page idle, about 20 under load in
either version; the columns to compare are the work.)

**What changed in v1.0.10.**

1. The log follows its end once per frame (`logFollow()`), not once per
   line. That is the whole difference in the table's "log open" rows.
2. A marker and its label are moved only when the aircraft's position
   changed (more than half of the messages carry none).
3. A meter in the page: `?perf=1` on the address, or `scopePerf(true)`
   in the console, writes a line to the log every five seconds –
   `[scope] perf: 300 lines/s, 20 ms/s in handleLine (68 us a line), 21
   frames/s, worst frame 100 ms, long tasks 1 (54 ms)`. Lines and time
   in `handleLine()` are the page's work per line; frames and long tasks
   (the browser's count of stretches over 50 ms) include style, layout
   and paint. It agrees with Chrome's counters in the test.

**Not done, in the order I would do them** – none is urgent after 1:

- *The parser's order.* `parse()` runs some forty regular expressions for
  boot, SD, settings and gain lines before it tries the one for an
  aircraft line, which is what 95 % of the lines are: a third of the
  page's own time per line (11 of 34 µs). Trying the aircraft line first
  needs the forty read through for what they must still see.
- *The log while its panel is closed.* Every line still moves a DOM node
  and sets four properties that nobody sees. Keeping the text only, and
  building the nodes when the panel opens, removes that.
- *The table.* Every message marks it dirty, so once a second all rows
  are sorted, rewritten and appended again, changed or not. Rewriting
  the rows that changed and re-appending only when the order did is a
  layout a second less; it matters with a few hundred aircraft.
- *Painting.* What is left of the main thread's work under load (some
  100 ms a second at 300 lines) is mostly paint: markers and SVG trails.
  Leaflet's canvas renderer for the trails is the thing to try, and
  `browser_profile.js` can say whether it helps – on a machine with a
  GPU, since software rendering here exaggerates it.

**Not measured:** a real serial port, a GPU, a phone. The meter is there
for that.
