// mqtt_feeder.c — MQTT publisher for ADS-B aircraft data
//
// See mqtt_feeder.h for API documentation.

#include "mqtt_feeder.h"
#include "mqtt_batch.h"
#include "device_settings.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "MQTT"

// ISRG Root X1 — root CA for Let's Encrypt certificates.
// Valid until 2035-06-04. PEM format, null-terminated.
// Source: https://letsencrypt.org/certs/isrgrootx1.pem
// Non-static: shared with ota_updater.c for HTTPS downloads.
const char ISRG_ROOT_X1_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

// ─── Configuration ───────────────────────────────────────────────────────────

#define BATCH_INTERVAL_MS   2000    // flush aircraft batch every 2s
#define STATUS_INTERVAL_MS  30000   // status heartbeat every 30s
#define TASK_POLL_MS        250     // feeder task wake-up period
#define TASK_STACK_SIZE     8192
#define TASK_PRIORITY       3
#define SLOW_PUBLISH_MS     1000    // a publish blocking this long is counted and logged

// MQTT client buffers, and the size of one aircraft message. A batch is
// split into as many messages as it needs (mqtt_batch.h), each of which
// fits the client's output buffer whole – header and topic included – and
// therefore one TLS record (CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN, 4096) and
// the TCP send buffer (CONFIG_LWIP_TCP_SND_BUF_DEFAULT, 4096). Nothing is
// fragmented by the client, and a publish does not have to wait for the far
// end to acknowledge half a message before it can send the rest.
#define MQTT_BUF_SIZE       4096
#define MSG_BUF_SIZE        3800    // JSON text of one message, NUL included
// fixed header (<= 4) + topic length (2) + topic ("adsb/" + id of up to 32
// characters + "/aircraft" = 46) + payload must fit; 85 more bytes cover
// the largest TLS 1.2 record overhead.
_Static_assert(MSG_BUF_SIZE + 4 + 2 + 46 + 85 <= MQTT_BUF_SIZE, "aircraft message must fit one buffer");
_Static_assert(MSG_BUF_SIZE >= MQTT_BATCH_MSG_MIN, "aircraft message buffer too small");

// ─── State ───────────────────────────────────────────────────────────────────

static esp_mqtt_client_handle_t s_client   = NULL;
static volatile bool            s_connected = false;
static TaskHandle_t             s_task      = NULL;

// Aircraft batch – PSRAM-allocated at init.
//   s_batch    latest state of each aircraft heard since the last flush;
//              written by the decoder, emptied by the feeder task, both
//              under s_ac_mutex
//   s_snap     the feeder task's private copy of it while it publishes
//   s_msg_buf  one JSON message
static mqtt_batch_t     *s_batch    = NULL;
static mqtt_aircraft_t  *s_snap     = NULL;
static char             *s_msg_buf  = NULL;
static SemaphoreHandle_t s_ac_mutex = NULL;

// Task shutdown handshake (mqtt_feeder_stop)
static volatile bool     s_task_stop = false;
static SemaphoreHandle_t s_task_done = NULL;

// Counters. s_stats is written under s_stats_lock (a spinlock: the
// sections are a few instructions long); the batch counters live in
// s_batch and are added to the s_base_* totals when it is freed.
static portMUX_TYPE        s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static mqtt_feeder_stats_t s_stats      = {0};
static volatile uint32_t   s_dropped_busy = 0;
static uint32_t s_base_updates = 0, s_base_coalesced = 0, s_base_dropped_full = 0;
static int64_t  s_connected_since_us = 0;   // 0 = not connected
static int64_t  s_last_drop_us       = 0;   // 0 = never
static int64_t  s_last_error_us      = 0;   // 0 = never

// Receiver status — updated externally, read by feeder task
static mqtt_status_t     s_status       = {0};
static SemaphoreHandle_t s_status_mutex = NULL;

// Device identity
static char s_device_id[33] = {0};

// ─── Reconnect State ────────────────────────────────────────────────────────
// Manual reconnect: immediate → 5s × 2 → 30s thereafter.
// WiFi down → paused (no retries). WiFi up → resume.
#define RETRY_FAST_MS       5000    // first 3 retries: 5s apart
#define RETRY_SLOW_MS       30000   // after 3 failures: 30s apart
#define RETRY_FAST_COUNT    3       // number of fast retries before switching to slow

static int                  s_retry_count = 0;
static esp_timer_handle_t   s_retry_timer = NULL;
static volatile bool        s_paused      = false;

// WiFi connectivity check — defined in main.cpp
extern bool wifi_hosted_is_connected_fn(void);

static void retry_timer_cb(void *arg) {
    if (s_paused || !s_client) return;
    ESP_LOGI(TAG, "Reconnect attempt %d", s_retry_count + 1);
    esp_mqtt_client_reconnect(s_client);
}

static void schedule_retry(void) {
    if (s_paused || !s_client || !s_retry_timer) return;

    // First attempt: immediate. No timer needed.
    if (s_retry_count == 0) {
        s_retry_count++;
        ESP_LOGI(TAG, "Reconnecting immediately (attempt 1)");
        esp_mqtt_client_reconnect(s_client);
        return;
    }

    uint32_t delay_ms = (s_retry_count < RETRY_FAST_COUNT) ? RETRY_FAST_MS : RETRY_SLOW_MS;
    s_retry_count++;

    ESP_LOGI(TAG, "Scheduling reconnect in %lums (attempt %d)",
             (unsigned long)delay_ms, s_retry_count);
    esp_timer_stop(s_retry_timer);  // cancel any pending
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void cancel_retry(void) {
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
}

// ─── PSRAM-backed cJSON allocator ────────────────────────────────────────────
// cJSON uses malloc/free internally for building JSON trees.
// Redirect to PSRAM so JSON serialization doesn't consume internal RAM.

static void *cjson_psram_malloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(size);  // fall back to internal if PSRAM exhausted
    return p;
}

static void cjson_psram_free(void *ptr) {
    free(ptr);  // heap_caps_malloc'd memory can be freed with free()
}

static void install_cjson_hooks(void) {
    static bool installed = false;
    if (installed) return;
    cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = cjson_psram_free };
    cJSON_InitHooks(&hooks);
    installed = true;
}

// ─── Forward declarations ────────────────────────────────────────────────────

static void feeder_task(void *arg);
static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
static void handle_admin_cmd(const char *cmd);
static bool pub_batch(void);
static void pub_status(void);
static void free_batch_buffers(void);
static void note_connection_lost(bool wifi);
static void pub_meta(void);

// ─── Device ID ───────────────────────────────────────────────────────────────

static void ensure_device_id(void) {
    extern device_settings_t g_settings;
    if (g_settings.mqtt_device_id[0] != '\0') {
        strncpy(s_device_id, g_settings.mqtt_device_id, sizeof(s_device_id) - 1);
        s_device_id[sizeof(s_device_id) - 1] = '\0';
        return;
    }
    // Use the P4's own eFuse base MAC — stable, per-chip, always available.
    // NOT ESP_MAC_WIFI_STA: on ESP-Hosted platforms the WiFi STA MAC belongs
    // to the C6 coprocessor and may differ from the P4's eFuse, or return
    // inconsistent values depending on whether ESP-Hosted has initialized.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_device_id, sizeof(s_device_id),
             "p4-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ─── Public API ──────────────────────────────────────────────────────────────

const char *mqtt_feeder_device_id(void) {
    ensure_device_id();
    return s_device_id;
}

void mqtt_feeder_init(void) {
    extern device_settings_t g_settings;

    if (!g_settings.mqtt_enabled) {
        ESP_LOGI(TAG, "MQTT feeder disabled in settings");
        return;
    }
    if (g_settings.mqtt_server[0] == '\0') {
        ESP_LOGI(TAG, "MQTT server not configured — skipping");
        return;
    }

    // Install PSRAM-backed cJSON hooks before any JSON work
    install_cjson_hooks();

    ensure_device_id();  // already called by mqtt_feeder_device_id(), but idempotent

    if (s_client) {
        ESP_LOGW(TAG, "Feeder already running – init ignored");
        return;
    }

    // Create mutexes if not already created (survive stop/reinit cycles)
    if (!s_ac_mutex) s_ac_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) s_status_mutex = xSemaphoreCreateMutex();
    if (!s_task_done) s_task_done = xSemaphoreCreateBinary();
    if (!s_ac_mutex || !s_status_mutex || !s_task_done) {
        ESP_LOGE(TAG, "Failed to create mutexes — MQTT disabled");
        return;
    }

    // Allocate the batch, its snapshot copy and the message buffer in PSRAM,
    // once: nothing is allocated per batch.
    mqtt_batch_t    *batch = (mqtt_batch_t *)heap_caps_malloc(sizeof(mqtt_batch_t), MALLOC_CAP_SPIRAM);
    mqtt_aircraft_t *snap  = (mqtt_aircraft_t *)heap_caps_malloc(
        sizeof(mqtt_aircraft_t) * MQTT_BATCH_SLOTS, MALLOC_CAP_SPIRAM);
    char            *msg   = (char *)heap_caps_malloc(MSG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!batch || !snap || !msg) {
        ESP_LOGE(TAG, "Failed to allocate aircraft buffer in PSRAM — MQTT disabled");
        heap_caps_free(batch);
        heap_caps_free(snap);
        heap_caps_free(msg);
        return;
    }
    mqtt_batch_init(batch);
    xSemaphoreTake(s_ac_mutex, portMAX_DELAY);
    s_batch   = batch;
    s_snap    = snap;
    s_msg_buf = msg;
    xSemaphoreGive(s_ac_mutex);

    // Build broker URI
    char uri[192];
    snprintf(uri, sizeof(uri), "mqtts://%s:%u",
             g_settings.mqtt_server, g_settings.mqtt_port);

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    cfg.broker.verification.certificate = ISRG_ROOT_X1_PEM;
    cfg.credentials.client_id = s_device_id;
    cfg.credentials.username = g_settings.mqtt_user[0] ? g_settings.mqtt_user : NULL;
    cfg.credentials.authentication.password = g_settings.mqtt_pass[0] ? g_settings.mqtt_pass : NULL;
    cfg.session.keepalive = 60;
    cfg.network.disable_auto_reconnect = true;  // we handle reconnect manually
    cfg.buffer.size = MQTT_BUF_SIZE;      // PSRAM via malloc (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0)
    cfg.buffer.out_size = MQTT_BUF_SIZE;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client — MQTT disabled");
        free_batch_buffers();
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, event_handler, NULL);

    // Pre-allocate retry timer to avoid internal RAM allocation during disconnect
    if (!s_retry_timer) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = retry_timer_cb;
        timer_args.name = "mqtt_retry";
        esp_timer_create(&timer_args, &s_retry_timer);
    }

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client (0x%x) — MQTT disabled", err);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        free_batch_buffers();
        return;
    }

    // Create feeder task with PSRAM stack
    s_task_stop = false;
    xSemaphoreTake(s_task_done, 0);  // clear a stale completion signal
    BaseType_t ret = xTaskCreateWithCaps(
        feeder_task, "mqtt_feeder",
        TASK_STACK_SIZE, NULL,
        TASK_PRIORITY, &s_task,
        MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create feeder task — MQTT disabled");
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_task = NULL;
        free_batch_buffers();
        return;
    }

    ESP_LOGI(TAG, "Feeder started → %s", uri);
}

void mqtt_feeder_update_aircraft(const mqtt_aircraft_t *ac) {
    // Fast path: bail immediately if not connected or not initialized
    if (!s_connected || !s_batch || !s_ac_mutex) return;

    if (xSemaphoreTake(s_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        // Latest state per aircraft: a later message from the same aircraft
        // replaces this one, so the batch size follows the number of
        // aircraft, not the message rate. (s_batch re-checked under the
        // lock: mqtt_feeder_stop() frees it under the same lock.)
        if (s_batch) mqtt_batch_put(s_batch, ac);
        xSemaphoreGive(s_ac_mutex);
    } else {
        // Mutex contention – never block the decode loop. The feeder holds
        // the lock only to copy the batch, so this should stay at zero.
        s_dropped_busy++;
    }
}

void mqtt_feeder_update_status(const mqtt_status_t *st) {
    if (!s_status_mutex) return;

    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        s_status = *st;
        xSemaphoreGive(s_status_mutex);
    }
}

// Free the batch buffers. The decoder may be inside
// mqtt_feeder_update_aircraft(), so the pointers are cleared under its lock.
static void free_batch_buffers(void) {
    mqtt_batch_t *batch = NULL;
    mqtt_aircraft_t *snap = NULL;
    char *msg = NULL;
    if (s_ac_mutex) xSemaphoreTake(s_ac_mutex, portMAX_DELAY);
    batch = s_batch;   s_batch = NULL;
    snap  = s_snap;    s_snap = NULL;
    msg   = s_msg_buf; s_msg_buf = NULL;
    if (batch) {  // keep its counters: they run from boot
        s_base_updates      += batch->updates;
        s_base_coalesced    += batch->coalesced;
        s_base_dropped_full += batch->dropped_full;
    }
    if (s_ac_mutex) xSemaphoreGive(s_ac_mutex);
    heap_caps_free(batch);
    heap_caps_free(snap);
    heap_caps_free(msg);
}

void mqtt_feeder_stop(void) {
    if (s_task) {
        // Ask the task to finish and wait for it, instead of deleting it
        // wherever it happens to be: deleted in the middle of a publish it
        // would take the MQTT client's lock with it, and the client could
        // then never be stopped. A publish can block for the client's 10 s
        // network timeout, hence the long wait.
        s_task_stop = true;
        xTaskNotifyGive(s_task);
        if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ESP_LOGW(TAG, "Feeder task did not finish in 15 s – deleting it");
        }
        // The task was created with a PSRAM stack: this call frees the
        // stack and task block, which plain vTaskDelete() would leak.
        vTaskDeleteWithCaps(s_task);
        s_task = NULL;
    }
    if (s_client) {
        cancel_retry();
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_connected) note_connection_lost(false);
    s_connected = false;
    free_batch_buffers();
    ESP_LOGI(TAG, "Feeder stopped");
}

// Bookkeeping when an established connection ends.
static void note_connection_lost(bool wifi) {
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_stats_lock);
    if (wifi) s_stats.wifi_pauses++; else s_stats.disconnects++;
    s_stats.last_drop_up_s = s_connected_since_us ? (uint32_t)((now - s_connected_since_us) / 1000000LL) : 0;
    s_last_drop_us = now;
    s_connected_since_us = 0;
    portEXIT_CRITICAL(&s_stats_lock);
}

void mqtt_feeder_get_stats(mqtt_feeder_stats_t *out) {
    if (!out) return;
    int64_t now = esp_timer_get_time();
    uint32_t updates = 0, coalesced = 0, dropped_full = 0;
    if (s_ac_mutex && xSemaphoreTake(s_ac_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        updates = s_base_updates; coalesced = s_base_coalesced; dropped_full = s_base_dropped_full;
        if (s_batch) {
            updates += s_batch->updates;
            coalesced += s_batch->coalesced;
            dropped_full += s_batch->dropped_full;
        }
        xSemaphoreGive(s_ac_mutex);
    }
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    out->connected_s      = s_connected_since_us ? (uint32_t)((now - s_connected_since_us) / 1000000LL) : 0;
    out->last_drop_ago_s  = s_last_drop_us  ? (uint32_t)((now - s_last_drop_us) / 1000000LL) : 0;
    out->last_error_ago_s = s_last_error_us ? (uint32_t)((now - s_last_error_us) / 1000000LL) : 0;
    portEXIT_CRITICAL(&s_stats_lock);
    out->updates = updates;
    out->coalesced = coalesced;
    out->dropped_full = dropped_full;
    out->dropped_busy = s_dropped_busy;
}

bool mqtt_feeder_is_connected(void) {
    return s_connected;
}

void mqtt_feeder_pause(void) {
    if (!s_client) return;
    s_paused = true;
    cancel_retry();
    bool was_connected = s_connected;
    s_connected = false;  // stop offering work to a client that is going away
    esp_mqtt_client_stop(s_client);
    if (was_connected) note_connection_lost(true);
    ESP_LOGI(TAG, "Paused (WiFi down)");
}

void mqtt_feeder_resume(void) {
    if (!s_client) return;
    s_paused = false;
    s_retry_count = 0;
    ESP_LOGI(TAG, "Resuming (WiFi up)");
    esp_mqtt_client_start(s_client);
}

// ─── Event Handler ───────────────────────────────────────────────────────────

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            portENTER_CRITICAL(&s_stats_lock);
            s_stats.connects++;
            s_connected_since_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_stats_lock);
            s_connected = true;
            s_retry_count = 0;
            cancel_retry();
            ESP_LOGI(TAG, "Connected to broker");
            pub_meta();
            // Subscribe to admin commands
            {
                char cmd_topic[64];
                snprintf(cmd_topic, sizeof(cmd_topic), "adsb/%s/cmd", s_device_id);
                esp_mqtt_client_subscribe(s_client, cmd_topic, 1);
                ESP_LOGI(TAG, "Subscribed to %s", cmd_topic);
            }
            // Check for pending rollback ack
            {
                extern bool ota_get_pending_rollback(char *, size_t, char *, size_t);
                extern void ota_clear_pending_rollback(void);
                char run_ver[32], fail_ver[32];
                if (ota_get_pending_rollback(run_ver, sizeof(run_ver),
                                              fail_ver, sizeof(fail_ver))) {
                    mqtt_feeder_publish_ack("firmware_update", "rollback",
                                            fail_ver, run_ver);
                    ota_clear_pending_rollback();
                }
            }
            break;
        case MQTT_EVENT_DISCONNECTED: {
            // The client reports this both when an established connection
            // ends and when a connection attempt fails.
            bool was_connected = s_connected;
            s_connected = false;
            if (was_connected) {
                note_connection_lost(false);
            } else {
                portENTER_CRITICAL(&s_stats_lock);
                s_stats.connect_failures++;
                portEXIT_CRITICAL(&s_stats_lock);
            }
            if (s_paused) {
                ESP_LOGI(TAG, "Disconnected (WiFi down — waiting for reconnect)");
            } else {
                ESP_LOGW(TAG, "Disconnected from broker");
                if (was_connected) {
                    // What the link was doing when it went: enough to tell a
                    // stalled network from a broker that hung up.
                    mqtt_feeder_stats_t st;
                    mqtt_feeder_get_stats(&st);
                    ESP_LOGW(TAG, "Link was up %lu s. Totals: %lu disconnects, %lu publish failures, "
                                  "longest publish %lu ms, last batch %lu aircraft in %lu msgs (%lu B)",
                             (unsigned long)st.last_drop_up_s, (unsigned long)st.disconnects,
                             (unsigned long)st.publish_failures, (unsigned long)st.publish_max_ms,
                             (unsigned long)st.last_aircraft, (unsigned long)st.last_messages,
                             (unsigned long)st.last_bytes);
                }
                schedule_retry();
            }
            break;
        }
        case MQTT_EVENT_DATA:
            // Incoming message — route admin commands
            if (event->topic && event->topic_len > 0 && event->data && event->data_len > 0) {
                // Check if topic ends with "/cmd"
                char topic_buf[96];
                int tlen = event->topic_len < (int)sizeof(topic_buf) - 1
                           ? event->topic_len : (int)sizeof(topic_buf) - 1;
                memcpy(topic_buf, event->topic, tlen);
                topic_buf[tlen] = '\0';

                char expected_cmd[64];
                snprintf(expected_cmd, sizeof(expected_cmd), "adsb/%s/cmd", s_device_id);

                if (strcmp(topic_buf, expected_cmd) == 0) {
                    // Parse JSON command
                    char data_buf[256];
                    int dlen = event->data_len < (int)sizeof(data_buf) - 1
                               ? event->data_len : (int)sizeof(data_buf) - 1;
                    memcpy(data_buf, event->data, dlen);
                    data_buf[dlen] = '\0';

                    cJSON *root = cJSON_Parse(data_buf);
                    if (root) {
                        cJSON *jcmd = cJSON_GetObjectItem(root, "cmd");
                        if (jcmd && cJSON_IsString(jcmd)) {
                            handle_admin_cmd(jcmd->valuestring);
                        }
                        cJSON_Delete(root);
                    }
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            if (event->error_handle) {
                const esp_mqtt_error_codes_t *e = event->error_handle;
                portENTER_CRITICAL(&s_stats_lock);
                s_stats.last_error_type    = (int)e->error_type;
                s_stats.last_sock_errno    = e->esp_transport_sock_errno;
                s_stats.last_tls_err       = (int)e->esp_tls_last_esp_err;
                s_stats.last_tls_stack_err = e->esp_tls_stack_err;
                s_stats.last_connect_rc    = (int)e->connect_return_code;
                s_last_error_us            = esp_timer_get_time();
                portEXIT_CRITICAL(&s_stats_lock);
                if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                    ESP_LOGE(TAG, "Transport error: %s",
                             strerror(e->esp_transport_sock_errno));
                    ESP_LOGE(TAG, "Transport detail: errno %d, esp-tls 0x%x, TLS stack -0x%04x",
                             e->esp_transport_sock_errno, (unsigned)e->esp_tls_last_esp_err,
                             (unsigned)(e->esp_tls_stack_err < 0 ? -e->esp_tls_stack_err : e->esp_tls_stack_err));
                } else if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    ESP_LOGE(TAG, "Connection refused by broker: CONNACK code %d",
                             (int)e->connect_return_code);
                }
            }
            break;
        default:
            break;
    }
}

// ─── Feeder Task ─────────────────────────────────────────────────────────────

static void feeder_task(void *arg) {
    int64_t last_batch  = esp_timer_get_time();
    int64_t last_status = esp_timer_get_time();
    bool    was_connected = false;

    while (!s_task_stop) {
        // Periodic wake-up; mqtt_feeder_stop() cuts the wait short.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TASK_POLL_MS));
        if (s_task_stop) break;

        int64_t now = esp_timer_get_time();

        if (!s_connected) {
            was_connected = false;
            continue;
        }
        if (!was_connected) {
            // New connection. Whatever was collected before the link went
            // down is stale: discard it and start a fresh interval.
            was_connected = true;
            if (xSemaphoreTake(s_ac_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (s_batch) mqtt_batch_reset(s_batch);
                xSemaphoreGive(s_ac_mutex);
            }
            last_batch = now;
            continue;
        }

        // Aircraft batch
        if ((now - last_batch) >= (BATCH_INTERVAL_MS * 1000LL)) {
            uint32_t gap_ms = (uint32_t)((now - last_batch) / 1000);
            if (pub_batch()) {
                portENTER_CRITICAL(&s_stats_lock);
                if (gap_ms > s_stats.flush_gap_max_ms) s_stats.flush_gap_max_ms = gap_ms;
                portEXIT_CRITICAL(&s_stats_lock);
                last_batch = now;
            }
            // else: the batch lock was busy; try again at the next wake-up
        }

        // Status heartbeat
        if ((now - last_status) >= (STATUS_INTERVAL_MS * 1000LL)) {
            pub_status();
            last_status = now;
        }
    }

    // Tell mqtt_feeder_stop() the task is out of the client and holds
    // nothing, then wait to be deleted.
    xSemaphoreGive(s_task_done);
    vTaskSuspend(NULL);
}

// ─── Publishers ──────────────────────────────────────────────────────────────

// Publish everything collected since the last batch. Returns false only if
// the batch could not be taken (lock busy), in which case nothing changed.
static bool pub_batch(void) {
    if (!s_batch || !s_snap || !s_msg_buf) return true;

    // Take the collected aircraft and clear the slots under the lock; the
    // JSON work and the network writes below happen without it, so the
    // decode loop is never held up by the network.
    if (xSemaphoreTake(s_ac_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    int count = mqtt_batch_take(s_batch, s_snap, MQTT_BATCH_SLOTS);
    xSemaphoreGive(s_ac_mutex);
    if (count == 0) return true;

    // Receiver position, if known
    bool have_rx = false;
    double rx_lat = 0.0, rx_lon = 0.0;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        rx_lat = s_status.lat;
        rx_lon = s_status.lon;
        have_rx = (rx_lat != 0.0 || rx_lon != 0.0);
        xSemaphoreGive(s_status_mutex);
    }

    char topic[64];
    snprintf(topic, sizeof(topic), "adsb/%s/aircraft", s_device_id);

    int next = 0, sent = 0, messages = 0, failures = 0, slow = 0;
    uint32_t bytes = 0, max_ms = 0;
    while (next < count) {
        int first = next;
        size_t len = mqtt_batch_serialise(s_snap, count, &next, have_rx, rx_lat, rx_lon,
                                          s_msg_buf, MSG_BUF_SIZE);
        if (len == 0) break;
        if (!s_connected || s_task_stop) { next = first; break; }

        int64_t t0 = esp_timer_get_time();
        int ret = esp_mqtt_client_publish(s_client, topic, s_msg_buf, (int)len, 0, 0);  // QoS 0
        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (ms > max_ms) max_ms = ms;
        if (ms >= SLOW_PUBLISH_MS) {
            slow++;
            ESP_LOGW(TAG, "Slow publish: %lu ms for %u bytes", (unsigned long)ms, (unsigned)len);
        }
        if (ret < 0) {
            // Not connected any more, or the write failed and the client
            // has dropped the connection. The rest would fail the same way.
            failures++;
            next = first;
            ESP_LOGW(TAG, "Publish failed after %lu ms – %d of %d aircraft not sent",
                     (unsigned long)ms, count - first, count);
            break;
        }
        sent += next - first;
        messages++;
        bytes += (uint32_t)len;
    }

    portENTER_CRITICAL(&s_stats_lock);
    s_stats.flushes++;
    s_stats.messages         += (uint32_t)messages;
    s_stats.bytes            += bytes;
    s_stats.aircraft_sent    += (uint32_t)sent;
    s_stats.aircraft_unsent  += (uint32_t)(count - sent);
    s_stats.publish_failures += (uint32_t)failures;
    s_stats.publish_slow     += (uint32_t)slow;
    if (max_ms > s_stats.publish_max_ms) s_stats.publish_max_ms = max_ms;
    s_stats.last_aircraft = (uint32_t)count;
    s_stats.last_messages = (uint32_t)messages;
    s_stats.last_bytes    = bytes;
    portEXIT_CRITICAL(&s_stats_lock);
    return true;
}

static void pub_status(void) {
    mqtt_status_t st;
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        st = s_status;
        xSemaphoreGive(s_status_mutex);
    } else {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    if (st.lat != 0.0 || st.lon != 0.0) {
        cJSON_AddNumberToObject(root, "lat", st.lat);
        cJSON_AddNumberToObject(root, "lon", st.lon);
    }
    cJSON_AddNumberToObject(root, "sats", st.sats);
    cJSON_AddNumberToObject(root, "hdop", st.hdop);
    cJSON_AddNumberToObject(root, "gain_db", st.gain_db);
    cJSON_AddStringToObject(root, "gain_phase",
        st.gain_phase == 1 ? "converging" : (st.gain_phase == 2 ? "steady" : "manual"));
    cJSON_AddNumberToObject(root, "error_rate", st.error_rate);
    cJSON_AddNumberToObject(root, "pos_rate", st.pos_rate);
    cJSON_AddNumberToObject(root, "max_range_nm", st.max_range_nm);
    cJSON_AddNumberToObject(root, "ac_count", st.ac_count);
    cJSON_AddNumberToObject(root, "msg_count", st.msg_count);
    cJSON_AddNumberToObject(root, "msg_rate", st.msg_rate);
    cJSON_AddNumberToObject(root, "mem_free", st.mem_free);
    cJSON_AddNumberToObject(root, "mem_psram", st.mem_psram);
    cJSON_AddNumberToObject(root, "uptime_s", st.uptime_s);
    if (st.firmware_ver[0])
        cJSON_AddStringToObject(root, "firmware_ver", st.firmware_ver);
    if (st.dongle_model[0])
        cJSON_AddStringToObject(root, "dongle_model", st.dongle_model);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        char topic[64];
        snprintf(topic, sizeof(topic), "adsb/%s/status", s_device_id);
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 0);  // QoS 1
        free(json);
    }
}

static void pub_meta(void) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "client_type", "firmware");
    cJSON_AddStringToObject(root, "platform", "esp32-p4");

    if (s_status.firmware_ver[0])
        cJSON_AddStringToObject(root, "firmware_ver", s_status.firmware_ver);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        char topic[64];
        snprintf(topic, sizeof(topic), "adsb/%s/meta", s_device_id);
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);  // QoS 1, retained
        free(json);
    }

    ESP_LOGI(TAG, "Published device meta (retained)");
}

// ─── Admin Commands ──────────────────────────────────────────────────────────

static void handle_admin_cmd(const char *cmd) {
    extern esp_err_t ota_download_start(bool force);
    extern esp_err_t ota_flash_from_sd(bool force);

    ESP_LOGI(TAG, "Admin command: %s", cmd);

    if (strcmp(cmd, "firmware_download") == 0) {
        esp_err_t err = ota_download_start(false);
        if (err == ESP_ERR_INVALID_STATE) {
            mqtt_feeder_publish_ack(cmd, "error", "already downloading", NULL);
        } else if (err != ESP_OK) {
            mqtt_feeder_publish_ack(cmd, "error", "download start failed", NULL);
        }
        // Success ack is published by the download task when it completes

    } else if (strcmp(cmd, "firmware_download_force") == 0) {
        esp_err_t err = ota_download_start(true);
        if (err == ESP_ERR_INVALID_STATE) {
            mqtt_feeder_publish_ack(cmd, "error", "already downloading", NULL);
        } else if (err != ESP_OK) {
            mqtt_feeder_publish_ack(cmd, "error", "download start failed", NULL);
        }

    } else if (strcmp(cmd, "firmware_update") == 0) {
        mqtt_feeder_publish_ack(cmd, "starting", NULL, NULL);
        // ota_flash_from_sd reboots on success — ack won't be sent on failure
        esp_err_t err = ota_flash_from_sd(false);
        if (err == ESP_ERR_NOT_FOUND) {
            mqtt_feeder_publish_ack(cmd, "error", "no firmware on SD card", NULL);
        } else if (err == ESP_ERR_INVALID_STATE) {
            mqtt_feeder_publish_ack(cmd, "error", "already running this version", NULL);
        } else if (err == ESP_ERR_INVALID_CRC) {
            mqtt_feeder_publish_ack(cmd, "error", "SHA256 verification failed", NULL);
        } else if (err != ESP_OK) {
            mqtt_feeder_publish_ack(cmd, "error", "flash failed", NULL);
        }

    } else if (strcmp(cmd, "firmware_update_force") == 0) {
        mqtt_feeder_publish_ack(cmd, "starting", NULL, NULL);
        esp_err_t err = ota_flash_from_sd(true);
        if (err != ESP_OK) {
            char reason[48];
            snprintf(reason, sizeof(reason), "flash failed: %s", esp_err_to_name(err));
            mqtt_feeder_publish_ack(cmd, "error", reason, NULL);
        }

    } else {
        ESP_LOGW(TAG, "Unknown admin command: %s", cmd);
        mqtt_feeder_publish_ack(cmd, "error", "unknown command", NULL);
    }
}

void mqtt_feeder_publish_ack(const char *cmd, const char *status,
                              const char *reason, const char *version) {
    if (!s_connected || !s_client) return;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "status", status);
    if (reason) cJSON_AddStringToObject(root, "reason", reason);

    // Include version — use provided or fall back to running version
    extern const char *ota_running_version(void);
    const char *ver = version ? version : ota_running_version();
    if (ver) cJSON_AddStringToObject(root, "version", ver);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        char topic[64];
        snprintf(topic, sizeof(topic), "adsb/%s/cmd/ack", s_device_id);
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 0);  // QoS 1
        free(json);
        ESP_LOGI(TAG, "Ack: %s %s%s%s", cmd, status,
                 reason ? " — " : "", reason ? reason : "");
    }
}
