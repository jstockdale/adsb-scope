#!/usr/bin/env python3
"""check_fastlog.py - tools/adsb-fastlog.py and the console-speed protocol
against the simulated device (sim_device, which runs the firmware's own
state machine): the cases a host meets, without hardware.

usage: check_fastlog.py [--keep]     (make check runs it; about 2 minutes)
needs: ./sim_device built (make), python3
"""
import os, re, select, signal, subprocess, sys, tempfile, termios, time

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "..", "sim_device")
TOOL = os.path.join(HERE, "..", "..", "tools", "adsb-fastlog.py")
fails = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        fails.append(what)


class Sim:
    def __init__(self, *args):
        self.p = subprocess.Popen([SIM, *args], stdout=subprocess.PIPE, text=True)
        self.path = self.p.stdout.readline().strip()

    def stop(self):
        self.p.kill()
        self.p.wait()


def run_tool(sim, *args, stall=None, timeout=120):
    """run the logger; returns (exit code, stderr text, log file text)"""
    out = tempfile.NamedTemporaryFile(suffix=".txt", delete=False).name
    p = subprocess.Popen([sys.executable, TOOL, "--port", sim.path, "--out", out, "--boot-wait", "20", *args],
                         stderr=subprocess.PIPE, text=True)
    if stall:
        time.sleep(stall[0]); p.send_signal(signal.SIGSTOP)
        time.sleep(stall[1]); p.send_signal(signal.SIGCONT)
    try:
        _, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill(); _, err = p.communicate()
        err += "\n(killed: timeout)"
    text = open(out, errors="replace").read() if os.path.exists(out) else ""
    os.unlink(out)
    return p.returncode, err, text


def seqs(text):
    return [int(m) for m in re.findall(r"seq=(\d+) \[8D", text)]


def tail_gaps(text, n):
    """how many of the last n sequence numbers are missing"""
    s = seqs(text)[-n:]
    return (s[-1] - s[0] + 1 - len(s)) if len(s) > 1 else 10**9


class Raw:
    """a bare client: what a web page does, with a reopen where it needs one"""
    def __init__(self, path, baud=115200):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.buf = ""
        self.speed(baud)

    def speed(self, baud):
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[4] = a[5] = getattr(termios, "B%d" % baud)
        termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def send(self, b):
        os.write(self.fd, b)

    def expect(self, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    self.buf += os.read(self.fd, 65536).decode("utf-8", "replace")
                except OSError:
                    pass
            m = re.search(pattern, self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
        return None

    def close(self):
        os.close(self.fd)


def main():
    if not os.path.exists(SIM):
        sys.exit("error: %s not built (run make in harness/)" % SIM)

    print("1. 300 lines a second, logger at 921600")
    sim = Sim("--rate", "300")
    rc, err, text = run_tool(sim, "--baud", "921600", "--seconds", "6")
    check(rc == 0, "logger exits cleanly")
    check("link up at 921600" in err, "link came up at 921600")
    check(tail_gaps(text, 600) == 0, "no line missing among the last 600 (%d missing)" % tail_gaps(text, 600))
    check("===== adsb status =====" in text and "921600 baud" in text, "the device's own status is appended, taken at 921600")
    check("BAUD: ok" not in text, "keepalive answers stay out of the log")
    raw = Raw(sim.path)
    raw.send(b"\x05")
    check(raw.expect(r"BAUD: ok 115200", 1.5) is not None, "after the logger left, the device is back at 115200 at once")
    raw.close(); sim.stop()

    print("2. the same traffic, logger kept at 115200")
    sim = Sim("--rate", "300")
    rc, err, text = run_tool(sim, "--baud", "115200", "--seconds", "6")
    s = seqs(text)[-200:]
    lost = 1 - len(s) / (s[-1] - s[0] + 1) if len(s) > 1 else 0
    check(rc == 0 and lost > 0.5, "at 115200 most lines never arrive (%.0f %% of the last stretch lost)" % (100 * lost))
    sim.stop()

    print("3. a link that does not work above 460800")
    sim = Sim("--rate", "200", "--max-baud", "460800")
    rc, err, text = run_tool(sim, "--baud", "921600", "--seconds", "6")
    check("no confirmation at 921600" in err and "link up at 460800" in err, "921600 is tried, not confirmed, and 460800 comes up")
    check(rc == 0 and tail_gaps(text, 400) == 0, "no line missing among the last 400 at 460800 (%d missing)" % tail_gaps(text, 400))
    sim.stop()

    print("4. firmware without `baud`")
    sim = Sim("--rate", "20", "--old-firmware", "--reset-on-open", "--boot-ms", "1000")
    rc, err, text = run_tool(sim, "--baud", "921600", "--seconds", "4")
    check(rc == 0 and "before coh4" in err, "recognised as older firmware")
    check(len(seqs(text)) > 40 and "ESP-ROM" in text, "logged at 115200, boot log included")
    sim.stop()

    print("5. a device that resets when the port is opened")
    sim = Sim("--rate", "300", "--reset-on-open", "--boot-ms", "1500")
    rc, err, text = run_tool(sim, "--baud", "921600", "--seconds", "6")
    check(rc == 0 and "link up at 921600" in err, "link up at 921600 after the boot")
    check("ESP-ROM:esp32p4" in text and "Serial console ready" in text, "the boot log is in the file")
    check(tail_gaps(text, 600) == 0, "no line missing among the last 600 (%d missing)" % tail_gaps(text, 600))
    sim.stop()

    print("6. the logger stalls for 12 s (the device falls back), then carries on")
    sim = Sim("--rate", "300")
    rc, err, text = run_tool(sim, "--baud", "921600", "--seconds", "32", stall=(6, 12))
    check("stopped answering" in err, "the logger notices the device has gone back to 115200")
    check(err.count("link up at 921600") == 2, "and brings the link up again")
    check(rc == 0 and tail_gaps(text, 600) == 0, "no line missing among the last 600 (%d missing)" % tail_gaps(text, 600))
    sim.stop()

    print("7. the probe")
    sim = Sim("--rate", "20")
    p = subprocess.run([sys.executable, TOOL, "--port", sim.path, "--probe", "921600", "--boot-wait", "20"],
                       capture_output=True, text=True, timeout=60)
    check(p.returncode == 0 and "estimates" in p.stdout, "probe runs and reports: %s" % p.stdout.strip().splitlines()[-1:] )
    sim.stop()

    print("8. a host that must reopen its port (the web page): `baud 921600 next`, on a device that resets on open")
    sim = Sim("--rate", "50", "--reset-on-open", "--boot-ms", "1200")
    c = Raw(sim.path)
    check(c.expect(r"Serial console ready", 5) is not None, "first open: boot at 115200")
    c.send(b"\x03"); c.expect(r"adsb>", 2)
    c.send(b"baud 921600 next\r")
    check(c.expect(r"BAUD: next 921600", 2) is not None, "request stored for the next start")
    c.send(b"log\r"); c.close()
    time.sleep(0.3)
    c = Raw(sim.path, 921600)                 # reopen at the new speed: the device resets
    check(c.expect(r"Serial console ready", 5) is not None, "second open at 921600: the console comes up readable")
    check("ESP-ROM" not in c.buf, "(its boot log, at 115200, was not)")
    c.send(b"\x03"); c.expect(r"adsb>", 2); c.send(b"baud ok\r")
    check(c.expect(r"BAUD: up 921600", 2) is not None, "confirmed: up at 921600")
    c.send(b"log\r")
    c.send(b"\x05")
    check(c.expect(r"BAUD: ok 921600", 2) is not None, "keepalive answered at 921600")
    t0 = time.time()
    c.speed(115200)                            # the page goes quiet: no more keepalives
    m = c.expect(r"BAUD: host silent for \d+ s – back to 115200", 14)
    check(m is not None and 8.0 < time.time() - t0 < 12.5, "silent host: the device is back at 115200 after about 10 s (%.1f s)" % (time.time() - t0))
    c.close(); sim.stop()

    print("9. the request is made but the host comes back at 115200")
    sim = Sim("--rate", "50", "--reset-on-open", "--boot-ms", "1200")
    c = Raw(sim.path)
    c.expect(r"Serial console ready", 5); c.send(b"\x03"); c.expect(r"adsb>", 2)
    c.send(b"baud 921600 next\r"); c.expect(r"BAUD: next 921600", 2); c.send(b"log\r"); c.close()
    time.sleep(0.3)
    c = Raw(sim.path, 115200)
    t0 = time.time()
    check(c.expect(r"BAUD: starting at 921600", 5) is not None, "the device says at 115200 that it is about to start at 921600")
    m = c.expect(r"BAUD: no 'baud ok' at 921600 – back to 115200", 14)
    check(m is not None, "and comes back to 115200 by itself (%.1f s after the open)" % (time.time() - t0))
    c.send(b"\x05")
    check(c.expect(r"BAUD: ok 115200", 2) is not None, "usable at 115200 afterwards")
    c.close()
    c = Raw(sim.path)                          # a later open: the request counted once
    c.expect(r"Serial console ready", 5)
    c.send(b"\x05")
    check(c.expect(r"BAUD: ok 115200", 2) is not None, "the next start is at 115200 again")
    c.close(); sim.stop()

    print("10. the request is made and no reset follows")
    sim = Sim("--rate", "20")
    c = Raw(sim.path)
    c.send(b"\x03"); c.expect(r"adsb>", 2); c.send(b"baud 921600 next\r"); c.expect(r"BAUD: next 921600", 2); c.send(b"log\r")
    check(c.expect(r"BAUD: no reset followed", 18) is not None, "it is dropped after 15 s")
    c.close(); sim.stop()

    print()
    if fails:
        print("check_fastlog: %d FAILED" % len(fails))
        sys.exit(1)
    print("check_fastlog: ok")


if __name__ == "__main__":
    main()
