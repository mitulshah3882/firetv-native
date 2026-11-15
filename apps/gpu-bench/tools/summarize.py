#!/usr/bin/env python3
"""Turns gpu-bench results (BENCH|... lines collected by run-bench.sh) into markdown tables.

    apps/gpu-bench/tools/summarize.py run1/results.psv [run2/results.psv ...] > tables.md

With several runs, each row shows the median of the per-run medians and the run-to-run spread
((max - min) / median across runs); with one run, the spread of its three repetitions.

SPDX-FileCopyrightText: 2026 Mitul Shah
SPDX-License-Identifier: MIT
"""
import statistics
import sys
from collections import OrderedDict


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            p = line.rstrip("\n").split("|")
            if len(p) < 10 or p[0] != "BENCH":
                continue
            rows.append({"group": p[1], "name": p[2], "params": p[3], "med": float(p[4]), "min": float(p[5]),
                         "max": float(p[6]), "cpu": float(p[7]), "iters": int(p[8]), "derived": "|".join(p[9:])})
    return rows


def main():
    runs = [load(p) for p in sys.argv[1:]]
    table = OrderedDict()
    for run in runs:
        for r in run:
            key = (r["group"], r["name"], r["params"])
            table.setdefault(key, []).append(r)
    groups = OrderedDict()
    for key, rs in table.items():
        groups.setdefault(key[0], []).append((key, rs))
    for g, items in groups.items():
        print(f"\n### {g}\n")
        print("| test | params | median ms | spread | cpu ms | summary (first run) |")
        print("|---|---|---|---|---|---|")
        for (grp, name, params), rs in items:
            meds = [r["med"] for r in rs]
            m = statistics.median(meds)
            if len(rs) > 1:
                spread = (max(meds) - min(meds)) / m * 100 if m else 0
            else:
                spread = (rs[0]["max"] - rs[0]["min"]) / m * 100 if m else 0
            cpu = statistics.median(r["cpu"] for r in rs)
            print(f"| {name} | {params} | {m:.3f} | {spread:.0f}% | {cpu:.2f} | {rs[0]['derived']} |")


if __name__ == "__main__":
    main()
