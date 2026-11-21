// wifi_hosted_api.h - what the console and the UI may ask of the WiFi
// connection (wifi_hosted.h, which main.cpp includes, implements it).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "wifi_sup.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     active;                    // the radio is on (initialised, not paused)
    uint8_t  state;                     // ws_state_t
    int      n;                         // saved networks
    int      cur;                       // the one being connected to or connected, -1
    char     ssid[WS_SSID_MAX + 1];     // its name
    int      rssi;                      // level of the connection in dBm, 0 when not connected
    char     ip[16];
    uint32_t retry_in_s;                // waiting: seconds to the next round
    uint32_t rounds_failed;             // since it was last connected
    uint32_t ups, losses;               // connections made, connections lost or left
    int8_t   seen[WS_MAX_NETS];         // level of each saved network in the last scan, WS_NOT_SEEN
} wifi_hosted_status_t;

void    wifi_hosted_get_status(wifi_hosted_status_t *out);
void    wifi_hosted_set_networks(const ws_net_t *nets, int n);   // the saved list, in priority order; may be called at any time
void    wifi_hosted_use(int idx);                                // switch to this saved network now
void    wifi_hosted_link_dead(void);                             // connected, but nothing gets through: reconnect
bool    wifi_hosted_scan_request(void);                          // a scan for the console; false if it cannot scan now
int     wifi_hosted_scan_results(ws_ap_t *out, int max, uint32_t *seq);
uint8_t wifi_hosted_ui_state(void);                              // 0 off, 1 looking for a network, 2 connected
void    wifi_hosted_cycle(void);                                 // power the radio down and up again

#ifdef __cplusplus
}
#endif
