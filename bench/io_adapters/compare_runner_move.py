#!/usr/bin/env python3
"""Compare a replacement flow_runner.h against this checkout's unchanged copy."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
from perf_probe import controlled, replace_once

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replacement", type=Path, required=True)
    parser.add_argument("--uring", type=Path, required=True)
    parser.add_argument("--dpdk", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--perf", required=True)
    parser.add_argument("--cpu", type=int, default=3)
    parser.add_argument("--count", type=int, default=1048576)
    parser.add_argument("--repeats", type=int, default=7)
    args = parser.parse_args()
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    overlay = directory / "overlay"
    (overlay / "flow").mkdir(parents=True, exist_ok=True)
    for name in ("base", "memory", "task", "utility", "executor", "extension"):
        target = overlay / name
        if not target.exists(): target.symlink_to(ROOT / name, target_is_directory=True)
    for header in (ROOT / "flow").glob("*.h"):
        if header.name != "flow_runner.h" and not (overlay / "flow" / header.name).exists():
            (overlay / "flow" / header.name).symlink_to(header)
    replacement = args.replacement.read_bytes()
    (overlay / "flow/flow_runner.h").write_bytes(replacement)
    before = (ROOT / "flow/flow_runner.h").read_bytes()
    (directory / "before-flow_runner.h").write_bytes(before)
    builds, binaries = [], {}
    for backend, reference in (("uring", args.uring.resolve()), ("dpdk", args.dpdk.resolve())):
        recipes = json.loads((reference / "environment.json").read_text())
        for variant in ("current", "terminal"):
            source = (reference / f"{variant}.cpp").read_text()
            source = '#include "perf_window.h"\n' + source
            start = "const auto start = probe::clock::now();"
            end = ("elapsed = probe::ns(start, probe::clock::now()) / count;" if backend == "uring" else
                   "const auto elapsed = probe::ns(start, probe::clock::now()) / bursts;")
            source = replace_once(source, start, 'if (std::getenv("FF_PERF_CONTROL_FD")) probe_perf::begin();\n' + start)
            source = replace_once(source, end, end + '\nif (std::getenv("FF_PERF_CONTROL_FD")) probe_perf::end();')
            generated = directory / f"{backend}-{variant}.cpp"
            generated.write_text(source)
            recipe = recipes["commands"][recipes["variants"].index(variant)]
            for version in ("before", "after"):
                binary = directory / f"{backend}-{variant}-{version}"
                command = [str(generated) if x == str(reference / f"{variant}.cpp") else x for x in recipe]
                command[command.index("-o") + 1] = str(binary)
                if version == "after": command.insert(1, f"-I{overlay}")
                subprocess.run(command, check=True)
                builds.append(command)
                binaries[backend, variant, version] = str(binary)
    metadata = {"commands": builds, "cpu": args.cpu, "count": args.count, "repeats": args.repeats,
                "before_sha256": hashlib.sha256(before).hexdigest(),
                "after_sha256": hashlib.sha256(replacement).hexdigest()}
    (directory / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    with (directory / "results.csv").open("w") as stream, (directory / "runtime.log").open("w") as log:
        writer = csv.writer(stream)
        writer.writerow(["repeat", "backend", "variant", "version", "path", "batch", "ns_per_unit"])
        for repeat in range(args.repeats):
            variants = ("current", "terminal") if repeat % 2 == 0 else ("terminal", "current")
            versions = ("before", "after") if repeat % 2 == 0 else ("after", "before")
            for variant in variants:
                for version in versions:
                    binary = binaries["uring", variant, version]
                    for path in ("staged", "kernel"):
                        text = subprocess.check_output([binary, path, "fast", str(args.count), "32", str(args.cpu)],
                                                       text=True, stderr=log, timeout=120)
                        row = next(csv.reader(io.StringIO(text)))
                        writer.writerow([repeat, "uring", variant, version, path, 32, row[5]])
                    binary = binaries["dpdk", variant, version]
                    text = subprocess.check_output([binary, str(args.count), "1", str(args.cpu)],
                                                   text=True, stderr=log, timeout=120)
                    for row in csv.DictReader(io.StringIO(text)):
                        writer.writerow([repeat, "dpdk", variant, version, "net_ring", row["batch"], row["ns_per_burst"]])
                    stream.flush()
            print(f"timings {repeat + 1}/{args.repeats} completed", flush=True)
    events = ",".join(x + ":u" for x in ("ls_locks.non_spec_lock", "ls_locks.spec_lock_hi_spec",
                                         "ls_locks.spec_lock_lo_spec", "ls_locks.bus_lock"))
    for backend in ("uring", "dpdk"):
        for variant in ("current", "terminal"):
            for version in ("before", "after"):
                label = f"{backend}-{variant}-{version}"
                binary = binaries[backend, variant, version]
                argv = ([binary, "staged", "fast", str(args.count), "32", str(args.cpu)] if backend == "uring" else
                        [binary, str(args.count), "1", str(args.cpu)])
                # DPDK's first timed phase is batch 1. Later batch-32 phases
                # stay disabled, so these counters cover batch 1 only.
                controlled(args.perf, ["stat", "-x", ";", "-e", "{" + events + "}",
                           "-o", str(directory / (label + ".stat"))], argv, os.environ.copy(),
                           directory / (label + ".log"))
    print("retired-lock verification completed", flush=True)


if __name__ == "__main__":
    main()
