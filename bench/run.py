#!/usr/bin/env python3
"""Portable, dependency-free core benchmarks, with optional libuv probes."""
import argparse
import csv
import datetime
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tarfile

from trace_samples import summarize as summarize_trace


ROOT = Path(__file__).resolve().parents[1]


def positive(value):
    n = int(value)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def capture(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, help="new result directory (default: build/bench-TIMESTAMP)")
    parser.add_argument("--trials", type=positive, default=3)
    parser.add_argument("--samples", type=positive, default=32768)
    parser.add_argument("--count", type=positive, default=1000000, help="tasks per UV throughput trial")
    parser.add_argument("--uv", choices=("auto", "on", "off"), default="auto")
    parser.add_argument("--skip-flow", action="store_true", help="only run cross-thread probes")
    parser.add_argument("--trace", action="store_true", help="export scheduled-event timelines after timing, then compress and summarize")
    args = parser.parse_args()
    if args.samples > 1000000:
        parser.error("--samples must be <= 1000000")
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    extra = shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    uv = args.uv != "off" and shutil.which("pkg-config") is not None
    uv = uv and subprocess.run(["pkg-config", "--exists", "libuv"]).returncode == 0
    if args.uv == "on" and not uv:
        parser.error("libuv not found through pkg-config; on macOS: brew install libuv pkg-config")
    out = (args.out or ROOT / "build" / datetime.datetime.now().strftime("bench-%Y%m%d-%H%M%S-%f")).resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []
    traces = []
    metadata = {
        "platform": platform.platform(), "machine": platform.machine(),
        "logical_cpus": os.cpu_count(), "compiler": capture(compiler + ["--version"]),
        "revision": capture(["git", "rev-parse", "HEAD"]),
        "git_status": capture(["git", "status", "--short"]),
        "arguments": vars(args).copy(), "affinity": "unpinned", "uv_enabled": bool(uv),
        "commands": commands,
    }
    metadata["arguments"]["out"] = str(out)
    if platform.system() == "Darwin":
        metadata["hardware"] = capture(["sysctl", "-n", "machdep.cpu.brand_string"])
    if uv:
        metadata["libuv"] = capture(["pkg-config", "--modversion", "libuv"])

    def run(command, name, trace=False):
        commands.append(command)
        (out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print("  " + name, flush=True)
        env = os.environ.copy()
        env.pop("FF_BENCH_TRACE_PREFIX", None)
        if trace:
            env["FF_BENCH_TRACE_PREFIX"] = str(out / name)
        with (out / (name + ".log")).open("w") as log, (out / (name + ".stderr")).open("w") as err:
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=err, check=True, env=env)
        if trace:
            files = sorted(out.glob(name + "-*.samples.csv"))
            if len(files) != (3 if name.startswith("queue-") else 1):
                raise RuntimeError("missing sample traces for " + name)
            for file in files:
                print("    analyze " + file.name, flush=True)
                traces.append(summarize_trace(file))
        return (out / (name + ".log")).read_text()

    flags = ["-std=c++14", "-O3", "-DNDEBUG", "-pthread", "-I" + str(ROOT)]
    noexc = ["-fno-exceptions", "-fno-rtti", "-DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1"]

    def build(name, source, additions=(), libs=()):
        binary = str(out / name)
        run(compiler + flags + list(additions) + extra + [str(ROOT / source), "-o", binary] + list(libs), "build-" + name)
        return binary

    print("Results: " + str(out), flush=True)
    print("Unpinned threads; release builds; current default allocator policy.", flush=True)
    flows = []
    if not args.skip_flow:
        flows = [build("flow-exceptions", "test/flow_perf.cpp"),
                 build("flow-noexcept", "test/flow_perf_noexcept.cpp", noexc)]
    queue = build("queue", "bench/queue_capacity/queue_capacity.cpp", noexc)
    if uv:
        uvflags = shlex.split(capture(["pkg-config", "--cflags", "libuv"]))
        uvlibs = shlex.split(capture(["pkg-config", "--libs", "libuv"]))
        throughput = build("uv-throughput", "bench/io_adapters/uv.cpp", uvflags, uvlibs)
        handoff = build("uv-handoff", "bench/io_adapters/uv_handoff.cpp", uvflags, uvlibs)
    else:
        print("UV skipped (enable with --uv on after installing libuv and pkg-config).", flush=True)

    rows = []

    def add(suite, case, metric, value, trial):
        rows.append(dict(suite=suite, case=case, metric=metric, value=float(value), trial=trial))

    def scheduled(text, suite, trial):
        measured = 0
        for row in csv.DictReader(io.StringIO(text)):
            if row.get("phase", "measured") != "measured":
                continue
            measured += 1
            case = row["mode"] + "/" + row["profile"]
            for metric in ("completed_per_s", "e2e_p99_us", "actual_p99_us", "submit_lag_p99_us"):
                add(suite, case, metric, row[metric], trial)
        if measured != (3 if suite == "simple-scheduled" else 1):
            raise RuntimeError("unexpected number of measured rows in " + suite)

    for trial in range(1, args.trials + 1):
        print("Trial %d/%d" % (trial, args.trials), flush=True)
        for binary in flows[::1 if trial % 2 else -1]:
            name = Path(binary).name
            output = run([binary], "%s-%d" % (name, trial))
            matches = re.findall(r"^(\S+)\s+warmup=.*?ns/op=([\d.]+)", output, re.MULTILINE)
            if not matches:
                raise RuntimeError("no flow results in " + name)
            for case, value in matches:
                add(name, case, "ns_per_op", value, trial)
        profiles = ["steady500k", "overload2m"]
        if trial % 2 == 0:
            profiles.reverse()
        for profile in profiles:
            output = run([queue, "--samples", str(args.samples), "--repeats", "1",
                          "--capacity", "1024", "--profile", profile], "queue-%s-%d" % (profile, trial), trace=args.trace)
            scheduled(output, "simple-scheduled", trial)
        if uv:
            for producers in ([1, 3] if trial % 2 else [3, 1]):
                output = run([throughput, "current", str(producers), str(args.count), "1024", "16", "-1", "-1"],
                             "uv-throughput-%dp-%d" % (producers, trial))
                fields = next(csv.reader(io.StringIO(output)))
                add("uv-throughput", "%d-producers" % producers, "completed_per_s", 1e9 / float(fields[6]), trial)
            modes = ["raw", "ordinary-fast", "normal"]
            modes = modes[(trial - 1) % 3:] + modes[:(trial - 1) % 3]
            for profile in profiles:
                for mode in modes:
                    output = run([handoff, mode, profile, str(args.samples), "128", "16", "-1", "-1"],
                                 "uv-%s-%s-%d" % (mode, profile, trial), trace=args.trace)
                    scheduled(output, "uv-scheduled", trial)

    with (out / "results.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["suite", "case", "metric", "value", "trial"])
        writer.writeheader()
        writer.writerows(rows)
    groups = {}
    for row in rows:
        groups.setdefault((row["suite"], row["case"], row["metric"]), []).append(row["value"])
    summary = ["Medians across process trials (min/max show run variability).",
               "Flow ns/op: inline microbenchmarks, not I/O latency.",
               "Scheduled probes: offered load 500k/s or 2M/s, work=128, capacity=1024.",
               "e2e_p99 includes lateness from planned arrival; actual_p99 starts at submission.",
               "UV throughput: saturated tiny tasks, capacity=1024, drain budget=16.", "",
               "suite | case | metric | median | min | max"]
    for key, values in groups.items():
        summary.append("%s | %s | %s | %.3f | %.3f | %.3f" %
                       (*key, statistics.median(values), min(values), max(values)))
    report = "\n".join(summary) + "\n"
    (out / "summary.txt").write_text(report)
    if traces:
        (out / "trace-summary.txt").write_text(
            "Diagnostic maxima, NOT p99. Times are relative to each case's planned origin.\n"
            "Timeline counts use their own event times; duration maxima use submission-time bins.\n"
            "pending_submit_end = planned - submitted; inflight_end = submitted - completed, at bin end.\n"
            "inflight is logical unfinished work, not executor queue occupancy.\n"
            "producer_gap includes planned waiting; consumer_gap includes callback cleanup and loop work.\n"
            "These gaps do not establish OS preemption. dispatch_call can overlap service.\n"
            "For the same event: e2e = submit_lag + pre_dispatch + dispatch_to_begin + service.\n"
            "See *.spikes.csv for the 16 largest e2e events and *.timeline.csv for 1 ms windows.\n\n"
            + "\n".join(traces) + "\n")
        with tarfile.open(out / "diagnostics.tar.gz", "w:gz") as archive:
            files = [out / name for name in ("summary.txt", "results.csv", "environment.json", "trace-summary.txt")]
            files += sorted(out.glob("*.timeline.csv")) + sorted(out.glob("*.spikes.csv"))
            for file in files:
                archive.add(file, arcname=file.name)
        print("Share diagnostic bundle: " + str(out / "diagnostics.tar.gz"), flush=True)
    print("\n" + report)
    print("Saved logs, commands, environment.json, results.csv, summary.txt in " + str(out))


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print("Benchmark failed: %s (see output directory logs)" % error, file=sys.stderr)
        sys.exit(1)
