# VENDOR.md – provenance, pins, and vendoring decisions

adsb-scope is a standalone repository cut from a fork of LilyGo's
T-Display-P4 tree.  Everything third-party is vendored in-tree at a
recorded pin; there are no git submodules.  This file is the single
source of truth for where every piece came from and why.

Cut date: 2026-07-29.

## Lineage

| What | Ref |
| --- | --- |
| Our fork tip at cut (`jstockdale/T-Display-P4` branch `adsb`) | `6fd3692ee2b690512a345c06c292bc4c5acc82d2` |
| Upstream merge-base (`Xinyuan-LilyGO/T-Display-P4` main) | `b205ec3f14f19e8c1205376daaa33d830edc89b7` (2026-03-05) |
| Upstream main at cut (for reference only – NOT merged) | `4930e8dd4c77d6268d46b25c1a1eea01174a0c46` (2026-07-28) |

Upstream diverged structurally after our merge-base (components/ →
libraries/ restructure, `lvgl_9_ui` deleted, cpp_bus_driver rewritten to
a v2.1.1 API, new usp_cpp_bus_driver / lilygo_device_driver stack, IDF
5.5.x compatibility, T-Display-P4 v1.1/v2.0 hardware support).  A merge
was evaluated and rejected – it would have been a re-port onto a driver
API this project is about to hide behind a board abstraction anyway.
Future support for LilyGo v1.1/v2.0 hardware is planned as a sibling BSP
built over upstream's v2.x libraries, not as a merge into this baseline.

## Vendored components

| Path | Origin | Pin | Local delta |
| --- | --- | --- | --- |
| `components/cpp_bus_driver` | github.com/Llgok/cpp_bus_driver | `158b9c3f4a6ef5fb1e67adf0f307758593bf9649` | `patches/cpp_bus_driver_adsb_scope.patch` **applied** |
| `components/ICM20948_WE` | github.com/Llgok/ICM20948_WE | `e548558e83fd149ac0210c87c2bd7279096a024e` | none |
| `components/kode_bq25896` | github.com/Llgok/kode_bq25896 | `5c7d812338e5ea494140c3f84703eba934d5f5b0` | none |
| `components/private_library` | LilyGo tree (in-tree, not a submodule) | as of fork tip | ours-modified (screen_detect, runtime panel create, IPA configs) |
| `components/ethernet_init` | LilyGo tree (in-tree) | as of fork tip | as-shipped |
| `components/esp_video` | LilyGo tree (in-tree copy of espressif/esp_video) | as of fork tip | as-shipped |
| `main/examples/adsb_receiver` | ours | fork tip | the application |
| `firmware/c6/*.bin` | LilyGo release (ESP-Hosted network_adapter slave) | v2.12.3, build 202606150919 | sha256 `12d164f7c5eb2d8ab434549fdb03d13d805de35f805efb9bfb11fee6fa487fc7` |

The vendored cpp_bus_driver is byte-reconstructible: `git clone` the
origin, `git checkout 158b9c3`, `git apply patches/cpp_bus_driver_adsb_scope.patch`.
This was verified during the cut (patch applies cleanly at the pin).

## Registry dependencies (main/idf_component.yml)

| Component | Pin | Rationale |
| --- | --- | --- |
| lvgl/lvgl | 9.3.0 | unchanged from working build |
| espressif/esp_tinyusb | ^1 | unchanged (match: !ENABLE_USB_DISPLAY) |
| espressif/esp-sr | ^2.1.3 | unchanged |
| espressif/esp_lcd_usb_display | ^1.0.0 | unchanged (match: ENABLE_USB_DISPLAY) |
| espressif/esp_hosted | **2.12.3** | Pinned to match the C6 slave bin exactly.  The old yml comment said 2.0.17, but the known-good generated sdkconfig contains 2.12-era Kconfig keys (`ESP_HOSTED_CLI_ENABLED`, `ESP_HOSTED_ENABLE_ITWT`, `ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER`), proving the real managed_components host was 2.12-era.  2.12.3 verified on the registry: requires idf >= 5.3 (v5.4.1 OK) and contains all three keys. |
| espressif/esp_wifi_remote | ~1.1.6 | Matches the old yml intent (^1.1); 1.1.6 is the newest 1.1.x on the registry. |

**First-build check:** confirm the ESP-Hosted host↔slave version
handshake in the boot log shows 2.12.3 on both sides, and that
`wifi_hosted.h` compiles against the 2.12.3 host API.  The previous
working build resolved these components before the yml lines were
commented out, so the exact previously-resolved versions are unknowable
from the repository alone.

## Dropped from the LilyGo tree (not carried)

- `main/examples/lvgl_9_ui` – **stock LilyGo demo** (music player / CIT /
  NFC icons), 45 MB.  Kept in the fork only as an untouched upstream
  reference; the live application is `adsb_receiver`.  Note: the old
  tree's sdkconfig still selected `CONFIG_EXAMPLE_BUILD_LVGL_9_UI=y`
  while no `EXAMPLE_BUILD_ADSB_RECEIVER` Kconfig symbol existed – a
  latent selector mismatch.  This repo defines the symbol properly and
  sets it in sdkconfig.defaults.
- All other `main/examples/*` and `main/keyboard_examples/*` demos.
- Submodules used only by the keyboard board or unused: RadioLib,
  NFC-RFAL, ST25R3916, arduino_cpp_bus_driver, esp-at.
- LilyGo firmware release binaries, `image/`, `debug/`, `information/`,
  `project/`, `relevant_test/`, `tool/`, `.github/`.

## Upstream harvest review (2026-07-29)

Two candidate fixes were identified between our cpp_bus_driver pin and
upstream, reviewed in full, and **not applied**:

1. **GT9895 touch (`_rst` init position, upstream bump 2026-03-14).**
   The actual delta (cbd `4a0c037…` → Mar-12 head) is a driver rework:
   new 32-bit-register I2C bus overloads, a device-ID verification gate
   in `begin()`, and a post-reset settle change **10 ms → 100 ms**.  The
   ID gate would change `begin()` failure semantics and interact badly
   with our raw-probe `screen_detect` (which exists precisely because
   probing GT9895 via `begin()` was unreliable).  **Transplantable
   insight if AMOLED touch init ever flakes:** lengthen the post-reset
   settle in our vendored `src/chip/iic/gt9895.cpp` `begin()` from
   `delay_ms(10)` to `delay_ms(100)` – a one-line change.
2. **`create_pwm` (upstream bump 2026-03-24).**  Pure API signature
   reorder (`timer_num` moved to 2nd positional parameter).  Zero
   functional content; applying it would only break call sites.

## sdkconfig corrections (vs. the fork's sdkconfig.defaults)

Ground truth: the generated sdkconfig from the known-good v5.4.1 build.

1. Removed invalid `//CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=…` line
   (`//` is not a Kconfig comment; the line was silently ignored).
2. `CONFIG_ESPTOOLPY_FLASHFREQ_120M=y` → `…_80M=y`.  120M was silently
   ignored; the working build resolved to 80 MHz.
3. ESP-Hosted keys renamed to the real symbols:
   `CONFIG_ESP_HOSTED_TRANSPORT_SDIO` → `CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE`,
   slot selection via `CONFIG_ESP_HOSTED_SDIO_SLOT_1=y`, and the six
   `CONFIG_ESP_HOSTED_SDIO_*_GPIO` pin lines **removed** (wrong names,
   silently ignored; the build worked because esp_hosted's slot-1
   defaults – CLK18 CMD19 D0–D3 = 14–17 – happen to match the
   T-Display-P4 wiring).  When adding a board with different wiring, set
   pins with the real key names (`CONFIG_ESP_HOSTED_SDIO_PIN_*`).
4. Added `CONFIG_EXAMPLE_BUILD_ADSB_RECEIVER=y` (see lvgl_9_ui note).

### Unverified keys (kept, flagged)

- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_RX_BUF_SIZE=16384` – not observed
  in the generated sdkconfig; may be ignored on v5.4.1.  Harmless if so.
- `CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n` – generated shows `=y`, likely
  select-forced by esp_wifi_remote.  Kept for intent.

## Known-good build environment

- ESP-IDF **v5.4.1** (all boards, hard rule) with
  `patches/esp-idf-v5.4.1-sd-coexistence.patch` applied – see PATCHES.md.
- Compiler optimization in the known-good build was `-Og`
  (`CONFIG_COMPILER_OPTIMIZATION_DEBUG=y`, IDF default).  All published
  performance numbers were measured at -Og; revisiting this is a
  deliberate future change, not part of the cut.
- Wake-word / speech models (`esp-sr`, WN9 HIESP+HILEXIN, model in
  flash) are configured but the ADS-B app does not use speech; pruning
  them is a future flash-diet item, deliberately untouched for parity.

## Weight notes

- `sdcard/data/aircraft.db` (27 MB) is committed deliberately to keep
  the barrier to entry low; it is regenerable via
  `main/examples/adsb_receiver/build_aircraft_db.py` from the OpenSky
  dataset.  Moving it to Releases/LFS is a future option.
- `firmware/jstockdale-adsb_receiver/` carries the released P4 bins as
  before.  The webapp fetches `latest.bin` / `latest.version.json`
  relative to `FW_BASE` (`https://adsb-scope.offx1.com/`), so binary
  file naming is a server-side concern; `update-version.sh --deploy`
  publishes the version file.

## Top-level license

`LICENSES/LICENSE.lilygo-t-display-p4` preserves the upstream license for
the vendored LilyGo material (cpp_bus_driver is GPL-3.0 per its headers;
esp_video is Apache-2.0 per its LICENSE).  The top-level license for the
adsb-scope application code is **an open decision** – dual GPL/commercial
(as used elsewhere at Off by One) is the working assumption; set before
publishing.

## Keyboard-guarded sources

`st25r3916_driver.{cpp,h}` and the `RadioLib.h` include in `main.cpp`
are wrapped in `#if defined CONFIG_BOARD_TYPE_T_DISPLAY_P4_KEYBOARD`
and compile to nothing on `BOARD_TYPE_T_DISPLAY_P4` (verified – same as
the known-good build).  The Kconfig still offers the keyboard board
type for parity, but its component dependencies (RadioLib, NFC-RFAL,
ST25R3916) were dropped at the cut: selecting it will fail at compile
with missing headers.  If keyboard support is ever wanted, re-vendor
those three at the pins recorded in the fork's .gitmodules history.

## Decoder workstream provenance (branch decoder-m1090, 2026-11-09)

- **more1090** v0.2.0, commit `bcb0bccdf` (github.com/daniestevez/more1090,
  (c) Daniel Estevez, MIT OR Apache-2.0). Used two ways:
  1. *Algorithms*: the syndrome-table approach to 1-bit/2-bit CRC error
     correction and the DF17/18 field-validity discipline in `mode-s.c`
     follow its `src/mode_s.rs`. The tables are re-derived and proven by
     `harness/gen_syndrome_tables.py`; no more1090 source is included.
  2. *Test tooling*: its simulator generates the ground-truth scenarios the
     harness scores against. Built from source by the harness instructions;
     not vendored, not linked into firmware.
- **Madrid test recording**: Zenodo record 23360412 (D. Estevez, 20 s,
  8 Msps). Fetched by the harness instructions; not redistributed here.
- `harness/baseline/` is this repository's own `mode-s.c` / `mode-s.h` as of
  commit `b8dd639`, kept as the A/B reference.
- Host-side Python dependencies: numpy, scipy, matplotlib (BSD-family).

## Correction and first verified build (2026-11-09)

**The vendor cut of 2026-07-29 could not be built.** Three defects, all
fixed on branch `decoder-m1090`:

1. Four components were dropped as "keyboard-only". They are not.
   `private_library` lists RadioLib, ST25R3916 and NFC-RFAL in its CMake
   `REQUIRES` (and compiles a RadioLib HAL bridge unconditionally), and
   `ICM20948_WE` requires `arduino_cpp_bus_driver`, which supplies the
   `Arduino.h` / `Wire.h` / `SPI.h` it includes. The statements above under
   "Dropped from the LilyGo tree" and "Keyboard-guarded sources" that these
   are unused are wrong: only the *source-level includes in the app* are
   keyboard-guarded. Restored at the commits the fork pinned at `6fd3692`:

   | Path | Origin | Pin |
   | --- | --- | --- |
   | `components/arduino_cpp_bus_driver` | github.com/Llgok/arduino_cpp_bus_driver | `0ccf01ac0629093fce2f967fc0441eedaa74964d` |
   | `components/RadioLib` | github.com/jgromes/RadioLib | `e0fc757dc009a0e3ad557189fa5faee1147dce19` |
   | `components/ST25R3916` | github.com/Llgok/ST25R3916 | `450ca6d2ac5851efd02f004004be69926eddde3c` |
   | `components/NFC-RFAL` | github.com/Llgok/NFC-RFAL | `543bae0b2ffd93c8011bbf5a4e32c0ba62bfe88d` |

   None of their code is referenced by the T-Display-P4 (non-keyboard)
   application, so the linker discards it; they cost build time, not flash.
   (`main/examples/esp-at` remains dropped; nothing requires it.)
2. `main/CMakeLists.txt` raised a fatal error whenever
   `CONFIG_EXAMPLE_BUILD_ADSB_RECEIVER` was unset - which is always true
   during ESP-IDF's early component-requirement pass. The path is now
   unconditional.
3. `main/idf_component.yml` gated `espressif/esp_tinyusb` on
   `$CONFIG{ENABLE_USB_DISPLAY}`. The component manager cannot evaluate
   Kconfig conditions on ESP-IDF 5.4.1; it ignores the clause and skips the
   dependency, leaving the USB mass-storage code with nothing to build
   against. `esp_tinyusb` is now unconditional; `esp_lcd_usb_display`
   (needed only with `CONFIG_ENABLE_USB_DISPLAY=y`) is commented out.

**Verified build.** ESP-IDF v5.4.1 with the SD-coexistence patch applied,
toolchain `riscv32-esp-elf` esp-14.2.0_20241119, Linux x86-64: clean
configure and build, 2175 steps, no errors. The resulting configuration
matches the known-good ground truth (-Og, 16 MB DIO 80 MHz, PSRAM HEX
200M, ESP-Hosted SDIO slot 1 on CLK18/CMD19/D0-3=14-17 with reset on 54,
FATFS 4096 x 2 volumes, TinyUSB MSC 4096 on CPU1 priority 5).
Not verified: anything on hardware, including the ESP-Hosted host/slave
handshake.

**Resolved registry versions** (now pinned by `dependencies.lock`; the
original tree carried no lock file, so the versions of the last known-good
build are unknown and these may differ from them): lvgl 9.3.0, esp_hosted
2.12.3, esp_wifi_remote 1.1.6, esp_tinyusb 1.7.6~2, tinyusb 0.21.0~4,
esp-sr 2.6.1 (which now pulls esp-dl 3.3.15, dl_fft 0.8.0, esp_new_jpeg
1.0.2), esp_cam_sensor 1.0.0, esp_ipa 0.3.0~1, esp_h264 1.0.4,
esp_sccb_intf 1.0.0, cmake_utilities 0.5.3, cjson 1.7.19~2, eppp_link
1.1.6, wifi_remote_over_eppp 0.3.3, esp_serial_slave_link 1.1.2. Nothing
in the application or `private_library` includes an esp-sr header.

**sdkconfig key confirmed unknown on v5.4.1:**
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_RX_BUF_SIZE` (flagged as unverified
above) is not a Kconfig symbol in this IDF version and is ignored.

## Measurement reference: dump1090-fa (2026-11-10)

`harness/experiments/rate_24.py` runs FlightAware's dump1090
(github.com/flightaware/dump1090, commit `465e063`, GPL-licensed) as an
external program to measure what a 2.4 Msps demodulator achieves on the
harness inputs. It is built separately by whoever runs the experiment;
no dump1090 source or binary is in this repository or in the firmware.
