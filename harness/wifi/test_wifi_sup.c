// test_wifi_sup.c - the WiFi supervisor's decisions (main/examples/adsb_receiver/wifi_sup.c),
// driven by scripted events: which saved network a round tries and in what
// order, that it never gives up, what it does when a link is lost, stalls or
// is declared dead, a manual choice, and edits of the list while running.
//
// usage: ./test_wifi_sup      (part of make check and make check-quick)
#include "wifi_sup.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL  "); printf(__VA_ARGS__); printf("  (line %d)\n", __LINE__); } } while (0)

static ws_net_t N(const char *s, const char *p) { ws_net_t n; memset(&n, 0, sizeof n); strcpy(n.ssid, s); strcpy(n.pass, p); return n; }
static ws_ap_t  A(const char *s, int r) { ws_ap_t a; memset(&a, 0, sizeof a); strcpy(a.ssid, s); a.rssi = (int8_t)r; return a; }

int main(void)
{
    ws_net_t nets[WS_MAX_NETS] = { N("home", "pw1"), N("shop", "pw2"), N("phone", "pw3") };
    int order[WS_MAX_NETS]; int8_t seen[WS_MAX_NETS];

    printf("1. the order a round tries the saved networks in\n");
    {
        ws_ap_t aps[] = { A("neighbour", -40), A("phone", -35), A("home", -70), A("shop", -78) };
        int k = ws_order(nets, 3, aps, 4, -80, order, seen);
        CHECK(k == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2, "all usable: list order, not strength (got %d: %d %d %d)", k, order[0], order[1], order[2]);
        CHECK(seen[0] == -70 && seen[2] == -35, "levels recorded");
    }
    {
        ws_ap_t aps[] = { A("home", -88), A("phone", -60), A("shop", -84) };
        int k = ws_order(nets, 3, aps, 3, -80, order, seen);
        CHECK(k == 3 && order[0] == 2 && order[1] == 1 && order[2] == 0, "below the floor: after the usable ones, strongest first (got %d %d %d)", order[0], order[1], order[2]);
    }
    {
        ws_ap_t aps[] = { A("home", -90), A("home", -62), A("other", -50) };
        int k = ws_order(nets, 3, aps, 3, -80, order, seen);
        CHECK(k == 1 && order[0] == 0 && seen[0] == -62 && seen[1] == WS_NOT_SEEN, "one name on two access points: the better one counts; unseen ones are not tried");
    }
    {
        ws_ap_t aps[] = { A("other", -50) };
        CHECK(ws_order(nets, 3, aps, 1, -80, order, seen) == 0, "none of ours in range: nothing to try");
        ws_ap_t hid[] = { A("other", -50), A("", -55) };
        int k = ws_order(nets, 3, hid, 2, -80, order, seen);
        CHECK(k == 3 && order[0] == 0 && order[2] == 2, "none of ours, but a hidden network: ours by name, in list order");
    }

    printf("2. a normal connect, and staying there\n");
    ws_t w; ws_cmd_t c; uint32_t t = 1000;
    ws_init(&w, nets, 3);
    c = ws_start(&w, t);                                   CHECK(c.act == WA_SCAN && w.state == WS_SCANNING, "start: scan");
    { ws_ap_t aps[] = { A("phone", -40), A("shop", -70) }; c = ws_on_scan(&w, aps, 2, t += 2000); }
    CHECK(c.act == WA_CONNECT && c.net == 1, "scan back: connect to 'shop' (higher on the list than the stronger 'phone'), got net %d", c.net);
    c = ws_on_got_ip(&w, t += 1500);                       CHECK(c.act == WA_NONE && w.state == WS_UP && w.cur == 1 && w.ups == 1, "address: up");
    for (int i = 0; i < 3600; i++) { c = ws_tick(&w, t += 1000); if (c.act != WA_NONE) break; }
    CHECK(c.act == WA_NONE && w.state == WS_UP, "an hour of ticks: nothing done, no roaming");

    printf("3. the link is lost: a round at once, the next candidate when one refuses\n");
    c = ws_on_disconnected(&w, t += 1000);                 CHECK(c.act == WA_SCAN && w.losses == 1, "lost: scan immediately");
    { ws_ap_t aps[] = { A("shop", -75), A("phone", -45) }; c = ws_on_scan(&w, aps, 2, t += 2000); }
    CHECK(c.act == WA_CONNECT && c.net == 1, "first candidate 'shop'");
    c = ws_on_disconnected(&w, t += 800);                  CHECK(c.act == WA_CONNECT && c.net == 2, "'shop' refuses: 'phone'");
    c = ws_on_got_ip(&w, t += 1500);                       CHECK(w.state == WS_UP && w.cur == 2, "up on 'phone'");

    printf("4. nothing in range: it never gives up, and the waits are 1, 2, 4 ... 60, 60 s\n");
    c = ws_on_disconnected(&w, t += 1000);
    uint32_t waits[12]; int nw = 0, rounds = 0;
    for (int i = 0; i < 4 * 3600 && rounds < 300; i++) {
        if (c.act == WA_SCAN) {
            rounds++;
            c = ws_on_scan(&w, NULL, 0, t += 2000);
            if (nw < 12) waits[nw++] = ws_retry_in_ms(&w, t);
            CHECK(c.act == WA_NONE && w.state == WS_WAITING, "empty scan: wait");
        } else c = ws_tick(&w, t += 1000);
    }
    CHECK(waits[0] == 1000 && waits[1] == 2000 && waits[2] == 4000 && waits[5] == 32000 && waits[6] == 60000 && waits[11] == 60000,
          "waits %u %u %u .. %u %u .. %u ms", waits[0], waits[1], waits[2], waits[5], waits[6], waits[11]);
    CHECK(rounds > 200 && w.state != WS_OFF, "four hours with nothing in range: %d rounds, still trying (the old code stopped after 10 attempts, 55 s)", rounds);
    while (c.act != WA_SCAN) c = ws_tick(&w, t += 1000);
    { ws_ap_t aps[] = { A("home", -66) }; c = ws_on_scan(&w, aps, 1, t += 2000); }
    CHECK(c.act == WA_CONNECT && c.net == 0, "'home' appears: connect");
    c = ws_on_got_ip(&w, t += 1000);
    CHECK(w.state == WS_UP && w.rounds_failed == 0, "up; the count of failed rounds starts over");
    c = ws_on_disconnected(&w, t += 5000); c = ws_on_scan(&w, NULL, 0, t += 2000);
    CHECK(ws_retry_in_ms(&w, t) == 1000, "and so do the waits: 1 s after the next loss");

    printf("5. a connect that gets no address, a scan that never reports, a disconnect that never reports\n");
    while (c.act != WA_SCAN) c = ws_tick(&w, t += 1000);
    { ws_ap_t aps[] = { A("home", -66), A("shop", -70) }; c = ws_on_scan(&w, aps, 2, t += 2000); }
    int ticks = 0; do { c = ws_tick(&w, t += 1000); ticks++; } while (c.act == WA_NONE && ticks < 60);
    CHECK(c.act == WA_DISCONNECT && ticks == 15 && w.state == WS_LEAVING, "no address after 15 s: disconnect (after %d s)", ticks);
    c = ws_on_disconnected(&w, t += 200);                  CHECK(c.act == WA_CONNECT && c.net == 1, "then the next candidate");
    ticks = 0; do { c = ws_tick(&w, t += 1000); ticks++; } while (c.act == WA_NONE && ticks < 60);
    CHECK(c.act == WA_DISCONNECT, "that one stalls too: disconnect");
    ticks = 0; do { c = ws_tick(&w, t += 1000); ticks++; } while (w.state == WS_LEAVING && ticks < 60);
    CHECK(ticks == 3 && w.state == WS_WAITING, "the disconnect is never confirmed: goes on after 3 s (%d), candidates used up, waiting", ticks);
    while (c.act != WA_SCAN) c = ws_tick(&w, t += 1000);
    ticks = 0; do { c = ws_tick(&w, t += 1000); ticks++; } while (w.state == WS_SCANNING && ticks < 60);
    CHECK(ticks == 10 && w.state == WS_WAITING, "a scan that never reports back is given up after 10 s (%d)", ticks);

    printf("6. up, but nothing gets through\n");
    while (c.act != WA_SCAN) c = ws_tick(&w, t += 1000);
    { ws_ap_t aps[] = { A("home", -66) }; c = ws_on_scan(&w, aps, 1, t += 2000); } c = ws_on_got_ip(&w, t += 1000);
    uint32_t losses = w.losses;
    c = ws_link_dead(&w, t += 300000);                     CHECK(c.act == WA_DISCONNECT && w.state == WS_LEAVING && w.losses == losses + 1, "declared dead: disconnect");
    c = ws_on_disconnected(&w, t += 200);                  CHECK(c.act == WA_SCAN, "then a fresh round");
    c = ws_link_dead(&w, t);                               CHECK(c.act == WA_NONE, "declared dead while not up: ignored");

    printf("7. a manual choice\n");
    { ws_ap_t aps[] = { A("home", -66), A("phone", -50) }; c = ws_on_scan(&w, aps, 2, t += 2000); } c = ws_on_got_ip(&w, t += 1000);
    CHECK(w.state == WS_UP && w.cur == 0, "up on 'home'");
    c = ws_use(&w, 0, t);                                  CHECK(c.act == WA_NONE, "choosing the one it is on: nothing");
    c = ws_use(&w, 2, t += 1000);                          CHECK(c.act == WA_DISCONNECT, "choosing 'phone': leave 'home'");
    c = ws_on_disconnected(&w, t += 200);                  CHECK(c.act == WA_CONNECT && c.net == 2, "connect to 'phone'");
    c = ws_on_got_ip(&w, t += 1000);                       CHECK(w.state == WS_UP && w.cur == 2, "up on 'phone', and it stays");
    c = ws_use(&w, 7, t);                                  CHECK(c.act == WA_NONE && w.state == WS_UP, "a number that is not on the list: nothing");
    c = ws_use(&w, 1, t += 1000); c = ws_on_disconnected(&w, t += 200); c = ws_on_disconnected(&w, t += 3000);
    CHECK(w.state == WS_WAITING, "a chosen network that refuses: back to ordinary rounds");

    printf("8. the list is edited while running\n");
    while (c.act != WA_SCAN) c = ws_tick(&w, t += 1000);
    { ws_ap_t aps[] = { A("shop", -60) }; c = ws_on_scan(&w, aps, 1, t += 2000); } c = ws_on_got_ip(&w, t += 1000);
    int n = 3;
    CHECK(ws_list_move(nets, n, 1, 0) && strcmp(nets[0].ssid, "shop") == 0 && strcmp(nets[1].ssid, "home") == 0, "move 'shop' to the top");
    c = ws_reload(&w, nets, n, t);                         CHECK(c.act == WA_NONE && w.state == WS_UP && w.cur == 0, "connected network still on the list: stay, under its new number");
    CHECK(ws_list_remove(nets, &n, 0) && n == 2 && strcmp(nets[0].ssid, "home") == 0, "remove 'shop'");
    c = ws_reload(&w, nets, n, t += 1000);                 CHECK(c.act == WA_DISCONNECT, "connected network removed: leave it");
    c = ws_on_disconnected(&w, t += 200);                  CHECK(c.act == WA_SCAN, "and look for another");
    n = 0; memset(nets, 0, sizeof nets);
    c = ws_reload(&w, nets, 0, t += 1000);                 CHECK(c.act == WA_NONE && w.state == WS_WAITING, "list emptied: wait");
    for (int i = 0; i < 300; i++) { c = ws_tick(&w, t += 1000); if (c.act != WA_NONE) break; }
    CHECK(c.act == WA_NONE, "nothing saved: no scans");
    CHECK(ws_list_add(nets, &n, "cabin", "") == 0 && n == 1, "add an open network");
    c = ws_reload(&w, nets, n, t += 1000);                 CHECK(c.act == WA_SCAN, "a network is added: scan at once");

    printf("9. the list itself\n");
    n = 0; memset(nets, 0, sizeof nets);
    CHECK(ws_list_add(nets, &n, "a", "1") == 0 && ws_list_add(nets, &n, "b", "2") == 1 && ws_list_add(nets, &n, "a", "new") == 0 && n == 2 && strcmp(nets[0].pass, "new") == 0,
          "adding a name that is there changes its password, not the list");
    ws_list_add(nets, &n, "c", ""); ws_list_add(nets, &n, "d", ""); ws_list_add(nets, &n, "e", "");
    CHECK(n == WS_MAX_NETS && ws_list_add(nets, &n, "f", "") == -1 && n == WS_MAX_NETS, "full at %d", WS_MAX_NETS);
    char long_ssid[40]; memset(long_ssid, 'x', 33); long_ssid[33] = 0;
    char long_pass[70]; memset(long_pass, 'y', 65); long_pass[65] = 0;
    CHECK(ws_list_add(nets, &n, "", "x") == -2 && ws_list_add(nets, &n, long_ssid, "") == -2 && ws_list_add(nets, &n, "a", long_pass) == -2, "empty or too long: refused");
    CHECK(ws_list_move(nets, n, 4, 0) && strcmp(nets[0].ssid, "e") == 0 && strcmp(nets[1].ssid, "a") == 0 && strcmp(nets[4].ssid, "d") == 0, "move up");
    CHECK(ws_list_move(nets, n, 0, 4) && strcmp(nets[0].ssid, "a") == 0 && strcmp(nets[4].ssid, "e") == 0, "move down");
    CHECK(!ws_list_move(nets, n, 0, 5) && !ws_list_remove(nets, &n, 9), "positions outside the list: refused");

    printf("10. the radio's power cycle is off unless asked for; asked for, once per period\n");
    ws_net_t one[1] = { N("home", "pw") };
    ws_init(&w, one, 1); t = 5000; c = ws_start(&w, t);
    int cycles = 0;
    for (int i = 0; i < 3600; i++) { if (c.act == WA_SCAN) c = ws_on_scan(&w, NULL, 0, t += 1000); else c = ws_tick(&w, t += 1000); if (c.act == WA_POWER_CYCLE) cycles++; }
    CHECK(cycles == 0, "default: an hour down, no power cycle");
    ws_init(&w, one, 1); w.cfg.cycle_after_ms = 600000; t = 5000; c = ws_start(&w, t);
    for (int i = 0; i < 3600; i++) {
        if (c.act == WA_POWER_CYCLE) { cycles++; c = ws_start(&w, t += 1000); w.last_cycle = t; continue; }
        if (c.act == WA_SCAN) c = ws_on_scan(&w, NULL, 0, t += 1000); else c = ws_tick(&w, t += 1000);
    }
    CHECK(cycles >= 4 && cycles <= 6, "with 10 minutes set: %d power cycles in an hour down", cycles);

    printf("11. the clock wraps\n");
    ws_init(&w, one, 1); t = 0xFFFFF000u; c = ws_start(&w, t); c = ws_on_scan(&w, NULL, 0, t += 1000);
    ticks = 0; do { c = ws_tick(&w, t += 1000); ticks++; } while (c.act == WA_NONE && ticks < 100);
    CHECK(c.act == WA_SCAN && ticks <= 2, "a wait across the wrap of the millisecond counter ends on time (%d s)", ticks);

    printf(fails ? "\ntest_wifi_sup: %d of %d FAILED\n" : "\ntest_wifi_sup: ok (%d checks)\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
