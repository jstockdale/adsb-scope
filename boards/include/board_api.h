/**
 * board_api.h — DRAFT board support contract for adsb-scope.
 *
 * STATUS: design draft.  Nothing includes this yet.  Phase 1 of the
 * multi-board plan extracts the board-owned code out of main.cpp into
 * boards/t_display_p4_v1_0/ behind exactly this surface, with zero
 * behavior change, verified for parity on hardware before anything else
 * moves.  See boards/README.md for the plan and phase gates.
 *
 * Design rules:
 *   - Compile-time board selection (Kconfig `BOARD` choice); exactly one
 *     BSP links in.  Link-time binding, no vtables.
 *   - Optional capabilities are weak no-op symbols; app code may also
 *     compile-gate on BOARD_CAP_* for UI/settings visibility.
 *   - Runtime detection is allowed INSIDE a BSP (e.g. the T-Display's
 *     HI8561-vs-RM69A10 screen_detect), never across boards.
 *   - Every function below is derived from an existing call site in
 *     main.cpp / sd_msc_mode.h / meshtastic_task.cpp — no speculative
 *     surface.  If a new board needs something not listed, the API grows
 *     then, not before.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Capabilities ──────────────────────────────────────────────────────
 * Defined (as 0/1) by each board's board_caps.h.  App code gates
 * settings panels, status-bar icons, and task creation on these.
 *
 *   BOARD_CAP_LORA          SX126x radio present (Meshy)
 *   BOARD_CAP_GPS           GNSS module present or attachable (UART)
 *   BOARD_CAP_GPS_PPS       PPS routed to a P4 GPIO (stratum-0 time;
 *                           prerequisite for MLAT/DX ambitions —
 *                           LilyGo v2.0 hardware, future BSP)
 *   BOARD_CAP_RTC           battery-backed external RTC (PCF8563-class)
 *   BOARD_CAP_HAPTIC        haptic driver (AW86224-class)
 *   BOARD_CAP_IMU           IMU (ICM20948-class)
 *   BOARD_CAP_FUEL_GAUGE    battery gauge (BQ27220-class)
 *   BOARD_CAP_CHARGER       charger IC (BQ25896-class)
 *   BOARD_CAP_SD_POWER_GATE SD card VCC can be power-cycled in software
 *   BOARD_CAP_CAMERA        MIPI-CSI sensor population expected
 *   BOARD_CAP_MIC_ARRAY     multi-mic + AEC front end (ES7210-class)
 */

/* ── Boot / power sequencing ──────────────────────────────────────────
 * Absorbs the XL9535 choreography on T-Display (rail enables, screen /
 * touch resets, boot-order constraints).  Runs before any peripheral
 * init; must leave I2C buses usable. */
bool board_early_init(void);

/* ── Display / touch / backlight ─────────────────────────────────────
 * board_display_init owns panel bring-up (DSI init, vendor init code,
 * intra-board variant detection) and reports geometry.  Returned panel
 * handle is esp_lcd's. */
typedef struct {
    void    *panel;          /* esp_lcd_panel_handle_t */
    uint16_t width, height;  /* native pixels, native orientation */
    bool     landscape;      /* native orientation of the glass */
    uint8_t  ui_profile;     /* BOARD_UI_PROFILE_* hint for lvgl layout */
} board_display_t;
bool board_display_init(board_display_t *out);
bool board_touch_read(int16_t *x, int16_t *y, bool *pressed);
void board_backlight_set(uint8_t percent);

/* ── SD card ──────────────────────────────────────────────────────────
 * mount/unmount cover the board's normal-operation transport (SPI3 on
 * T-Display, native SDMMC on Waveshare 7B).  msc_mount covers the
 * high-speed transport used for USB MSC where it differs.
 * power_cycle and the dma fence hooks are optional (weak no-ops when a
 * board lacks the gate / needs no fence). */
bool board_sd_mount(void);
void board_sd_unmount(void);
bool board_sd_mount_msc(void);          /* optional; default = board_sd_mount */
bool board_sd_power_cycle(void);        /* optional; BOARD_CAP_SD_POWER_GATE */
bool board_sd_dma_fence_take(uint32_t timeout_ms);  /* optional */
void board_sd_dma_fence_give(void);                 /* optional */

/* ── WiFi co-processor ───────────────────────────────────────────────
 * ESP-Hosted transport config lives in per-board sdkconfig fragments;
 * the BSP only owns the enable/reset lines and any wake pulse. */
void board_c6_set_power(bool on);
void board_c6_wakeup(void);             /* optional */

/* ── Time hardware ───────────────────────────────────────────────────
 * RTC read/write feed time_manager's stratum-16 seed / write-back.
 * gps_uart binding lets gps_task open whatever UART the board routes to
 * a GNSS module (built-in L76K on T-Display; PH2.0-header L76K on the
 * Waveshare 7B).  pps_gpio returns -1 when absent; when present,
 * time_manager's reserved TIME_SRC_PPS (stratum 0) gets wired to it. */
bool board_rtc_read(struct tm *out);    /* optional; BOARD_CAP_RTC */
bool board_rtc_write(const struct tm *t);
typedef struct { int uart_num; int tx_gpio; int rx_gpio; int baud; } board_gps_uart_t;
bool board_gps_uart(board_gps_uart_t *out);   /* optional; BOARD_CAP_GPS */
int  board_gps_pps_gpio(void);                /* optional; BOARD_CAP_GPS_PPS */

/* ── Misc peripherals ────────────────────────────────────────────────*/
void board_haptic_pulse(uint8_t strength_pct, uint16_t ms);  /* optional */

/* ── Meshy radio port ────────────────────────────────────────────────
 * Narrow SX126x-shaped port so meshtastic_task stops holding raw
 * Sx126x/Xl95x5 pointers.  Implemented by the T-Display BSP; absent
 * (compiled out with BOARD_CAP_LORA=0) elsewhere.  An external SX1262
 * module on the Waveshare GPIO header later = a BSP config, not a port. */
typedef struct board_lora_port board_lora_port_t;
const board_lora_port_t *board_lora(void);    /* NULL when absent */

#ifdef __cplusplus
}
#endif
