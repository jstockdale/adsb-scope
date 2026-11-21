# adsb-scope

Standalone ADS-B receiver firmware for ESP32-P4 boards with an RTL-SDR
dongle on the native USB 2.0 HS host port.  1090 MHz Mode S/ADS-B decode
on-device, live scope + aircraft table UI (LVGL), GPS/NTP-disciplined
time, SD logging with WiFi-coexistence hardening, Meshtastic-compatible
LoRa messaging (board-dependent), MQTT feeder, and a serial-log-driven
web app (`adsb_scope.htm`) with live map, replay, and browser flashing.

This repository is the standalone cut (2026-07-29) of what previously
lived as a branch on a LilyGo T-Display-P4 fork.  All third-party code
is vendored at recorded pins – see **VENDOR.md**.  Local patches and the
SD/WiFi coexistence fence are documented in **PATCHES.md**.  The full
architecture reference (with a dated correction block) is
**docs/ARCHITECTURE.md**.  Multi-board plans live under **boards/**.

**History.**  `main` starts with the history of that fork's `adsb` branch
(up to April 2026), continues with one commit that holds the standalone
tree as of decoder-m1090 **r10** – the commits between the cut and r10
are not in this repository – and then the **coh** series commit by
commit.  Branches `r10`, `coh3`, `coh4` and `coh5` stay at those
releases; `main` is the latest.  Commits from r10 on carry an
`Original-commit:` line: the hash they had before this history was put
together, which is what firmware built before then reports as its
version (`git log --grep 7629907` finds coh4, for one).

## Status

| Release | What it is | Run on a device? |
| --- | --- | --- |
| r10 | decoder-m1090: 2.4 Msps demodulator, CRC repair, capture and replay | yes |
| coh1 – coh3 | soft rescue and coherent second look (on by default since coh2), decoder at half the cost, gain loop on levels and yield | coh3: yes – 47 messages a second with 8 aircraft, about 300 a second in heavy traffic |
| coh4 | per-message cost around the decoder cut to an eighth, aircraft lock held for a copy only, console speed negotiated up from 115200, `tools/` | **no** – built, tested on a PC and against a simulated device |
| coh5 | web app 1.0.10: flashing keeps the settings, flashing fixes found against the real esptool-js, the log's forced layout per line removed, a meter | **no** – as coh4; the page's flashing is tested with the real esptool-js against an emulated chip, the page itself in a headless Chrome |

`docs/DECODER.md` and `docs/UPLINK.md` say for each change what was
measured, how, and what was not.

## Hardware

| Board | Status |
| --- | --- |
| LilyGo T-Display-P4 **v1.0** (HI8561 LCD or RM69A10 AMOLED, runtime-detected) | supported – the shipping target |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-7B | planned (boards/waveshare_p4_7b) |
| LilyGo T-Display-P4 v1.1 / v2.0 | future sibling BSP (v2.0 brings GPS PPS → stratum-0 time) |

Plus: RTL-SDR dongle (Blog V4 / V4 Lite / generic R820T/R828D handled),
1090 MHz antenna, FAT32 SD card.

## Build (T-Display-P4 v1.0)

1. **ESP-IDF v5.4.1 exactly** (hard rule for every board – do not
   assume newer-IDF APIs anywhere in this tree).
2. Apply the coexistence patch to your IDF checkout (once):
   ```
   cd $IDF_PATH
   git apply /path/to/adsb-scope/patches/esp-idf-v5.4.1-sd-coexistence.patch
   ```
3. Build and flash:
   ```
   idf.py set-target esp32p4
   idf.py build flash monitor
   ```
   `sdkconfig.defaults` selects the app and the known-good
   configuration; corrections vs. the historical defaults are listed in
   VENDOR.md. `dependencies.lock` pins the registry components to the
   versions of the first verified build. If `idf.py` reports that cmake
   or ninja is missing, run
   `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja` and
   re-source `export.sh`. A clean build is about 2200 compile steps.
   Source files are picked up from their folders at configure time: after
   pulling a commit that adds one, run `idf.py reconfigure` once.

   The decoder's repair stages – soft rescue and coherent second look,
   on by default – are described in `docs/DECODER.md` section 4 and can
   be switched on the console (`adsb dec`, `adsb coh`). For an image
   that boots without them, as r10 did:
   ```
   idf.py -B build-plain -D SDKCONFIG=build-plain/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.plain" build
   ```
4. **C6 co-processor** must run the ESP-Hosted `network_adapter` SDIO
   slave, version **2.12.3** – the exact LilyGo build is committed at
   `firmware/c6/` (sha256 in VENDOR.md).  Flash it per LilyGo's C6
   flashing procedure.  The host component is pinned to 2.12.3 in
   `main/idf_component.yml`; on first build, confirm the host↔slave
   version handshake in the boot log.
5. SD card: copy `sdcard/data/aircraft.db` to `/data/` on the card for
   registration/type lookups (regenerable via
   `main/examples/adsb_receiver/build_aircraft_db.py`).

## Installing a build without building it

`tools/flash-adsb-scope.sh` installs a firmware kit (boot loader,
partition table, application, merged image) with esptool.  It reads the
device's partition table first, keeps a copy of the first 64 kB of
flash, and keeps the NVS – settings, WiFi, the Meshtastic identity –
whenever the layout allows; `--dry-run` shows what it would do.  See
`tools/README.md`.

## Console

USB-C "P4.U" is the console, 115200 8N1; on this board opening the port
resets the processor.  A host can ask for more speed for as long as it
stays connected (`baud 921600`, confirmed with `baud ok`, kept alive; the
device is back at 115200 ten seconds after the host goes) –
`docs/UPLINK.md` 9.8.  `tools/adsb-fastlog.py` does that and logs every
line to a file.  Commands worth knowing: `adsb status`, `diag`, `top`,
`gain` / `gain sweep`, `adsb dec` / `adsb coh` (decoder stages),
`adsb replay <file>`, `baud`.

## Web app

Open `main/examples/adsb_receiver/adsb_scope.htm` in Chrome/Edge,
connect via Web Serial.  The serial log line format is the wire API
between firmware and webapp – treat log strings as a compatibility
surface.  `update-version.sh --deploy` publishes `latest.version.json`;
the webapp fetches `latest.bin` / `latest.version.json` relative to its
`FW_BASE` for browser flashing (esptool-js 0.5.4, loaded from unpkg).

- **Serial speed** (firmware panel): the page connects at 115200 and
  negotiates up, 921600 by default.  Web Serial can only change speed by
  reopening the port, which resets this board, so the page asks the
  device to start its next boot at the new speed: a connect takes one
  boot longer.  115200 gives the old behaviour.
- **Flashing** takes the merged image.  "Settings: keep when flashing"
  (on by default) writes it around the NVS partition after reading the
  device's partition table, and asks before it resets settings it cannot
  keep.  "Flash speed" tries 460800 and falls back to 115200.
- **Meter**: `?perf=1` on the address, or `scopePerf(true)` in the
  browser console, logs lines per second, the page's time per line,
  frames and long tasks every five seconds (`docs/UPLINK.md` 9.9).

## Repository map

```
main/examples/adsb_receiver/   the application (firmware + webapp + tools)
components/                    vendored: cpp_bus_driver (patched),
                               ICM20948_WE, kode_bq25896, private_library,
                               ethernet_init, esp_video
boards/                        multi-board scaffolding + board_api.h draft
patches/                       ESP-IDF fence patch, cpp_bus_driver patch
firmware/                      released P4 bins + the C6 slave bin
sdcard/                        aircraft.db + card layout
docs/ARCHITECTURE.md           deep reference (read the correction block)
docs/DECODER.md                decoder workstream: changes, measurements, caveats
docs/UPLINK.md                 the device under heavy traffic: MQTT feeder, capture and replay,
                               console instruments, what a message costs (9.7), the console's
                               speed and flashing from the page (9.8), the web app under load (9.9)
docs/ROADMAP-AVIATION.md       audit and phased plan toward an aviation instrument cluster
tools/                         for the PC the device is plugged into: install script, fast logger
harness/                       host-side tests and measurements: the decoder against recordings
                               and synthetic signals, the gain loop, number formatting, the
                               console-speed protocol against a simulated device, the web app's
                               serial code and flashing (real esptool-js, emulated chip), the web
                               app under load in Node and in a headless Chrome.  `make check`
VENDOR.md / PATCHES.md         provenance and patch mechanism
```

## Roadmap (agreed plan)

1. BSP extraction (`boards/t_display_p4_v1_0`) with hardware parity.
2. Capability gating (Meshy/GPS/haptics/gauge/RTC compile-out).
3. Waveshare 7B bring-up.
4. 1024×600 wide layout profile.
5. Per-board sdkconfig/partitions (real OTA slots), per-board artifacts.

License: see VENDOR.md ("Top-level license") – decision pending;
vendored components keep their own licenses.
