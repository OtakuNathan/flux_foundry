#!/usr/bin/env python3
"""Compare UV callback budgets and production backoff parameters in one binary."""
import argparse
import csv
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run import ROOT, capture, positive
from trace_samples import summarize as summarize_trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--trials", type=positive, default=5)
    parser.add_argument("--samples", type=positive, default=1000000)
    parser.add_argument("--profiles", nargs="+", choices=("steady500k", "overload2m"),
                        default=["steady500k", "overload2m"])
    parser.add_argument("--modes", nargs="+", choices=("raw", "ordinary-fast", "normal"),
                        default=["raw", "ordinary-fast"])
    parser.add_argument("--budgets", type=positive, nargs="+", default=[16])
    parser.add_argument("--policies", nargs="+", choices=("adaptive", "spin"), default=["adaptive", "spin"])
    parser.add_argument("--owner-cpu", type=int, default=-1)
    parser.add_argument("--producer-cpu", type=int, default=-1)
    parser.add_argument("--trace", action="store_true", help="also export event timelines (extra disk/time)")
    args = parser.parse_args()
    if args.samples > 1000000:
        parser.error("--samples must be <= 1000000")
    if min(args.owner_cpu, args.producer_cpu) < -1:
        parser.error("CPU must be -1 (unpinned) or nonnegative")
    if args.owner_cpu >= 0 and args.owner_cpu == args.producer_cpu:
        parser.error("choose distinct owner and producer CPUs")
    if platform.system() != "Linux" and max(args.owner_cpu, args.producer_cpu) >= 0:
        parser.error("CPU affinity is only available on Linux")
    # Deduplicate filters so every trial has exactly one observation per policy/case.
    modes, profiles = list(dict.fromkeys(args.modes)), list(dict.fromkeys(args.profiles))
    budgets, policies = list(dict.fromkeys(args.budgets)), list(dict.fromkeys(args.policies))
    variants = [(budget, policy) for budget in budgets for policy in policies]
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    extra = shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    uvflags = shlex.split(capture(["pkg-config", "--cflags", "libuv"]))
    uvlibs = shlex.split(capture(["pkg-config", "--libs", "libuv"]))
    out = (args.out or ROOT / "build" / datetime.datetime.now().strftime("backoff-%Y%m%d-%H%M%S-%f")).resolve()
    out.mkdir(parents=True, exist_ok=False)
    metadata = dict(platform=platform.platform(), machine=platform.machine(), logical_cpus=os.cpu_count(),
        compiler=capture(compiler + ["--version"]), libuv=capture(["pkg-config", "--modversion", "libuv"]),
        revision=capture(["git", "rev-parse", "HEAD"]), git_status=capture(["git", "status", "--short"]),
        arguments={**vars(args), "out": str(out)}, commands=[], trace_prefixes={},
        source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in
            [ROOT / name for name in ("bench/io_adapters/uv_handoff.cpp", "bench/load_probe.h",
                                     "utility/back_off.h", "executor/uv_executor.h")]})
    if platform.system() == "Darwin":
        metadata["hardware"] = capture(["sysctl", "-n", "machdep.cpu.brand_string"])

    def run(command, name, trace=False):
        print("  " + name, flush=True)
        metadata["commands"].append(command)
        env = os.environ.copy()
        env.pop("FF_BENCH_TRACE_PREFIX", None)
        if trace:
            env["FF_BENCH_TRACE_PREFIX"] = str(out / name)
            metadata["trace_prefixes"][name] = str(out / name)
        (out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
        with (out / (name + ".log")).open("w") as log, (out / (name + ".stderr")).open("w") as err:
            subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=err, check=True)
        return (out / (name + ".log")).read_text()

    print("Results: " + str(out), flush=True)
    binary = str(out / "uv-handoff")
    run(compiler + ["-std=c++14", "-O3", "-DNDEBUG", "-pthread", "-I" + str(ROOT)] +
        uvflags + extra + [str(ROOT / "bench/io_adapters/uv_handoff.cpp"), "-o", binary] + uvlibs, "build")
    rows, traces = [], []
    cases = [(mode, profile) for profile in profiles for mode in modes]
    for trial in range(1, args.trials + 1):
        order = cases if trial % 2 else cases[::-1]
        for mode, profile in order:
            if len(variants) == 2:
                variant_order = variants if trial % 2 else variants[::-1]
            else:
                offset = (trial - 1) % len(variants)
                variant_order = variants[offset:] + variants[:offset]
                if ((trial - 1) // len(variants)) % 2:
                    variant_order = variant_order[::-1]
            pending_traces = []
            for budget, policy in variant_order:
                name = "%s-%s-budget%d-%s-%d" % (mode, profile, budget, policy, trial)
                output = run([binary, mode, profile, str(args.samples), "128", str(budget),
                              str(args.owner_cpu), str(args.producer_cpu), policy], name, args.trace)
                measured = [r for r in csv.DictReader(io.StringIO(output)) if r["phase"] == "measured"]
                if len(measured) != 1 or measured[0]["backoff"] != policy or int(measured[0]["budget"]) != budget:
                    raise RuntimeError("unexpected measured rows: " + name)
                row = dict(trial=trial, **measured[0])
                if policy == "spin" and int(row["scheduler_yields"]):
                    raise RuntimeError("spin run reached the scheduler-yield threshold")
                rows.append(row)
                if args.trace:
                    files = list(out.glob(name + "-*.samples.csv"))
                    if len(files) != 1:
                        raise RuntimeError("expected one trace: " + name)
                    pending_traces.extend(files)
            # No Python compression between variants of the same case/trial.
            for path in pending_traces:
                print("    analyze " + path.name, flush=True)
                traces.append(summarize_trace(path))
    with (out / "results.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    metrics = ("completed_per_s", "e2e_p99_us", "e2e_p999_us", "actual_p99_us",
               "submit_lag_p99_us", "dispatch_p99_us", "dispatch_max_us", "retries", "scheduler_yields", "cpu_percent")
    labels = ["%s/budget%d" % (policy, budget) for budget, policy in variants]
    summary = ["UV budget/backoff comparison: adaptive=backoff_strategy<>, spin=backoff_strategy<SIZE_MAX>.",
               "Same binary, interleaved variants, rotating/reversing order; capacity=1024, work=128.",
               "Variants: " + ", ".join(labels),
               "Policies keep max_loop=1024 and only change the probe retry boundary; budget controls tasks per UV callback.",
               "Counters add per-rejection instrumentation equally to both policies.",
               "scheduler_yields counts calls, not observed context switches; zero means this mechanism was not exercised.",
               "cpu_percent is process CPU / wall time: 100% is one core, 200% is two.",
               "CPU interval includes the 2 ms lead-in and drain fence; excludes setup, sorting and trace output.",
               "Scheduled throughput is offered-load constrained. p99 columns are medians of per-trial quantiles.", "",
               "case | metric | " + " | ".join(label + " median [min,max]" for label in labels)]
    for mode, profile in cases:
        groups = [[r for r in rows if r["mode"] == mode and r["profile"] == profile and
                   r["backoff"] == policy and int(r["budget"]) == budget] for budget, policy in variants]
        for metric in metrics:
            values = [[float(r[metric]) for r in group] for group in groups]
            cells = ["%.3f [%.3f,%.3f]" % (statistics.median(v), min(v), max(v)) for v in values]
            summary.append("%s/%s | %s | %s" % (mode, profile, metric, " | ".join(cells)))
        by_trial = [{int(r["trial"]): float(r["e2e_p99_us"]) for r in group} for group in groups]
        for index in range(1, len(variants)):
            wins = sum(by_trial[index][i] < by_trial[0][i] for i in by_trial[0])
            summary.append("  %s had lower e2e p99 than %s in %d/%d trials (descriptive, not a significance test)." %
                           (labels[index], labels[0], wins, args.trials))
    report = "\n".join(summary) + "\n"
    (out / "summary.txt").write_text(report)
    if traces:
        (out / "trace-summary.txt").write_text("Maxima, NOT p99; see bench/README.md for trace semantics.\n" + "\n".join(traces) + "\n")
    with tarfile.open(out / "backoff-diagnostics.tar.gz", "w:gz") as archive:
        files = [out / n for n in ("summary.txt", "results.csv", "environment.json")]
        if traces:
            files += [out / "trace-summary.txt"] + sorted(out.glob("*.timeline.csv")) + sorted(out.glob("*.spikes.csv"))
        for path in files:
            archive.add(path, arcname=path.name)
    print("\n" + report)
    print("Share: " + str(out / "backoff-diagnostics.tar.gz"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print("Backoff comparison failed: %s (check logs; libuv needs pkg-config)" % error, file=sys.stderr)
        sys.exit(1)
