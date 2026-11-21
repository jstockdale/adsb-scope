// sim_device.c - a stand-in for the device's serial console on a
// pseudo-terminal, for testing hosts (tools/adsb-fastlog.py, and the
// protocol the web app speaks) without hardware.
//
// It runs the firmware's own speed state machine (console_link.c) behind a
// copy of the console's `baud` command, and models what matters to a host:
//   - the wire carries baud/10 bytes a second; log lines wait in a 16 kB
//     queue and are dropped when it is full (as on the device);
//   - when the host's speed differs from the device's, both directions are
//     garbage (the host's speed is read from the terminal settings);
//   - --max-baud: a bridge or cable that does not work above a speed;
//   - --reset-on-open: opening the port resets the device (the
//     T-Display-P4: RTS is wired to reset), with a boot log at 115200;
//   - --old-firmware: no `baud` command, keepalive bytes ignored;
//   - --download-first: the first reset lands in the ROM's download mode
//     ("waiting for download"), as the DTR/RTS race on open sometimes does.
// SIGUSR1 restarts the device (a crash, the reset button); SIGUSR2 is a host
// pulsing RTS with DTR released (a restart into the application).
// Each log line carries seq=N, so a host test can count what it lost.
//
// usage: sim_device [--rate LINES_PER_S] [--max-baud B] [--reset-on-open]
//                   [--boot-ms MS] [--old-firmware] [--download-first] [--link PATH]
// It prints the terminal's path on its first line and runs until killed.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "console_link.h"

static int mfd = -1;
static console_link_t lk;
static uint32_t dev_baud = 115200, max_baud = 0, next_baud = 0;
static int64_t next_armed_us = 0;
static int old_fw = 0, reset_on_open = 0, boot_ms = 1500, download_first = 0, in_download = 0;
static volatile sig_atomic_t sig_reset = 0, sig_rts = 0;
static void on_usr1(int s) { (void)s; sig_reset = 1; }
static void on_usr2(int s) { (void)s; sig_rts = 1; }
static double rate = 50.0;

enum { MODE_LOG, MODE_CMD };
static int mode = MODE_LOG;
static char cmdbuf[256]; static int cmdlen = 0;
static int booting = 0; static int64_t boot_done_us = 0;
static uint32_t apply_baud = 0;   // a speed to change to ...
static size_t apply_at = 0; static int apply_marked = 0;   // ... once the output up to here has gone out
static int64_t probe_until_us = 0; static uint32_t probe_seen = 0;

// direct output (command replies, boot text), then the log queue
static char direct[1 << 20]; static size_t d_head = 0, d_tail = 0;
#define LOGQ 16384
static char logq[LOGQ]; static size_t q_len = 0;
static unsigned long seq = 0, dropped = 0, lines_out = 0;
static double budget = 0;

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }

static uint32_t speed_of(speed_t s)
{
    switch (s) {
        case B9600: return 9600; case B19200: return 19200; case B38400: return 38400; case B57600: return 57600;
        case B115200: return 115200; case B230400: return 230400; case B460800: return 460800; case B921600: return 921600;
        case B1000000: return 1000000; case B1500000: return 1500000; case B2000000: return 2000000; case B3000000: return 3000000;
        default: return 0;
    }
}
static uint32_t host_baud(void) { struct termios t; if (tcgetattr(mfd, &t)) return 0; return speed_of(cfgetospeed(&t)); }
// does a byte get through in this direction right now?
static int clean(void) { uint32_t h = host_baud(); return h == dev_baud && !(max_baud && dev_baud > max_baud); }

// What a byte becomes when the two ends' speeds differ. A receiver that is
// slower than the sender samples the sender's bits here and there: any value
// can come out. A receiver that is faster sees each of the sender's bits as a
// run of its own, so only values made of one run of zeros come out - never a
// control byte with isolated bits (the keepalive and goodbye bytes are such).
static unsigned char garble(unsigned char b, int sender_faster)
{
    static uint32_t g = 2463534242u;
    static const unsigned char runs[] = { 0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF };
    g ^= g << 13; g ^= g >> 17; g ^= g << 5; g += b;
    return sender_faster ? (unsigned char)(g >> 8) : runs[(g >> 8) % 9];
}

// the UART's receive FIFO while the application is not reading it yet (the boot)
static unsigned char prefifo[128]; static int prefifo_n = 0;

static void out(const char *fmt, ...)
{
    char t[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof t - 1) n = sizeof t - 1;
    for (int i = 0; i < n; i++) {
        if (t[i] == '\n') direct[d_head++ % sizeof direct] = '\r';   // the console adds the CR
        direct[d_head++ % sizeof direct] = t[i];
    }
}
static void prompt(void) { out("\033[33madsb>\033[0m "); }

// the device's UART changes speed: what was waiting has gone out at the old
// one, and what had been received at the old one is discarded (the firmware
// resets the receive FIFO)
static void set_baud(uint32_t b) { dev_baud = b; prefifo_n = 0; }

static void fall_back(console_link_why_t why, uint32_t from)
{
    set_baud(lk.baud);
    if (why == CONSOLE_LINK_WHY_NO_CONFIRM) out("BAUD: no 'baud ok' at %u – back to %u\n", from, lk.baud);
    else if (why == CONSOLE_LINK_WHY_SILENT) out("BAUD: host silent for %d s – back to %u\n", (int)(CONSOLE_LINK_SILENT_US / 1000000), lk.baud);
    else out("BAUD: host left – back to %u\n", lk.baud);
}

static void cmd_baud(const char *a1, const char *a2)
{
    int64_t now = now_us();
    if (!a1) { out("  Console: %u baud, %s\n", lk.baud, console_link_state_name(lk.state)); return; }
    if (!strcmp(a1, "ok")) {
        if (console_link_confirm(&lk, now)) out("  BAUD: up %u\n", lk.baud);
        else out("  BAUD: nothing to confirm (%u baud, %s)\n", lk.baud, console_link_state_name(lk.state));
        return;
    }
    if (!strcmp(a1, "probe")) {   // the device measures what arrives: here, what the host's port is set to during the window
        int ms = a2 ? atoi(a2) : 1500;
        out("  BAUD: probe for %d ms – send 'U' characters now, at any speed\n", ms);
        probe_until_us = now + (int64_t)ms * 1000; probe_seen = 0;
        return;
    }
    uint32_t b = (a1[0] >= '0' && a1[0] <= '9') ? (uint32_t)strtoul(a1, NULL, 10) : 0;
    if (!console_link_rate_allowed(b)) { out("  BAUD: error: '%s' is not a speed this console offers ('baud' lists them)\n", a1); return; }
    if (a2 && !strcmp(a2, "next")) {
        if (b == CONSOLE_LINK_BASE_BAUD) { next_baud = 0; next_armed_us = 0; out("  BAUD: next start at %u (the default)\n", CONSOLE_LINK_BASE_BAUD); }
        else { next_baud = b; next_armed_us = now; out("  BAUD: next %u – a reset within %d s starts the console at %u, once\n", b, (int)(CONSOLE_LINK_NEXT_ARMED_US / 1000000), b); }
        return;
    }
    int stay = a2 && !strcmp(a2, "stay");
    if (a2 && !stay) { out("  BAUD: error: '%s' – use 'stay' or 'next', or nothing\n", a2); return; }
    int act = console_link_request(&lk, b, stay, now);
    if (act == CONSOLE_LINK_SET_BAUD) {
        if (lk.state == CONSOLE_LINK_PENDING) out("  BAUD: switching to %u – send 'baud ok' at that speed within %d s\n", b, (int)(CONSOLE_LINK_CONFIRM_US / 1000000));
        else out("  BAUD: switching to %u\n", b);
        apply_baud = b;
    } else if (lk.state == CONSOLE_LINK_PENDING) out("  BAUD: already at %u – send 'baud ok' within %d s\n", b, (int)(CONSOLE_LINK_CONFIRM_US / 1000000));
    else out("  BAUD: already at %u (%s)\n", b, console_link_state_name(lk.state));
}

static void dispatch(char *line)
{
    char *cmd = strtok(line, " \t"), *a1 = strtok(NULL, " \t"), *a2 = strtok(NULL, " \t");
    if (!cmd) return;
    if (!strcmp(cmd, "log")) { out("  Entering log mode. Press Ctrl+C to return.\n"); mode = MODE_LOG; return; }
    if (!strcmp(cmd, "version")) { out("  Version: simdev\n  IDF: v5.4.1\n"); return; }
    if (!strcmp(cmd, "adsb") && a1 && !strcmp(a1, "status")) {
        out("  Log:       %lu lines written (console carries %u B/s at %u baud); dropped %lu\n", lines_out, dev_baud / 10, dev_baud, dropped);
        return;
    }
    if (!strcmp(cmd, "baud") && !old_fw) { cmd_baud(a1, a2); return; }
    out("  unknown command: '%s' (type 'help' for commands)\n", cmd);
}

static void input(unsigned char ch)
{
    if (booting) { if (prefifo_n < (int)sizeof prefifo) prefifo[prefifo_n++] = ch; return; }   // waits in the FIFO
    if (probe_until_us) return;
    if (!old_fw && ch == CONSOLE_LINK_KEEPALIVE) { console_link_heard(&lk, now_us()); out("BAUD: ok %u\n", lk.baud); return; }
    if (!old_fw && ch == CONSOLE_LINK_GOODBYE) { uint32_t from = lk.baud; if (console_link_goodbye(&lk) == CONSOLE_LINK_SET_BAUD) fall_back(CONSOLE_LINK_WHY_GOODBYE, from); return; }
    if (mode == MODE_LOG) {
        if (ch == 3) { mode = MODE_CMD; out("\n\033[33m── Command mode ── (type 'help' for commands, 'log' to resume)\033[0m\n"); cmdlen = 0; prompt(); }
        return;
    }
    if (ch == 3) { out("\n"); cmdlen = 0; prompt(); }
    else if (ch == '\r' || ch == '\n') {
        out("\n"); cmdbuf[cmdlen] = 0;
        console_link_heard(&lk, now_us());
        dispatch(cmdbuf);
        cmdlen = 0;
        if (mode == MODE_CMD && !probe_until_us) prompt();
        // the speed changes when the reply and the prompt have gone out (serial_console_task)
        if (apply_baud && !apply_marked) { apply_at = d_head; apply_marked = 1; }
    } else if (ch >= 0x20 && cmdlen < (int)sizeof cmdbuf - 1) { cmdbuf[cmdlen++] = (char)ch; char e[2] = { (char)ch, 0 }; out("%s", e); }
}

static void reset_device(void)
{
    d_head = d_tail = 0; q_len = 0; cmdlen = 0; mode = MODE_LOG; apply_baud = 0; apply_marked = 0; budget = 0; prefifo_n = 0;
    console_link_init(&lk); set_baud(CONSOLE_LINK_BASE_BAUD);
    booting = 1; boot_done_us = now_us() + (int64_t)boot_ms * 1000;
    if (download_first) {   // the boot pin was low at reset: the ROM waits for a download, at 115200, for ever
        download_first = 0; in_download = 1;
        out("ESP-ROM:esp32p4-eco2-20240710\nBuild:Jul 10 2024\nrst:0x1 (POWERON),boot:0x307 (DOWNLOAD(USB/UART0/SPI))\nwaiting for download\n");
        return;
    }
    in_download = 0;
    out("ESP-ROM:esp32p4-eco2-20240710\nBuild:Jul 10 2024\nrst:0x1 (POWERON),boot:0xf (SPI_FAST_FLASH_BOOT)\n");
    out("I (25) boot: ESP-IDF v5.4.1 2nd stage bootloader\nI (1866) app_init: App version:      simdev\n");
    out("I (13184) MQTT: Device ID: p4-simdev\n");
}

static void boot_finished(void)
{
    booting = 0;
    if (next_baud && !old_fw) {
        uint32_t b = next_baud; next_baud = 0; next_armed_us = 0;
        out("BAUD: starting at %u, as asked before the reset – 'baud ok' is due within %d s\n", b, (int)(CONSOLE_LINK_BOOT_CONFIRM_US / 1000000));
        apply_baud = b; apply_at = d_head; apply_marked = 1;   // after that line has gone out, before the next
        console_link_boot(&lk, b, now_us());
    }
    out("[boot+11.168] CONSOLE: Serial console ready\n");
    if (apply_marked) prefifo_n = 0;       // the speed changes before anything is read: see set_baud()
    { unsigned char keep[sizeof prefifo]; int n = prefifo_n; memcpy(keep, prefifo, n); prefifo_n = 0; for (int i = 0; i < n; i++) input(keep[i]); }
}

// write n bytes to the host, as garbage if the two ends are not at the same speed
static void wire(char *buf, size_t n)
{
    if (!n) return;
    if (!clean()) { int dev_faster = dev_baud > host_baud(); for (size_t i = 0; i < n; i++) buf[i] = (char)garble((unsigned char)buf[i], dev_faster); }
    if (write(mfd, buf, n) < 0) {}
}

// move bytes to the host at the device's speed
static void flush(double dt)
{
    budget += dt * dev_baud / 10.0;
    if (budget > dev_baud / 10.0 * 0.05) budget = dev_baud / 10.0 * 0.05;   // at most 50 ms of burst
    char buf[4096]; size_t n = 0;
    for (;;) {
        if (apply_marked && d_tail == apply_at) {
            // everything written before the change has left at the old speed
            wire(buf, n); n = 0;
            set_baud(apply_baud); apply_baud = 0; apply_marked = 0; budget = 0;
            break;
        }
        if (d_tail == d_head || budget < 1 || n >= sizeof buf) break;
        buf[n++] = direct[d_tail++ % sizeof direct]; budget -= 1;
    }
    // log lines: only in log mode, only when the direct output is out, and not while the host is changing speed
    while (d_tail == d_head && !apply_marked && mode == MODE_LOG && !booting && !console_link_quiet(&lk) && q_len && budget >= 1 && n < sizeof buf) {
        size_t take = q_len < sizeof buf - n ? q_len : sizeof buf - n;
        if (take > (size_t)budget) take = (size_t)budget;
        memcpy(buf + n, logq, take); n += take; budget -= take;
        memmove(logq, logq + take, q_len - take); q_len -= take;
    }
    wire(buf, n);
}

static void make_lines(double dt)
{
    static double owed = 0;
    if (booting) return;
    owed += dt * rate;
    while (owed >= 1) {
        owed -= 1;
        char l[200];
        int n = snprintf(l, sizeof l, "[boot+%.3f] A%05lX UAL%-5lu %5luft 451kt 271° -64fpm (37.7749N,122.4194W) 12.3nm@045° SQK:1200 seq=%lu [8DA1234558C382D690C8AC2863A7]\r\n",
                         now_us() / 1e6, 0x10000 + seq % 60, seq % 60, 30000 + (seq % 60) * 100, seq);   // 60 aircraft, in turn
        seq++;
        if (q_len + (size_t)n > LOGQ) { dropped++; continue; }
        memcpy(logq + q_len, l, n); q_len += n; lines_out++;
    }
}

int main(int argc, char **argv)
{
    const char *link = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-baud") && i + 1 < argc) max_baud = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--boot-ms") && i + 1 < argc) boot_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--link") && i + 1 < argc) link = argv[++i];
        else if (!strcmp(argv[i], "--reset-on-open")) reset_on_open = 1;
        else if (!strcmp(argv[i], "--old-firmware")) old_fw = 1;
        else if (!strcmp(argv[i], "--download-first")) download_first = 1;
        else { fprintf(stderr, "usage: sim_device [--rate LINES_PER_S] [--max-baud B] [--reset-on-open] [--boot-ms MS] [--old-firmware] [--download-first] [--link PATH]\n"); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    mfd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (mfd < 0 || grantpt(mfd) || unlockpt(mfd)) { perror("pty"); return 1; }
    const char *path = ptsname(mfd);
    {   // raw, 115200; and open and close the other side once, so that "nobody has it open" is the starting state
        int s = open(path, O_RDWR | O_NOCTTY);
        struct termios t; tcgetattr(s, &t); cfmakeraw(&t); cfsetispeed(&t, B115200); cfsetospeed(&t, B115200); tcsetattr(s, TCSANOW, &t);
        close(s);
    }
    if (link) { unlink(link); if (symlink(path, link)) { perror("symlink"); return 1; } }
    printf("%s\n", link ? link : path); fflush(stdout);
    console_link_init(&lk);

    int was_open = 0;
    int64_t last = now_us();
    for (;;) {
        struct pollfd p = { mfd, POLLIN, 0 };
        poll(&p, 1, 1);
        int64_t t = now_us(); double dt = (t - last) / 1e6; last = t;
        int open_now = !(p.revents & POLLHUP);
        if (open_now && !was_open && reset_on_open) reset_device();
        was_open = open_now;
        if (sig_reset) { sig_reset = 0; reset_device(); }
        if (sig_rts) { sig_rts = 0; if (reset_on_open) reset_device(); }
        if (p.revents & POLLIN) {
            unsigned char b[512]; ssize_t n = read(mfd, b, sizeof b);
            int ok = clean(), host_faster = host_baud() > dev_baud;
            for (ssize_t i = 0; i < n; i++) input(ok ? b[i] : garble(b[i], host_faster));
        }
        if (probe_until_us) {
            uint32_t h = host_baud();
            if (h != dev_baud) probe_seen = h;
            if (t >= probe_until_us) {
                probe_until_us = 0;
                uint32_t half = probe_seen ? 40000000u / probe_seen : 0;
                out("  BAUD: probe edges=%u low=%u high=%u pos=%u neg=%u clock=40000000 est=%u\n",
                    probe_seen ? 1000u : 0u, half, half, 2 * half, 2 * half, half ? 40000000u / half : 0u);
                prompt();
            }
        }
        if (booting && !in_download && t >= boot_done_us) boot_finished();
        if (!old_fw) {
            console_link_why_t why; uint32_t from = lk.baud;
            if (!apply_baud && console_link_poll(&lk, t, &why) == CONSOLE_LINK_SET_BAUD) fall_back(why, from);   // (never while a change is still waiting to go out)
            if (next_armed_us && t - next_armed_us > CONSOLE_LINK_NEXT_ARMED_US) { next_armed_us = 0; next_baud = 0; out("BAUD: no reset followed – the request for the next start is dropped\n"); }
        }
        make_lines(dt);
        if (open_now) flush(dt); else { d_tail = d_head; q_len = 0; }   // nobody listening: it goes nowhere
    }
}
