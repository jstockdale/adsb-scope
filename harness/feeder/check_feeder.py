#!/usr/bin/env python3
"""Validate the MQTT feeder's JSON with an independent parser.

Runs harness/test_batch (C tests of main/examples/adsb_receiver/mqtt_batch.c)
and checks every message it produced:

  - it parses as JSON, and its length is what the serialiser reported and
    below the buffer size it was given;
  - the envelope is {"aircraft":[...]} plus rx_lat/rx_lon exactly when a
    valid receiver position was supplied;
  - across the messages of one batch every aircraft appears exactly once,
    in order, with the ICAO text it was given;
  - every entry uses only the feeder's keys, in the feeder's order, with
    values of the right kind and range, and a position exactly when the
    input had a valid one.

Usage: check_feeder.py [path to test_batch]     (default ./test_batch)
"""
import json
import math
import subprocess
import sys

KEY_ORDER = ["icao", "callsign", "lat", "lon", "alt", "speed", "hdg", "vr", "dist", "brg"]
INT_RANGE = {"alt": (-2**31, 2**31 - 1), "speed": (-32768, 32767), "hdg": (0, 32767),
             "vr": (-32768, 32767), "brg": (0, 32767)}


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def reject_constant(name):
    raise ValueError("non-standard JSON constant " + name)


def check_entry(cid, pairs, want_icao, want_pos):
    keys = [k for k, _ in pairs]
    if len(set(keys)) != len(keys):
        fail(f"case {cid}: duplicate key in {keys}")
    if [k for k in KEY_ORDER if k in keys] != keys:
        fail(f"case {cid}: unexpected keys or order {keys}")
    e = dict(pairs)
    if "icao" not in e or not isinstance(e["icao"], str):
        fail(f"case {cid}: entry without icao")
    try:
        raw = e["icao"].encode("latin-1")
    except UnicodeEncodeError:
        fail(f"case {cid}: icao has a character above U+00FF")
    if raw.hex() != want_icao:
        fail(f"case {cid}: icao {raw.hex()} where {want_icao} was expected")
    if "callsign" in e and (not isinstance(e["callsign"], str) or not 1 <= len(e["callsign"]) <= 8):
        fail(f"case {cid}: bad callsign {e['callsign']!r}")
    has_pos = "lat" in e
    if has_pos != ("lon" in e):
        fail(f"case {cid}: lat without lon or the reverse")
    if has_pos != want_pos:
        fail(f"case {cid}: position present={has_pos}, expected {want_pos}")
    if has_pos:
        if not (-90.0 <= e["lat"] <= 90.0 and -180.0 <= e["lon"] <= 180.0):
            fail(f"case {cid}: position out of range {e['lat']}, {e['lon']}")
    for k, (lo, hi) in INT_RANGE.items():
        if k in e and (type(e[k]) is not int or not lo <= e[k] <= hi):
            fail(f"case {cid}: {k} = {e[k]!r}")
    if "dist" in e and not (isinstance(e["dist"], (int, float)) and math.isfinite(e["dist"]) and 0 <= e["dist"] < 100000):
        fail(f"case {cid}: dist = {e['dist']!r}")


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else "./test_batch"
    try:
        run = subprocess.run([exe], capture_output=True)
    except OSError as err:
        fail(f"cannot run {exe}: {err} (build it with: make test_batch)")
    sys.stderr.write(run.stderr.decode("ascii", "replace"))
    if run.returncode != 0:
        fail(f"{exe} reported {run.returncode} failed C-level checks")
    try:
        text = run.stdout.decode("ascii")       # the serialiser promises pure ASCII
    except UnicodeDecodeError:
        fail("output is not pure ASCII")

    cases = messages = entries = 0
    cur = None
    for line in text.split("\n"):
        if not line:
            continue
        tag, _, rest = line.partition(" ")
        if tag == "CASE":
            cid, count, cap, have_rx = (int(x) for x in rest.split())
            cur = {"id": cid, "count": count, "cap": cap, "rx": have_rx, "expect": [], "got": [], "rx_seen": None}
        elif tag == "E":
            cid, icao_hex, pos = rest.split()
            cur["expect"].append(("" if icao_hex == "-" else icao_hex, pos == "1"))
        elif tag == "M":
            cid, length, js = rest.split(" ", 2)
            cid, length = int(cid), int(length)
            if cid != cur["id"]:
                fail(f"message for case {cid} inside case {cur['id']}")
            if len(js) != length:
                fail(f"case {cid}: reported length {length}, text is {len(js)}")
            if length >= cur["cap"]:
                fail(f"case {cid}: {length} characters do not fit a {cur['cap']}-byte buffer")
            try:
                top = json.loads(js, object_pairs_hook=list, parse_constant=reject_constant)
            except ValueError as err:
                fail(f"case {cid}: not valid JSON ({err}): {js[:200]}")
            keys = [k for k, _ in top]
            if keys not in (["aircraft"], ["aircraft", "rx_lat", "rx_lon"]):
                fail(f"case {cid}: envelope keys {keys}")
            has_rx = len(keys) == 3
            if has_rx and not cur["rx"]:
                fail(f"case {cid}: receiver position sent though none was supplied")
            if cur["rx_seen"] is not None and cur["rx_seen"] != has_rx:
                fail(f"case {cid}: receiver position in some messages of a batch only")
            cur["rx_seen"] = has_rx
            if has_rx:
                rx = dict(top)
                if not (-90.0 <= rx["rx_lat"] <= 90.0 and -180.0 <= rx["rx_lon"] <= 180.0):
                    fail(f"case {cid}: receiver position out of range")
            if not top[0][1]:
                fail(f"case {cid}: message with no aircraft")
            cur["got"].extend(top[0][1])
            messages += 1
        elif tag == "END":
            if len(cur["got"]) != len(cur["expect"]) or len(cur["expect"]) != cur["count"]:
                fail(f"case {cur['id']}: {len(cur['got'])} entries for {cur['count']} aircraft")
            for pairs, (icao_hex, pos) in zip(cur["got"], cur["expect"]):
                check_entry(cur["id"], pairs, icao_hex, pos)
            entries += len(cur["got"])
            cases += 1
            cur = None
        else:
            fail(f"unexpected output line: {line[:80]}")
    if cur is not None or cases == 0:
        fail("incomplete output")
    print(f"feeder JSON: {cases} batches, {messages} messages, {entries} aircraft entries: all valid, "
          "each aircraft exactly once")


if __name__ == "__main__":
    main()
