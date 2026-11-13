#!/usr/bin/env python3
"""Build and measure native DPDK software ring loopback (no NIC required)."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/io-perf-dpdk")
    parser.add_argument("--bursts", type=int, default=1048576)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--cpu", type=int, default=0)
    args = parser.parse_args()
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++14", "-O3", "-DNDEBUG", "-pthread", "-Wall", "-Wextra", "-Werror"]
    flags += shlex.split(os.environ.get("EXTRA_CXXFLAGS", ""))
    pkg = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "libdpdk"], text=True))
    libdir = subprocess.check_output(["pkg-config", "--variable=libdir", "libdpdk"], text=True).strip()
    # Keep PMD registration constructors. Only these two software drivers are
    # needed; do not require every hardware PMD packaged in libdpdk.pc.
    libraries = ["-Wl,--no-as-needed", "-lrte_net_ring", "-lrte_mempool_ring", "-Wl,--as-needed",
                 "-lrte_ethdev", "-lrte_mbuf", "-lrte_mempool", "-lrte_ring", "-lrte_eal", "-lrte_log"]
    command = cxx + flags + pkg + [f"-I{ROOT}", str(ROOT / "bench/io_adapters/dpdk.cpp"),
              "-o", str(directory / "dpdk"), f"-L{libdir}", f"-Wl,-rpath-link,{libdir}"] + libraries
    subprocess.run(command, check=True)
    (directory / "environment.json").write_text(json.dumps({"command": command,
        "version": subprocess.check_output(["pkg-config", "--modversion", "libdpdk"], text=True).strip(),
        "bursts": args.bursts, "repeats": args.repeats, "cpu": args.cpu}, indent=2) + "\n")
    with (directory / "dpdk.csv").open("w") as results, (directory / "runtime.log").open("w") as log:
        subprocess.run([str(directory / "dpdk"), str(args.bursts), str(args.repeats), str(args.cpu)],
                       stdout=results, stderr=log, check=True)
    print(f"Results: {directory / 'dpdk.csv'}")


if __name__ == "__main__":
    main()
