#!/usr/bin/env python3
"""Zen 2 follow-up: count retired locked ops and store-to-load interlocks."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from perf_probe import controlled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perf", required=True)
    parser.add_argument("--reference", type=Path, required=True)
    args = parser.parse_args()
    reference = args.reference.resolve()
    info = json.loads((reference / "environment.json").read_text())
    directory = reference / "atomics"
    directory.mkdir(exist_ok=True)
    names = ["cycles", "ls_locks.non_spec_lock", "ls_locks.spec_lock_hi_spec", "ls_locks.spec_lock_lo_spec",
             "ls_locks.bus_lock", "ls_bad_status2.stli_other"]
    events = ",".join(name + ":u" for name in names)
    selected = {"uring-staged-current", "uring-staged-terminal", "uring-staged-terminal_view",
                "dpdk-1-current", "dpdk-1-terminal_view", "dpdk-32-raw", "dpdk-32-current",
                "dpdk-32-terminal_view"}
    lanes = [lane for lane in info["lanes"] if lane[0] in selected]
    with (directory / "event-descriptions.txt").open("w") as stream:
        subprocess.run([args.perf, "list", "--details", "ls_locks*", "ls_bad_status2*"], stdout=stream,
                       stderr=stream, check=True)
    (directory / "environment.json").write_text(json.dumps(info | {"events": events, "lanes": lanes}, indent=2) + "\n")
    for repeat in range(info["repeats"]):
        order = lanes if repeat % 2 == 0 else list(reversed(lanes))
        for label, command, extra in order:
            controlled(args.perf, ["stat", "-x", ";", "-e", "{" + events + "}", "-o",
                       str(directory / f"{label}-{repeat}.stat")], command, os.environ | extra,
                       directory / f"{label}-{repeat}.log")
        print(f"atomic counters {repeat + 1}/{info['repeats']} completed", flush=True)


if __name__ == "__main__":
    main()
