// wifi_sup.h - which saved network to be on, and when to try again.
//
// The decisions of the WiFi connection, without the WiFi: a list of saved
// networks in priority order, and a state machine that is told what happens
// (a scan came back, we got an address, we were disconnected, a second
// passed) and answers with at most one thing to do (scan, connect to network
// n, disconnect). wifi_hosted.h carries that out with esp_wifi; the harness
// (harness/wifi/test_wifi_sup.c) drives it with scripted events.
//
// The rules:
//   - It never gives up. A round is: scan, then try the saved networks that
//     are in range, best first. A round that connects nowhere is followed by
//     a wait of 1, 2, 4 ... 60 s, then 60 s for as long as it takes.
//   - Best first means: the saved networks at or above a usable level
//     (floor_dbm, -80) in the order of the list; then the ones below it,
//     strongest first. Priority before strength - a phone's hotspot on the
//     same desk is always the strongest, and rarely the one wanted.
//   - If no saved network is seen but a network that hides its name is, the
//     saved ones are tried by name in list order.
//   - A link that works is kept: no roaming. Losing it starts a round at once.
//   - A connect that produces no address in connect_timeout_ms is abandoned.
//   - "The link is up but nothing gets through" (ws_link_dead) and a manual
//     choice (ws_use) leave the current network on purpose.
//
// Pure C, no ESP-IDF, no allocation, no clock of its own: the caller passes
// the time in milliseconds (it may wrap).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_MAX_NETS 5
#define WS_SSID_MAX 32
#define WS_PASS_MAX 64
#define WS_NOT_SEEN (-128)

typedef struct { char ssid[WS_SSID_MAX + 1]; char pass[WS_PASS_MAX + 1]; } ws_net_t;
typedef struct { char ssid[WS_SSID_MAX + 1]; int8_t rssi; } ws_ap_t;

typedef enum { WS_OFF, WS_SCANNING, WS_CONNECTING, WS_UP, WS_WAITING, WS_LEAVING } ws_state_t;
typedef enum { WA_NONE, WA_SCAN, WA_CONNECT, WA_DISCONNECT, WA_POWER_CYCLE } ws_action_t;
typedef struct { ws_action_t act; int net; } ws_cmd_t;

typedef struct {
    int      floor_dbm;            // a network at or above this is "usable"
    uint32_t connect_timeout_ms;   // from "connect" to an address
    uint32_t scan_timeout_ms;      // a scan that never reports back
    uint32_t leave_timeout_ms;     // a disconnect that never reports back
    uint32_t backoff_min_ms, backoff_max_ms;
    uint32_t cycle_after_ms;       // down this long: ask for a power cycle of the radio (0: never)
} ws_cfg_t;

typedef struct {
    ws_cfg_t   cfg;
    ws_net_t   nets[WS_MAX_NETS];
    int        n;
    ws_state_t state;
    int        cur;                         // network being connected to or connected, -1: none
    int        cand[WS_MAX_NETS], n_cand, i_cand;
    int8_t     seen[WS_MAX_NETS];           // level in the last scan, WS_NOT_SEEN
    uint32_t   t_state;                     // when the current state began
    uint32_t   wait_until, backoff_ms;
    uint32_t   down_since, last_cycle;
    uint32_t   rounds_failed;               // since the last time it was up
    uint32_t   ups, losses;                 // counters for the console
    int        after_leave, leave_target;   // what follows the disconnect it asked for
} ws_t;

void     ws_init(ws_t *w, const ws_net_t *nets, int n);            // defaults, state OFF
ws_cmd_t ws_start(ws_t *w, uint32_t now);                          // the radio is ready
void     ws_stop(ws_t *w);                                         // the radio is being switched off
ws_cmd_t ws_on_scan(ws_t *w, const ws_ap_t *aps, int n_aps, uint32_t now);
ws_cmd_t ws_on_got_ip(ws_t *w, uint32_t now);
ws_cmd_t ws_on_disconnected(ws_t *w, uint32_t now);
ws_cmd_t ws_tick(ws_t *w, uint32_t now);                           // about once a second
ws_cmd_t ws_link_dead(ws_t *w, uint32_t now);                      // up, but nothing gets through
ws_cmd_t ws_use(ws_t *w, int net, uint32_t now);                   // the user wants this one, now
ws_cmd_t ws_reload(ws_t *w, const ws_net_t *nets, int n, uint32_t now);   // the list was edited
uint32_t ws_retry_in_ms(const ws_t *w, uint32_t now);              // WAITING: time to the next round
const char *ws_state_name(ws_state_t s);

// The order a round tries the saved networks in, given a scan. Returns how many.
int ws_order(const ws_net_t *nets, int n, const ws_ap_t *aps, int n_aps, int floor_dbm, int *order, int8_t *seen);

// The list. Order is priority; names are unique.
int  ws_list_add(ws_net_t *nets, int *n, const char *ssid, const char *pass);   // index; -1 full; -2 bad name or password
bool ws_list_remove(ws_net_t *nets, int *n, int idx);
bool ws_list_move(ws_net_t *nets, int n, int from, int to);
int  ws_list_find(const ws_net_t *nets, int n, const char *ssid);

#ifdef __cplusplus
}
#endif
