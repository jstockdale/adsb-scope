/**
 * wifi_hosted.h — WiFi connectivity via ESP-Hosted (C6 over SDIO).
 *
 * The ESP32-C6 runs the ESP-Hosted "network_adapter" slave firmware,
 * communicating with the P4 over SDMMC Slot 1 (GPIOs 14-19).
 * This module wraps the standard ESP-IDF WiFi APIs — esp_hosted +
 * esp_wifi_remote make the transport transparent.
 *
 * Architecture:
 *   - C6 is held in reset (XL9535_ESP32C6_EN = LOW) until wifi_hosted_init()
 *   - ESP-Hosted manages the SDIO transport automatically
 *   - Standard esp_wifi_* APIs work once initialized
 *   - SNTP time sync starts automatically on WiFi connect
 *
 * Prerequisites:
 *   - SD card must be on SPI (not SDMMC) to avoid DMA conflict (Issue #17889)
 *   - C6 must be pre-flashed with ESP-Hosted SPI slave firmware
 *   - idf_component.yml must include esp_hosted and esp_wifi_remote
 *
 * sdkconfig entries (add to sdkconfig.defaults):
 *   CONFIG_ESP_WIFI_REMOTE_ENABLED=y
 *   CONFIG_ESP_HOSTED_ENABLED=y
 *   CONFIG_ESP_HOSTED_TRANSPORT_SDIO=y
 *   CONFIG_ESP_HOSTED_SDIO_SLOT=1
 *   CONFIG_ESP_HOSTED_SDIO_CLK_GPIO=18
 *   CONFIG_ESP_HOSTED_SDIO_CMD_GPIO=19
 *   CONFIG_ESP_HOSTED_SDIO_D0_GPIO=14
 *   CONFIG_ESP_HOSTED_SDIO_D1_GPIO=15
 *   CONFIG_ESP_HOSTED_SDIO_D2_GPIO=16
 *   CONFIG_ESP_HOSTED_SDIO_D3_GPIO=17
 *
 * Part of ADS-B Scope — T-Display-P4.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "wifi_sup.h"          // which saved network, and when to try again
#include "wifi_hosted_api.h"   // what the console and the UI may ask

// Time manager — must be included before event handler uses esp_sntp / NTP functions
#include "time_manager.h"
#include "serial_console.h"  // for serial_console_print, log_format_timestamp

// XL9535 GPIO expander pin control (from your project)
// These must be provided by the caller or linked from main.cpp
extern "C" void xl9535_c6_enable(bool enable);   // drive XL9535_ESP32C6_EN
extern "C" void xl9535_c6_wakeup(void);           // pulse XL9535_ESP32C6_WAKE_UP

static const char *WIFI_TAG = "WIFI";

// ─── Configuration ───────────────────────────────────────────────────────────

// WiFi credentials — stored in NVS in production, hardcoded for dev
#ifndef WIFI_SSID
#define WIFI_SSID     ""       // set via menuconfig or NVS
#endif
#ifndef WIFI_PASS
#define WIFI_PASS     ""
#endif

// NTP config lives in time_manager.h / device_settings

// Event group bits
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

// Reconnecting is wifi_sup.c's business: it never gives up (rounds of scan +
// connect, waits of 1, 2, 4 ... 60 s between rounds that connect nowhere).
// Until coh6 this file retried 10 times with a growing delay inside the event
// handler - 55 s in all - and then stopped for good.

// ─── State ───────────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_event_group = NULL;
static volatile bool s_wifi_initialized = false;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_paused = false;
static esp_netif_t *s_sta_netif = NULL;

// WiFi credentials (runtime configurable)

// ─── Supervisor: the saved networks and the task that acts on wifi_sup's decisions ───
typedef enum { WE_START, WE_SCAN_DONE, WE_GOT_IP, WE_DISC, WE_RELOAD, WE_USE, WE_LINK_DEAD, WE_SCAN_REQ } wifi_ev_t;
typedef struct { uint8_t ev; int8_t arg; } wifi_msg_t;
#define WIFI_SCAN_MAX 24
static ws_t s_ws;                              // guarded by s_ws_mu
static ws_net_t s_nets[WS_MAX_NETS];           // the saved list as last given; guarded by s_ws_mu once it exists
static int s_n_nets = 0;
static bool s_ws_started = false;
static QueueHandle_t s_ws_q = NULL;
static SemaphoreHandle_t s_ws_mu = NULL;
static EXT_RAM_BSS_ATTR ws_ap_t s_scan[WIFI_SCAN_MAX];
static int s_scan_n = 0;
static uint32_t s_scan_seq = 0;
static int s_wifi_last_reason = 0;

extern "C" void wifi_hosted_pause(void);
extern "C" void wifi_hosted_resume(void);

static inline uint32_t wifi_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline void wifi_post(wifi_ev_t ev, int arg) {
    if (!s_ws_q) return;
    wifi_msg_t m = { (uint8_t)ev, (int8_t)arg };
    xQueueSend(s_ws_q, &m, 0);                 // never blocks: this is called from the event handler
}
#define WIFI_SAY(fmt, ...) do { char _ts[32]; log_format_timestamp(_ts, sizeof(_ts)); serial_console_print("%sWIFI: " fmt "\n", _ts, ##__VA_ARGS__); } while (0)

// Carry out one decision. Returns what follows from it when it fails on the spot.
static ws_cmd_t wifi_sup_exec(ws_cmd_t c) {
    ws_cmd_t next = { WA_NONE, -1 };
    switch (c.act) {
    case WA_SCAN: {
        wifi_scan_config_t sc = {};
        sc.show_hidden = true;
        if (esp_wifi_scan_start(&sc, false) != ESP_OK) {       // cannot scan: an empty scan, so that the round ends and is retried
            xSemaphoreTake(s_ws_mu, portMAX_DELAY);
            next = ws_on_scan(&s_ws, NULL, 0, wifi_now_ms());
            xSemaphoreGive(s_ws_mu);
        }
        break;
    }
    case WA_CONNECT: {
        ws_net_t net; int lvl;
        xSemaphoreTake(s_ws_mu, portMAX_DELAY);
        net = s_ws.nets[c.net]; lvl = s_ws.seen[c.net];
        xSemaphoreGive(s_ws_mu);
        wifi_config_t wc = {};
        strncpy((char *)wc.sta.ssid, net.ssid, sizeof(wc.sta.ssid));
        strncpy((char *)wc.sta.password, net.pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = net.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;   // no password: an open network
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (e == ESP_OK) e = esp_wifi_connect();
        if (lvl != WS_NOT_SEEN) WIFI_SAY("Connecting to %s (%d dBm)", net.ssid, lvl);
        else                    WIFI_SAY("Connecting to %s (not seen in the scan)", net.ssid);
        if (e != ESP_OK) {
            ESP_LOGW(WIFI_TAG, "connect to %s failed at once: %s", net.ssid, esp_err_to_name(e));
            xSemaphoreTake(s_ws_mu, portMAX_DELAY);
            next = ws_on_disconnected(&s_ws, wifi_now_ms());
            xSemaphoreGive(s_ws_mu);
        }
        break;
    }
    case WA_DISCONNECT:
        esp_wifi_disconnect();                 // the event that follows (or wifi_sup's 3 s) moves things on
        break;
    case WA_POWER_CYCLE:
        WIFI_SAY("Down for a long time - power-cycling the radio");
        wifi_hosted_pause();
        wifi_hosted_resume();                  // STA_START follows and starts the supervisor again
        break;
    default: break;
    }
    return next;
}

static void wifi_sup_task(void *arg) {
    static EXT_RAM_BSS_ATTR wifi_ap_record_t recs[WIFI_SCAN_MAX];
    for (;;) {
        wifi_msg_t m = { 0, 0 };
        ws_cmd_t c = { WA_NONE, -1 };
        bool got = xQueueReceive(s_ws_q, &m, pdMS_TO_TICKS(1000)) == pdTRUE;
        uint16_t n_recs = 0;
        if (got && m.ev == WE_SCAN_DONE) {
            n_recs = WIFI_SCAN_MAX;
            if (esp_wifi_scan_get_ap_records(&n_recs, recs) != ESP_OK) n_recs = 0;
        }
        bool want_scan = false;
        uint32_t now = wifi_now_ms();
        xSemaphoreTake(s_ws_mu, portMAX_DELAY);
        ws_state_t before = s_ws.state;
        uint32_t failed_before = s_ws.rounds_failed;
        if (!got) {
            if (s_ws_started && !s_wifi_paused) c = ws_tick(&s_ws, now);
        } else switch (m.ev) {
            case WE_START: {
                ws_t old = s_ws;
                ws_init(&s_ws, s_nets, s_n_nets);
                if (s_ws_started) { s_ws.cfg = old.cfg; s_ws.ups = old.ups; s_ws.losses = old.losses; }
                s_ws_started = true;
                c = ws_start(&s_ws, now);
                break;
            }
            case WE_SCAN_DONE:
                s_scan_n = n_recs;
                for (int i = 0; i < n_recs; i++) {
                    memset(&s_scan[i], 0, sizeof(s_scan[i]));
                    strncpy(s_scan[i].ssid, (const char *)recs[i].ssid, WS_SSID_MAX);
                    s_scan[i].rssi = recs[i].rssi;
                }
                s_scan_seq++;
                c = ws_on_scan(&s_ws, s_scan, s_scan_n, now);
                break;
            case WE_GOT_IP:    c = ws_on_got_ip(&s_ws, now); break;
            case WE_DISC:      c = ws_on_disconnected(&s_ws, now); break;
            case WE_RELOAD:    if (s_ws_started) c = ws_reload(&s_ws, s_nets, s_n_nets, now); break;
            case WE_USE:       c = ws_use(&s_ws, m.arg, now); break;
            case WE_LINK_DEAD: c = ws_link_dead(&s_ws, now); break;
            case WE_SCAN_REQ:  want_scan = (s_ws.state == WS_UP || s_ws.state == WS_WAITING); break;
        }
        ws_state_t after = s_ws.state;
        uint32_t failed = s_ws.rounds_failed, retry = ws_retry_in_ms(&s_ws, now);
        xSemaphoreGive(s_ws_mu);

        if (before == WS_UP && after != WS_UP)
            WIFI_SAY("Connection lost (reason %d) - looking for a saved network", s_wifi_last_reason);
        if (after == WS_WAITING && failed != failed_before && (failed == 1 || failed % 10 == 0))
            WIFI_SAY("No saved network could be joined - next try in %u s (round %u)", (unsigned)((retry + 500) / 1000), (unsigned)failed);
        if (want_scan) { wifi_scan_config_t sc = {}; sc.show_hidden = true; esp_wifi_scan_start(&sc, false); }
        while (c.act != WA_NONE) c = wifi_sup_exec(c);
    }
}

// ─── Event Handler ───────────────────────────────────────────────────────────

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(WIFI_TAG, "STA started, looking for a saved network...");
                wifi_post(WE_START, 0);
                break;

            case WIFI_EVENT_SCAN_DONE:
                wifi_post(WE_SCAN_DONE, 0);
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                s_wifi_connected = false;
                wifi_event_sta_disconnected_t *d =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(WIFI_TAG, "Disconnected (reason=%d)", d->reason);

                s_wifi_last_reason = d->reason;
                if (s_wifi_paused) break;      // switched off on purpose
                wifi_post(WE_DISC, 0);         // what happens next is the supervisor's decision; nothing waits in here
                break;
            }

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(WIFI_TAG, "Associated with AP");
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        wifi_post(WE_GOT_IP, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Log to serial console so webapp can see the IP
        char _ts[32]; log_format_timestamp(_ts, sizeof(_ts));
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        serial_console_print("%sWIFI: Connected, IP: %s\n", _ts, ip);

        // Start NTP on every (re)connect — time_manager handles dedup
        if (!esp_sntp_enabled()) {
            time_manager_start_ntp();
        }
    }
}

// ─── Time Manager Integration ────────────────────────────────────────────────
// NTP start/stop managed by time_manager.h (included at top of this file).

// ─── Public API ──────────────────────────────────────────────────────────────

/**
 * The saved networks, in priority order. May be called before init and at any
 * time after: the supervisor keeps the connection it has if that network is
 * still on the list, and looks again if not.
 */
extern "C" void wifi_hosted_set_networks(const ws_net_t *nets, int n) {
    if (n > WS_MAX_NETS) n = WS_MAX_NETS;
    if (s_ws_mu) xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    memset(s_nets, 0, sizeof(s_nets));
    for (int i = 0; i < n; i++) s_nets[i] = nets[i];
    s_n_nets = n;
    if (s_ws_mu) xSemaphoreGive(s_ws_mu);
    wifi_post(WE_RELOAD, 0);
}

/** One network, as before the list existed. */
static inline void wifi_hosted_set_credentials(const char *ssid, const char *password) {
    ws_net_t one[1]; int n = 0;
    memset(one, 0, sizeof(one));
    ws_list_add(one, &n, ssid, password);
    wifi_hosted_set_networks(one, n);
}

/**
 * Initialize WiFi via ESP-Hosted.
 * Powers on the C6, starts ESP-Hosted SDIO transport, connects to AP.
 *
 * Degrades gracefully if the C6 doesn't have ESP-Hosted firmware:
 *   - esp_wifi_init() runs on a separate task with a 3-second timeout
 *   - If the C6 doesn't respond on SDIO, returns ESP_ERR_TIMEOUT
 *   - All partial init is cleaned up (C6 powered off, netif destroyed)
 *   - Caller prints a message and continues without WiFi
 *
 * Returns ESP_OK on success, error code on failure.
 * Call AFTER SD card is mounted on SPI (not SDMMC).
 */

// ── esp_wifi_init timeout wrapper ────────────────────────────────────────────
// esp_wifi_init() establishes the ESP-Hosted SDIO transport to the C6.
// If the C6 has no ESP-Hosted firmware, this call hangs indefinitely
// waiting for the SDIO slave to respond.  We run it on a dedicated task
// and wait on a semaphore with a timeout to detect this case.
//
// The context and semaphore are static so the task can safely write to
// them even if we time out and return — avoids use-after-free on stack.

struct wifi_init_ctx_t {
    wifi_init_config_t cfg;
    esp_err_t result;
    SemaphoreHandle_t done;
};

static struct wifi_init_ctx_t s_wifi_init_ctx;

static void wifi_init_task_fn(void *arg) {
    struct wifi_init_ctx_t *ctx = (struct wifi_init_ctx_t *)arg;
    ctx->result = esp_wifi_init(&ctx->cfg);
    // Semaphore may have been taken by the caller already (timeout path).
    // xSemaphoreGive is safe regardless — worst case it goes to max count.
    if (ctx->done) xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

static esp_err_t wifi_init_with_timeout(wifi_init_config_t *cfg, uint32_t timeout_ms) {
    s_wifi_init_ctx.cfg = *cfg;
    s_wifi_init_ctx.result = ESP_ERR_TIMEOUT;
    s_wifi_init_ctx.done = xSemaphoreCreateBinary();
    if (!s_wifi_init_ctx.done) return ESP_ERR_NO_MEM;

    TaskHandle_t task = NULL;
    BaseType_t ret = xTaskCreateWithCaps(wifi_init_task_fn, "wifi_probe", 4096,
                                 &s_wifi_init_ctx, 5, &task, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        vSemaphoreDelete(s_wifi_init_ctx.done);
        s_wifi_init_ctx.done = NULL;
        return ESP_ERR_NO_MEM;
    }

    bool completed = (xSemaphoreTake(s_wifi_init_ctx.done,
                                     pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    if (!completed) {
        // Task is stuck in SDIO — leave it running.  SDIO Slot 1 is
        // dedicated to ESP-Hosted, so no resource conflict with SD card
        // (which uses SPI3).  The ~200B semaphore + task stack leak is
        // acceptable for a one-time error path.
        ESP_LOGE(WIFI_TAG, "esp_wifi_init timed out after %lums — "
                 "C6 not responding on SDIO (no ESP-Hosted firmware?)",
                 (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(s_wifi_init_ctx.done);
    s_wifi_init_ctx.done = NULL;
    return s_wifi_init_ctx.result;
}

static inline esp_err_t wifi_hosted_init(void) {
    if (s_wifi_initialized) {
        ESP_LOGW(WIFI_TAG, "Already initialized");
        return ESP_OK;
    }

    if (s_n_nets == 0) {
        ESP_LOGW(WIFI_TAG, "No SSID configured — skipping WiFi init");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(WIFI_TAG, "=== Initializing WiFi (ESP-Hosted over SDIO) ===");

    // ── 1. Power on the C6 ──
    ESP_LOGI(WIFI_TAG, "Enabling ESP32-C6...");
    xl9535_c6_enable(true);
    vTaskDelay(pdMS_TO_TICKS(100));  // C6 boot time

    // ── 2. Initialize networking stack ──
    // These are idempotent — Ethernet_Init() may have already called them.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(WIFI_TAG, "netif init failed: %s", esp_err_to_name(err));
        goto fail_c6;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(WIFI_TAG, "Event loop create failed: %s", esp_err_to_name(err));
        goto fail_c6;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // ── 3. Initialize WiFi (ESP-Hosted SDIO transport to C6) ──
    // This is the critical call that communicates with the C6.  If the C6
    // doesn't have ESP-Hosted firmware, this either returns an error or
    // hangs.  The timeout wrapper handles both cases.
    {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = wifi_init_with_timeout(&cfg, 3000);  // 3s timeout
        if (err != ESP_OK) {
            if (err == ESP_ERR_TIMEOUT) {
                ESP_LOGE(WIFI_TAG, "C6 does not have ESP-Hosted firmware — WiFi unavailable");
                ESP_LOGE(WIFI_TAG, "Flash ESP-Hosted network_adapter to C6 to enable WiFi");
            } else {
                ESP_LOGE(WIFI_TAG, "esp_wifi_init failed: %s — WiFi unavailable",
                         esp_err_to_name(err));
            }
            goto fail_netif;
        }
    }
    ESP_LOGI(WIFI_TAG, "ESP-Hosted transport established (C6 responding)");

    // ── 4. Register event handlers ──
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(WIFI_TAG, "Failed to create event group");
        err = ESP_ERR_NO_MEM;
        goto fail_wifi;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "WiFi event handler register failed: %s", esp_err_to_name(err));
        goto fail_event;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "IP event handler register failed: %s", esp_err_to_name(err));
        goto fail_event;
    }

    // ── 5. Configure and start ──
    {
        // The supervisor has to exist before the radio starts: STA_START hands over to it.
        if (!s_ws_mu) s_ws_mu = xSemaphoreCreateMutex();
        if (!s_ws_q)  s_ws_q = xQueueCreate(16, sizeof(wifi_msg_t));
        static bool sup_task_made = false;
        if (s_ws_mu && s_ws_q && !sup_task_made)
            sup_task_made = xTaskCreateWithCaps(wifi_sup_task, "wifi_sup", 6144, NULL, 2, NULL, MALLOC_CAP_SPIRAM) == pdPASS;
        if (!sup_task_made) {
            ESP_LOGE(WIFI_TAG, "WiFi supervisor could not be created");
            err = ESP_ERR_NO_MEM;
            goto fail_event;
        }

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(WIFI_TAG, "set_mode failed: %s", esp_err_to_name(err));
            goto fail_event;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(WIFI_TAG, "wifi_start failed: %s", esp_err_to_name(err));
            goto fail_event;
        }
    }

    s_wifi_initialized = true;
    s_wifi_paused = false;
    ESP_LOGI(WIFI_TAG, "WiFi started — connecting in background");

    // Non-blocking: connection happens asynchronously via event handler.
    // IP_EVENT → starts NTP. No 30s boot delay.
    // Caller can poll wifi_hosted_is_connected() if needed.
    return ESP_OK;

    // ── Cleanup on failure ─────────────────────────────────────────────────
    // Unwind partial initialization so the system can continue without WiFi.
    // Everything downstream (MQTT, NTP, OTA) gates on s_wifi_initialized or
    // s_wifi_has_ip, so leaving these false is sufficient.
fail_event:
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
fail_wifi:
    esp_wifi_deinit();
fail_netif:
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
fail_c6:
    xl9535_c6_enable(false);  // power off C6 — nothing to talk to
    ESP_LOGW(WIFI_TAG, "Continuing without WiFi — all other features remain functional");
    return err;
}

/**
 * Pause WiFi — graceful teardown for SD SDMMC mode swap.
 * Disconnects, stops WiFi, powers down C6.
 * Call wifi_hosted_resume() to restart.
 */
extern "C" void wifi_hosted_pause(void) {
    if (!s_wifi_initialized || s_wifi_paused) return;

    ESP_LOGI(WIFI_TAG, "Pausing WiFi...");
    s_wifi_paused = true;
    if (s_ws_mu) { xSemaphoreTake(s_ws_mu, portMAX_DELAY); ws_stop(&s_ws); xSemaphoreGive(s_ws_mu); }

    time_manager_stop_ntp();
    esp_wifi_disconnect();
    esp_wifi_stop();

    // Power down C6 to fully release SDIO bus
    xl9535_c6_enable(false);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_wifi_connected = false;
    ESP_LOGI(WIFI_TAG, "WiFi paused, C6 powered down");
}

/**
 * Resume WiFi after a pause.
 * Powers on C6 and reconnects.
 */
extern "C" void wifi_hosted_resume(void) {
    if (!s_wifi_initialized || !s_wifi_paused) return;

    ESP_LOGI(WIFI_TAG, "Resuming WiFi...");

    // Power on C6
    xl9535_c6_enable(true);
    vTaskDelay(pdMS_TO_TICKS(200));  // C6 boot time

    s_wifi_paused = false;

    esp_wifi_start();  // will trigger STA_START → connect
    ESP_LOGI(WIFI_TAG, "WiFi resuming, reconnecting...");
}

/**
 * Full WiFi shutdown — deinit everything.
 * After this, wifi_hosted_init() must be called again.
 */
static inline void wifi_hosted_deinit(void) {
    if (!s_wifi_initialized) return;

    ESP_LOGI(WIFI_TAG, "Deinitializing WiFi...");
    s_wifi_paused = true; // prevent reconnect attempts
    if (s_ws_mu) { xSemaphoreTake(s_ws_mu, portMAX_DELAY); ws_stop(&s_ws); xSemaphoreGive(s_ws_mu); }

    time_manager_stop_ntp();

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    xl9535_c6_enable(false);

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_wifi_initialized = false;
    s_wifi_connected = false;
    s_wifi_paused = false;
    ESP_LOGI(WIFI_TAG, "WiFi deinitialized");
}

/**
 * Check if WiFi is currently connected.
 */
extern "C" bool wifi_hosted_is_active(void) {
    return s_wifi_initialized && !s_wifi_paused;
}

static inline bool wifi_hosted_is_connected(void) {
    return s_wifi_connected;
}

/**
 * Get the current IP address as a string.
 * Returns "0.0.0.0" if not connected.
 */
static inline const char *wifi_hosted_get_ip(void) {
    static char ip_str[16] = "0.0.0.0";
    if (s_sta_netif && s_wifi_connected) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    return ip_str;
}

// ─── For the console and the UI (wifi_hosted_api.h) ─────────────────────────

extern "C" uint8_t wifi_hosted_ui_state(void) {
    if (!s_wifi_initialized || s_wifi_paused) return 0;
    return s_wifi_connected ? 2 : 1;
}

extern "C" void wifi_hosted_get_status(wifi_hosted_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->cur = -1;
    for (int i = 0; i < WS_MAX_NETS; i++) out->seen[i] = WS_NOT_SEEN;
    out->active = s_wifi_initialized && !s_wifi_paused;
    out->n = s_n_nets;
    strcpy(out->ip, "0.0.0.0");
    if (!s_ws_mu) return;
    uint32_t now = wifi_now_ms();
    xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    out->state = (uint8_t)s_ws.state;
    out->n = s_ws_started ? s_ws.n : s_n_nets;
    out->cur = s_ws.cur;
    if (s_ws.cur >= 0 && s_ws.cur < s_ws.n) strcpy(out->ssid, s_ws.nets[s_ws.cur].ssid);
    out->retry_in_s = (ws_retry_in_ms(&s_ws, now) + 500) / 1000;
    out->rounds_failed = s_ws.rounds_failed;
    out->ups = s_ws.ups; out->losses = s_ws.losses;
    memcpy(out->seen, s_ws.seen, sizeof(out->seen));
    xSemaphoreGive(s_ws_mu);
    if (out->active && s_wifi_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
        strncpy(out->ip, wifi_hosted_get_ip(), sizeof(out->ip) - 1);
    }
}

extern "C" void wifi_hosted_use(int idx)    { wifi_post(WE_USE, idx); }
extern "C" void wifi_hosted_link_dead(void) { wifi_post(WE_LINK_DEAD, 0); }
extern "C" bool wifi_hosted_scan_request(void) {
    if (!s_wifi_initialized || s_wifi_paused || !s_ws_q) return false;
    wifi_post(WE_SCAN_REQ, 0);
    return true;
}
extern "C" int wifi_hosted_scan_results(ws_ap_t *out, int max, uint32_t *seq) {
    if (!s_ws_mu) { if (seq) *seq = 0; return 0; }
    xSemaphoreTake(s_ws_mu, portMAX_DELAY);
    int n = s_scan_n < max ? s_scan_n : max;
    for (int i = 0; i < n; i++) out[i] = s_scan[i];
    if (seq) *seq = s_scan_seq;
    xSemaphoreGive(s_ws_mu);
    return n;
}
extern "C" void wifi_hosted_cycle(void) {
    if (!s_wifi_initialized) return;
    if (!s_wifi_paused) wifi_hosted_pause();
    wifi_hosted_resume();
}
