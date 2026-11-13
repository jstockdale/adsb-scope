#!/usr/bin/env python3
"""How much of the traffic does the MQTT feeder uplink? Old feeder against r7.

A model, not a measurement. Each aircraft produces decoded messages as a
Poisson process at its own rate; the feeder flushes every 2 s.

  old (up to r6)  a 64-entry buffer filled with one entry per decoded
                  message, first come first served; messages arriving after
                  it is full are dropped until the next flush.
  r7              the latest state of every aircraft heard since the last
                  flush, each aircraft once (256 slots).

Two assumptions about message rates, because the real mix depends on the
site and is not known here:

  even    every aircraft is decoded 2 times a second
  mixed   rates spread log-uniformly from 0.1 to 6 per second (mean 1.44):
          a few strong nearby aircraft, many weak distant ones

Reported per aircraft count N:
  msg/s      decoded messages per second in total
  in batch   average share of the N aircraft present in a batch
  interval   average time between uplinked updates of one aircraft, taken
             over aircraft (median aircraft / worst tenth)
  >30 s      aircraft that had at least one gap longer than 30 s between
             uplinked updates during the run
The r7 columns show what the decoded stream itself allows: an aircraft
heard once in 40 s has a 40 s gap whatever the feeder does.

Usage: coverage_sim.py [seconds]        (default 600; needs only Python 3)
"""
import math
import random
import sys

FLUSH_S = 2.0
OLD_SLOTS = 64
NEW_SLOTS = 256


def rates_for(kind, n, rng):
    if kind == "even":
        return [2.0] * n
    lo, hi = math.log(0.1), math.log(6.0)
    return [math.exp(rng.uniform(lo, hi)) for _ in range(n)]


def simulate(kind, n, seconds, seed):
    rng = random.Random(seed)
    rates = rates_for(kind, n, rng)
    events = []
    for i, r in enumerate(rates):
        t = rng.expovariate(r)
        while t < seconds:
            events.append((t, i))
            t += rng.expovariate(r)
    events.sort()

    windows = int(seconds / FLUSH_S)
    res = {}
    for name in ("old", "new"):
        res[name] = {"last": [0.0] * n, "maxgap": [0.0] * n, "count": [0] * n, "share": 0.0}
    pos = 0
    for w in range(windows):
        t_flush = (w + 1) * FLUSH_S
        old_set, new_set, taken = set(), set(), 0
        while pos < len(events) and events[pos][0] < t_flush:
            i = events[pos][1]
            if taken < OLD_SLOTS:
                taken += 1
                old_set.add(i)
            if i in new_set or len(new_set) < NEW_SLOTS:
                new_set.add(i)
            pos += 1
        for name, s in (("old", old_set), ("new", new_set)):
            r = res[name]
            r["share"] += len(s) / n
            for i in s:
                gap = t_flush - r["last"][i]
                if gap > r["maxgap"][i]:
                    r["maxgap"][i] = gap
                r["last"][i] = t_flush
                r["count"][i] += 1
    out = {"msgs": len(events) / seconds}
    for name in ("old", "new"):
        r = res[name]
        for i in range(n):              # the gap still open at the end counts too
            gap = seconds - r["last"][i]
            if gap > r["maxgap"][i]:
                r["maxgap"][i] = gap
        intervals = sorted(seconds / c if c else float("inf") for c in r["count"])
        out[name] = {
            "share": r["share"] / windows,
            "median": intervals[n // 2],
            "p90": intervals[min(n - 1, int(n * 0.9))],
            "gap30": sum(1 for g in r["maxgap"] if g > 30.0),
        }
    return out


def fmt_s(x):
    return "  never" if math.isinf(x) else f"{x:6.1f}s"


def main():
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 600
    print(f"MQTT feeder coverage model, {seconds} s per run, flush every {FLUSH_S:.0f} s (see coverage_sim.py for the assumptions)")
    for kind in ("even", "mixed"):
        print()
        print(f"Message rates: {kind}")
        print("     N   msg/s | old: in batch   interval (median / worst tenth)   >30 s | r7: in batch   interval (median / worst tenth)   >30 s")
        for n in (10, 20, 40, 60, 80, 100, 150, 200):
            r = simulate(kind, n, seconds, seed=1000 + n)
            o, w = r["old"], r["new"]
            print(f"  {n:4d}  {r['msgs']:6.0f} |      {o['share']*100:5.1f}%          {fmt_s(o['median'])} / {fmt_s(o['p90'])}"
                  f"            {o['gap30']:4d}  |     {w['share']*100:5.1f}%          {fmt_s(w['median'])} / {fmt_s(w['p90'])}"
                  f"            {w['gap30']:4d}")


if __name__ == "__main__":
    main()
