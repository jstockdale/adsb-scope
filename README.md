<!--
 * @Description: ADS-B Receiver on LILYGO T-Display-P4
 * @Author: John Stockdale / Off by One (fork of LILYGO_L original)
 * @Date: 2025-06-13 15:12:02
 * @LastEditTime: 2026-11-21
 * @License: GPL 3.0
-->
<h1 align="center">ADS-B Scope – T-Display-P4</h1>

<p align="center">
  A portable 1090 MHz ADS-B receiver and Meshtastic-compatible mesh radio built on the LILYGO T-Display-P4, using an RTL-SDR USB dongle for RF reception, an SX1262 LoRa radio for mesh networking, and the ESP32-P4's dual RISC-V cores for real-time signal processing.
</p>

## Overview

This project turns a LILYGO T-Display-P4 development board into a portable ADS-B receiver, Meshtastic-compatible mesh radio, and (yes, really) an mp3 player 🎵. An RTL-SDR dongle connected via USB Host receives 1090 MHz transponder signals, which are decoded in real-time on the ESP32-P4. The onboard SX1262 LoRa radio runs a Meshtastic-compatible mesh network for off-grid messaging. Decoded aircraft and mesh messages are displayed on the built-in touchscreen, logged to SD card, and can be viewed live via the companion web app, ADS-B Scope.

![Screenshots of on device interface](adsb_screenshots.png)

### What works today

- **On-device ADS-B display** – touchscreen UI with aircraft table (ICAO, callsign, altitude, speed, heading, distance, message count), stats panel (message rate, GPS, nearest aircraft, UTC time), draggable divider between panels, 4-way screen rotation toggle (persists across app visits)
- **On-device radar scope** – canvas-drawn aircraft positions with range rings, historical trails with per-segment altitude/speed coloring, pan/zoom, aircraft selection, and helicopter silhouette icons for rotorcraft
- **Aircraft database** – 587K-record database on SD card (`aircraft.db`, built from OpenSky data) with PSRAM-cached lookups. Enriches aircraft with registration, type, manufacturer, model, owner, operator, and aircraft class (landplane/helicopter/amphibian/gyrocopter/tilt-rotor). Downloadable via CDN from the webapp with CRC32-verified serial transfer.
- **Helicopter icons** – aircraft class from the database drives distinct map icons: top-down rotorcraft silhouette (body + tail boom + rotor blades) on both the webapp and LVGL scope, rotated to heading
- **Unified firmware** – single binary supports both HI8561 LCD (540×1168) and RM69A10 AMOLED (568×1232) display variants via runtime I2C probe detection at boot
- **Real-time ADS-B reception** – 2.4 Msps demodulator (`mode_s_2400.c`) with CRC repair, soft rescue and a coherent second look at frames that fail their CRC (`docs/DECODER.md`). On a device (release coh3): 47 messages/second with 8 aircraft in view, about 300 messages/second in heavy traffic; up to 256 aircraft tracked. (April's v1.0.8, at 2.0 Msps: 15–30 messages/second, 12–30+ simultaneous aircraft.)
- **Adaptive gain** – automatic RTL-SDR tuner gain control with two-phase algorithm: fast convergence (P1) and steady-state tracking (P2). Since coh3 the loop lives in `gain_ctl.c` and steers on signal levels and decode yield instead of the error rate (why: `docs/DECODER.md` 5.8); its thresholds come from one recording and want tuning on a device – `gain sweep` measures what each gain step yields. Signal quality gating prevents nighttime death spirals. Manual override via webapp gain popover or serial command. Gain state (steady/converging/manual) displayed in webapp.
- **~30 nm range** from Oakland, CA with a 7" telescopic antenna on RTL-SDR
- **USB hot-plug** – RTL-SDR can be connected/disconnected at any time; firmware detects disconnect (10 consecutive read errors), frees stuck USB transfer, and re-initializes cleanly on reconnect. EP0 STALL recovery handles flaky USB connections.
- **Three-state RTL-SDR status** – status bar and CIT page distinguish between disconnected (grey), connected (green), and error (red) states
- **SD card CSV logging** – UTC timestamps, raw Mode-S hex, decoded fields (ICAO, callsign, altitude, speed, heading, vertical rate, position, squawk), receiver GPS metadata (sats, HDOP). PSRAM-buffered writes with periodic flush for clean filesystem state.
- **AXI DMA fence** – SDHOST IDMAC (ESP-Hosted WiFi SDIO) and GDMA-AXI (GP-SPI SD card) share the AXI interconnect; a silicon bus arbitration bug can corrupt FAT structures. All SD file I/O is serialized through `sd_io_take()/sd_io_give()` which acquires the ESP-Hosted SDIO DMA lock, preventing concurrent DMA on the shared bus.
- **SD card hot-remove recovery** – consecutive flush failure counter detects card removal, unmounts dead filesystem, and retries remount every 30 seconds. Card reinsertion resumes logging automatically with fresh log files.
- **USB Mass Storage mode** – on-device UI and serial command (`msc`) to expose the SD card to a connected computer as a USB drive. Pauses ADS-B and Meshy logging, shows transfer stats, and resumes normal operation on disconnect.
- **Serial file transfer** – `recv` command implements CRC32-verified chunked file transfer over the serial console. Base64-encoded, 32KB chunks with resume support. CHUNK v1.1 header enables instant truncation detection. `verify_crc32` command provides incremental file verification with progress reporting.
- **OTA firmware update** – `ota_updater.c` downloads firmware from HTTP, streams to SD, verifies SHA256, flashes to OTA partition. Rollback detection with failure marker on SD. Can be triggered via MQTT. **Not usable at present:** since the standalone cut the partition table has a single `factory` application partition (`partitions.csv`), so there is no OTA slot to flash into; the code is in the tree for when the per-board partition tables with real OTA slots arrive (Roadmap, item 5).
- **MQTT cloud telemetry** – `mqtt_feeder.c` publishes aircraft positions, receiver stats, and gain state over MQTT for remote monitoring. Supports OTA update commands. Under heavy traffic the feeder batches (`mqtt_batch.c`; findings and test plan in `docs/UPLINK.md`).
- **Meshy mesh networking** – compatible TX/RX on the SX1262 LoRa radio with channel encryption, PKI direct message decryption, node discovery, position/telemetry display, MQTT gateway forwarding, TX echo in message history, packet ID dedup, and configurable hop limit (1–7, default 5).
- **Music player** – SD card MP3 playback via the ES8311 DAC with ID3 tag parsing, cover art display, 512 KB PSRAM readahead buffer, stereo-to-mono downmix, and gapless track scanning. Because every SIGINT platform needs a jukebox 🎶🐱
- **WiFi via ESP-Hosted** – ESP32-C6 coprocessor provides WiFi 6 connectivity over SDMMC Slot 1. SD card runs on SPI3 to avoid the SDMMC DMA conflict (Espressif Issue #17889). NTP time sync when connected. Graceful fallback with goto-based error unwinding if C6 is unresponsive – system continues without WiFi.
- **Time manager** – coordinates GPS, NTP, and RTC time sources with priority and accuracy tracking. GPS is primary (650ms serial delay compensation), NTP provides initial time before GPS fix, RTC seeds POSIX clock at boot for TLS cert validation. System clock stays in `[boot+elapsed]` format until a verified source provides time.
- **GPS timezone with DST** – L76K GNSS (5 Hz) sets the system clock; timezone determined from GPS coordinates using a compiled 0.1° grid covering 61 regions with 12 DST rules; syncs the PCF8563 RTC with local time
- **GPS position validation** – direction string check rejects corrupt NMEA data; jump rejection (>2°) with 3-fix forgiveness catches sign flips while allowing real movement during GPS loss
- **Screenshots** – capture device screen as PNG to SD card via serial command or webapp button. Atomic framebuffer snapshot under LVGL lock (~1ms copy, ~1.3s encode). Base64 transfer over serial for download via webapp.
- **Persistent settings** – all device and radio settings stored in NVS with version-migrated blob, validated on load, deferred writes from LVGL via internal-RAM helper task (PSRAM stack safe)
- **Serial console** – interactive command mode (Ctrl+C) with file management, WiFi/NTP config, USB MSC control, screenshot capture, file transfer (`recv`, `verify_crc32`), gain control, MQTT config, status, version query; decoder stage switches (`adsb dec`, `adsb coh`), on-device replay of recorded samples (`adsb replay`), field-report block (`diag`), per-task CPU use (`top`), and a console speed that a host can raise for its session (`baud`); ADS-B output buffered and replayed on return to log mode. Console task pinned to core 1 with priority boost during file transfers.
- **ADS-B Scope** – WebSerial-based live map viewer (v1.0.10) with firmware flashing that can keep the device's settings, negotiated serial speed, CSV replay, file browser, Meshy message panel, aircraft database detail cards, helicopter icons, 3D aircraft view, gain control popover, MQTT integration, and a performance meter ([adsb-scope.offx1.com](https://adsb-scope.offx1.com))
- **SD card reliability** – power-cycled on boot via XL9535 GPIO to reset card state machine after hard resets; AXI DMA fence prevents WiFi SDIO bus contention; boot counter and device registry in `/sdcard/adsb_scope.cfg`
- **Task priority architecture** – FreeRTOS priorities by what must never wait: USB host library (12), sample reader (11), audio DMA (6), serial transfer boost (5), class driver (4), decoder / GPS / console / Meshy RX (3), periodic tasks and the console log writer (2), LVGL rendering (1). The decoder and the console are pinned to core 1. (April's v1.0.8 had six tiers with the USB data path at 4 and the decoder on core 0; the reader/decoder split came with decoder-m1090.)
- **Status bar** – dynamic icons for GPS (sat count + fix status), ADS-B (aircraft count + connection state), SD card (mounted/logging/USB), and battery level with automatic spacing based on rendered text width Since coh6 also a WiFi glyph (grey: looking for a network, white: connected) with an MQTT arrow next to it (amber: broker not reached, green: connected) – not yet seen on a device.
- **Sample path with a ring and on-device replay** – a reader task at priority 11 only moves USB transfers into a 192-slot sample ring in PSRAM; the decoder task works from the ring. `adsb replay <file>` runs a recorded raw 8-bit I/Q file from the SD card through the same decoder on the device.
- **Field instruments** – `diag` prints everything a field report needs in one block, `top` each task's CPU use over a second, `adsb status` the receiver's counters, `gain sweep` what each gain step yields (`docs/UPLINK.md` section 9).
- **WiFi that comes back, and up to five saved networks** – since coh6 a supervisor (`wifi_sup.c`) reconnects for as long as it takes (rounds of scan + connect, waits of 1 to 60 s) instead of giving up after 55 s, and picks among saved networks by priority, then signal; `wifi list / add / remove / prio / use / scan` on the console (`docs/UPLINK.md` 9.10). Not yet run on a device.
- **Negotiated console speed** – the console always starts at 115200; a host can raise it for as long as it stays connected (`baud 921600`, up to 3 Mbaud) and the device returns to 115200 by itself when the host goes away (`docs/UPLINK.md` 9.8). New in coh4 – not yet run on a device.
- **Host tools** – `tools/flash-adsb-scope.sh` installs a firmware kit with esptool and keeps the settings whenever the flash layout allows; `tools/adsb-fastlog.py` logs the console to a file at a negotiated speed (`tools/README.md`).
- **Test and measurement harness** – `harness/`: the decoder against recordings and synthetic signals, the gain loop, the console-speed protocol against a simulated device, the web app's serial code and its flashing (real esptool-js against an emulated chip), and the web app under load in Node and in a headless Chrome (`harness/README.md`).

### Known limitations

- **WebSerial triggers device reboot** – USB-JTAG auto-reset circuit fires on DTR toggle during port open; handled gracefully (scope reconnects, boot log is parsed). (Since coh4, a closer look: by the build's configuration the console is UART0 behind the board's USB bridge, with USB-Serial-JTAG as output-only secondary, and LilyGO's wiki says the bridge's RTS line can reset the P4 – `docs/UPLINK.md` 9.8. Either way, opening the port resets the device; the web app's speed negotiation and flashing are built around that.)
- **WiFi requires C6 flash** – ESP-Hosted slave firmware must be flashed to the ESP32-C6 coprocessor manually. WiFi works when configured but is not yet tested end-to-end in the field.
- **PKI direct messages cannot be decrypted** – the code uses a different cipher than Meshtastic does (Pending Work, item 1); the "PKI DM decryption" bullet above describes code that was never verified against a real device.
- **Meshy still under heavy development** – PKI DM sending not yet implemented (receive works). Private channel support implemented but still testing. Device does not yet rebroadcast traffic. Operates similar to CLIENT_MUTE role.
- **coh4 and coh5 have not run on a device** – what they change (per-message cost around the decoder, the aircraft lock, console speed, the web app at v1.0.10) is built, and tested on a PC and against simulated devices only. See **Status**.
- **Heavy traffic** – in the first field run of coh3 the on-device aircraft list stopped updating at about 300 messages/second while the Scope app carried on. coh4 removes the two likeliest causes; why it happened is not established (`docs/UPLINK.md` 9.7).
- **No OTA at present** – single `factory` partition; see the OTA bullet above.

## Status

| Release | What it is | Run on a device? |
|---|---|---|
| v1.0.8 (April 2026) | the last state of the `adsb` branch on the T-Display-P4 fork; the screenshots and most of this README date from it | yes |
| r10 | decoder-m1090 r10: the standalone tree as this repository first has it – 2.4 Msps demodulator and CRC repair (`docs/DECODER.md`); MQTT feeder rework, aircraft limits, continuous capture, replay and field instruments (r7 to r10, `docs/UPLINK.md`) | not recorded here – the first device run the docs describe is coh2's (2026-11-18, `docs/UPLINK.md` 9.6) |
| coh1 – coh3 | soft rescue and coherent second look (on by default since coh2), decoder at half the cost, gain loop on levels and yield | coh2 and coh3: yes – coh3 gave 47 messages/second with 8 aircraft, about 300/second in heavy traffic |
| coh4 | per-message cost around the decoder cut to an eighth, aircraft lock held for a copy only, console speed negotiated up from 115200, `tools/` | **no** – built, tested on a PC and against a simulated device |
| coh5 | web app v1.0.10: flashing keeps the settings and works with esptool-js as it is, the log's layout-per-line removed, a meter | **no** – as coh4; the page's flashing is tested with the real esptool-js against an emulated chip, the page itself in a headless Chrome |
| coh6 | WiFi: reconnects for good, up to five saved networks chosen by priority then signal, WiFi + MQTT indicator in the status bar | **no** – the decisions are tested on a PC with scripted events; nothing that touches the radio has run |
| coh7 | Meshtastic relay (off by default) and raw log of undecoded packets; home-screen clock no longer overflows with seconds or long month names | **no** – builds; nothing in it has run |

`docs/DECODER.md` and `docs/UPLINK.md` say for each change what was measured, how, and what was not. Branches `r10` and `coh3` to `coh6` stay at those releases; `main` is the latest.

## ADS-B Scope

A self-contained HTML file that connects to the receiver over WebSerial and plots aircraft on a live map.

![Screenshot of ADS-B Scope](adsb_scope.png)

**Live version:** [adsb-scope.offx1.com](https://adsb-scope.offx1.com)

**Current version:** v1.0.10 – `main/examples/adsb_receiver/adsb_scope.htm`. The firmware carries a copy (`webapp` on the console dumps it). The screenshot is from v1.0.8.

Features:
- Dark radar-scope aesthetic with Leaflet/CartoDB dark tiles
- Aircraft icons with correct heading rotation and position trails (up to 3000 points per aircraft; older points are thinned out, the last five minutes kept at full resolution)
- Three.js 3D aircraft view with labeled markers, altitude-scaled positions, and smooth camera controls
- Helicopter silhouette icons for rotorcraft (from aircraft database class field)
- Aircraft detail cards with registration, type, manufacturer, model, owner, operator from aircraft database
- Aircraft database CDN download with CRC32-verified serial upload and on-device verification
- Range rings at 10, 25, and 50 nm
- Receiver position trail (dashed green polyline showing movement history)
- GNSS receiver status, device UTC clock (live/stale indicator), and SD card logging status (separate ADS-B and Meshy indicators)
- Adaptive gain control popover: auto/manual mode toggle, real-time gain slider, dual-range error band editor with presets, gain state display (steady/converging/manual) with color-coded LED
- MQTT integration for remote data streaming (aircraft positions, stats, gain state)
- Aircraft detail panel on selection (click icon or label, multi-select with Cmd/Ctrl+click)
- Sortable aircraft table with drag-resizable panel, message count column, and keyboard navigation (arrow keys, shift-select, escape)
- Detail card bottom position tracks panel height dynamically
- Color modes: MONO (terminal green), RAINBOW (by ICAO hash), ALT (altitude heatmap), SPD (speed heatmap)
- Label modes: altitude or distance from receiver
- Serial log panel with ADS-B/GNSS/Meshy/Other filters and pause-to-inspect
- Meshy panel with live message feed, timestamps, node map overlay, position tracking, text/join/position/telemetry filters, PKI DM detection with "DM" badge, packet-ID-based dedup with multi-reception expansion, TX echo display, and shift+click range selection for node analysis
- Screenshot button – captures device screen and downloads as PNG via base64 serial transfer
- SD card file browser over serial (list, download, replay, delete)
- CSV replay with timeline scrubber and variable speed (0.25×–32×), supporting both ADS-B and Meshy CSV files simultaneously
- Firmware flashing via esptool-js 0.5.4 (WebSerial). Takes the merged image. "Settings: keep when flashing" (on by default) reads the device's partition table and writes the image around the settings partition, so settings, WiFi and the Meshtastic identity survive; it asks before it resets settings it cannot keep. "Flash speed" tries 460800 and falls back to 115200 (`docs/UPLINK.md` 9.8)
- Negotiated serial speed ("Serial speed" in the firmware panel): connects at 115200 and asks the device for 921600 by default. Web Serial can only change speed by reopening the port, which resets this board, so the page has the device start its next boot at the new speed – a connect takes one boot longer. 115200 gives the old behaviour
- Performance meter: `?perf=1` on the address, or `scopePerf(true)` in the browser console, logs lines per second, the page's time per line, frames and long tasks every five seconds (`docs/UPLINK.md` 9.9)
- Firmware version checking against `latest.version.json` with update badge
- Google Analytics integration (G-T71ZHWG166) loaded at end of body for offline resilience
- Auto-reconnect after flash
- Responsive layout with touch-friendly targets for mobile use
- DTR/RTS deasserted on connect to minimize resets
- Title attributes on all buttons for accessibility

Requirements: Chrome or Edge (WebSerial API). Connect via USB, click "Connect Serial", select the ESP32-P4 port.

The serial log line format is the wire API between firmware and webapp – treat log strings as a compatibility surface. `update-version.sh --deploy` publishes `latest.version.json`; the webapp fetches `latest.bin` / `latest.version.json` relative to its `FW_BASE` for browser flashing.

## Serial Console

Press **Ctrl+C** over serial (115200 baud) to enter command mode. Type `log` to return to streaming mode. ADS-B and GNSS output is buffered in PSRAM during command mode (up to 500 lines) and replayed when returning to log mode.

The console always starts at 115200. A host that wants more asks for it and keeps it only while it stays connected: `baud 921600`, then `baud ok` at the new speed within five seconds, then a keepalive; ten seconds after the host falls silent the device is back at 115200. `tools/adsb-fastlog.py` does this and logs every line to a file; the web app does it through a reboot (see ADS-B Scope). Protocol and limits: `docs/UPLINK.md` 9.8.

```
help              show available commands
log               return to streaming log mode

── Filesystem ──
ls [path]         list directory (default: cwd)
cat <file>        print file contents
head <file> [n]   print first n lines (default 20)
tail <file> [n]   print last n lines (default 20)
rm <file>         delete a file
mv <src> <dst>    rename/move a file
cp <src> <dst>    copy a file
cd [path]         change directory (default: /sdcard)
pwd               print working directory
mkdir <path>      create directory
rmdir <path>      remove empty directory
df                show SD card free space
mount             mount SD card
unmount           safely unmount SD card

── Services ──
adsb [on|off|status]             ADS-B receiver control
adsb replay <file>|stop          decode a recorded sample file (raw 8-bit I/Q); no argument: progress
adsb dec [default|soft|fix2]     error correction: 1-bit only, + soft rescue, + soft rescue and 2-bit
adsb coh [on|off]                coherent second look at frames that fail their CRC; no argument: show
diag [mark]                      everything a field report needs, in one block; mark: restart its checks
meshy [on|off|status]            Meshtastic radio control
meshy relay [on|off]             retransmit received packets by the rules of the role (default off)
meshy undecoded [on|off]         print packets that could not be decoded, header fields and all bytes
usb [status]                     USB host status
wifi [on|off|status|list|scan|cycle]      WiFi control and state
wifi add <name> [pw] | remove <n> | prio <n> <pos> | use <n>   saved networks, in priority order (up to 5)
wifi ssid <n> | pass <pw>                 network 1, as before
mqtt [on|off|status|server <h>|port <n>|user <u>|pass <p>|id <id>]   MQTT config

── Info ──
status            show receiver status
top               per-task CPU use over one second, largest first
version           show firmware version
time              show time source status (GPS, NTP, RTC)
gain [auto|<dB>|band [<low> <high>]|restart]   show/set RTL-SDR tuner gain
gain sweep [secs|stop]                         measure what each gain step yields

── Config ──
timezone [auto|UTC±N]            show or set timezone (tz is accepted too)
heartbeat [on|off|5-255]         show/set console heartbeat
baud [<speed> [stay|next]|ok|probe]   console speed for this session (always starts at 115200)
ntp [server] [poll_s]            show/set NTP server and poll interval
gps [basic|strict]               show/set GPS time integrity mode

── Meshtastic ──
meshy_tx <text>   send Meshy text message (broadcast)
channels [list|reset|add|remove]   manage Meshtastic channels
identity [<long> [<short>]]        show/set Meshy node identity

── Advanced ──
nvs [list|dump|clear] [device|meshy|pki|all]   NVS management
msc               enter USB Mass Storage mode (stops logging)
cdc               exit USB Mass Storage mode (resumes logging)
webapp            dump built-in adsb_scope.htm to serial
screenshot        save device screen as PNG to SD card
screenshot send   send last screenshot as base64 over serial
recv <path> <sz>  receive file over serial (base64, CRC32-verified, resume)
verify_crc32 <path> <hex>  verify file CRC32 on SD card
stat <path>       show file size (for webapp pre-check)
reboot            software reset
```

## Meshy – Meshtastic-compatible Mesh Radio

The firmware implements a Meshtastic-compatible LoRa mesh radio using the onboard SX1262 (HPD16A module via SPI). It can send and receive encrypted text messages, broadcast node identity, and interoperate with standard Meshtastic devices on the same channel.

### What it does

- **Channel-encrypted TX/RX** – text messages and NODEINFO broadcasts encrypted with the standard Meshtastic AES-CTR scheme, compatible with any Meshtastic device on the same channel + PSK
- **PKI direct message decryption** – receives X25519/XChaCha20-Poly1305 encrypted DMs addressed to this node, using a persistent keypair stored in NVS. Public key is broadcast in NODEINFO packets.
- **MQTT gateway forwarding** – `ok_to_mqtt` flag encoded in the Data protobuf bitfield (field 9, bit 0) so MQTT gateways forward our packets to the internet. Confirmed working with 34+ gateways across the Bay Area mesh.
- **Node discovery** – receives and stores NODEINFO, position, and telemetry from other nodes (512-node table in PSRAM). Nodes with public keys are tagged `[PKI]`.
- **Configurable settings** – region (US/EU/etc), preset (LongFast through ShortSlow), frequency slot, TX power (10–30 dBm), hop limit (1–7, default 5), role (Client/ClientMute/RouterLate), NODEINFO broadcast period, and MQTT forwarding toggle. All settings persist across reboots.
- **SD card logging** – all decoded messages logged to CSV with timestamps, RSSI, SNR, hop count, port type, decoded payload, and PKI flag. Log files use `mesh_` prefix.
- **TX echo** – sent messages appear in on-device message history and are logged to SD, matched by packet ID
- **Web app integration** – live Meshy panel in ADS-B Scope shows message feed with timestamps, node names, position pins on the map, telemetry readouts, PKI DM badges, packet-ID-based dedup, and radio config display (region, preset, role, TX power, hop count, frequency)

### On-device Meshy UI

- Draggable message panel with text input, floating preview bar, and on-screen keyboard with haptic feedback
- Message canvas with emoji-capable rendering, per-message coloring (TX=accent, RX text=white, system=dim green), and manual word-wrap (1800px scrollable)
- Settings panel: LoRa Radio enable/disable, SD Card Logging, Region, Preset, Freq Slot, OK to MQTT, Hop Limit, TX Power, Role, NODEINFO period
- Factory reset dialog with separate checkboxes for settings, Meshy channels, and PKI keypair

### Architecture notes

- CSMA/CA random backoff before TX (per Meshtastic spec)
- SPI mutex shared between RX polling (50ms) and TX
- RX/TX task stacks on internal RAM (SPI DMA requirement)
- Message history: 1000 slots in PSRAM (376 KB)
- Node table: 512 slots in PSRAM (69 KB)
- NVS writes dispatched to a short-lived internal-RAM task (SPI flash disables cache, making PSRAM inaccessible)

## Music Player 🎵

Because every open-source SIGINT platform deserves a soundtrack 🎧🛩️

The firmware includes an SD card MP3 player using the onboard ES8311 DAC and NS4150B amplifier. Drop `.mp3` files in `/sdcard/music/` and they show up in the on-device player. See `sdcard/music/README.md` for details on supported formats, album art, and the `prepare_music.py` preparation tool.

### Features

- MP3 decoding via minimp3 (header-only, no external dependencies)
- Stereo-to-mono downmix for the ES8311 mono codec (no content loss)
- ID3v2 tag parsing for track title, artist, and album
- Cover art extraction and display on the AMOLED/LCD screen (max 1024×1024, downscaled to 544×544)
- 512 KB PSRAM readahead buffer with 64 KB chunk fills for gapless playback
- Configurable sample rate via `set_clock_rate()` – 44.1 kHz default, auto-switches per track
- Track scanning at boot with up to 64 tracks
- Readahead fill task on PSRAM stack (4 KB)

## CSV Log Format

### ADS-B log

Logged to `/sdcard/logs/adsb_YYYY-MM-DDTHHMMSSZ.csv` (or `adsb_bootNNNNNNNN.csv` before GPS fix, where the number is a persistent boot counter from `/sdcard/adsb_scope.cfg`):

```
timestamp_utc,raw_msg,icao,callsign,altitude_ft,speed_kt,heading_deg,vrate_fpm,lat,lon,squawk,rx_lat,rx_lon,range_km,bearing_deg,rx_sats,rx_hdop
2026-03-16T04:13:35.123Z,8DA105E8582594BADAC53333EE95,A105E8,SKW5567,6425,285,109,-128,37.72680,-122.44730,0000,37.83328,-122.27379,11.0,233,5,2.7
```

Timestamps are ISO-8601 UTC with millisecond resolution. Raw Mode-S hex is the second column for easy replay. Every row includes receiver GPS sats/HDOP for data quality assessment.

### Meshy log

Logged to `/sdcard/logs/mesh_YYYY-MM-DDTHHMMSSZ.csv` with timestamps, RSSI, SNR, hop count, port type, decoded payload, position, telemetry, and PKI flag.

## SD Card Layout

```
/sdcard/
├── adsb_scope.cfg          # Boot counter + device registry (auto-managed)
├── data/
│   └── aircraft.db         # Aircraft database (ADSBDB02 format, ~27 MB)
├── logs/
│   ├── adsb_*.csv          # ADS-B flight logs
│   └── mesh_*.csv          # Meshtastic message logs
├── firmware/
│   ├── ota.bin             # OTA firmware update (downloaded, SHA256-verified)
│   └── ota.meta.json       # OTA metadata
├── screenshots/
│   └── screenshot_*.png    # Device screenshots
└── music/
    ├── README.md           # Music player documentation
    └── *.mp3               # MP3 files with ID3 tags and embedded cover art
```

The `aircraft.db` file is built from the OpenSky Network aircraft database using `build_aircraft_db.py`. Place it in `/sdcard/data/` – the firmware loads the 512 KB bucket header into PSRAM at boot and performs lookups on demand.

## Hardware

### Board: LILYGO T-Display-P4 V1.0

| Component | Detail |
|---|---|
| **SoC** | ESP32-P4, 360 MHz dual RISC-V, 32 MB PSRAM, 16 MB flash |
| **Coprocessor** | ESP32-C6-MINI-1U (WiFi 6 / BLE 5 via SDIO) |
| **Display** | HI8561 LCD 540×1168 or RM69A10 AMOLED 568×1232 (auto-detected at boot) |
| **GPS** | Quectel L76K (UART, 9600→115200 baud auto-detect) |
| **RTC** | PCF8563 (I²C, stores local time with GPS-derived timezone) |
| **LoRa** | SX1262 via SPI (HPD16A module) |
| **Audio** | ES8311 DAC + NS4150B amplifier + electret mic |
| **IMU** | ICM20948 9-axis (I²C) |
| **Battery** | BQ27220 gauge + LGS4056H charger |
| **Camera** | OV2710 MIPI-CSI |
| **IO Expander** | XL9535 (I²C) |
| **Storage** | SD card (SPI3, coexists with ESP-Hosted SDIO on Slot 1; SDMMC 4-bit for USB MSC mode) |

### External: RTL-SDR USB Dongle

Connected via the ESP32-P4's USB 2.0 Host port. The firmware implements a custom USB host driver that initializes the RTL2832U + R820T tuner, tunes to 1090 MHz at 2.4 MS/s (2 MS/s, as up to v1.0.8, can be selected in menuconfig: Example Configuration → ADS-B demodulator), and reads IQ samples via USB bulk transfers. RTL-SDR Blog V4 / V4 Lite and generic R820T/R828D dongles are handled.

### Board support

| Board | Status |
|---|---|
| LilyGo T-Display-P4 **v1.0** (HI8561 LCD or RM69A10 AMOLED, runtime-detected) | supported – the shipping target |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-7B | planned (`boards/waveshare_p4_7b`) |
| LilyGo T-Display-P4 v1.1 / v2.0 | future sibling BSP (v2.0 brings GPS PPS → stratum-0 time) |

Also needed: a 1090 MHz antenna and a FAT32 SD card.

## Source Files

All ADS-B firmware source is in `main/examples/adsb_receiver/` (`main/examples/lvgl_9_ui/` on the fork, up to v1.0.8):

| File | Description |
|---|---|
| `main.cpp` | Boot, peripheral init, GPS task (with position validation), LVGL UI, task orchestration (6-tier priority scheme), screen detection, USB pre-alloc, settings persistence, WiFi init, time manager, MSC/screenshot wrappers, memory checkpoints, AXI DMA fence (`sd_io_take/give`) |
| `class_driver.c / .h` | USB host, RTL-SDR sample reader task and sample ring, decoder task (pinned to core 1), console log writer task, on-device replay, aircraft table, SD logging with hot-remove recovery, aircraft DB ICAO log emission, stats with error/gain state |
| `mode-s.c / .h` | Mode S decoder (from dump1090/libmodes) with preamble/CRC/signal statistics for adaptive gain feedback; CRC repair from syndrome tables (`mode_s_syndromes.h`), soft rescue |
| `serial_console.c / .h` | Interactive console with command mode, PSRAM replay buffer, shared `log_format_timestamp`, WiFi/NTP/MSC/screenshot commands, `recv` file transfer (CHUNK v1.1 protocol), `verify_crc32` with progress, gain control, MQTT config, `adsb` / `diag` / `top`, and the `baud` command with the UART speed change. Task pinned to core 1 with priority boost during transfers. |
| `lvgl_ui.cpp / .h` | LVGL UI: ADS-B app, radar scope (with helicopter icons), Meshy app (with preview bar + haptic keyboard), music player, settings panels (WiFi, USB MSC), status bar, CIT tests, rotation persistence |
| `esp_libusb.c / .h` | USB-to-librtlsdr shim, idempotent init, early DMA pre-alloc, EP0 STALL recovery |
| `librtlsdr.c` | RTL-SDR driver (adapted from osmocom) with dongle identification, EEPROM read, bias tee support |
| `tuner_r82xx.c` | R820T/R828D tuner driver with Blog V4/V4L band switching and bias tee GPIO control |
| `device_settings.h` | Settings struct (1024-byte NVS blob), version-migrated load/save, validation, deferred NVS write via internal-RAM task. Includes gain, MQTT, and OTA settings. |
| `meshtastic_task.cpp / .h` | Meshy FreeRTOS tasks: SX1262 RX polling, TX queue with echo, CSMA/CA, node table, message history, SD CSV logging with hot-remove recovery |
| `meshtastic_radio.h` | `MeshSession` class: channel encrypt/decrypt, packet build, PKI decrypt, hop limit, ok_to_mqtt |
| `meshtastic_pb.h` | Meshtastic protobuf encoder/decoder: Data, User, Position, Telemetry, bitfield (ok_to_mqtt) |
| `meshtastic_crypto.h` | AES-CTR channel encryption, X25519 ECDH, XChaCha20-Poly1305 PKI, SHA-256 KDF (mbedtls) |
| `meshy_channels.h` | Channel store (NVS), PSK management, node identity, PKI keypair generation/persistence |
| `aircraft_db.h` | Aircraft database lookup module (header-only, PSRAM cache + SD bucket file, ADSBDB02 versioned format). All SD access wrapped in `sd_io_take/give`. |
| `music_player.cpp / .h` | MP3 player: minimp3 decode, ES8311 DAC output, stereo-to-mono downmix, ID3 parsing, cover art, 512 KB PSRAM readahead. All SD access wrapped in `sd_io_take/give`. |
| `mqtt_feeder.c / .h` | MQTT cloud telemetry: publishes aircraft positions, receiver stats, gain state. Supports OTA trigger commands. |
| `ota_updater.c / .h` | OTA firmware update: HTTP download to SD, SHA256 verification, dual OTA partition flash, rollback detection with failure marker. All SD access wrapped in `sd_io_take/give`. |
| `sd_logger.c / .h` | SD card logging module: flush coordination, dirty flag tracking, bus failure detection, hot-remove recovery. DMA fence handled automatically via `sd_io_take`. |
| `sd_http_server.c` | HTTP server for SD card file serving over WiFi. Directory listing and file download with `sd_io_take/give`. |
| `sd_forensics.py` | SD card forensics tool: FAT analysis, cluster chain validation, raw sector dump |
| `emoji_draw.h` | LVGL canvas text renderer with manual word-wrap and clip optimization |
| `screenshot.h` | Atomic framebuffer snapshot to PNG on SD card, base64 serial transfer |
| `sd_msc_mode.h` | USB Mass Storage mode: SPI→SDMMC mode switch, TinyUSB MSC, transfer stats, safe enter/exit sequencing |
| `sd_config.h` | SD card boot counter and device registry (`/sdcard/adsb_scope.cfg`) |
| `wifi_hosted.h` | WiFi via ESP-Hosted (C6 over SDMMC Slot 1), credential management, pause/resume, graceful fallback with goto-based error unwinding if C6 is unresponsive |
| `time_manager.h` | Time source coordinator: GPS primary, NTP secondary, RTC boot seed for TLS. Priority and accuracy tracking per source. |
| `tz_lookup.c / .h` | GPS coordinate → timezone offset with DST |
| `timezone_data.h` | Generated 0.1° grid (61 regions, 12 DST rules) |
| `generate_tz_data.py` | Timezone data generator script |
| `build_aircraft_db.py` | Converts OpenSky CSV → `aircraft.db` (ADSBDB02 format) |
| `prepare_music.py` | Prepares MP3 files: TLEN duration tag, Unicode normalization, album art resize |
| `screen_detect.h` | Runtime screen type detection (HI8561 vs RM69A10) – now in `components/private_library/` |
| `t_display_p4_driver.cpp / .h` | Runtime MIPI DSI panel creation for both display variants – now in `components/private_library/` |
| `adsb_scope.htm` | ADS-B Scope companion web app (single-file HTML, v1.0.10) |
| `mode_s_2400.c` | Mode S demodulator for magnitude samples at 2.4 Msps (the default since r10): timing quantised to a fifth of a sample, so no alignment is blind |
| `mode_s_coh.c` | Coherent second look: re-slices frames that failed their CRC with a phase estimate from the frame itself |
| `mode_s_syndromes.h` | Generated CRC syndrome tables for 1-bit and 2-bit repair (`harness/gen_syndrome_tables.py`) |
| `gain_ctl.c / .h` | Adaptive tuner gain from signal levels and yield (P1/P2), separate from the driver so that it runs in the harness too |
| `sample_ring.c / .h`, `usb_stream.h` | Ring of USB transfers between the reader task and the decoder; transfer and ring sizes |
| `adsb_fmt.c / .h` | Single-precision great-circle distance and bearing, and number text without `printf`'s `%f` – what a message costs around the decoder (`docs/UPLINK.md` 9.7) |
| `console_link.c / .h` | Console speed as a property of a session: the state machine behind `baud` (pure C, also built into the harness's simulated device) |
| `mqtt_batch.c / .h` | Batching of aircraft updates for the MQTT feeder |

Outside the application folder: `tools/` (install script, fast logger), `harness/` (host-side tests and measurements), `docs/` (ARCHITECTURE, DECODER, UPLINK, ROADMAP-AVIATION), `patches/`, `boards/`, `components/` (vendored – see `VENDOR.md`).

## Architecture

```
RTL-SDR (1090 MHz, 2.4 MS/s IQ; 2 MS/s selectable)
    │ USB bulk transfers (5.5 KB each, DMA buffers pre-allocated at boot)
    ▼
Sample reader (class_driver.c) [Priority 11]
    │ copies each transfer into the sample ring (192 slots, PSRAM) and nothing else
    ▼
Decoder (class_driver.c) [Priority 3, Core 1]
    │ magnitude → 2.4 Msps demodulator (mode_s_2400.c) → CRC, 1-bit repair,
    │ soft rescue (mode-s.c), coherent second look (mode_s_coh.c)
    │ Adaptive gain (gain_ctl.c): signal levels + yield → P1 (fast) / P2 (steady-state)
    ▼
Aircraft Table (256 slots, 60s expiry)
    │
    ├──→ Aircraft DB lookup (aircraft_db.h, 512KB PSRAM header + cache)
    │    └── ICAO: log line (reg, type, mfr, model, owner, operator, class)
    ├──→ On-device LVGL display (lvgl_ui.cpp, aircraft table + stats panel)
    ├──→ On-device radar scope (lvgl_ui.cpp, canvas with helicopter icons)
    ├──→ Serial output (console log writer task [Priority 2], paced to the console's speed; buffered during CMD mode)
    ├──→ SD card CSV log (PSRAM buffer, periodic flush, hot-remove recovery)
    └──→ MQTT telemetry (mqtt_feeder.c, aircraft positions + stats + gain)

AXI DMA Fence (sd_io_take / sd_io_give in main.cpp)
    │ Serializes ALL SD card file I/O with ESP-Hosted SDIO DMA lock
    │ Prevents silicon-level AXI bus arbitration bug (SDHOST IDMAC vs GDMA-AXI)
    └──→ Every SD caller (class_driver, meshtastic, music, HTTP, OTA, aircraft_db)

Aircraft Database (aircraft_db.h)
    │ /sdcard/data/aircraft.db (ADSBDB02, 65K buckets, 587K records)
    │ Background loader → 512KB PSRAM header → 512-entry cache
    │ CDN download via webapp → serial transfer (recv) → CRC32 verify
    └──→ Lookup by ICAO24 → reg, typecode, mfr, model, owner, op, class

SX1262 LoRa Radio (913.125 MHz MediumFast default)
    │ SPI with shared mutex, 50ms RX poll
    ▼
Meshtastic Protocol (meshtastic_radio.h)
    │ AES-CTR channel decrypt / XChaCha20 PKI decrypt
    ▼
Message Processing (meshtastic_task.cpp)
    │
    ├──→ Node table (512 slots, PSRAM) – name, position, telemetry, public keys
    ├──→ Message history (1000 slots, PSRAM) – dedup by packet ID, TX echo
    ├──→ On-device Meshy panel (lvgl_ui.cpp, scrollable message canvas)
    ├──→ Serial output (tagged MESHY: lines, parsed by webapp)
    └──→ SD card CSV log (mesh_YYYY-MM-DDTHHMMSSZ.csv)

ES8311 DAC + NS4150B Amplifier
    │ I2S, 44.1 kHz, 16-bit stereo (downmixed to mono)
    ▼
Music Player (music_player.cpp)
    │ minimp3 decode → 512KB PSRAM readahead → I2S write
    └──→ On-device player UI (track list, cover art, playback controls)

L76K GPS (UART, 5 Hz NMEA)
    │
    ├──→ Receiver position → range/bearing calculation
    │    └── Direction validation + jump rejection (>2°, 3-fix forgiveness)
    ├──→ Time manager (time_manager.h) – GPS primary source
    ├──→ Timezone lookup (tz_lookup.c, 0.1° grid with DST)
    └──→ PCF8563 RTC (local time, every 60s, write-only)

ESP32-C6 Coprocessor (WiFi 6 via ESP-Hosted, SDMMC Slot 1)
    │ Graceful fallback if C6 unresponsive – system continues without WiFi
    ├──→ NTP sync → Time manager (secondary source)
    └──→ MQTT → mqtt_feeder.c (cloud telemetry, OTA commands)

Time Manager (time_manager.h)
    │ Coordinates GPS, NTP, RTC – single settimeofday authority
    │ RTC seeds POSIX clock at boot for TLS cert validation
    └──→ System clock (stays in [boot+elapsed] until verified source)

OTA Updater (ota_updater.c)
    │ HTTP download → SD card → SHA256 verify → flash OTA partition
    └──→ Rollback detection with failure marker on SD

USB Mass Storage (sd_msc_mode.h)
    │ Stop RTL-SDR → close logs → unmount SPI → remount SDMMC → TinyUSB MSC
    └──→ On-device status screen (transfer stats, disconnect button)

SD Card Recovery (class_driver.c, meshtastic_task.cpp)
    │ 5 consecutive flush failures → unmount → retry remount every 30s
    └──→ Card reinsertion resumes logging with fresh files

Task Priority Architecture (dual-core)
    │ USB host library (12), sample reader (11) – the samples must never wait
    │ iis_tx (6) – audio DMA, usually suspended
    │ Core 1: decoder (3) and console (3, boosted to 5 during recv)
    │ Floating: class driver (4), GPS (3), speaker (3), Meshy RX (3),
    │           periodic tasks and console log writer (2), LVGL (1)
    └──→ (v1.0.8: six tiers, USB host + class_driver at 4 on core 0)

Runtime Screen Detection (screen_detect.h)
    │ I2C probe: GT9895 (0x5D) → AMOLED, else → LCD
    ▼
Screen_Init_Runtime (t_display_p4_driver.cpp)
    │ MIPI DSI panel creation with variant-specific timing
    ▼
LVGL Display (g_screen_width × g_screen_height) [Priority 1]

Serial Console (serial_console.c) [Priority 3, Core 1]
    │
    ├──→ LOG mode: streaming ADS-B/GNSS output
    ├──→ CMD mode: file management, WiFi/NTP, MSC, screenshot, gain, MQTT, adsb/diag/top
    ├──→ baud: session speed above 115200, back by itself (console_link.c)
    ├──→ recv: CRC32-verified file transfer (CHUNK v1.1, priority 5 during xfer)
    ├──→ verify_crc32: incremental file CRC32 with progress reporting
    └──→ Replay buffer: 500 lines in PSRAM, flushed on return to LOG
```

## Memory Budget

> Measured on v1.0.8 (April 2026) and **not re-measured since**. Known additions in PSRAM since then are listed under the second table; the internal-RAM figures may have moved.

| Stage | Internal RAM Free | Largest Block |
|---|---|---|
| Boot start | ~181 KB | ~102 KB |
| After screen detect | ~160 KB | ~82 KB |
| After Ethernet init | ~130 KB | ~69 KB |
| After USB host + DMA pre-alloc | ~120 KB | ~60 KB |
| After WiFi init | ~100 KB | ~45 KB |
| After LVGL init | ~90 KB | ~40 KB |
| After all tasks | ~70 KB | ~32 KB |
| Steady state (heartbeat) | ~74 KB | ~32 KB |
| PSRAM free (steady) | ~16 MB | – |

USB bulk transfer buffer (17 KB) is pre-allocated immediately after `usb_host_install()` while internal RAM is still contiguous. A 20 KB DMA reservation block is held from the very start of boot and released just before allocation to guarantee a contiguous hole even under fragmentation.

All application task stacks (speaker, readahead, ethernet, NFC, microphone, etc.) are allocated from PSRAM via `xTaskCreateWithCaps()`. Only `usb_host_lib_task` (8 KB) and the Meshy RX/TX tasks remain on internal RAM due to SPI DMA requirements. NVS writes are dispatched to a short-lived internal-RAM task because SPI flash operations disable the cache, making PSRAM inaccessible.

### PSRAM consumers

| What | Size |
|---|---|
| Aircraft DB header | 512 KB (65K buckets) |
| Aircraft DB cache | ~60 KB (512 slots) |
| Meshy message history | 376 KB (1000 slots) |
| Meshy node table | 69 KB (512 slots) |
| Scope trail slots | 901 KB (64 aircraft × 600 pts with alt/speed) |
| Music readahead buffer | 512 KB |
| Screenshot framebuffer copy | 1.4 MB (allocated during capture only) |
| LVGL draw buffer + art | ~2 MB |
| Speaker task stack | 32 KB |
| LVGL task stack | 64 KB |
| Other task stacks | ~40 KB |
| Sample ring (since r10) | ~1.1 MB (192 transfers × 5632 bytes) |
| Aircraft list row buffer (since coh4) | 256 rows, `EXT_RAM_BSS` |

## Quick Start – Pre-built Binary

If you just want to flash and go, a pre-built binary is available. No build environment needed.

Three ways. All of them need the **merged image** (boot loader + partition table + application in one file, named `jstockdale-adsb_receiver-t_display_p4-<date>[-<release>].bin`). Release kits – merged image, the three separate pieces, install script, notes – are handed out per release; the images under `firmware/jstockdale-adsb_receiver/` in this repository are the March 2026 ones.

### Option 1: Flash from browser (easiest)

1. Open [adsb-scope.offx1.com](https://adsb-scope.offx1.com) in Chrome or Edge
2. Click **Flash** → **Flash Latest** (whatever `latest.bin` the server has), or **Local .bin** and pick a merged image
   - "Settings: keep when flashing" (on by default) keeps settings, WiFi and the Meshtastic identity; untick it for a factory reset
3. Select the ESP32-P4 serial port when prompted
4. Wait for flash to complete (~60s), device auto-reboots (the ~60s is from v1.0.8; the page now tries 460800 first and falls back to 115200)

If the device doesn't enter bootloader mode automatically, hold **BOOT** + tap **RESET** before clicking Flash Latest.

### Option 2: Flash with esptool.py

- [esptool.py](https://github.com/espressif/esptool) (`pip install esptool`)
- USB cable to the T-Display-P4
- Hold **BOOT** + tap **RESET** to enter download mode

### Flash

```bash
esptool.py --chip esp32p4 --port /dev/tty.usbmodem* \
    write_flash 0x0 jstockdale-adsb_receiver-t_display_p4-260317.bin
```

On Windows, replace `/dev/tty.usbmodem*` with the appropriate COM port (e.g., `COM3`).

After flashing, press RESET. The device will boot, initialize all peripherals, and begin scanning 1090 MHz as soon as an RTL-SDR dongle is connected to the USB Host port. Connect to [adsb-scope.offx1.com](https://adsb-scope.offx1.com) via Chrome WebSerial to see aircraft on a live map.

Writing the merged image at `0x0` also overwrites the settings partition: settings, WiFi and the Meshtastic identity are reset. To keep them, use the install script.

### Option 3: Install script (keeps the settings)

```bash
bash tools/flash-adsb-scope.sh --dry-run     # shows what it found and what it would write; writes nothing
bash tools/flash-adsb-scope.sh               # --port /dev/ttyACM1, --build plain, --kit <folder>
```

Run it next to an unpacked release kit. It checks for esptool first, reads the device's partition table, saves a copy of the first 64 kB of flash, and then writes the application only (same layout), boot loader + table + application around the settings (older layout), or – only after a typed YES – the merged image. See `tools/README.md`.

### Hardware needed

- LILYGO T-Display-P4 V1.0 (either LCD or AMOLED variant)
- RTL-SDR USB dongle (RTL2832U + R820T/R820T2; RTL-SDR Blog V4 / V4 Lite with R828D also handled)
- Antenna – the included telescopic whip works, ~30 nm range from a window

## Building from Source

### Prerequisites

- **ESP-IDF v5.4.1 exactly** (hard rule for every board – do not assume newer-IDF APIs anywhere in this tree)
- Visual Studio Code + ESP-IDF extension (recommended)

### Apply the ESP-IDF coexistence patch

Once, to your IDF checkout – the SD/WiFi coexistence fence needs it (`PATCHES.md`):

```
cd $IDF_PATH
git apply /path/to/adsb-scope/patches/esp-idf-v5.4.1-sd-coexistence.patch
```

### Apply cpp_bus_driver patch

The LilyGo `cpp_bus_driver` component ships with two issues that affect long-running firmware on the T-Display-P4. A patch is included at `patches/cpp_bus_driver_adsb_scope.patch` (`components/cpp_bus_driver_adsb_scope.patch` on the fork). **In this repository the vendored `components/cpp_bus_driver` already has it applied** (`VENDOR.md`) – nothing to do unless you re-vendor the component.

**SPI DMA bounce buffer fix** – ESP-IDF's SPI master driver allocates and frees an internal DMA bounce buffer on every transaction when the caller's buffer lives in PSRAM or on the stack. The SX1262 LoRa radio fires ~20 SPI transactions per second; after ~83 seconds the internal heap fragments to the point where allocation fails and the radio dies. The fix pre-allocates a persistent 512-byte DMA buffer pair at `begin()` and bounces all transactions through it – zero per-transaction allocations, no fragmentation.

**I2S clock reconfiguration** – Adds `set_clock_rate()` to `Hardware_Iis`, allowing the music player to switch sample rates (e.g. 44100 → 48000 Hz) without tearing down and rebuilding the I2S channels. Also bumps `dma_desc_num` from 3 to 6 to prevent underruns during MP3 decode bursts.

**Logging suppression** – Hooks `serial_console_log_enabled()` into the bus driver's `assert_log` so verbose SPI/I2C/UART output is suppressed when the serial console is in command mode. Also disables `CPP_BUS_DRIVER_LOG_LEVEL_DEBUG` by default to suppress raw NMEA sentence dumps that overwhelm the serial link.

To re-apply after re-vendoring, from the `cpp_bus_driver` component root:

```
git apply ../../patches/cpp_bus_driver_adsb_scope.patch
```

### Build & Flash

```bash
git clone https://github.com/jstockdale/adsb-scope.git       # no submodules: everything third-party is vendored (VENDOR.md)
cd adsb-scope

# sdkconfig.defaults selects the application (CONFIG_EXAMPLE_BUILD_ADSB_RECEIVER) and the known-good configuration
idf.py set-target esp32p4
idf.py build
idf.py flash
```

If auto-reset doesn't work (USB-JTAG bridge disabled in firmware), hold BOOT + tap RESET, then flash.

`dependencies.lock` pins the registry components to the versions of the first verified build. If `idf.py` reports that cmake or ninja is missing, run `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja` and re-source `export.sh`. A clean build is about 2200 compile steps. Source files are picked up from their folders at configure time: after pulling a commit that adds one, run `idf.py reconfigure` once.

The decoder's repair stages – soft rescue and coherent second look, on by default – are described in `docs/DECODER.md` section 4 and can be switched on the console (`adsb dec`, `adsb coh`). For an image that boots without them, as r10 did:

```bash
idf.py -B build-plain -D SDKCONFIG=build-plain/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.plain" build
```

### C6 co-processor and SD card

- The **C6 co-processor** must run the ESP-Hosted `network_adapter` SDIO slave, version **2.12.3** – the exact LilyGo build is committed at `firmware/c6/` (sha256 in `VENDOR.md`). Flash it per LilyGo's C6 flashing procedure. The host component is pinned to 2.12.3 in `main/idf_component.yml`; on first build, confirm the host↔slave version handshake in the boot log.
- **SD card:** copy `sdcard/data/aircraft.db` to `/data/` on the card for registration/type lookups (regenerable via `main/examples/adsb_receiver/build_aircraft_db.py`).

### Merged firmware binary (for OTA / distribution)

```bash
cd build && esptool.py --chip esp32p4 merge_bin -o ../release.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x2000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 adsb-scope.bin
```

### Configuration Notes

- `CONFIG_HEAP_POISONING_LIGHT=y` – enabled for heap corruption debugging *(v1.0.8; the current `sdkconfig.defaults` does not set it and the generated configuration has heap poisoning disabled – corrections to the historical defaults are listed in `VENDOR.md`)*
- `CONFIG_FREERTOS_HZ=1000` – 1ms tick resolution (was 100 Hz / 10ms). Required for responsive spin-when-active strategy in serial recv.
- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_RX_BUF_SIZE=16384` – 16 KB USB Serial JTAG RX ring buffer. Sized to survive 14ms task preemption gaps at USB bulk transfer speeds. *(kept in `sdkconfig.defaults` as an unverified key: it does not appear in the configuration ESP-IDF v5.4.1 generates, where the console is UART0 at 115200 with USB-Serial-JTAG as secondary – `VENDOR.md`, "unverified keys")*
- ESP-Hosted enabled via `wifi_hosted.h` – C6 connects over SDMMC Slot 1, SD card uses SPI3
- `CPP_BUS_DRIVER_LOG_LEVEL_DEBUG` is commented out in `config.h` to suppress raw NMEA dumps
- `sdkconfig` retains `CONFIG_SCREEN_TYPE_HI8561=y` for compile-time header constants; runtime detection overrides all display-dependent behavior

## Repository

This repository is the standalone cut (2026-07-29) of what previously lived as the `adsb` branch on a LilyGo T-Display-P4 fork. All third-party code is vendored at recorded pins – see **VENDOR.md**. Local patches and the SD/WiFi coexistence fence are documented in **PATCHES.md**. The full architecture reference (with a dated correction block) is **docs/ARCHITECTURE.md**. Multi-board plans live under **boards/**.

**History.** `main` starts with the history of that fork's `adsb` branch (up to April 2026), continues with one commit that holds the standalone tree as of decoder-m1090 **r10** – the commits between the cut and r10 are not in this repository – and then the **coh** series commit by commit. Branches `r10` and `coh3` to `coh6` stay at those releases. Commits from r10 to coh4 carry an `Original-commit:` line: the hash they had before this history was put together, which is what firmware built before then reports as its version (`git log --grep 7629907` finds coh4, for one).

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
harness/                       host-side tests and measurements (make check)
VENDOR.md / PATCHES.md         provenance and patch mechanism
```

## Pending Work

> Items 1–16 and the struck-through list are as they stood in April 2026 (v1.0.8). Items 3, 4, 12, 13 and 15 were settled on 2026-11-21, items 1, 6, 7 and 9 on 2026-11-27; the others have **not been re-checked** against the current tree. What has been added since is under the headings below.

1. **PKI DM sending** – `buildPkiTx` with recipient pubkey lookup, DM recipient selector in UI. **Blocked (2026-11-27):** the PKI decrypt in `meshtastic_crypto.h` is XChaCha20-Poly1305 with a 16-byte tag; Meshtastic's firmware encrypts these messages with AES-256-CCM, an 8-byte tag and a 4-byte extra nonce appended (`CryptoEngine::encryptCurve25519`). So the receive side cannot have opened a real PKI DM – it was never verified (item 8) – and sending has to start with the right cipher for both directions
2. **Webapp PKI DM display** – show decrypted DM text in Meshy panel
3. ~~First WiFi field test~~ – done: WiFi has run on a device (confirmed by John, 2026-11-21)
4. ~~Flash C6 with ESP-Hosted slave firmware~~ – done on the unit(s) in use – WiFi could not have run otherwise; still a manual step for every new board (Building from Source)
5. **tz-embedded open source release** – timezone library as standalone repo
6. ~~scope_aircraft_t array consolidation~~ – superseded: the scope's aircraft arrays are two PSRAM allocations of `MAX_SCOPE_AIRCRAFT` entries made at start (`lvgl_ui.cpp`), nothing is left in `.bss` (seen in the code, 2026-11-27)
7. ~~Meshy decode-failed packet logging~~ – done in coh7: `meshy undecoded on` prints header fields and all bytes of every packet that neither a channel key nor PKI could open (off by default, until the next reboot)
8. **PKI DM field testing** – verify XChaCha20-Poly1305 with real Meshtastic DMs from other devices
9. ~~Meshy rebroadcast~~ – done in coh7, **off by default** (`meshy relay on`): Client relays after an SNR-weighted wait and yields if it hears the packet again, ClientMute never, RouterLate always and late. Not yet on the air
10. **Clock sync indicators** – on-device display of GPS-synced vs NTP-synced status
11. **Multi-chunk recv sessions** – stream chunks without returning to command mode between each; would cut ~10 minutes overhead from 27MB transfers
12. ~~Webapp retry on RECV_ERR~~ – done: the upload loop in `adsb_scope.htm` retries a failed chunk up to `MAX_RETRIES` times before aborting (seen in the code, 2026-11-21)
13. ~~Webapp CHUNK v1.1 header~~ – done: the page sends `CHUNK <size>` before the base64 data (seen in the code, 2026-11-21)
14. **Strip recv debug diagnostics** – remove `[recv] phase1/phase2` diagnostic output once transfer is proven reliable long-term
15. ~~mqtt_feeder forward declaration warning~~ – gone: the coh5 build has no warning in `mqtt_feeder` (build log, 2026-11-20)
16. **Monitor SD card health post-DMA-fence** – long-term validation that AXI bus contention fix eliminates FAT corruption
33. ~~AXI DMA corruption~~ – fixed: DMA fence in `sd_io_take/give` serializes GP-SPI SD card with ESP-Hosted WiFi SDIO DMA on shared AXI interconnect
34. ~~Serial file transfer~~ – done: `recv` command with CRC32-verified chunked transfer, resume, CHUNK v1.1 truncation detection, `verify_crc32` with progress
35. ~~Task priority inversion~~ – fixed: usb_host_lib was priority 2 below class_driver at 3; restructured to 6-tier scheme with USB data path at priority 4
14. ~~Restore WiFi~~ – done: ESP-Hosted over SDMMC Slot 1, SD card moved to SPI3 to avoid DMA conflict
15. ~~On-device radar scope~~ – done: canvas-drawn aircraft with range rings, trails, pan/zoom, helicopter icons, per-segment coloring
16. ~~Seed POSIX clock from RTC at boot~~ – resolved differently: system stays in `[boot+elapsed]` until GPS or NTP provides verified time; RTC seeds POSIX for TLS
17. ~~SD card graceful handling~~ – done: consecutive flush failure counter → unmount → periodic remount retry; card reinsertion resumes logging
18. ~~Fix screen~~ – fixed: root cause was `esp_lcd_panel_reset()` called after `Screen_Init()` wiping DSI lane config
19. ~~Fix USB DMA heap corruption~~ – fixed: early pre-alloc of 17KB bulk transfer buffer before heap fragmentation
20. ~~Re-enable LVGL aircraft display~~ – fixed: on-device ADS-B app with stats panel, aircraft table, rotation toggle
21. ~~Investigate WebSerial reset~~ – DTR race handled gracefully; scope deasserts DTR/RTS and parses reboot log
22. ~~SD card corruption on reboot~~ – fixed: power-cycle SD card via XL9535 SD_EN early in boot to reset card state machine
23. ~~Heading display~~ – fixed: ground speed heading (DF17 TC19 subtypes 1/2) now correctly applied
24. ~~ADS-B decoding~~ – fixed: removed double-decode in on_msg that zeroed all messages via memset
25. ~~Timezone~~ – fixed: GPS-derived timezone with DST support, 0.1° resolution grid covering 61 regions
26. ~~Runtime screen detection~~ – fixed: single firmware supports both HI8561 LCD and RM69A10 AMOLED via I2C probe
27. ~~USB hot-plug~~ – fixed: consecutive error detection, transfer buffer free/re-alloc, three-state status
28. ~~Settings not persisting~~ – fixed: `static volatile` flag in header gave each translation unit its own copy; now `extern` with definition in main.cpp
29. ~~ok_to_mqtt not reaching MQTT gateways~~ – fixed: was incorrectly in radio header `via_mqtt` bit; now encoded in encrypted Data protobuf field 9 (bitfield bit 0)
30. ~~CLIENT_MUTE blocking TX~~ – fixed: CLIENT_MUTE only suppresses relay, not origination
31. ~~NVS crash on settings save~~ – fixed: NVS write from PSRAM-stack task crashed on cache disable; now dispatches to internal-RAM helper task
32. ~~MSC crash on enter~~ – fixed: reordered sd_msc_enter() to stop RTL-SDR before unmounting SD, preventing in-flight SPI DMA from overwriting freed PSRAM

### WiFi and MQTT (coh6 – written, not yet run on a device)

- ~~WiFi + MQTT indicator in the status bar~~ – done in coh6; to be looked at on a device
- ~~WiFi that comes back~~ – done in coh6 (`wifi_sup.c`); to be tested with a real access point going away and coming back
- ~~More than one network~~ – done in coh6: five saved networks, priority then signal, console commands. Still open: managing the list from the on-device settings screen (it shows and edits network 1 only) and from the web app (which has no WiFi settings), and whether the radio should be power-cycled automatically after a long outage (`wifi cycle` to try it by hand)

### Since April 2026

- **Run coh4 and coh5 on a device** – nothing they change has (see Status): the per-message cost and lock changes, the console speed (is the console UART clocked as assumed, does the bridge do 921600), the web app's connect at 921600, its flashing with settings kept, and the page under real traffic (`?perf=1`)
- **Why the on-device list stopped updating near 300 messages/second** – not established (`docs/UPLINK.md` 9.7)
- **Gain loop thresholds** – from one recording; tune with `gain sweep` on a device (`docs/DECODER.md` 5.8)
- **Two differences from the April build that are not addressed** – the RTC read as local time, and the SD card's LDO
- **Web app** – parser order (aircraft line first), no DOM work for the log while its panel is closed, table rows updated only when they change, canvas renderer for trails (`docs/UPLINK.md` 9.9)
- **OTA** – needs partition tables with OTA slots (Roadmap, item 5)

### Roadmap (agreed plan)

1. BSP extraction (`boards/t_display_p4_v1_0`) with hardware parity.
2. Capability gating (Meshy/GPS/haptics/gauge/RTC compile-out).
3. Waveshare 7B bring-up.
4. 1024×600 wide layout profile.
5. Per-board sdkconfig/partitions (real OTA slots), per-board artifacts.

The longer view – an aviation instrument cluster – is in `docs/ROADMAP-AVIATION.md`.

## Acknowledgments

This project builds on the work of several open source authors:

- **[kvhnuke](https://github.com/kvhnuke/esp32-rtl-sdr)** – Original proof of concept for RTL-SDR on ESP32 via USB Host. The ESP32 USB-to-librtlsdr shim layer (`esp_libusb.c/h`) and the adapted `librtlsdr.c` driver originated from this project. Thank you for proving it could be done.

- **[Salvatore Sanfilippo (antirez)](https://github.com/antirez/dump1090)** – Author of dump1090, the Mode S decoder for RTL-SDR devices. The `mode-s.c` decoder is derived from dump1090's signal processing, preamble detection, CRC validation, and message decoding. Released under the BSD 3-Clause License.

- **[Thomas Watson](https://github.com/watson/libmodes)** – Author of libmodes, which refactored the dump1090 decoder into a clean reusable C library with the `mode_s_init` / `mode_s_detect` / `mode_s_decode` API that this project uses directly.

- **[Meshtastic](https://meshtastic.org/)** – The Meshtastic protocol specification and firmware were the reference for the mesh networking implementation. The protobuf wire format, channel encryption scheme (AES-CTR), PKI encryption (X25519 + XChaCha20-Poly1305), and radio parameters are all modeled on Meshtastic project's open documentation. **No Meshtastic code is utilized by or included in this repository.**

- **[meshtastic-lite](https://github.com/jstockdale/meshtastic-lite)** – Header-only C/C++ Meshtastic LoRa protocol stack for embedded systems.

- **[Martin Fiedler / minimp3](https://github.com/lieff/minimp3)** – Header-only MP3 decoder used for music playback. Public domain.

- **[LILYGO](https://github.com/Xinyuan-LilyGO/T-Display-P4)** – T-Display-P4 hardware design and the base ESP-IDF project with LVGL UI framework, peripheral drivers, and board support package.

- **[osmocom / Steve Markgraf](https://github.com/steve-m/librtlsdr)** – Original librtlsdr and the R820T/R828D tuner drivers.

- **[Daniel Estévez / more1090](https://github.com/daniestevez/more1090)** – A review of more1090 (v0.2.0, MIT OR Apache-2.0) was the origin of the decoder workstream: CRC syndrome repair, the timing-phase finding, the comparison recordings (`docs/DECODER.md`). The syndrome tables are generated by `harness/gen_syndrome_tables.py`; no more1090 source is included.

- **[FlightAware / dump1090](https://github.com/flightaware/dump1090)** – Used as an external measurement reference for the 2.0 vs 2.4 Msps comparison (`VENDOR.md`). No dump1090-fa source or binary is in this repository or in the firmware.

- **[Espressif / esptool-js](https://github.com/espressif/esptool-js)** – Browser flashing in ADS-B Scope (loaded from a CDN at run time; Apache-2.0). **[Leaflet](https://leafletjs.com/)**, **[three.js](https://threejs.org/)** and **[MQTT.js](https://github.com/mqttjs/MQTT.js)** – map, 3D view and MQTT in the web app.

## License

This project contains code under multiple licenses:

- **ADS-B Scope, Meshtastic-compatible implementation, music player, serial console, SD logging, timezone system, and all Off by One contributions: BSD 3-Clause**
- Mode S decoder: **BSD 3-Clause** (antirez/dump1090, watson/libmodes)
- minimp3: **Public Domain** (CC0)
- RTL-SDR driver and tuner code: **GPL v2** (osmocom/librtlsdr)
- LILYGO board support and UI framework: **GPL v3** (LILYGO)

> **Open point:** `VENDOR.md` ("Top-level license"), written at the standalone cut, records the top-level license for the adsb-scope application code as *an open decision*, with dual GPL/commercial as the working assumption – which does not match the BSD 3-Clause line above. One of the two needs to change; until it is decided, neither should be relied on. `LICENSES/LICENSE.lilygo-t-display-p4` preserves the upstream license for the vendored LilyGo material.

See individual file headers for details.

---

## Original LilyGo Documentation

<details>
<summary>Click to expand original T-Display-P4 hardware documentation</summary>

> Kept as it was on the fork. Its relative links pointed into the LilyGo tree, most of which the standalone cut left behind; those now point at the fork's `adsb` branch, where the files are.

## VersionIteration:
| Version                               | Update date                       |Update description|
| :-------------------------------: | :-------------------------------: |:--------------: |
| T-Display-P4_V1.0                      | 2025-06-13                    |   Original version      |
| T-Display-P4-Keyboard_V1.0                      | 2025-09-12                    |   Original version      |

## PurchaseLink

| Product                     | SOC           |  FLASH  |  PSRAM   | Link                   |
| :------------------------: | :-----------: |:-------: | :---------: | :------------------: |
| T-Display-P4_V1.0   | NULL |   NULL   | NULL |  [NULL]()   |

## Module

### T-Display-P4 Section
### 1. Core Processor  

* Chip: ESP32-P4  
* FLASH: 16M  
* Related Documents:  
    >[Espressif](https://www.espressif.com/en/support/documents/technical-documents)  

### 2. Auxiliary Processor

* Module: ESP32-C6-MINI-1U
* Chip: ESP32-C6-FH4
* PSRAM: -
* FLASH: 4M 
* Communication Protocol: SDIO
* Other: For more information, please visit [Espressif ESP32-C6-MINI-1U datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6-mini-1_mini-1u_datasheet_en.pdf)

### 3. Display & Touch  

> #### Model: H0405S002T002-V0  
> * Display Size (Diagonal): 4.05 inch  
> * LCD Type: α-Si TFT  
> * Resolution: 540(H) × 1168(V) px  
> * Active Area: 41.9904(H) × 91.1040(V) mm  
> * Module Dimensions: 44(H) × 95.5(V) × 1.46(T) mm  
> * Display Colors: 16.7M  
> * Display Interface: MIPI  
> * Touch Interface: IIC
> * Display & Touch Driver IC: HI8561  
> * Maximum touch points: 10-point touch
> * Luminance on surface: 550 cd/m²
> * View Direction: All
> * Contrast ratio: 1200:1
> * Color gamut: 70%
> * PPI: 326
> * Window effect: No all-black  
> * Cover plate surface effect: No AF/AG
> * Operating Temperature: -20～70  ºC
> * Storage Temperature: -30～80 ºC
> * Related Documents:  
>    >[HI8561](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/HI8561_Preliminary%20_DS_V0.00_20230511.pdf)  

### 4. Speaker & Microphone  

* DAC Chip: ES8311  
* Amplifier Chip: NS4150B  
* Microphone: Electret Condenser Mic  
* Communication Protocol: IIS
* Related Documents:  
    >[ES8311](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/ES8311.pdf)  
    >[NS4150B](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/NS4150B.pdf)

### 5. LoRa  

* Module: HPD16A  
* Chip: SX1262, SKY13453-385LF
* Communication Protocol: Standard SPI  
* Other notes: Use a dedicated RF analog switch to switch the antenna
* Related Documents:  
    >[SX1261-2](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/DS_SX1261-2_V2_1.pdf)  
* Dependent Libraries:  
    >[cpp_bus_driver](https://github.com/Llgok/cpp_bus_driver)  

### 6. GPS  

* Module: L76K  
* Communication Protocol: Uart
* Related Documents:  
    >[L76K](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/L76KB-A58.pdf)  
* Dependent Libraries:  
    >[cpp_bus_driver](https://github.com/Llgok/cpp_bus_driver)  

### 7. RTC  

* Chip: PCF8563  
* Communication Protocol: IIC
* Related Documents:  
    >[PCF8563](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/PCF8563.pdf)  
* Dependent Libraries:  
    >[cpp_bus_driver](https://github.com/Llgok/cpp_bus_driver)  

### 8. Battery Gauge  

* Chip: BQ27220  
* Communication Protocol: IIC
* Related Documents:  
    >[BQ27220](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/bq27220_en.pdf)  
* Dependent Libraries:  
    >[cpp_bus_driver](https://github.com/Llgok/cpp_bus_driver)  

### 9. Camera  

* Model: OV2710  
* Interface: MIPI  
* Related Documents:  
    >[OV2710](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/OV2710_CSP3_DS_2.0_KING%20HORN%20ENTERPRISES%20Ltd..pdf)  

### 10. IMU

* Chip: ICM20948
* Communication Protocol: IIC
* Related Documents:  
    >[ICM20948](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/ICM20948.pdf)

### 11. IO Expansion

* Chip: XL9535
* Communication Protocol: IIC
* Related Materials:
    > [XL9535](https://github.com/jstockdale/T-Display-P4/tree/adsb/information/XL95x5.pdf)

</details>

## SoftwareDeployment

### Examples Support

#### T-Display-P4 Examples
| example | `[vscode][esp-idf-v5.4.0]` | description | picture |
| ------  | ------ | ------ | ------ | 
| [afe](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/afe) |  <p align="center">![alt text][supported] | | |
| [aw86224](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/aw86224) |  <p align="center">![alt text][supported] | | |
| [bq27220](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/bq27220) |  <p align="center">![alt text][supported] | | |
| [deep_sleep](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/deep_sleep) |  <p align="center">![alt text][supported] | | |
| [es8311](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/es8311) |  <p align="center">![alt text][supported] | | |
| [es8311_sd_wav](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/es8311_sd_wav) |  <p align="center">![alt text][supported] | | |
| [esp_hosted_mcu_sdio_wifi](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/esp_hosted_mcu_sdio_wifi) |  <p align="center">![alt text][supported] | | |
| [esp32c6_at_host_sdio_uart](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/esp32c6_at_host_sdio_uart) |  <p align="center">![alt text][supported] | | |
| [esp32c6_at_host_sdio_wifi](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/esp32c6_at_host_sdio_wifi) |  <p align="center">![alt text][supported] | | |
| [icm20948](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/icm20948) |  <p align="center">![alt text][supported] | | |
| [iic_scan](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/iic_scan) |  <p align="center">![alt text][supported] | | |
| [l76k](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/l76k) |  <p align="center">![alt text][supported] | | |
| [lvgl_9_ui](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/lvgl_9_ui) |  <p align="center">![alt text][supported] |factory example / ADS-B receiver | |
| [pcf8563](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/pcf8563) |  <p align="center">![alt text][supported] | | |
| [radiolib_sx1262_send_receive](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/radiolib_sx1262_send_receive) |  <p align="center">![alt text][supported] | | |
| [screen_camera](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/screen_camera) |  <p align="center">![alt text][supported] | | |
| [screen_lvgl](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/screen_lvgl) |  <p align="center">![alt text][supported] | | |
| [screen_lvgl_touch_draw](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/screen_lvgl_touch_draw) |  <p align="center">![alt text][supported] | | |
| [sgm38121](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/sgm38121) |  <p align="center">![alt text][supported] | | |
| [sx1262_gfsk_send_receive](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/sx1262_gfsk_send_receive) |  <p align="center">![alt text][supported] | | |
| [sx1262_lora_send_receive](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/sx1262_lora_send_receive) |  <p align="center">![alt text][supported] | | |
| [sx1262_tx_continuous_wave](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/sx1262_tx_continuous_wave) |  <p align="center">![alt text][supported] | | |
| [tusb_serial_device](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/tusb_serial_device) |  <p align="center">![alt text][supported] | | |
| [xl9535](https://github.com/jstockdale/T-Display-P4/tree/adsb/main/examples/Vibration_Motor) |  <p align="center">![alt text][supported] | | |
| [xiaozhi](https://github.com/78/xiaozhi-esp32) |  <p align="center">![alt text][supported] | | |

[supported]: https://img.shields.io/badge/-supported-green "example"

| firmware | description | picture |
| ------  | ------  | ------ |
| [t_display_p4_lvgl_9_ui](https://github.com/jstockdale/T-Display-P4/tree/adsb/firmware/[T-Display-P4][lvgl_9_ui]) | factory program |  |
| [esp32c6_at](https://github.com/jstockdale/T-Display-P4/tree/adsb/firmware/[T-Display-P4][esp32c6_at_slave]) | esp32c6-at factory program |  |
| [esp32c6_slave_esp_hosted_mcu_network_adapter](https://github.com/jstockdale/T-Display-P4/tree/adsb/firmware/[T-Display-P4][esp32c6_slave_esp_hosted_mcu_network_adapter]) |  |  |

## PinOverview

For pin definitions, please refer to the configuration file: 
<br />

[t_display_p4_config.h](./components/private_library/t_display_p4_config.h)  

## FAQ

* Q. After reading the above tutorials, I still don't know how to build a programming environment. What should I do?
* A. If you still don't understand how to build an environment after reading the above tutorials, you can refer to the [LilyGo-Document](https://github.com/Xinyuan-LilyGO/LilyGo-Document) document instructions to build it.

<br />

* Q. Why is my board continuously failing to download the program?
* A. Please hold down the "BOOT" button and try downloading the program again.

<br />

* Q. Why does the device reboot every time I connect a serial monitor?
* A. The ESP32-P4's USB-JTAG peripheral has an auto-reset circuit that triggers when DTR/RTS are toggled during port open. The firmware attempts to disable this via `usb_serial_jtag_ll_phy_set_jtag_bridge(false)`. If the device still resets, use tools that suppress DTR/RTS (e.g., `pyserial` with `dtr=False, rts=False`), or use ADS-B Scope which deasserts control lines immediately after connecting. *(Update, coh4: by the build's configuration the console is UART0 behind the board's USB bridge, whose RTS line – per LilyGO's wiki – can reset the P4; `docs/UPLINK.md` 9.8. The effect is the same.)*

<br />

* Q. Why is WiFi not working?
* A. WiFi requires the ESP32-C6 coprocessor to be flashed with the ESP-Hosted slave firmware. The C6 connects to the P4 over SDMMC Slot 1, while the SD card runs on SPI3 to avoid DMA conflicts (Espressif Issue #17889). Configure WiFi via the on-device settings UI or serial commands (`wifi ssid <name>`, `wifi pass <pw>`, `wifi on`). WiFi is optional – the device works fully offline with GPS alone.
