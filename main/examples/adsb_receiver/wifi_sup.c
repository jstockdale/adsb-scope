// wifi_sup.c - see wifi_sup.h.
#include "wifi_sup.h"
#include <string.h>

enum { AFTER_NEXT, AFTER_ROUND, AFTER_CONNECT };
static const ws_cmd_t NONE = { WA_NONE, -1 };
static bool since(uint32_t now, uint32_t t, uint32_t ms) { return (int32_t)(now - t) >= (int32_t)ms; }

const char *ws_state_name(ws_state_t s)
{
    switch (s) {
    case WS_OFF: return "off";
    case WS_SCANNING: return "scanning";
    case WS_CONNECTING: return "connecting";
    case WS_UP: return "connected";
    case WS_WAITING: return "waiting";
    case WS_LEAVING: return "leaving";
    }
    return "?";
}

// ---- the list ----
int ws_list_find(const ws_net_t *nets, int n, const char *ssid)
{
    for (int i = 0; i < n; i++) if (strcmp(nets[i].ssid, ssid) == 0) return i;
    return -1;
}
int ws_list_add(ws_net_t *nets, int *n, const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > WS_SSID_MAX) return -2;
    if (!pass) pass = "";
    if (strlen(pass) > WS_PASS_MAX) return -2;
    int i = ws_list_find(nets, *n, ssid);
    if (i < 0) {
        if (*n >= WS_MAX_NETS) return -1;
        i = (*n)++;
        memset(&nets[i], 0, sizeof nets[i]);
        strcpy(nets[i].ssid, ssid);
    }
    memset(nets[i].pass, 0, sizeof nets[i].pass);
    strcpy(nets[i].pass, pass);
    return i;
}
bool ws_list_remove(ws_net_t *nets, int *n, int idx)
{
    if (idx < 0 || idx >= *n) return false;
    for (int i = idx; i + 1 < *n; i++) nets[i] = nets[i + 1];
    (*n)--;
    memset(&nets[*n], 0, sizeof nets[*n]);
    return true;
}
bool ws_list_move(ws_net_t *nets, int n, int from, int to)
{
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    ws_net_t t = nets[from];
    if (from < to) for (int i = from; i < to; i++) nets[i] = nets[i + 1];
    else           for (int i = from; i > to; i--) nets[i] = nets[i - 1];
    nets[to] = t;
    return true;
}

// ---- which, in what order ----
int ws_order(const ws_net_t *nets, int n, const ws_ap_t *aps, int n_aps, int floor_dbm, int *order, int8_t *seen)
{
    int8_t lvl[WS_MAX_NETS];
    bool hidden = false;
    for (int i = 0; i < n; i++) lvl[i] = WS_NOT_SEEN;
    for (int a = 0; a < n_aps; a++) {
        if (!aps[a].ssid[0]) { hidden = true; continue; }
        int i = ws_list_find(nets, n, aps[a].ssid);
        if (i >= 0 && aps[a].rssi > lvl[i]) lvl[i] = aps[a].rssi;      // several access points, one name: the best
    }
    if (seen) for (int i = 0; i < n; i++) seen[i] = lvl[i];
    int k = 0;
    for (int i = 0; i < n; i++) if (lvl[i] != WS_NOT_SEEN && lvl[i] >= floor_dbm) order[k++] = i;   // usable: by priority
    int first_weak = k;
    for (int i = 0; i < n; i++) {                                                                  // the rest: strongest first
        if (lvl[i] == WS_NOT_SEEN || lvl[i] >= floor_dbm) continue;
        int p = k++;
        while (p > first_weak && lvl[order[p - 1]] < lvl[i]) { order[p] = order[p - 1]; p--; }
        order[p] = i;
    }
    if (k == 0 && hidden) for (int i = 0; i < n; i++) order[k++] = i;                              // maybe one of ours hides its name
    return k;
}

// ---- the state machine ----
void ws_init(ws_t *w, const ws_net_t *nets, int n)
{
    memset(w, 0, sizeof *w);
    w->cfg = (ws_cfg_t){ .floor_dbm = -80, .connect_timeout_ms = 15000, .scan_timeout_ms = 10000, .leave_timeout_ms = 3000,
                         .backoff_min_ms = 1000, .backoff_max_ms = 60000, .cycle_after_ms = 0 };
    if (n > WS_MAX_NETS) n = WS_MAX_NETS;
    for (int i = 0; i < n; i++) w->nets[i] = nets[i];
    w->n = n;
    for (int i = 0; i < WS_MAX_NETS; i++) w->seen[i] = WS_NOT_SEEN;
    w->state = WS_OFF; w->cur = -1;
    w->backoff_ms = w->cfg.backoff_min_ms;
}

static ws_cmd_t new_round(ws_t *w, uint32_t now)
{
    w->cur = -1;
    if (w->n == 0) { w->state = WS_WAITING; w->wait_until = now; return NONE; }     // nothing saved: wait for the list to change
    w->state = WS_SCANNING; w->t_state = now;
    return (ws_cmd_t){ WA_SCAN, -1 };
}
static ws_cmd_t round_failed(ws_t *w, uint32_t now)
{
    w->state = WS_WAITING; w->cur = -1; w->t_state = now;
    w->wait_until = now + w->backoff_ms;
    w->backoff_ms = w->backoff_ms * 2 > w->cfg.backoff_max_ms ? w->cfg.backoff_max_ms : w->backoff_ms * 2;
    w->rounds_failed++;
    return NONE;
}
static ws_cmd_t connect_cand(ws_t *w, uint32_t now)
{
    if (w->i_cand >= w->n_cand) return round_failed(w, now);
    w->cur = w->cand[w->i_cand];
    w->state = WS_CONNECTING; w->t_state = now;
    return (ws_cmd_t){ WA_CONNECT, w->cur };
}
static ws_cmd_t leave(ws_t *w, int after, int target, uint32_t now)
{
    if (w->state == WS_UP) { w->losses++; w->down_since = now; }
    w->state = WS_LEAVING; w->t_state = now; w->after_leave = after; w->leave_target = target;
    return (ws_cmd_t){ WA_DISCONNECT, -1 };
}
static ws_cmd_t left(ws_t *w, uint32_t now)
{
    switch (w->after_leave) {
    case AFTER_NEXT:    w->i_cand++; return connect_cand(w, now);
    case AFTER_CONNECT: w->n_cand = 1; w->cand[0] = w->leave_target; w->i_cand = 0; return connect_cand(w, now);
    default:            w->backoff_ms = w->cfg.backoff_min_ms; return new_round(w, now);
    }
}

ws_cmd_t ws_start(ws_t *w, uint32_t now)
{
    w->backoff_ms = w->cfg.backoff_min_ms; w->rounds_failed = 0;
    w->down_since = now; w->last_cycle = now;
    return new_round(w, now);
}
void ws_stop(ws_t *w) { w->state = WS_OFF; w->cur = -1; }

ws_cmd_t ws_on_scan(ws_t *w, const ws_ap_t *aps, int n_aps, uint32_t now)
{
    int order[WS_MAX_NETS];
    int k = ws_order(w->nets, w->n, aps, n_aps, w->cfg.floor_dbm, order, w->seen);
    if (w->state != WS_SCANNING) return NONE;          // a scan somebody asked for while we were doing something else
    w->n_cand = k; w->i_cand = 0;
    for (int i = 0; i < k; i++) w->cand[i] = order[i];
    return connect_cand(w, now);
}
ws_cmd_t ws_on_got_ip(ws_t *w, uint32_t now)
{
    if (w->state != WS_CONNECTING) return NONE;
    w->state = WS_UP; w->t_state = now; w->ups++;
    w->backoff_ms = w->cfg.backoff_min_ms; w->rounds_failed = 0;
    return NONE;
}
ws_cmd_t ws_on_disconnected(ws_t *w, uint32_t now)
{
    switch (w->state) {
    case WS_CONNECTING: w->i_cand++; return connect_cand(w, now);
    case WS_UP:         w->losses++; w->down_since = now; w->backoff_ms = w->cfg.backoff_min_ms; return new_round(w, now);
    case WS_LEAVING:    return left(w, now);
    default:            return NONE;
    }
}
ws_cmd_t ws_tick(ws_t *w, uint32_t now)
{
    switch (w->state) {
    case WS_SCANNING:   if (since(now, w->t_state, w->cfg.scan_timeout_ms)) return round_failed(w, now); break;
    case WS_CONNECTING: if (since(now, w->t_state, w->cfg.connect_timeout_ms)) return leave(w, AFTER_NEXT, -1, now); break;
    case WS_LEAVING:    if (since(now, w->t_state, w->cfg.leave_timeout_ms)) return left(w, now); break;
    case WS_WAITING:
        if (w->n == 0) break;
        if (w->cfg.cycle_after_ms && since(now, w->down_since, w->cfg.cycle_after_ms) && since(now, w->last_cycle, w->cfg.cycle_after_ms)) {
            w->last_cycle = now; w->state = WS_OFF;
            return (ws_cmd_t){ WA_POWER_CYCLE, -1 };
        }
        if (since(now, w->wait_until, 0)) return new_round(w, now);
        break;
    default: break;
    }
    return NONE;
}
ws_cmd_t ws_link_dead(ws_t *w, uint32_t now)
{
    if (w->state != WS_UP) return NONE;
    return leave(w, AFTER_ROUND, -1, now);
}
ws_cmd_t ws_use(ws_t *w, int net, uint32_t now)
{
    if (net < 0 || net >= w->n || w->state == WS_OFF) return NONE;
    if (w->state == WS_UP && w->cur == net) return NONE;
    if (w->state == WS_UP || w->state == WS_CONNECTING) return leave(w, AFTER_CONNECT, net, now);
    if (w->state == WS_LEAVING) { w->after_leave = AFTER_CONNECT; w->leave_target = net; return NONE; }
    w->n_cand = 1; w->cand[0] = net; w->i_cand = 0;
    return connect_cand(w, now);
}
ws_cmd_t ws_reload(ws_t *w, const ws_net_t *nets, int n, uint32_t now)
{
    char cur[WS_SSID_MAX + 1] = "";
    if (w->cur >= 0 && w->cur < w->n) strcpy(cur, w->nets[w->cur].ssid);
    if (n > WS_MAX_NETS) n = WS_MAX_NETS;
    memset(w->nets, 0, sizeof w->nets);
    for (int i = 0; i < n; i++) w->nets[i] = nets[i];
    w->n = n;
    for (int i = 0; i < WS_MAX_NETS; i++) w->seen[i] = WS_NOT_SEEN;
    int keep = cur[0] ? ws_list_find(w->nets, w->n, cur) : -1;
    switch (w->state) {
    case WS_OFF: return NONE;
    case WS_UP:
        if (keep >= 0) { w->cur = keep; return NONE; }                 // still on the list: stay (a changed password applies next time)
        return leave(w, AFTER_ROUND, -1, now);
    case WS_CONNECTING: return leave(w, AFTER_ROUND, -1, now);         // the candidates are numbers into the old list
    case WS_LEAVING:    w->after_leave = AFTER_ROUND; return NONE;
    default:            w->backoff_ms = w->cfg.backoff_min_ms; return new_round(w, now);
    }
}
uint32_t ws_retry_in_ms(const ws_t *w, uint32_t now)
{
    if (w->state != WS_WAITING || w->n == 0) return 0;
    int32_t d = (int32_t)(w->wait_until - now);
    return d > 0 ? (uint32_t)d : 0;
}
