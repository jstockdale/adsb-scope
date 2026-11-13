# boards/ – multi-board support (scaffolding)

Phase plan (agreed 2026-07-29; nothing here is wired yet):

1. **Mechanical extraction** – move board-owned code from `main.cpp`
   into `boards/t_display_p4_v1_0/` behind `include/board_api.h`,
   zero behavior change, full parity soak on hardware (both TFT and
   AMOLED units) before anything else.
2. **Capability gating** – compile-out Meshy / GPS / haptics / gauge /
   RTC paths via `BOARD_CAP_*`; verify the "minimal board" config still
   runs on T-Display hardware before the 7B arrives on the bench.
3. **Waveshare 7B bring-up** – display+touch → USB host + RTL-SDR →
   SD (+ coexistence soak) → WiFi/C6 → audio.
4. **Wide layout profile** for 1024×600 landscape (scope + table
   side-by-side).
5. **Housekeeping** – per-board sdkconfig fragments and partitions
   (including real ota_0/ota_1), per-board firmware artifacts.

Board naming carries the hardware revision (`t_display_p4_v1_0`) because
LilyGo v1.1/v2.0 differ materially (BQ25896 charging; v2.0 routes GPS
PPS to the core – the stratum-0 time source that unlocks MLAT/DX work).
Those arrive later as sibling BSPs over upstream's v2.x driver stack.
