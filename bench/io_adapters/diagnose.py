#!/usr/bin/env python3
"""Ablate blueprint ownership and awaitable contract; retain original probes."""
import argparse
import csv
import io
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
VARIANTS = ("current", "view", "terminal", "terminal_view")


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"diagnostic patch expected one occurrence of {old!r}")
    return source.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("uring", "dpdk"), required=True)
    parser.add_argument("--reference", type=Path, required=True, help="directory with prior environment.json")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--count", type=int, default=1048576)
    args = parser.parse_args()
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    recipe = json.loads((args.reference / "environment.json").read_text())
    command = recipe["commands"][-1] if args.backend == "uring" else recipe["command"]
    source_path = ROOT / f"bench/io_adapters/{args.backend}.cpp"
    original = source_path.read_text()
    commands = []
    for variant in VARIANTS:
        source = original
        include = f'#include "extension/{"io_uring" if args.backend == "uring" else "dpdk"}_awaitable.h"'
        source = replace_once(source, include, include + f'\n#define FF_DIAG_{args.backend.upper()}\n#include "diagnostic_terminal.h"')
        if "terminal" in variant:
            old = "await_io_uring<operation>()" if args.backend == "uring" else "await_dpdk_rx<Size>()"
            new = "await<probe::uring_terminal<operation>>()" if args.backend == "uring" else "await<probe::dpdk_terminal<Size>>()"
            source = replace_once(source, old, new)
        if "view" in variant:
            source = replace_once(source, "make_fast_runner(shared,", "make_fast_runner_view(shared.get(),")
        if args.backend == "dpdk":
            source = replace_once(source, "position < 5", "position < 1")
            source = replace_once(source, "const auto mode = (position + repeat) % 5;", "const unsigned mode = 4;")
        generated = directory / f"{variant}.cpp"
        generated.write_text(source)
        binary = directory / variant
        build = [str(generated) if arg == str(source_path) else arg for arg in command]
        if str(generated) not in build:
            raise ValueError("reference compile command does not match this source checkout")
        build[build.index("-o") + 1] = str(binary)
        build += [f"-I{ROOT / 'bench/io_adapters'}", "-g"]
        subprocess.run(build, check=True)
        commands.append(build)
    (directory / "environment.json").write_text(json.dumps({"commands": commands,
        "backend": args.backend, "cpu": args.cpu, "count": args.count, "repeats": args.repeats,
        "variants": list(VARIANTS), "note": "terminal variants change cancellation contract; benchmark only"}, indent=2) + "\n")
    with (directory / "results.csv").open("w") as results, (directory / "runtime.log").open("w") as log:
        writer = csv.writer(results)
        writer.writerow(["repeat", "variant", "path", "batch", "ns_per_unit"])
        for repeat in range(args.repeats):
            variants = VARIANTS[repeat % 4:] + VARIANTS[:repeat % 4]
            for variant in variants:
                binary = str(directory / variant)
                if args.backend == "uring":
                    for path in ("staged", "kernel"):
                        text = subprocess.check_output([binary, path, "fast", str(args.count), "32", str(args.cpu)],
                                                       text=True, stderr=log, timeout=120)
                        row = next(csv.reader(io.StringIO(text)))
                        writer.writerow([repeat, variant, path, row[4], row[5]])
                else:
                    text = subprocess.check_output([binary, str(args.count), "1", str(args.cpu)],
                                                   text=True, stderr=log, timeout=120)
                    for row in csv.DictReader(io.StringIO(text)):
                        writer.writerow([repeat, variant, row["path"], row["batch"], row["ns_per_burst"]])
                results.flush()
            print(f"{args.backend}: completed {repeat + 1}/{args.repeats}", flush=True)


if __name__ == "__main__":
    main()
