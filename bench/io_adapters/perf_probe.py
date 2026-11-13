#!/usr/bin/env python3
"""Profile adapter variants with user-space hardware counters gated after warmup."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"expected one occurrence: {old}")
    return source.replace(old, new, 1)


def build(reference, directory, backend):
    metadata = json.loads((reference / "environment.json").read_text())
    binaries = {}
    for variant, recipe in zip(metadata["variants"], metadata["commands"]):
        source = (reference / f"{variant}.cpp").read_text()
        source = '#include "perf_window.h"\n' + source
        start = "const auto start = probe::clock::now();"
        end = ("elapsed = probe::ns(start, probe::clock::now()) / count;" if backend == "uring" else
               "const auto elapsed = probe::ns(start, probe::clock::now()) / bursts;")
        source = replace_once(source, start, "probe_perf::begin();\n" + start)
        source = replace_once(source, end, end + "\nprobe_perf::end();")
        if backend == "dpdk":
            source = replace_once(source, "const unsigned mode = 4;",
                                  'const unsigned mode = std::stoul(std::getenv("FF_PERF_MODE"));')
            source = replace_once(source, "run_cases<1>(port, pool, bursts, repeats);",
                'if (std::string(std::getenv("FF_PERF_BATCH")) == "1") run_cases<1>(port, pool, bursts, repeats);')
            source = replace_once(source, "run_cases<32>(port, pool, bursts, repeats);",
                'if (std::string(std::getenv("FF_PERF_BATCH")) == "32") run_cases<32>(port, pool, bursts, repeats);')
        generated = directory / f"{backend}-{variant}.cpp"
        generated.write_text(source)
        binary = directory / f"{backend}-{variant}"
        command = [str(generated) if arg == str(reference / f"{variant}.cpp") else arg for arg in recipe]
        if str(generated) not in command:
            raise ValueError("reference must be an absolute path from this checkout/host")
        command[command.index("-o") + 1] = str(binary)
        subprocess.run(command, check=True)
        binaries[variant] = str(binary)
    return binaries


def controlled(perf, options, command, env, log):
    ctl_read, ctl_write = os.pipe()
    ack_read, ack_write = os.pipe()
    child_env = env | {"FF_PERF_CONTROL_FD": str(ctl_write), "FF_PERF_ACK_FD": str(ack_read)}
    invocation = [perf, *options, "--delay=-1", f"--control=fd:{ctl_read},{ack_write}", "--", *command]
    try:
        with log.open("w") as stream:
            subprocess.run(invocation, env=child_env, pass_fds=(ctl_read, ctl_write, ack_read, ack_write),
                           stdout=stream, stderr=stream, check=True, timeout=180)
    finally:
        for fd in (ctl_read, ctl_write, ack_read, ack_write):
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perf", required=True)
    parser.add_argument("--uring", type=Path, required=True)
    parser.add_argument("--dpdk", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=3)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--count", type=int, default=16777216)
    parser.add_argument("--record-only", action="store_true", help="reuse built binaries and completed stat trials")
    args = parser.parse_args()
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if args.record_only:
        previous = json.loads((directory / "environment.json").read_text())
        if (previous["count"], previous["repeats"], previous["cpu"]) != (args.count, args.repeats, args.cpu):
            raise ValueError("record-only arguments must match the completed stat run")
        bins = {backend: {variant: str(directory / f"{backend}-{variant}") for variant in
                          ("current", "view", "terminal", "terminal_view")} for backend in ("uring", "dpdk")}
    else:
        bins = {"uring": build(args.uring.resolve(), directory, "uring"),
                "dpdk": build(args.dpdk.resolve(), directory, "dpdk")}
    lanes = []
    for path in ("staged", "kernel"):
        for lane in ("raw", "context", "current", "view", "terminal", "terminal_view"):
            variant = lane if lane in bins["uring"] else "current"
            mode = lane if lane in ("raw", "context") else "fast"
            lanes.append((f"uring-{path}-{lane}", [bins["uring"][variant], path, mode,
                          str(args.count), "32", str(args.cpu)], {}))
    for batch in (1, 32):
        for lane in ("raw", "context", "current", "view", "terminal", "terminal_view"):
            variant = lane if lane in bins["dpdk"] else "current"
            mode = {"raw": 0, "context": 2}.get(lane, 4)
            lanes.append((f"dpdk-{batch}-{lane}", [bins["dpdk"][variant], str(args.count), "1", str(args.cpu)],
                          {"FF_PERF_BATCH": str(batch), "FF_PERF_MODE": str(mode)}))
    info = {"perf": subprocess.check_output([args.perf, "version"], text=True).strip(),
            "count": args.count, "repeats": args.repeats, "cpu": args.cpu,
            "events": "cycles:u,instructions:u,branches:u,branch-misses:u", "lanes": lanes}
    (directory / "environment.json").write_text(json.dumps(info, indent=2) + "\n")
    for repeat in range(0 if args.record_only else args.repeats):
        order = lanes[repeat:] + lanes[:repeat]
        if repeat % 2:
            order.reverse()
        for label, command, extra in order:
            controlled(args.perf, ["stat", "-x", ";", "-e", info["events"],
                       "-o", str(directory / f"{label}-{repeat}.stat")], command,
                       os.environ | extra, directory / f"{label}-{repeat}.log")
        print(f"stat {repeat + 1}/{args.repeats} completed", flush=True)
    profiles = {"uring-staged-current", "uring-staged-terminal", "uring-staged-terminal_view",
                "uring-kernel-current", "uring-kernel-terminal_view", "dpdk-1-current",
                "dpdk-1-terminal_view", "dpdk-32-raw", "dpdk-32-current", "dpdk-32-terminal_view"}
    for label, command, extra in lanes:
        if label not in profiles:
            continue
        command = command[:]
        # Longer userspace profiles; 32-packet DPDK already has ample samples.
        count_index = 3 if label.startswith("uring") else 1
        command[count_index] = str(args.count * (1 if label.startswith("dpdk-32") else 4))
        data = directory / f"{label}.data"
        controlled(args.perf, ["record", "-e", "cycles:u", "-F", "997", "--call-graph", "dwarf,16384",
                   "--user-callchains",
                   "-o", str(data)], command, os.environ | extra, directory / f"{label}-record.log")
        for suffix, options in (("self", ["--no-children"]), ("children", ["--children"])):
            with (directory / f"{label}-{suffix}.txt").open("w") as report:
                subprocess.run([args.perf, "report", "-i", str(data), "--stdio", "--stdio-color", "never",
                                "--percent-limit", "0.5", "--sort", "dso,symbol", "--call-graph", "none",
                                "-n", *options], stdout=report, stderr=report, check=True)
        print(f"record {label} completed", flush=True)


if __name__ == "__main__":
    main()
