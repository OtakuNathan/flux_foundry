#!/usr/bin/env python3
"""Print medians and full observed ranges, keeping different units separate."""
import argparse
from collections import defaultdict
import csv
from pathlib import Path
from statistics import median


def table(path, keys, metric):
    groups = defaultdict(list)
    with path.open() as stream:
        for row in csv.DictReader(stream):
            groups[tuple(row[key] for key in keys)].append(float(row[metric]))
    print(f"\n### {path.name}: {metric}\n")
    print("| " + " | ".join(keys + ["runs", "median", "min", "max"]) + " |")
    print("|" + "---|" * (len(keys) + 4))
    for group, values in groups.items():
        cells = list(group) + [str(len(values)), f"{median(values):.3f}", f"{min(values):.3f}", f"{max(values):.3f}"]
        print("| " + " | ".join(cells) + " |")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    args = parser.parse_args()
    for directory in args.directories:
        for name, keys, metric in (
            ("uv.csv", ["version", "producers", "capacity", "budget"], "ns_per_op"),
            ("uring.csv", ["path", "mode", "batch"], "ns_per_op"),
            ("dpdk.csv", ["path", "mode", "batch"], "ns_per_burst"),
        ):
            path = directory / name
            if path.exists():
                table(path, keys, metric)


if __name__ == "__main__":
    main()
