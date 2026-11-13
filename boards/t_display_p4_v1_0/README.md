# t_display_p4_v1_0 (stub – phase 1 target)

LilyGo T-Display-P4 v1.0.  SGM38121 power tree, XL9535 expander,
HI8561 LCD or RM69A10 AMOLED (runtime screen_detect), GT9895/HI8561
touch, SX1262 LoRa, L76K GNSS, PCF8563 RTC, ES8311+NS4150B audio,
AW86224 haptics, ICM20948 IMU, BQ27220 gauge, OV2710 camera, C6 on
SDMMC slot 1 (ESP-Hosted), SD on GP-SPI3 (SDMMC slot 0 for MSC mode).

Phase 1 moves here, verbatim, from main.cpp: the XL9535 power/reset
choreography, screen_detect + panel/touch bring-up, SD mount paths +
power cycle, C6 enable/wake, PCF8563 access, haptic wrapper, and the
SX1262 lora-port implementation.  Caps: everything =1 except GPS_PPS.
