// test_link.c - the console-speed state machine (console_link.c) through
// its cases, with the clock in the test's hands.
#include <stdio.h>
#include <string.h>
#include "console_link.h"

static int fails, checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("FAIL line %d: %s\n", __LINE__, msg); } } while (0)
#define S 1000000LL

int main(void)
{
    console_link_t l;
    console_link_why_t why;

    // start: base, nothing due
    console_link_init(&l);
    CHECK(l.baud == 115200 && l.state == CONSOLE_LINK_BASE, "starts at 115200, base");
    CHECK(console_link_poll(&l, 1000 * S, &why) == CONSOLE_LINK_NOTHING && why == CONSOLE_LINK_WHY_NONE, "nothing ever falls due at base");
    CHECK(!console_link_quiet(&l), "not quiet at base");
    CHECK(!console_link_confirm(&l, 0), "nothing to confirm at base");
    CHECK(console_link_goodbye(&l) == CONSOLE_LINK_NOTHING, "goodbye at base changes nothing");

    // speeds
    CHECK(console_link_rate_allowed(115200) && console_link_rate_allowed(921600) && console_link_rate_allowed(2000000), "listed speeds allowed");
    CHECK(!console_link_rate_allowed(0) && !console_link_rate_allowed(9600) && !console_link_rate_allowed(921601) && !console_link_rate_allowed(12000000), "others refused");
    CHECK(console_link_request(&l, 57600, false, 0) == -1 && l.baud == 115200 && l.state == CONSOLE_LINK_BASE, "a refused speed changes nothing");
    {
        const uint32_t *r; int n = console_link_rates(&r), asc = 1;
        for (int i = 1; i < n; i++) asc &= r[i] > r[i - 1];
        CHECK(n >= 4 && r[0] == 115200 && asc, "speed list ascending from 115200");
    }

    // negotiated: request, confirm, keepalives, silence
    CHECK(console_link_request(&l, 921600, false, 10 * S) == CONSOLE_LINK_SET_BAUD, "request changes the UART");
    CHECK(l.baud == 921600 && l.state == CONSOLE_LINK_PENDING && console_link_quiet(&l), "pending and quiet after the request");
    CHECK(console_link_poll(&l, 10 * S + CONSOLE_LINK_CONFIRM_US - 1, &why) == CONSOLE_LINK_NOTHING, "still waiting just before the deadline");
    CHECK(console_link_confirm(&l, 12 * S) && l.state == CONSOLE_LINK_UP && !console_link_quiet(&l), "confirmed: up");
    CHECK(console_link_poll(&l, 12 * S + CONSOLE_LINK_SILENT_US - 1, &why) == CONSOLE_LINK_NOTHING, "up until the host has been silent long enough");
    console_link_heard(&l, 20 * S);
    CHECK(console_link_poll(&l, 20 * S + CONSOLE_LINK_SILENT_US - 1, &why) == CONSOLE_LINK_NOTHING, "a keepalive moves the deadline");
    CHECK(console_link_confirm(&l, 25 * S) && l.state == CONSOLE_LINK_UP, "a second 'baud ok' is a keepalive");
    CHECK(console_link_poll(&l, 25 * S + CONSOLE_LINK_SILENT_US, &why) == CONSOLE_LINK_SET_BAUD && why == CONSOLE_LINK_WHY_SILENT, "silence: back");
    CHECK(l.baud == 115200 && l.state == CONSOLE_LINK_BASE && l.n_silent == 1 && l.n_up == 1, "at base after silence");
    CHECK(console_link_poll(&l, 99 * S, &why) == CONSOLE_LINK_NOTHING, "falls back once");

    // no confirmation
    CHECK(console_link_request(&l, 2000000, false, 100 * S) == CONSOLE_LINK_SET_BAUD && l.tried == 2000000, "second request");
    console_link_heard(&l, 101 * S);   // garbage at the wrong speed must not count
    CHECK(console_link_poll(&l, 100 * S + CONSOLE_LINK_CONFIRM_US, &why) == CONSOLE_LINK_SET_BAUD && why == CONSOLE_LINK_WHY_NO_CONFIRM, "no 'baud ok': back, whatever was heard");
    CHECK(l.baud == 115200 && l.state == CONSOLE_LINK_BASE && l.n_no_confirm == 1, "at base after no confirmation");

    // goodbye
    console_link_request(&l, 460800, false, 200 * S);
    console_link_confirm(&l, 201 * S);
    CHECK(console_link_goodbye(&l) == CONSOLE_LINK_SET_BAUD && l.baud == 115200 && l.state == CONSOLE_LINK_BASE && l.n_goodbye == 1, "goodbye: back at once");
    console_link_request(&l, 460800, false, 210 * S);
    CHECK(console_link_goodbye(&l) == CONSOLE_LINK_SET_BAUD && l.state == CONSOLE_LINK_BASE, "goodbye while pending: back");

    // same speed again while up: a keepalive, no change
    console_link_request(&l, 921600, false, 300 * S);
    console_link_confirm(&l, 301 * S);
    CHECK(console_link_request(&l, 921600, false, 305 * S) == CONSOLE_LINK_NOTHING && l.state == CONSOLE_LINK_UP, "same speed while up: nothing to do");
    CHECK(console_link_poll(&l, 305 * S + CONSOLE_LINK_SILENT_US - 1, &why) == CONSOLE_LINK_NOTHING, "... and it counted as hearing the host");
    // another speed while up: a new negotiation
    CHECK(console_link_request(&l, 460800, false, 306 * S) == CONSOLE_LINK_SET_BAUD && l.state == CONSOLE_LINK_PENDING && l.baud == 460800, "another speed while up: pending again");
    console_link_confirm(&l, 307 * S);
    // explicit return to the base speed needs no confirmation
    CHECK(console_link_request(&l, 115200, false, 308 * S) == CONSOLE_LINK_SET_BAUD && l.state == CONSOLE_LINK_BASE && !console_link_quiet(&l), "'baud 115200': base at once");
    CHECK(console_link_request(&l, 115200, false, 309 * S) == CONSOLE_LINK_NOTHING, "'baud 115200' at 115200: nothing");

    // set by hand: no confirmation, no keepalive, no goodbye
    CHECK(console_link_request(&l, 921600, true, 400 * S) == CONSOLE_LINK_SET_BAUD && l.state == CONSOLE_LINK_STAY && !console_link_quiet(&l), "stay");
    CHECK(console_link_poll(&l, 100000 * S, &why) == CONSOLE_LINK_NOTHING && l.baud == 921600, "stays without a host");
    CHECK(console_link_goodbye(&l) == CONSOLE_LINK_NOTHING && l.baud == 921600, "a goodbye byte does not undo a hand-set speed");
    CHECK(!console_link_confirm(&l, 401 * S), "nothing to confirm when set by hand");
    CHECK(console_link_request(&l, 921600, false, 402 * S) == CONSOLE_LINK_NOTHING && l.state == CONSOLE_LINK_PENDING, "a negotiated request at a hand-set speed still wants confirming");
    CHECK(console_link_confirm(&l, 403 * S) && l.state == CONSOLE_LINK_UP, "... and then it is a session");
    console_link_request(&l, 115200, false, 404 * S);

    // a start at a remembered speed: the longer window
    console_link_boot(&l, 921600, 500 * S);
    CHECK(l.state == CONSOLE_LINK_PENDING && l.baud == 921600, "boot at a remembered speed: pending");
    CHECK(console_link_poll(&l, 500 * S + CONSOLE_LINK_CONFIRM_US + 1, &why) == CONSOLE_LINK_NOTHING, "the boot window is longer than a switch's");
    CHECK(console_link_poll(&l, 500 * S + CONSOLE_LINK_BOOT_CONFIRM_US, &why) == CONSOLE_LINK_SET_BAUD && why == CONSOLE_LINK_WHY_NO_CONFIRM && l.baud == 115200, "... and ends");
    console_link_boot(&l, 921600, 600 * S);
    CHECK(console_link_confirm(&l, 605 * S) && l.state == CONSOLE_LINK_UP, "boot window confirmed");

    // what the ESP32-P4's UART makes of each speed from its 40 MHz crystal: within 0.2 %
    {
        const uint32_t *r; int n = console_link_rates(&r), ok = 1;
        for (int i = 0; i < n; i++) {
            const uint32_t a = console_link_actual_baud(40000000u, r[i]);
            const double err = ((double)a - (double)r[i]) / (double)r[i];
            if (err > 0.002 || err < -0.002) { ok = 0; printf("  %u -> %u (%+.3f %%)\n", r[i], a, 100 * err); }
        }
        CHECK(ok, "every offered speed is within 0.2 % from a 40 MHz clock");
        CHECK(console_link_actual_baud(40000000u, 2000000) == 2000000 && console_link_actual_baud(40000000u, 921600) == 922190, "known dividers");
    }

    if (fails) { printf("test_link: %d of %d FAILED\n", fails, checks); return 1; }
    printf("test_link: ok (%d checks)\n", checks);
    return 0;
}
