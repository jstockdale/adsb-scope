/*
 * console_link.h - the speed of the serial console, as a session property.
 *
 * The console always starts at 115200 baud. A host that wants more asks for
 * it, and keeps it only while it is there:
 *
 *   host:   baud 921600            (at 115200)
 *   device: BAUD: switching to 921600 ...   then changes its UART
 *   host:   changes its own speed, sends `baud ok`
 *   device: BAUD: up 921600        (the link is up)
 *   host:   sends a keepalive (the byte 0x05) every second or two;
 *           the device answers each with `BAUD: ok 921600`
 *   ...
 *   host:   sends 0x04 when it leaves, or just goes away
 *   device: back to 115200 - at once on 0x04, or when it has not heard the
 *           host for CONSOLE_LINK_SILENT_US
 *
 * If `baud ok` does not arrive at the new speed in time, the device goes
 * back to 115200 by itself, so a speed that does not work (the bridge chip,
 * the cable) costs a few seconds and nothing else.
 *
 * A host that cannot change the speed of an open port has to close and
 * reopen it, and on the T-Display-P4 opening the port resets the processor
 * (RTS is wired to its reset). For those, `baud 921600 next` stores the
 * request for the next start only: the device comes up at that speed and
 * waits for `baud ok` (console_link_boot).
 *
 * `baud 921600 stay` is for a person at a terminal: no confirmation, no
 * keepalive, until `baud 115200` or a reset.
 *
 * This file is the state machine only - no UART, no clock: the caller
 * passes the time and carries out CONSOLE_LINK_SET_BAUD. It is built on a
 * PC by harness/link/test_link.c and harness/link/sim_device.c.
 */
#ifndef CONSOLE_LINK_H
#define CONSOLE_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONSOLE_LINK_BASE_BAUD        115200u
#define CONSOLE_LINK_CONFIRM_US       5000000LL    // `baud ok` is due this long after a switch
#define CONSOLE_LINK_BOOT_CONFIRM_US  10000000LL   // ... and this long after a start at a remembered speed
#define CONSOLE_LINK_SILENT_US        10000000LL   // an up link falls back when the host is silent this long
#define CONSOLE_LINK_NEXT_ARMED_US    15000000LL   // a `next` request is dropped if no reset follows in this time

#define CONSOLE_LINK_KEEPALIVE  0x05   // ENQ: host -> device, "I am still here"
#define CONSOLE_LINK_GOODBYE    0x04   // EOT: host -> device, "I am leaving"

typedef enum {
    CONSOLE_LINK_BASE = 0,   // 115200, nothing asked for
    CONSOLE_LINK_PENDING,    // switched, waiting for `baud ok` at the new speed
    CONSOLE_LINK_UP,         // confirmed; needs the host's keepalive
    CONSOLE_LINK_STAY,       // switched by hand; stays
} console_link_state_t;

typedef enum {
    CONSOLE_LINK_NOTHING = 0,
    CONSOLE_LINK_SET_BAUD,   // set the UART to link->baud now
} console_link_act_t;

typedef enum {
    CONSOLE_LINK_WHY_NONE = 0,
    CONSOLE_LINK_WHY_NO_CONFIRM,   // `baud ok` did not arrive
    CONSOLE_LINK_WHY_SILENT,       // the host stopped sending keepalives
    CONSOLE_LINK_WHY_GOODBYE,      // the host said it was leaving
} console_link_why_t;

typedef struct {
    uint32_t baud;                 // what the UART is (to be) set to
    console_link_state_t state;
    int64_t deadline_us;           // PENDING: confirmation due. UP: the host must be heard by then.
    uint32_t tried;                // PENDING: the speed being tried (for the message when it fails)
    // counters, for `baud`
    uint32_t n_switch, n_up, n_no_confirm, n_silent, n_goodbye;
} console_link_t;

void console_link_init(console_link_t *l);

// The speeds a host may ask for.
bool console_link_rate_allowed(uint32_t baud);
int console_link_rates(const uint32_t **list);   // count; *list points at them, ascending

// A host asks for `baud`. Returns CONSOLE_LINK_SET_BAUD if the UART has to
// change (the caller announces it at the old speed first, lets the
// transmitter empty, then sets link->baud), CONSOLE_LINK_NOTHING if it is at
// that speed already, -1 if the speed is not allowed. `stay`: no
// confirmation and no keepalive.
int console_link_request(console_link_t *l, uint32_t baud, bool stay, int64_t now_us);

// The device has started at a remembered speed (the caller has set the
// UART): wait for `baud ok` as after a switch, for the longer boot window.
void console_link_boot(console_link_t *l, uint32_t baud, int64_t now_us);

// `baud ok` arrived. True if it confirmed a pending switch or refreshed an
// up link; false if there was nothing to confirm.
bool console_link_confirm(console_link_t *l, int64_t now_us);

// The host was heard (a keepalive byte, a command, upload data).
void console_link_heard(console_link_t *l, int64_t now_us);

// The host said goodbye. CONSOLE_LINK_SET_BAUD if the UART has to go back
// to 115200.
int console_link_goodbye(console_link_t *l);

// Call regularly. CONSOLE_LINK_SET_BAUD when a deadline has passed and the
// UART has to go back to 115200; *why says which.
int console_link_poll(console_link_t *l, int64_t now_us, console_link_why_t *why);

// The device should hold its log lines back: it has just switched and the
// host is still changing its own speed.
bool console_link_quiet(const console_link_t *l);

const char *console_link_state_name(console_link_state_t s);

// What a UART whose divider has four fractional bits makes of `baud` from
// a clock of clock_hz (the ESP32-P4's console UART: 40 MHz crystal).
uint32_t console_link_actual_baud(uint32_t clock_hz, uint32_t baud);

#ifdef __cplusplus
}
#endif

#endif // CONSOLE_LINK_H
