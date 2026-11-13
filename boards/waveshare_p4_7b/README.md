# waveshare_p4_7b (stub – phase 3 target)

Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.  Two units on hand (no camera
installed; eventual camera support wanted – the -C SKU ships OV5647).
L76K GNSS modules on order for the PH2.0 UART header → BOARD_CAP_GPS=1
via board_gps_uart() from day one.

Known from docs: P4NRW32 (32 MB PSRAM) + 32 MB flash, C6-MINI-1 on SDIO
(own flashing UART header), 7" 1024×600 LANDSCAPE MIPI-DSI + touch,
TF on native SDIO 3.0, USB 2.0 OTG HS Type-A (RTL-SDR direct),
ES8311 + ES7210 dual-mic, RTC coin cell, MX1.25 battery + switch,
CAN / RS485 / I2C / UART PH2.0 headers, 17 free GPIOs.
Schematic: https://files.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B/ESP32-P4-WIFI6-Touch-LCD-7B.pdf
Demos: https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B

To extract from schematic/demos before bring-up:
- touch controller (GT911-class?) + I2C addr/reset wiring
- DSI panel controller (EK79007/JD9165-class?) + init sequence
  (backport to IDF v5.4.1 – do NOT assume newer-IDF APIs from demos)
- any IO expander; backlight control path
- SDMMC slot assignments: TF slot vs C6 SDIO – both live on the one
  SDHOST; verify the #17889 contention regime and whether the driver's
  host lock serializes them (no GP-SPI escape hatch on this board;
  the sd fence strategy may differ entirely from T-Display)
- RTC chip identity (external vs P4 LP backup domain)
- charger / gauge chips (battery status icon capability)
- C6 slave firmware: which network_adapter build Waveshare ships /
  whether firmware/c6 needs a second, Waveshare-targeted bin
