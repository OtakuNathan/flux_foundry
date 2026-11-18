#!/usr/bin/env python3
"""Build matched controls, then run them serially with rotating variant order.

All raw process outputs and commands are retained. Instrumented mechanism probes
are separate binaries and never contribute timing samples. Optional backends use
pkg-config; an unavailable kernel is recorded, not silently replaced by staged I/O.
"""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BASELINE = "a93d246981a49648949c489a1918f5ccb61ed15d"
FAMILIES = {"core", "controller", "simple", "identity", "uv", "uring", "dpdk"}


def capture(command, **kwargs):
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--baseline", default=BASELINE)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--families", default=",".join(sorted(FAMILIES)))
    parser.add_argument("--cpu", type=int)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--build-only", action="store_true")
    modes.add_argument("--run-only", action="store_true")
    args = parser.parse_args()
    selected = set(args.families.split(","))
    if selected - FAMILIES or not 1 <= args.repeats <= 20:
        parser.error("unknown family or repeats outside 1..20")
    available_cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    cpu = args.cpu if args.cpu is not None else (available_cpus[0] if available_cpus else -1)
    if cpu >= 0 and available_cpus and cpu not in available_cpus:
        parser.error("requested CPU is outside the available affinity mask")
    other_cpus = [c for c in available_cpus if c != cpu]
    out = args.out.resolve()
    binaries, logs = out / "binaries", out / "logs"
    binaries.mkdir(parents=True, exist_ok=True)
    logs.mkdir(exist_ok=True)
    baseline = out / "baseline"
    manifest_path = out / "manifest.json"
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++14", "-O3", "-DNDEBUG", "-pthread", "-Wall", "-Wextra"]
    flags += shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    manifest = {"baseline": args.baseline, "root": str(ROOT), "platform": platform.platform(),
                "compiler": capture(compiler + ["--version"], check=True).stdout,
                "flags": flags, "cpu": cpu, "available_cpus": available_cpus,
                "repeats": args.repeats, "build_commands": [], "run_commands": [], "backends": {}}
    if args.run_only:
        manifest = json.loads(manifest_path.read_text())
        manifest.update(repeats=args.repeats, cpu=cpu, available_cpus=available_cpus)
        manifest["run_commands"] = []
    elif manifest_path.exists():
        previous = json.loads(manifest_path.read_text())
        manifest["build_commands"] = previous.get("build_commands", [])
    manifest["runner_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()

    def save_manifest():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    def package(name):
        try:
            found = capture(["pkg-config", "--exists", name]).returncode == 0
        except FileNotFoundError:
            found = False
        if not found:
            return None
        version = capture(["pkg-config", "--modversion", name], check=True).stdout.strip()
        options = shlex.split(capture(["pkg-config", "--cflags", "--libs", name], check=True).stdout)
        manifest["backends"][name] = version
        return options

    deps = {"uv": package("libuv"), "uring": package("liburing"), "dpdk": package("libdpdk")}
    if not args.run_only:
        sha = capture(["git", "rev-parse", args.baseline], cwd=ROOT, check=True).stdout.strip()
        manifest["baseline"] = sha
        if baseline.exists():
            if not (baseline / ".source-sha").exists() or (baseline / ".source-sha").read_text().strip() != sha:
                raise RuntimeError("baseline directory belongs to a different source; choose a fresh --out")
        else:
            data = subprocess.check_output(["git", "archive", sha], cwd=ROOT)
            baseline.mkdir()
            with tarfile.open(fileobj=io.BytesIO(data)) as archive:
                archive.extractall(baseline, filter="data")
            (baseline / ".source-sha").write_text(sha + "\n")
        files = capture(["git", "ls-files", "-co", "--exclude-standard"], cwd=ROOT, check=True).stdout.splitlines()
        manifest["source_sha256"] = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in sorted(set(files)) if (ROOT / name).is_file() and Path(name).suffix in {".h", ".cpp", ".py"}}

        def build(name, source, include=ROOT, extra=(), libraries=()):
            command = compiler + flags + list(extra) + ["-I" + str(include), str(source), "-o", str(binaries / name)] + list(libraries)
            print("build", name, flush=True)
            result = capture(command)
            (logs / ("build-" + name + ".log")).write_text(result.stdout + result.stderr)
            manifest["build_commands"].append({"name": name, "command": command, "returncode": result.returncode})
            save_manifest()
            if result.returncode:
                raise RuntimeError("build failed: " + name + "; see " + str(logs))

        if "core" in selected:
            for variant, include in [("before", baseline), ("after", ROOT)]:
                build("flow-" + variant, HERE / "flow_core.cpp", include)
        if "controller" in selected:
            build("controller", HERE / "controller.cpp")
            if platform.system() == "Linux":
                build("controller-trace", HERE / "controller.cpp", extra=["-DFF_CONTROLLER_TRACE_ALLOC"],
                      libraries=["-Wl,--wrap=posix_memalign"])
        if "simple" in selected:
            for variant, include in [("before", baseline), ("after", ROOT)]:
                build("simple-" + variant, HERE / "simple_drain.cpp", include)
                build("scheduled-" + variant, HERE / "../queue_capacity/queue_capacity.cpp", include,
                      extra=["-fno-exceptions", "-fno-rtti", "-DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1"])
        if "identity" in selected:
            build("identity", HERE / "identity_routes.cpp")
            build("identity-histogram", HERE / "identity_routes.cpp", extra=["-DFF_IDENTITY_ROUTES_TRACE=1"])
        for family, source in [("uv", "uv_batch.cpp"), ("uring", "io_fast.cpp"), ("dpdk", "dpdk_fast.cpp")]:
            if family in selected and deps[family] is not None:
                extra = ["-lrte_net_ring"] if family == "dpdk" else []
                build(family, HERE / source, libraries=deps[family] + extra)
        save_manifest()
    if args.build_only:
        return

    required = {"core": ["flow-before", "flow-after"], "controller": ["controller"],
                "simple": ["simple-before", "simple-after", "scheduled-before", "scheduled-after"],
                "identity": ["identity", "identity-histogram"]}
    for family in selected:
        names = required.get(family, [family] if deps.get(family) is not None else [])
        for name in names:
            if not (binaries / name).is_file():
                raise RuntimeError("missing binary " + name + "; build the selected families before measuring")

    # All builds have completed before the first measurement. Each subprocess is
    # awaited before another starts; no background benchmark workers are used.
    raw, statuses = [], []
    sequence = 0
    metrics = {"core": "ns_per_op", "controller": "ns_per_op", "simple": "ns_per_task",
               "identity": "median_ns_per_release", "uv": "ns_per_task", "uring": "ns_per_op",
               "dpdk": "ns_per_burst", "scheduled": "completed_per_s"}

    def run(family, case, variant, binary, parameters, repeat, pin=False, header=None,
            skip_codes=(), timing=True):
        nonlocal sequence
        sequence += 1
        command = [str(binaries / binary)] + [str(x) for x in parameters]
        # taskset changes only this child; UV and scheduled drivers pin their own
        # threads to distinct CPUs and must retain the larger process mask.
        if pin and cpu >= 0:
            command = ["taskset", "-c", str(cpu)] + command
        print(f"run {sequence}: {family}/{case}/{variant} repeat={repeat}", flush=True)
        start = time.time()
        try:
            result = capture(command, timeout=180)
        except subprocess.TimeoutExpired as error:
            def decoded(value):
                return value.decode(errors="replace") if isinstance(value, bytes) else (value or "")
            result = subprocess.CompletedProcess(command, 124, decoded(error.stdout),
                decoded(error.stderr) + "\nprobe exceeded the 180-second timeout\n")
        stem = f"{sequence:04d}-{family}-{case}-{variant}-r{repeat}"
        (logs / (stem + ".stdout")).write_text(result.stdout)
        (logs / (stem + ".stderr")).write_text(result.stderr)
        record = {"sequence": sequence, "family": family, "case": case, "variant": variant,
                  "repeat": repeat, "returncode": result.returncode, "command": command,
                  "wall_seconds": time.time() - start, "log": stem, "timing": timing}
        statuses.append(record)
        manifest["run_commands"].append(record)
        save_manifest()
        if result.returncode:
            if result.returncode in skip_codes:
                return False
            raise RuntimeError("probe failed: " + stem + "; see " + str(logs))
        rows = csv.DictReader(io.StringIO(result.stdout), fieldnames=header)
        for row in rows:
            if not row:
                continue
            entry = dict(row)
            entry.update(sequence=sequence, family=family, case=case, variant=variant,
                         process_repeat=repeat, timing=int(timing))
            raw.append(entry)
        return True

    def group(family, case, variants, pin=True, header=None):
        for repeat in range(args.repeats):
            offset = repeat % len(variants)
            for variant, binary, parameters in variants[offset:] + variants[:offset]:
                run(family, case, variant, binary, parameters, repeat, pin, header)

    if "core" in selected:
        for lane in ["continuation", "aggregate"]:
            group("core", lane, [(v, "flow-" + v, [lane, 1000000]) for v in ["before", "after"]])
    if "controller" in selected:
        group("controller", "fresh-normal-runner", [(v, "controller", [v, 1000000]) for v in ["system", "system-eager", "pooled"]])
        if (binaries / "controller-trace").exists():
            for variant in ["system", "system-eager", "pooled"]:
                run("controller-trace", "warmed-allocation", variant, "controller-trace", [variant, 100000], 0, True, timing=False)
    if "simple" in selected:
        group("simple", "prefilled-drain", [(v, "simple-" + v, [1000]) for v in ["before", "after"]])
        for mode in ["raw", "fast", "normal"]:
            for profile in ["steady100k", "steady500k", "overload2m"]:
                parameters = ["--samples", 32768, "--repeats", 1, "--work", 32, "--capacity", 1024,
                              "--mode", mode, "--profile", profile]
                if cpu >= 0 and other_cpus:
                    parameters += ["--producer-cpu", cpu, "--consumer-cpu", other_cpus[0]]
                group("scheduled", mode + "-" + profile, [(v, "scheduled-" + v, parameters) for v in ["before", "after"]], pin=False)
    if "identity" in selected:
        for owners in [1, 2, 4, 8]:
            for order in ["grouped", "interleaved"]:
                case = f"owners{owners}-{order}"
                parameters = ["--owners", owners, "--blocks", 128, "--order", order]
                group("identity", case, [("routes" + str(k), "identity", parameters + ["--routes", k, "--rounds", 200, "--warmup", 20]) for k in [1, 4]], pin=False)
                for k in [1, 4]:
                    run("identity-histogram", case, "routes" + str(k), "identity-histogram",
                        parameters + ["--routes", k, "--rounds", 4, "--warmup", 2], 0, timing=False)
    if "uv" in selected and deps["uv"] is not None and (binaries / "uv").exists():
        control_affinity = [cpu, other_cpus[0]] if cpu >= 0 and other_cpus else []
        group("uv", "producers1-width1", [(mode, "uv", [mode, 131072, 1, 1, 65536, 4096, "automatic"] + control_affinity)
              for mode in ["single", "batch"]], pin=False)
        for producers in [1, 4]:
            # The native driver accepts a contiguous producer CPU range.
            producer_start = next((c for c in other_cpus if all(c + i in other_cpus for i in range(producers))), None)
            affinity = [cpu, producer_start] if cpu >= 0 and producer_start is not None else []
            variants = [(mode + "-" + fence, "uv", [mode, 131072, 16, producers, 65536, 4096, fence] + affinity)
                        for mode in ["single", "batch"] for fence in ["fence", "automatic"]]
            group("uv", "producers" + str(producers), variants, pin=False)
    if "uring" in selected and deps["uring"] is not None and (binaries / "uring").exists():
        header = ["backend", "path", "mode", "count", "batch", "ns_per_op"]
        kernel = run("uring-availability", "kernel-nop", "raw", "uring", ["kernel", "raw", 32, 32, cpu], 0,
                     header=header, skip_codes=(77,), timing=False)
        manifest["backends"]["io_uring_kernel_available"] = kernel
        for backend in ["staged"] + (["kernel"] if kernel else []):
            for batch in [1, 32]:
                group("uring", backend + "-batch" + str(batch), [(mode, "uring", [backend, mode, 262144, batch, cpu])
                    for mode in ["raw", "context", "normal", "ordinary-owning", "ordinary-view", "terminal-owning", "terminal-view"]],
                    pin=False, header=header)
    if "dpdk" in selected and deps["dpdk"] is not None and (binaries / "dpdk").exists():
        usable = run("dpdk", "net-ring", "rotating-modes", "dpdk", [131072, args.repeats, cpu], 0,
                     skip_codes=(77,))
        manifest["backends"]["dpdk_net_ring_available"] = usable
    for family in ["uv", "uring", "dpdk"]:
        if family in selected and deps[family] is None:
            statuses.append({"family": family, "status": "not-run", "reason": "pkg-config dependency unavailable"})

    fields = ["sequence", "family", "case", "variant", "process_repeat", "timing"]
    fields += sorted(set().union(*(row.keys() for row in raw)) - set(fields)) if raw else []
    with (out / "raw.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(raw)
    (out / "status.json").write_text(json.dumps(statuses, indent=2) + "\n")
    grouped = {}
    for row in raw:
        metric = metrics.get(row["family"])
        if not row["timing"] or metric is None:
            continue
        variant, case = row["variant"], row["case"]
        if row["family"] == "dpdk":
            variant, case = row["mode"], "net-ring-batch" + row["batch"]
        key = (row["family"], case, variant, metric)
        grouped.setdefault(key, []).append(float(row[metric]))
    with (out / "summary.csv").open("w", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(["family", "case", "variant", "metric", "samples", "median", "minimum", "maximum"])
        for key, values in sorted(grouped.items()):
            writer.writerow([*key, len(values), statistics.median(values), min(values), max(values)])
    save_manifest()
    print("Results:", out / "summary.csv", flush=True)


if __name__ == "__main__":
    main()
