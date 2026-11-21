/*
 * console_link.c - see console_link.h.
 */
#include "console_link.h"

#include <string.h>

static const uint32_t RATES[] = { 115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000 };
#define N_RATES ((int)(sizeof(RATES) / sizeof(RATES[0])))

void console_link_init(console_link_t *l)
{
    memset(l, 0, sizeof(*l));
    l->baud = CONSOLE_LINK_BASE_BAUD;
    l->state = CONSOLE_LINK_BASE;
}

bool console_link_rate_allowed(uint32_t baud)
{
    for (int i = 0; i < N_RATES; i++)
        if (RATES[i] == baud) return true;
    return false;
}

int console_link_rates(const uint32_t **list)
{
    if (list) *list = RATES;
    return N_RATES;
}

int console_link_request(console_link_t *l, uint32_t baud, bool stay, int64_t now_us)
{
    if (!console_link_rate_allowed(baud)) return -1;
    const bool change = (baud != l->baud);
    if (baud == CONSOLE_LINK_BASE_BAUD) {
        l->state = CONSOLE_LINK_BASE;          // the base speed needs no confirmation
    } else if (stay) {
        l->state = CONSOLE_LINK_STAY;
    } else if (!change && l->state == CONSOLE_LINK_UP) {
        l->deadline_us = now_us + CONSOLE_LINK_SILENT_US;   // already up at this speed: a keepalive
        return CONSOLE_LINK_NOTHING;
    } else {
        l->state = CONSOLE_LINK_PENDING;
        l->tried = baud;
        l->deadline_us = now_us + CONSOLE_LINK_CONFIRM_US;
    }
    l->baud = baud;
    if (change) l->n_switch++;
    return change ? CONSOLE_LINK_SET_BAUD : CONSOLE_LINK_NOTHING;
}

void console_link_boot(console_link_t *l, uint32_t baud, int64_t now_us)
{
    l->baud = baud;
    l->state = CONSOLE_LINK_PENDING;
    l->tried = baud;
    l->deadline_us = now_us + CONSOLE_LINK_BOOT_CONFIRM_US;
    l->n_switch++;
}

bool console_link_confirm(console_link_t *l, int64_t now_us)
{
    if (l->state == CONSOLE_LINK_PENDING) {
        l->state = CONSOLE_LINK_UP;
        l->n_up++;
    } else if (l->state != CONSOLE_LINK_UP) {
        return false;
    }
    l->deadline_us = now_us + CONSOLE_LINK_SILENT_US;
    return true;
}

void console_link_heard(console_link_t *l, int64_t now_us)
{
    if (l->state == CONSOLE_LINK_UP) l->deadline_us = now_us + CONSOLE_LINK_SILENT_US;
}

static int to_base(console_link_t *l)
{
    const bool change = (l->baud != CONSOLE_LINK_BASE_BAUD);
    l->baud = CONSOLE_LINK_BASE_BAUD;
    l->state = CONSOLE_LINK_BASE;
    return change ? CONSOLE_LINK_SET_BAUD : CONSOLE_LINK_NOTHING;
}

int console_link_goodbye(console_link_t *l)
{
    // A hand-set speed is not a session: it stays.
    if (l->state != CONSOLE_LINK_UP && l->state != CONSOLE_LINK_PENDING) return CONSOLE_LINK_NOTHING;
    l->n_goodbye++;
    return to_base(l);
}

int console_link_poll(console_link_t *l, int64_t now_us, console_link_why_t *why)
{
    if (why) *why = CONSOLE_LINK_WHY_NONE;
    if (l->state == CONSOLE_LINK_PENDING && now_us >= l->deadline_us) {
        l->n_no_confirm++;
        if (why) *why = CONSOLE_LINK_WHY_NO_CONFIRM;
        return to_base(l);
    }
    if (l->state == CONSOLE_LINK_UP && now_us >= l->deadline_us) {
        l->n_silent++;
        if (why) *why = CONSOLE_LINK_WHY_SILENT;
        return to_base(l);
    }
    return CONSOLE_LINK_NOTHING;
}

bool console_link_quiet(const console_link_t *l)
{
    return l->state == CONSOLE_LINK_PENDING;
}

const char *console_link_state_name(console_link_state_t s)
{
    switch (s) {
        case CONSOLE_LINK_BASE:    return "base";
        case CONSOLE_LINK_PENDING: return "waiting for 'baud ok'";
        case CONSOLE_LINK_UP:      return "up";
        case CONSOLE_LINK_STAY:    return "set by hand";
    }
    return "?";
}

uint32_t console_link_actual_baud(uint32_t clock_hz, uint32_t baud)
{
    if (baud == 0) return 0;
    // uart_ll_set_baudrate(): divider = (clock << 4) / baud, integer part and four fractional bits
    const uint64_t div16 = ((uint64_t)clock_hz << 4) / baud;
    return div16 ? (uint32_t)((((uint64_t)clock_hz << 4) + div16 / 2) / div16) : 0;
}
