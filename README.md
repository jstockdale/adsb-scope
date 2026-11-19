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

## Web app

Open `main/examples/adsb_receiver/adsb_scope.htm` in Chrome/Edge,
connect via Web Serial.  The serial log line format is the wire API
between firmware and webapp – treat log strings as a compatibility
surface.  `update-version.sh --deploy` publishes `latest.version.json`;
the webapp fetches `latest.bin` / `latest.version.json` relative to its
`FW_BASE` for browser flashing (esptool-js).

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
docs/UPLINK.md                 MQTT feeder under heavy traffic: findings, r7 changes, test plan
docs/ROADMAP-AVIATION.md       audit and phased plan toward an aviation instrument cluster
harness/                       host-side decoder test + measurement harness
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
