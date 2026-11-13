#!/usr/bin/env python3
"""Build matched release probes and interleave independent process runs."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def output(command):
    return subprocess.check_output(command, text=True, timeout=180).strip()


def sensors():
    paths = ["/sys/class/thermal/thermal_zone0/temp",
             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"]
    return {path: Path(path).read_text().strip() for path in paths if Path(path).exists()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/io-perf")
    parser.add_argument("--baseline", default="HEAD")
    parser.add_argument("--baseline-header", type=Path, help="use an exported old header without Git history")
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--count", type=int, default=262144)
    parser.add_argument("--owner-cpu", type=int, default=0)
    parser.add_argument("--first-producer-cpu", type=int, default=1)
    args = parser.parse_args()
    if args.count <= 0 or args.count % 256 or args.repeats <= 0:
        parser.error("count must be a positive multiple of 256; repeats must be positive")
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    overlay = directory / "baseline"
    (overlay / "executor").mkdir(parents=True, exist_ok=True)
    if args.baseline_header:
        revision = args.baseline
        header = args.baseline_header.read_text()
    else:
        revision = output(["git", "-C", str(ROOT), "rev-parse", args.baseline])
        header = output(["git", "-C", str(ROOT), "show", f"{revision}:executor/uv_executor.h"])
    (overlay / "executor/uv_executor.h").write_text(header + "\n")
    for path in ROOT.iterdir():
        if path.is_dir() and path.name not in ("executor", "build", ".git"):
            link = overlay / path.name
            if not link.exists():
                link.symlink_to(path, target_is_directory=True)
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++14", "-O3", "-DNDEBUG", "-pthread", "-Wall", "-Wextra", "-Werror"]
    flags += shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    commands = []
    for name, includes, source, package in [
        ("uv-old", [overlay, ROOT], "uv.cpp", "libuv"),
        ("uv-new", [ROOT], "uv.cpp", "libuv"),
        ("uring", [ROOT], "uring.cpp", "liburing"),
    ]:
        # Separate PKG_CONFIG_* environments may be needed for extracted deps.
        pkg_env = os.environ.copy()
        if package == "libuv":
            pkg_env.pop("PKG_CONFIG_SYSROOT_DIR", None)
            pkg_env.pop("PKG_CONFIG_PATH", None)
        pkg = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", package], text=True, env=pkg_env))
        command = cxx + flags + [f"-I{x}" for x in includes]
        command += [str(ROOT / "bench/io_adapters" / source), "-o", str(directory / name)] + pkg
        commands.append(command)
        subprocess.run(command, check=True)
    metadata = dict(baseline=revision, commands=commands, uname=output(["uname", "-a"]),
                    started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    sensors_before=sensors(), lscpu=output(["lscpu"]),
                    compiler=output(cxx + ["--version"]), arguments=vars(args).copy())
    metadata["sha256"] = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in (
        "executor/uv_executor.h", "utility/close_gate.h", "extension/io_uring_awaitable.h",
        "bench/io_adapters/uv.cpp", "bench/io_adapters/uring.cpp")}
    metadata["arguments"]["out"] = str(directory)
    metadata["arguments"]["baseline_header"] = str(args.baseline_header) if args.baseline_header else None
    metadata["baseline_header_sha256"] = hashlib.sha256(header.encode()).hexdigest()
    (directory / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    with (directory / "uv.csv").open("w") as uv, (directory / "uring.csv").open("w") as uring:
        uv.write("repeat,backend,version,producers,count,capacity,budget,ns_per_op,close_us,retries\n")
        uring.write("repeat,backend,path,mode,count,batch,ns_per_op\n")
        for repeat in range(args.repeats):
            uv_cases = [(p, b) for p in (0, 1, 3) for b in (16, 64) if p or b == 16]
            uv_cases = uv_cases[repeat % len(uv_cases):] + uv_cases[:repeat % len(uv_cases)]
            for producers, budget in uv_cases:
                for version in (("old", "new") if repeat % 2 == 0 else ("new", "old")):
                    command = [str(directory / f"uv-{version}"), version, str(producers),
                               str(args.count), "1024", str(budget), str(args.owner_cpu),
                               str(args.first_producer_cpu)]
                    uv.write(f"{repeat}," + output(command) + "\n")
                    uv.flush()
            for path, batch in (("staged", 32), ("kernel", 1), ("kernel", 32), ("kernel", 256)):
                modes = ["raw", "context", "normal", "fast"]
                modes = modes[repeat % 4:] + modes[:repeat % 4]
                for mode in modes:
                    command = [str(directory / "uring"), path, mode, str(args.count), str(batch),
                               str(args.owner_cpu)]
                    uring.write(f"{repeat}," + output(command) + "\n")
                    uring.flush()
            print(f"Completed repeat {repeat + 1}/{args.repeats}", flush=True)
    metadata["sensors_after"] = sensors()
    (directory / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
