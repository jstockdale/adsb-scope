#!/usr/bin/env python3
"""adsb-fastlog.py - log the ADS-B Scope console to a file, at a speed
negotiated with the device.

The console is 115200 baud by default, which carries about 80 aircraft
lines a second; beyond that the device drops lines. This connects at
115200, asks the device for a faster speed (`baud 921600`), changes the
speed of the port it already has open, confirms, and from then on sends the
device a keepalive every two seconds. When it stops - or is killed - the
device goes back to 115200 by itself. Firmware before coh4 has no `baud`
command: it is logged at 115200 as before.

Opening the port resets a T-Display-P4 (its RTS line is wired to reset), so
a run starts with the boot log.

usage:  python3 adsb-fastlog.py [--port /dev/ttyACM0] [--baud 921600]
                                [--out FILE] [--seconds N] [--no-status]
        python3 adsb-fastlog.py --probe 921600     (diagnostic, see below)

  --port P      serial port (default /dev/ttyACM0)
  --baud B      speed to ask for: 115200 230400 460800 921600 1000000
                1500000 2000000 3000000 (default 921600). If the device
                does not confirm at B, lower speeds are tried.
  --out FILE    log file (default adsb-log-<date>-<time>.txt here)
  --seconds N   stop after N seconds of logging (default: until Ctrl+C)
  --no-status   do not append `adsb status` and `baud` at the end
  --probe B     no logging: have the device measure, with its UART's pulse
                counters, a run of 'U' sent at B while it stays at 115200.
                Answers whether a host that opens the port at B could be
                recognised without being told (docs/UPLINK.md 9.8).

Needs Python 3.6 or later on Linux or macOS; nothing else.
"""
import argparse, errno, fcntl, os, re, select, signal, struct, sys, termios, time

BASE = 115200
RATES = [115200, 230400, 460800, 921600, 1000000, 1500000, 2000000, 3000000]
KEEPALIVE, GOODBYE, CTRL_C = b"\x05", b"\x04", b"\x03"
KEEPALIVE_S, LOST_S = 2.0, 8.0


def say(msg):
    print("[fastlog] " + msg, file=sys.stderr, flush=True)


def stop(msg):
    print("STOP: " + msg, file=sys.stderr)
    sys.exit(1)


class Port:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.saved = termios.tcgetattr(self.fd)
        self.baud = None
        self.buf = b""
        self.set_baud(BASE)
        # Release the modem lines the system raised on open: DTR first (the
        # boot-mode pin), then RTS (reset), so the processor starts normally.
        for bit in (termios.TIOCM_DTR, termios.TIOCM_RTS):
            try:
                fcntl.ioctl(self.fd, termios.TIOCMBIC, struct.pack("I", bit))
            except OSError:
                pass            # not a real serial port
            time.sleep(0.05)

    def set_baud(self, baud):
        a = termios.tcgetattr(self.fd)
        a[0] = 0                                           # iflag: raw
        a[1] = 0                                           # oflag
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag: 8N1, no flow control, no hang-up on close
        a[3] = 0                                           # lflag
        a[6][termios.VMIN] = 0
        a[6][termios.VTIME] = 0
        const = getattr(termios, "B%d" % baud, None)
        if const is not None:
            a[4] = a[5] = const
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
        elif sys.platform == "darwin":
            a[4] = a[5] = termios.B115200
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
            fcntl.ioctl(self.fd, 0x80085402, struct.pack("L", baud))   # IOSSIOSPEED
        else:
            raise OSError("this system has no setting for %d baud" % baud)
        self.baud = baud

    def write(self, data):
        while data:
            try:
                n = os.write(self.fd, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self.fd], [], 0.2)

    def drain(self):
        termios.tcdrain(self.fd)

    def read(self, timeout):
        """bytes that arrive within `timeout` seconds (b'' if none)"""
        r, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not r:
            return b""
        try:
            return os.read(self.fd, 65536)
        except BlockingIOError:
            return b""
        except OSError as e:
            if e.errno == errno.EIO:
                stop("%s went away (unplugged, or another program opened it)" % self.path)
            raise

    def lines(self, timeout):
        """complete lines received within `timeout`; what follows the last newline stays in self.buf"""
        data = self.read(timeout)
        if not data:
            return []
        self.buf += data
        parts = self.buf.split(b"\n")
        self.buf = parts.pop()
        if len(self.buf) > 4096:        # garbage without newlines
            self.buf = b""
        return [p.rstrip(b"\r").decode("utf-8", "replace") for p in parts]

    def close(self):
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        except (OSError, termios.error):
            pass
        os.close(self.fd)


def readable(line):
    """text, not what a wrong speed looks like"""
    if not line:
        return True
    bad = sum(1 for c in line if c == "�" or (ord(c) < 32 and c not in "\t\x1b"))
    return bad * 10 <= len(line)


class Session:
    def __init__(self, port, sink):
        self.port = port
        self.sink = sink                # called with each log line
        self.fast = False               # the device has `baud` and the keepalive
        self.last_ok = 0.0              # when the device last answered a keepalive
        self.last_ping = 0.0

    def pump(self, timeout, want=None, hide=False):
        """Read for up to `timeout`; pass lines on; return the first line matching `want` (a regex) or None.
        hide: the lines belong to a command's reply and stay out of the log."""
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            for line in self.port.lines(min(max(left, 0.0), 0.1)):
                m = re.search(r"BAUD: ok (\d+)", line)
                if m:
                    self.last_ok = time.monotonic()
                    self.fast = True
                    line = line.replace(m.group(0), "").strip()
                    if not line or line.replace("\x1b[33madsb>\x1b[0m", "").strip() == "":
                        if want and re.search(want, m.group(0)):
                            return m.group(0)
                        continue
                if want and re.search(want, line):
                    return line
                if not hide and readable(line):
                    self.sink(line)
            if want and self.port.buf and re.search(want, self.port.buf.decode("utf-8", "replace")):
                text = self.port.buf.decode("utf-8", "replace")     # a prompt has no newline
                self.port.buf = b""
                return text
            if left <= 0:
                return None

    def prompt(self, timeout=2.0):
        self.port.write(CTRL_C)
        return self.pump(timeout, r"adsb>", hide=True) is not None

    def command(self, cmd, timeout=3.0):
        """Send a command in command mode; return its reply lines, or None if no prompt came back."""
        got = []
        keep, self.sink = self.sink, got.append
        try:
            self.port.write(cmd.encode() + b"\r")
            ok = self.pump(timeout, r"adsb>") is not None
        finally:
            self.sink = keep
        return [l for l in got if l.strip() and l.strip() != cmd] if ok else None

    def sync(self, timeout):
        """Wait until the console answers a keepalive at 115200. True: it does (it has `baud`).
        False: a console is there but does not answer (firmware before coh4). None: nothing
        readable arrived at all."""
        start = time.monotonic()
        seen_text, ready_at = False, None
        while time.monotonic() - start < timeout:
            self.port.write(KEEPALIVE)
            t0 = time.monotonic()
            while time.monotonic() - t0 < 1.0:
                for line in self.port.lines(0.1):
                    if re.search(r"BAUD: ok %d" % BASE, line):
                        self.last_ok = time.monotonic()
                        self.fast = True
                        return True
                    if readable(line) and line.strip():
                        seen_text = True
                        if ready_at is None and re.search(r"Serial console (ready|heartbeat)", line):
                            ready_at = time.monotonic()
                        self.sink(line)
            now = time.monotonic()
            # The console task is running and has had several keepalives, or text has been
            # arriving for a long time without the console ever answering: older firmware.
            if (ready_at is not None and now - ready_at > 5.0) or (seen_text and now - start > 30.0):
                return False
        return False if seen_text else None

    def negotiate(self, want):
        """Bring the link up at `want` or the fastest lower speed that confirms. Returns the speed in use."""
        ladder = [r for r in reversed(RATES) if BASE < r <= want]
        if not self.prompt():
            say("no command prompt – staying at %d" % BASE)
            return BASE
        for rate in ladder:
            reply = self.command("baud %d" % rate)
            text = " ".join(reply or [])
            if reply is None or "unknown command" in text:
                say("this firmware has no `baud` command – staying at %d" % BASE)
                break
            if "switching to %d" % rate not in text:
                say("device said: %s" % (text or "(nothing)"))
                continue
            self.port.drain()
            time.sleep(0.05)
            self.port.buf = b""
            self.port.set_baud(rate)
            ok = False
            if self.prompt(1.5):
                r2 = self.command("baud ok", 2.0)
                ok = r2 is not None and any("BAUD: up %d" % rate in l for l in r2)
            if ok:
                self.port.write(b"log\r")
                self.last_ok = self.last_ping = time.monotonic()
                say("link up at %d baud" % rate)
                return rate
            say("no confirmation at %d – back to %d, waiting for the device to follow" % (rate, BASE))
            self.port.set_baud(BASE)
            self.port.buf = b""
            self.pump(7.0, r"BAUD: no 'baud ok'", hide=True)
            if not self.prompt(3.0):
                say("the device did not come back to %d – staying there and waiting" % BASE)
                return BASE
        self.port.write(b"log\r")
        return BASE

    def keepalive(self):
        """Send the keepalive when due. False when the device has stopped answering."""
        now = time.monotonic()
        if now - self.last_ping >= KEEPALIVE_S:
            self.port.write(KEEPALIVE)
            self.last_ping = now
        return now - self.last_ok < LOST_S


def probe(sess, rate):
    port = sess.port
    if not sess.prompt():
        stop("no command prompt from the device")
    port.write(b"baud probe 2500\r")
    if sess.pump(3.0, r"BAUD: probe for", hide=True) is None:
        stop("the device did not start the probe (firmware before coh4?)")
    port.drain()
    time.sleep(0.1)
    port.set_baud(rate)
    t0 = time.monotonic()
    while time.monotonic() - t0 < 1.5:
        port.write(b"U" * 64)
        port.drain()
        time.sleep(0.02)
    port.drain()
    time.sleep(0.1)
    port.set_baud(BASE)
    port.buf = b""
    line = sess.pump(6.0, r"BAUD: probe edges=", hide=True)
    port.write(b"log\r")
    if line is None:
        stop("no result line from the device")
    print(line.strip())
    m = re.search(r"edges=(\d+) low=(\d+) high=(\d+).*clock=(\d+) est=(\d+)", line)
    if m:
        edges, lo, hi, clk, est = (int(x) for x in m.groups())
        print("sent 'U' at %d baud; the device counted %d edges and estimates %d baud (%+.1f %%)"
              % (rate, edges, est, 100.0 * (est - rate) / rate if est else 0.0))


def main():
    ap = argparse.ArgumentParser(description="Log the ADS-B Scope console at a negotiated speed.", add_help=True)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--out", default=None)
    ap.add_argument("--seconds", type=float, default=0.0)
    ap.add_argument("--no-status", action="store_true")
    ap.add_argument("--probe", type=int, default=0)
    ap.add_argument("--boot-wait", type=float, default=60.0, help=argparse.SUPPRESS)
    a = ap.parse_args()

    # Everything this needs, checked before the port is touched
    if sys.version_info < (3, 6):
        stop("Python 3.6 or later is needed (this is %s)" % sys.version.split()[0])
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        stop("this runs on Linux and macOS (on Windows: WSL with the port attached, or the web app)")
    for b in (a.baud, a.probe or BASE):
        if b not in RATES:
            stop("%d is not a speed the device offers: %s" % (b, " ".join(map(str, RATES))))
    if not os.path.exists(a.port):
        stop("%s does not exist – is the device plugged in? (ls /dev/ttyACM* /dev/ttyUSB*)" % a.port)
    if not os.access(a.port, os.R_OK | os.W_OK):
        stop("no permission for %s – add yourself to its group (sudo usermod -aG dialout $USER, then log in again)" % a.port)
    out_path = a.out or time.strftime("adsb-log-%Y%m%d-%H%M%S.txt")
    out = None
    if not a.probe:
        try:
            out = open(out_path, "w", buffering=1 << 16)
        except OSError as e:
            stop("cannot write %s: %s" % (out_path, e))

    stats = {"lines": 0, "bytes": 0}

    def sink(line):
        stats["lines"] += 1
        stats["bytes"] += len(line) + 1
        if out:
            out.write(line + "\n")

    try:
        port = Port(a.port)
    except OSError as e:
        stop("cannot open %s: %s" % (a.port, e))
    sess = Session(port, sink)
    done = {"flag": False}
    signal.signal(signal.SIGINT, lambda *_: done.__setitem__("flag", True))
    signal.signal(signal.SIGTERM, lambda *_: done.__setitem__("flag", True))

    say("%s open at %d; waiting for the console (a T-Display-P4 restarts when the port is opened)" % (a.port, BASE))
    has_baud = sess.sync(a.boot_wait)
    if has_baud is None:
        port.close()
        stop("nothing readable from %s in %d s – wrong port, or the device is waiting in download mode (press RESET)" % (a.port, a.boot_wait))

    if a.probe:
        if not has_baud:
            stop("this firmware does not answer the keepalive (before coh4): no probe")
        probe(sess, a.probe)
        port.write(GOODBYE)
        port.close()
        return

    rate = BASE
    if not has_baud:
        say("the console does not answer the keepalive (firmware before coh4) – logging at %d" % BASE)
    elif a.baud > BASE:
        rate = sess.negotiate(a.baud)
    say("logging to %s – Ctrl+C to stop" % out_path)

    t_start = t_report = time.monotonic()
    relinks = 0
    last_lines = 0
    while not done["flag"]:
        sess.pump(0.2)
        now = time.monotonic()
        if has_baud and not sess.keepalive():
            # The device stopped answering: it restarted, or it fell back. Meet it at 115200 again.
            relinks += 1
            say("the device stopped answering at %d – back to %d to find it again" % (port.baud, BASE))
            port.set_baud(BASE)
            port.buf = b""
            rate = BASE
            if sess.sync(a.boot_wait) and a.baud > BASE and relinks <= 5:
                rate = sess.negotiate(a.baud)
            sess.last_ok = time.monotonic()
        if now - t_report >= 10.0:
            say("%d lines (%.0f a second), %d baud" % (stats["lines"], (stats["lines"] - last_lines) / (now - t_report), port.baud))
            t_report, last_lines = now, stats["lines"]
        if a.seconds and now - t_start >= a.seconds:
            break

    # Finish: the device's own account of what it sent and dropped, then let it go back to 115200
    if has_baud and not a.no_status and sess.prompt():
        for cmd in ("adsb status", "baud"):
            reply = sess.command(cmd, 5.0)
            if reply is not None and out:
                out.write("\n===== %s =====\n" % cmd + "\n".join(reply) + "\n")
        port.write(b"log\r")
        port.drain()
        time.sleep(0.2)
    if has_baud:
        port.write(GOODBYE)
        port.drain()
        time.sleep(0.2)
    port.close()
    if out:
        out.close()
    say("%d lines, %d bytes in %.0f s -> %s" % (stats["lines"], stats["bytes"], time.monotonic() - t_start, out_path))


if __name__ == "__main__":
    main()
