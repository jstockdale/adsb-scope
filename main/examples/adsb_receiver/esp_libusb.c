#include "usb/usb_host.h"
#include "esp_log.h"
#include "esp_libusb.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "esp_timer.h"

#define CTRL_TRANSFER_MAX_SIZE 1024  // largest control transfer for RTL-SDR

// DMA RAM reservation — allocated early in app_main() before heap fragmentation,
// freed here right before the USB transfer alloc to guarantee a contiguous hole.
void *g_usb_dma_reservation = NULL;

class_adsb_dev *adsbdev;

// Allocate whichever of the bulk transfers are still missing. Each is
// ADSB_USB_XFER_BYTES of DMA-capable internal RAM; together they take a
// little less than the single 17 kB transfer they replace, and they fit
// the hole left by the boot-time reservation. Returns how many exist.
static int alloc_stream_transfers(void)
{
    int have = 0;
    adsbdev->transfer = NULL;
    for (int i = 0; i < ADSB_USB_XFER_COUNT; i++) {
        if (adsbdev->stream_xfer[i] == NULL) {
            esp_err_t r = usb_host_transfer_alloc(ADSB_USB_XFER_BYTES, 0, &adsbdev->stream_xfer[i]);
            if (r != ESP_OK) adsbdev->stream_xfer[i] = NULL;
        }
        if (adsbdev->stream_xfer[i] != NULL) {
            have++;
            if (adsbdev->transfer == NULL) adsbdev->transfer = adsbdev->stream_xfer[i];
        }
    }
    if (have == ADSB_USB_XFER_COUNT) {
        ESP_LOGI(TAG_ADSB, "bulk transfer alloc success (%d x %d bytes)", have, ADSB_USB_XFER_BYTES);
    } else if (have > 0) {
        ESP_LOGW(TAG_ADSB, "only %d of %d bulk transfers allocated (internal free: %ld) – capture will have gaps",
                 have, ADSB_USB_XFER_COUNT,
                 (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else {
        ESP_LOGE(TAG_ADSB, "bulk transfer alloc failed (internal free: %ld)",
                 (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return have;
}

void init_adsb_dev()
{
    // Idempotent — may be called early from main.cpp and again from rtlsdr_open()
    if (adsbdev != NULL) return;

    adsbdev = calloc(1, sizeof(class_adsb_dev));
    if (adsbdev == NULL) {
        ESP_LOGE(TAG_ADSB, "init_adsb_dev: calloc failed");
        return;
    }
    adsbdev->is_adsb = true;

    // Pre-allocate control transfer buffer once — eliminates the rapid
    // alloc/free cycle during R820T tuner init that fragments internal
    // heap and corrupts TLSF metadata.
    esp_err_t r = usb_host_transfer_alloc(CTRL_TRANSFER_MAX_SIZE, 0, &adsbdev->ctrl_transfer);
    if (r != ESP_OK) {
        ESP_LOGE(TAG_ADSB, "ctrl_transfer pre-alloc failed: %d", r);
        adsbdev->ctrl_transfer = NULL;
    }
    adsbdev->response_buf = calloc(CTRL_TRANSFER_MAX_SIZE, sizeof(uint8_t));
    if (adsbdev->response_buf == NULL) {
        ESP_LOGE(TAG_ADSB, "response_buf pre-alloc failed");
    }

    // Free the DMA reservation to create a contiguous hole, then immediately
    // allocate the USB bulk transfer into that space.  The reservation was
    // made at the very start of app_main() before peripheral init fragmented
    // the heap.
    if (g_usb_dma_reservation != NULL) {
        heap_caps_free(g_usb_dma_reservation);
        g_usb_dma_reservation = NULL;
        ESP_LOGI(TAG_ADSB, "Released DMA reservation for USB transfer");
    }

    alloc_stream_transfers();
}

void alloc_adsb_transfer(void) {
    if (adsbdev == NULL) return;
    bool complete = true;
    for (int i = 0; i < ADSB_USB_XFER_COUNT; i++)
        if (adsbdev->stream_xfer[i] == NULL) complete = false;
    if (complete) {
        ESP_LOGI(TAG_ADSB, "transfer alloc: already pre-allocated");
        return;
    }
    // Release DMA reservation to free contiguous hole for the transfer
    if (g_usb_dma_reservation) {
        free(g_usb_dma_reservation);
        g_usb_dma_reservation = NULL;
        ESP_LOGI(TAG_ADSB, "Released DMA reservation for USB transfer");
    }
    alloc_stream_transfers();
}

// Check if bulk transfer buffer is ready for use
bool adsb_transfer_ready(void) {
    return (adsbdev != NULL && adsbdev->transfer != NULL);
}

// Don't free the bulk transfer buffer — it was pre-allocated early when
// internal RAM was contiguous and can't be re-allocated later due to
// fragmentation.  The buffer is just DMA-capable memory; the device_handle
// is set fresh on every esp_libusb_bulk_transfer() call, so it works
// across device reconnects without re-allocation.
void free_adsb_transfer(void) {
    // Intentionally empty — buffer stays allocated for reuse on reconnect
}

/* ---------------------------------------------------------------------------
 * EP0 STALL recovery
 *
 * The RTL-SDR Blog V4 (R828D tuner) STALLs certain vendor control
 * requests on EP0 — typically the demod status read-back in
 * rtlsdr_demod_write_reg().  On Linux, libusb treats EP0 STALL as
 * non-fatal: the pipe stays usable and the next SETUP packet clears it
 * per USB spec §8.5.3.4.
 *
 * ESP-IDF's USB host marks the default pipe as HALTED on STALL, but
 * the USBH daemon task clears it internally after processing the event.
 * We just need to yield long enough for the daemon to run.
 *
 * Previous attempts to close/reopen the device broke the bulk endpoint
 * (interface release destroys EP 0x81).  The simplest and most correct
 * fix: don't touch anything.  Wait for the daemon, return -1 for the
 * failed transfer, and let the caller continue.  The next control
 * transfer will find the pipe in a clean state.
 * ------------------------------------------------------------------------- */
static bool stream_running(void);

static bool esp_libusb_recover_stall(class_driver_t *driver_obj)
{
    if (stream_running()) {
        // Samples are streaming: keep delivering their transfers while we
        // wait, or the endpoint runs dry for the whole 50 ms. No log line
        // here either: writing one can hold this task for several
        // milliseconds when the console is busy, with the same result.
        int64_t until = esp_timer_get_time() + 50000;
        while (esp_timer_get_time() < until)
            usb_host_client_handle_events(driver_obj->client_hdl, pdMS_TO_TICKS(5));
    } else {
        ESP_LOGW(TAG_ADSB, "EP0 STALL — yielding for USBH daemon recovery");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

void bulk_transfer_read_cb(usb_transfer_t *transfer)
{
    adsbdev->is_done = true;
    adsbdev->is_success = transfer->status == 0;
    adsbdev->stall_detected = false;
    adsbdev->bytes_transferred = transfer->actual_num_bytes;
}

void transfer_read_cb(usb_transfer_t *transfer)
{
    for (int i = 0; i < transfer->actual_num_bytes; i++)
    {
        adsbdev->response_buf[i] = transfer->data_buffer[i];
    }
    adsbdev->is_done = true;
    adsbdev->is_success = transfer->status == 0;
    adsbdev->stall_detected = (transfer->status == USB_TRANSFER_STATUS_STALL);
    adsbdev->bytes_transferred = transfer->actual_num_bytes - sizeof(usb_setup_packet_t);
}

int esp_libusb_bulk_transfer(class_driver_t *driver_obj, unsigned char endpoint, unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    if (adsbdev->transfer == NULL) {
        ESP_LOGI(TAG_ADSB, "esp_libusb_bulk_transfer: no transfer allocated");
        return -1;
    }

    if (stream_running() || length > ADSB_USB_XFER_BYTES) {
        // The synchronous read shares the first stream transfer.
        return -1;
    }
    adsbdev->transfer->num_bytes = length;
    adsbdev->transfer->device_handle = driver_obj->dev_hdl;
    adsbdev->transfer->timeout_ms = timeout;
    adsbdev->transfer->bEndpointAddress = endpoint;
    adsbdev->transfer->callback = bulk_transfer_read_cb;
    adsbdev->transfer->context = (void *)&driver_obj;
    adsbdev->is_done = false;
    esp_err_t r = usb_host_transfer_submit(adsbdev->transfer);
    if (r != ESP_OK)
    {
        ESP_LOGI(TAG_ADSB, "esp_libusb_bulk_transfer b failed with %d", r);
        return -1;
    }
    while (!adsbdev->is_done)
    {
        usb_host_client_handle_events(driver_obj->client_hdl, portMAX_DELAY);
    }
    if (!adsbdev->is_success)
    {
        ESP_LOGI(TAG_ADSB, "esp_libusb_bulk_transfer c failed");
        return -1;
    }
    *transferred = adsbdev->bytes_transferred;
    memcpy(data, adsbdev->transfer->data_buffer, *transferred);
    return 0;
}

int esp_libusb_control_transfer(class_driver_t *driver_obj, uint8_t bm_req_type, uint8_t b_request, uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength, unsigned int timeout)
{
    size_t sizePacket = sizeof(usb_setup_packet_t) + wLength;

    // Sanity check — control transfer must fit in pre-allocated buffer
    if (sizePacket > CTRL_TRANSFER_MAX_SIZE) {
        ESP_LOGE(TAG_ADSB, "control transfer too large: %u > %d", (unsigned)sizePacket, CTRL_TRANSFER_MAX_SIZE);
        return -1;
    }
    if (adsbdev->ctrl_transfer == NULL || adsbdev->response_buf == NULL) {
        ESP_LOGE(TAG_ADSB, "control transfer buffers not allocated");
        return -1;
    }

    // Reuse pre-allocated ctrl_transfer and response_buf — no alloc/free churn
    memset(adsbdev->response_buf, 0, sizePacket);

    USB_SETUP_PACKET_INIT_CONTROL((usb_setup_packet_t *)adsbdev->ctrl_transfer->data_buffer, bm_req_type, b_request, wValue, wIndex, wLength);
    adsbdev->ctrl_transfer->num_bytes = sizePacket;
    adsbdev->ctrl_transfer->device_handle = driver_obj->dev_hdl;
    adsbdev->ctrl_transfer->timeout_ms = timeout;
    adsbdev->ctrl_transfer->context = (void *)&driver_obj;
    adsbdev->ctrl_transfer->callback = transfer_read_cb;
    adsbdev->is_done = false;
    adsbdev->stall_detected = false;

    if (bm_req_type == CTRL_OUT)
    {
        for (uint8_t i = 0; i < wLength; i++)
        {
            adsbdev->ctrl_transfer->data_buffer[sizeof(usb_setup_packet_t) + i] = data[i];
        }
    }
    esp_err_t r = usb_host_transfer_submit_control(driver_obj->client_hdl, adsbdev->ctrl_transfer);
    if (r != ESP_OK)
    {
        if (r == ESP_ERR_INVALID_STATE) {
            /* Default pipe is halted from a previous STALL — recover it.
             * This transfer still fails, but the next one should work. */
            esp_libusb_recover_stall(driver_obj);
        } else {
            ESP_LOGI(TAG_ADSB, "libusb_control_transfer failed with %d", r);
        }
        return -1;
    }

    while (!adsbdev->is_done)
    {
        usb_host_client_handle_events(driver_obj->client_hdl, portMAX_DELAY);
    }
    if (!adsbdev->is_success)
    {
        if (adsbdev->stall_detected) {
            /* V4 dongle STALLed this request — recover the pipe so the
             * next control transfer works.  This matches libusb behavior
             * where EP0 STALL is non-fatal. */
            esp_libusb_recover_stall(driver_obj);
        }
        /* Don't log for STALL — it's a known V4 behavior, not an error
         * worth spamming the console about on every demod read-back. */
        return -1;
    }
    for (uint8_t i = 0; i < wLength; i++)
    {
        data[i] = adsbdev->response_buf[sizeof(usb_setup_packet_t) + i];
    }
    return adsbdev->bytes_transferred;
}

/* ---------------------------------------------------------------------------
 * Continuous capture – see usb_stream.h.
 * ------------------------------------------------------------------------- */

// Written to a transfer's status field while it is queued. The host stack
// overwrites it (from its interrupt handler) when the transfer finishes,
// which lets a completion callback see whether the others are still out.
#define XFER_QUEUED ((usb_transfer_status_t)0x7f)

static struct {
    volatile bool running;
    bool dead;                  // cannot continue: nothing queued any more
    int in_flight;              // queued, callback not yet run
    int consec_errors;
    adsb_stream_cb_t cb;
    void *ctx;
    usb_device_handle_t dev_hdl;
    uint8_t ep;
    adsb_stream_stats_t stats;
} s_stream;

static bool stream_running(void) { return s_stream.running; }

static void stream_xfer_cb(usb_transfer_t *t);

static bool stream_submit(usb_transfer_t *t)
{
    t->num_bytes = ADSB_USB_XFER_BYTES;
    t->device_handle = s_stream.dev_hdl;
    t->bEndpointAddress = s_stream.ep;
    t->timeout_ms = 0;
    t->callback = stream_xfer_cb;
    t->context = NULL;
    t->status = XFER_QUEUED;
    if (usb_host_transfer_submit(t) != ESP_OK) {
        t->status = USB_TRANSFER_STATUS_ERROR;
        s_stream.stats.resubmit_failed++;
        return false;
    }
    s_stream.in_flight++;
    return true;
}

static void stream_xfer_cb(usb_transfer_t *t)
{
    int64_t now = esp_timer_get_time();
    usb_transfer_status_t status = t->status;
    s_stream.in_flight--;

    if (status == USB_TRANSFER_STATUS_COMPLETED) {
        s_stream.consec_errors = 0;
        s_stream.stats.completed++;
        if (s_stream.running && t->actual_num_bytes > 0) {
            bool starved = true;
            for (int i = 0; i < ADSB_USB_XFER_COUNT; i++) {
                usb_transfer_t *o = adsbdev->stream_xfer[i];
                if (o != NULL && o != t && o->status == XFER_QUEUED) starved = false;
            }
            if (starved) s_stream.stats.starved++;
            s_stream.cb(t->data_buffer, t->actual_num_bytes, now, starved, s_stream.ctx);
        }
    } else {
        s_stream.consec_errors++;
        s_stream.stats.errors++;
        s_stream.stats.last_error = (int)status;
    }

    // Queue it again unless the stream is ending or the endpoint is gone.
    bool again = s_stream.running &&
                 status != USB_TRANSFER_STATUS_NO_DEVICE &&
                 status != USB_TRANSFER_STATUS_CANCELED &&
                 s_stream.consec_errors < 10;
    if (again) stream_submit(t);
    if (s_stream.running && s_stream.in_flight == 0) s_stream.dead = true;
}

int esp_libusb_stream_start(class_driver_t *driver_obj, unsigned char endpoint, adsb_stream_cb_t cb, void *ctx)
{
    if (adsbdev == NULL || driver_obj == NULL || cb == NULL || s_stream.running) return -1;
    memset(&s_stream, 0, sizeof(s_stream));
    s_stream.cb = cb;
    s_stream.ctx = ctx;
    s_stream.dev_hdl = driver_obj->dev_hdl;
    s_stream.ep = endpoint;
    s_stream.running = true;
    for (int i = 0; i < ADSB_USB_XFER_COUNT; i++) {
        if (adsbdev->stream_xfer[i] != NULL && stream_submit(adsbdev->stream_xfer[i]))
            s_stream.stats.transfers++;
    }
    if (s_stream.stats.transfers == 0) {
        s_stream.running = false;
        return -1;
    }
    return s_stream.stats.transfers;
}

void esp_libusb_stream_poll(class_driver_t *driver_obj, uint32_t timeout_ticks)
{
    usb_host_client_handle_events(driver_obj->client_hdl, timeout_ticks);
}

void esp_libusb_stream_stop(class_driver_t *driver_obj)
{
    if (!s_stream.running) return;
    s_stream.running = false;   // callbacks stop resubmitting and stop delivering
    if (s_stream.in_flight > 0) {
        // Cancel what is queued. These fail harmlessly if the device has
        // gone; its transfers then come back on their own.
        usb_host_endpoint_halt(s_stream.dev_hdl, s_stream.ep);
        usb_host_endpoint_flush(s_stream.dev_hdl, s_stream.ep);
    }
    int64_t until = esp_timer_get_time() + 1000000;
    while (s_stream.in_flight > 0 && esp_timer_get_time() < until)
        usb_host_client_handle_events(driver_obj->client_hdl, pdMS_TO_TICKS(10));
    if (s_stream.in_flight > 0)
        ESP_LOGE(TAG_ADSB, "stream stop: %d transfers did not come back", s_stream.in_flight);
    usb_host_endpoint_clear(s_stream.dev_hdl, s_stream.ep);
}

bool rtlsdr_stream_alive(void)
{
    return s_stream.running && !s_stream.dead;
}

void rtlsdr_stream_get_stats(adsb_stream_stats_t *out)
{
    if (out) *out = s_stream.stats;
}

void esp_libusb_get_string_descriptor_ascii(const usb_str_desc_t *str_desc, char *str)
{
    if (str_desc == NULL)
    {
        str[0] = '\0';
        return;
    }

    /* bLength includes the 2-byte header (bLength + bDescriptorType).
     * The actual UTF-16 character count is (bLength - 2) / 2. */
    int nchars = (str_desc->bLength - 2) / 2;
    for (int i = 0; i < nchars; i++)
    {
        str[i] = (char)str_desc->wData[i];
    }
    str[nchars] = '\0';
}
