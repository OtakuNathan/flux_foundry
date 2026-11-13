#!/usr/bin/env python3
"""Normalize gated perf stat counters per request/burst, retaining all trials."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
from statistics import median


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory
    metadata = json.loads((directory / "environment.json").read_text())
    groups = defaultdict(lambda: defaultdict(list))
    rows = []
    stats = directory / "stats" if (directory / "stats").is_dir() else directory
    for path in sorted(stats.glob("*.stat")):
        label, repeat = path.stem.rsplit("-", 1)
        values = {}
        for line in path.read_text().splitlines():
            cells = line.split(";")
            if len(cells) < 5 or cells[2] not in metadata["events"].split(","):
                continue
            if cells[0].startswith("<"):
                raise ValueError(f"counter unavailable: {path}: {line}")
            running = float(cells[4])
            if running < 99.0:
                raise ValueError(f"multiplexed counter: {path}: {line}")
            values[cells[2]] = float(cells[0]) / metadata["count"]
        if set(values) != set(metadata["events"].split(",")):
            raise ValueError(f"missing event: {path}")
        for key, value in values.items():
            groups[label][key].append(value)
        rows.append({"lane": label, "repeat": repeat, **values})
    with (directory / "normalized.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=["lane", "repeat", *metadata["events"].split(",")])
        writer.writeheader()
        writer.writerows(rows)
    print("| lane | trials | cycles/unit | instructions/unit | IPC | branch misses/unit |")
    print("|---|---:|---:|---:|---:|---:|")
    for label, values in groups.items():
        cycles, instructions = median(values["cycles:u"]), median(values["instructions:u"])
        print(f"| {label} | {len(values['cycles:u'])} | {cycles:.2f} | {instructions:.2f} | "
              f"{instructions / cycles:.2f} | {median(values['branch-misses:u']):.5f} |")


if __name__ == "__main__":
    main()
