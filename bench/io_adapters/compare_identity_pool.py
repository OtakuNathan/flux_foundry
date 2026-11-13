#!/usr/bin/env python3
"""Paired UV handoff screen; run on an otherwise idle, CPU-affinity-capable host."""
import argparse
import csv
import hashlib
import io
import json
import platform
import statistics
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument("--out", type=Path, required=True)
p.add_argument("--trials", type=int, default=5)
p.add_argument("--owner", type=int, default=3)
p.add_argument("--producers", type=int, nargs="+", default=[2, 4])
p.add_argument("--compilers", nargs="+", default=["clang++", "g++"])
p.add_argument("--samples", type=int, default=32768)
p.add_argument("--cache", type=int, default=256, help="Identity cache capacity; legacy stays at 512")
p.add_argument("--capacities", type=int, nargs="+", help="Screen these identity cache capacities; legacy stays at 512")
a = p.parse_args()
a.root = a.root.resolve()
a.out = a.out.resolve()
a.out.mkdir(parents=True, exist_ok=True)
commands, versions, rows = [], {}, []
capacities = a.capacities or [a.cache]
variants = [(0, 512)] + [(1, cap) for cap in capacities]
for compiler in a.compilers:
    versions[compiler] = subprocess.check_output([compiler, "--version"], text=True)
    for variant, capacity in variants:
        binary = a.out / f"{Path(compiler).name}-{variant}-{capacity}"
        cmd = [compiler, "-std=c++14", "-O3", "-DNDEBUG", "-pthread", "-I" + str(a.root),
               f"-DFLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL={variant}",
               f"-DFLUX_FOUNDRY_AWAITABLE_POOL_SLOT_COUNT={capacity}",
               str(a.root / "bench/io_adapters/uv_handoff.cpp"), "-luv", "-o", str(binary)]
        commands.append(cmd)
        subprocess.run(cmd, check=True)
for trial in range(a.trials):
    for compiler in a.compilers:
        for cpu in a.producers:
            for profile in ("steady500k", "overload2m"):
                for variant, capacity in (variants if trial % 2 == 0 else list(reversed(variants))):
                    cmd = [str(a.out / f"{Path(compiler).name}-{variant}-{capacity}"), "ordinary-fast", profile,
                           str(a.samples), "128", "16", str(a.owner), str(cpu)]
                    commands.append(cmd)
                    result = subprocess.run(cmd, text=True, capture_output=True, check=True)
                    measured = list(csv.DictReader(io.StringIO(result.stdout)))[-1]
                    assert measured["phase"] == "measured"
                    rows.append(dict(compiler=compiler, trial=trial, variant=variant, pool_capacity=capacity, **measured))
with (a.out / "results.csv").open("w") as f:
    writer = csv.DictWriter(f, fieldnames=rows[0].keys(), lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
metadata = dict(uname=list(platform.uname()), versions=versions, commands=commands,
                capacities=capacities, samples=a.samples, trials=a.trials,
                headers={str(path): hashlib.sha256((a.root / path).read_bytes()).hexdigest()
                         for path in ["memory/identity_pool.h", "memory/pooling.h", "flow/flow_awaitable.h",
                                      "flow/flow_runner.h", "executor/uv_executor.h", "bench/load_probe.h"]})
(a.out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
summary = ["| Compiler | Producer | Pool capacity | Profile | completed/s baseline → identity | p99 µs baseline → identity |",
           "|---|---:|---:|---|---:|---:|"]
for compiler in a.compilers:
    for cpu in a.producers:
        for profile in ("steady500k", "overload2m"):
            for capacity in capacities:
                groups = [[r for r in rows if r["compiler"] == compiler and r["variant"] == variant
                           and (variant == 0 or r["pool_capacity"] == capacity)
                           and int(r["producer_cpu"]) == cpu and r["profile"] == profile] for variant in (0, 1)]
                rates = [statistics.median(float(r["completed_per_s"]) for r in g) for g in groups]
                tails = [statistics.median(float(r["e2e_p99_us"]) for r in g) for g in groups]
                summary.append(f"| {compiler} | {cpu} | {capacity} | {profile} | {rates[0]:.0f} → {rates[1]:.0f} | {tails[0]:.2f} → {tails[1]:.2f} |")
(a.out / "summary.md").write_text("\n".join(summary) + "\n")
print("\n".join(summary))
