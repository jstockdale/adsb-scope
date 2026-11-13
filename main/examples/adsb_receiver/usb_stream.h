// usb_stream.h – continuous sample capture from the RTL-SDR's bulk endpoint.
//
// Several small transfers are kept queued on the endpoint at all times and
// each one is resubmitted from its completion callback. The host
// controller driver starts the next queued transfer from its interrupt
// handler, so the dongle keeps being read while the task that owns the
// stream is doing something else – for as long as that something else
// takes less than (ADSB_USB_XFER_COUNT - 1) transfer periods.
//
// Everything here runs in ONE task: the one that calls
// rtlsdr_stream_start(), _poll() and _stop(). Completion callbacks are
// delivered inside _poll() (and inside any control transfer that task
// makes), never from an interrupt or another task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Three transfers of 5632 bytes (11 packets of 512) replace the single
// 17408-byte transfer used up to r8: 16.9 kB of DMA-capable internal RAM
// instead of 17.4 kB. One transfer is 1.17 ms of signal at 2.4 Msps; with
// three queued the stream survives the owner being away for 2.3 ms.
#define ADSB_USB_XFER_COUNT 3
#define ADSB_USB_XFER_BYTES 5632

// Called for every completed transfer. 'starved' is true when no other
// transfer was still pending at that moment: the endpoint had run dry and
// the dongle was not being read for some time before this call.
typedef void (*adsb_stream_cb_t)(const uint8_t *data, int len, int64_t t_us, bool starved, void *ctx);

typedef struct {
    int      transfers;       // transfers in use (ADSB_USB_XFER_COUNT unless memory was short)
    uint32_t completed;       // transfers delivered
    uint32_t errors;          // transfers that ended with an error status
    uint32_t resubmit_failed; // transfers the host stack refused to queue again
    uint32_t starved;         // completions that found the endpoint dry
    int      last_error;      // status of the most recent failed transfer
} adsb_stream_stats_t;

struct rtlsdr_dev;

// Start streaming. Returns the number of transfers queued, or -1.
int rtlsdr_stream_start(struct rtlsdr_dev *dev, adsb_stream_cb_t cb, void *ctx);
// Deliver completed transfers (and other USB client events); waits up to
// timeout_ticks for something to happen.
void rtlsdr_stream_poll(struct rtlsdr_dev *dev, uint32_t timeout_ticks);
// False once the stream cannot continue (device gone, repeated errors).
bool rtlsdr_stream_alive(void);
// Stop and wait for the queued transfers to come back.
void rtlsdr_stream_stop(struct rtlsdr_dev *dev);
void rtlsdr_stream_get_stats(adsb_stream_stats_t *out);

#ifdef __cplusplus
}
#endif
