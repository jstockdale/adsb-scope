#!/usr/bin/env python3
"""count_es.py - summarise decoder output with no ground truth available.
usage: count_es.py decoded.txt [label]"""
import sys
msgs = []
for line in open(sys.argv[1]):
    if line.startswith("MSG "):
        msgs.append(dict(kv.split("=", 1) for kv in line[4:].split()))
es = [m for m in msgs if m["bits"] == "112" and m["df"] in ("17", "18")]
hexes = [m["hex"] for m in es]
f1 = sum(1 for m in es if 0 <= int(m["err"]) < 256)
f2 = sum(1 for m in es if int(m["err"]) >= 256)
label = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1]
print(f"{label:34s} all={len(msgs):5d} ES={len(es):5d} (DF18 {sum(m['df']=='18' for m in es)}) "
      f"unique={len(set(hexes)):5d} aircraft={len({h[2:8] for h in hexes}):3d} 1bit-fixed={f1:3d} 2bit-fixed={f2:3d}")
