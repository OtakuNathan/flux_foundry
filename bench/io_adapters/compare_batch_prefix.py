#!/usr/bin/env python3
"""Compare packet-batch headers at fixed capacity 32 and varying RX occupancy."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
from perf_probe import replace_once

ROOT = Path(__file__).resolve().parents[2]


def overlay(directory, batch_header, runner_header):
    directory.mkdir(parents=True, exist_ok=True)
    for name in ("base", "memory", "task", "utility", "executor"):
        if not (directory / name).exists(): (directory / name).symlink_to(ROOT / name, target_is_directory=True)
    for name, replacement, filename in (("extension", batch_header, "dpdk_awaitable.h"),
                                         ("flow", runner_header, "flow_runner.h")):
        dest = directory / name
        dest.mkdir(exist_ok=True)
        for item in (ROOT / name).iterdir():
            if item.name != filename and not (dest / item.name).exists():
                (dest / item.name).symlink_to(item, target_is_directory=item.is_dir())
        (dest / filename).write_bytes(replacement.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--runner", type=Path, required=True, help="identical runner header for both versions")
    parser.add_argument("--references", nargs="+", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--count", type=int, default=1048576)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--cpu", type=int, default=3)
    args = parser.parse_args()
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "bench/io_adapters/dpdk.cpp").read_text()
    source = replace_once(source, "using E = flow_async_agg_err_t;", "using E = flow_async_agg_err_t;\nunsigned actual_burst = 32;")
    source = replace_once(source, "require(n == Size,", "require(n == actual_burst,")
    source = replace_once(source, "require(batch.size() == Size,", "require(batch.size() == actual_burst,")
    source = replace_once(source, "seen.packets == bursts * Size && seen.sum == bursts * Size * (Size + 1) / 2",
                          "seen.packets == bursts * actual_burst && seen.sum == bursts * actual_burst * (actual_burst + 1) / 2")
    source = replace_once(source, "rte_pktmbuf_alloc_bulk(pool, packets.data(), Size)", "rte_pktmbuf_alloc_bulk(pool, packets.data(), actual_burst)")
    source = replace_once(source, "i < Size; ++i", "i < actual_burst; ++i")
    source = replace_once(source, "rte_eth_tx_burst(port, 0, packets.data(), Size) == Size", "rte_eth_tx_burst(port, 0, packets.data(), actual_burst) == actual_burst")
    source = replace_once(source, "rte_eth_rx_burst(port, 0, packets.data(), Size) == Size", "rte_eth_rx_burst(port, 0, packets.data(), Size) == actual_burst")
    source = replace_once(source, "for (auto* packet : packets) rte_pktmbuf_free(packet);", "for (unsigned i = 0; i < actual_burst; ++i) rte_pktmbuf_free(packets[i]);")
    source = replace_once(source, "run_cases<1>(port, pool, bursts, repeats);", "")
    source = replace_once(source, "const auto bursts = std::stoull(argv[1]);", 'actual_burst = std::stoul(std::getenv("FF_BURST_FILL"));\n    require(actual_burst > 0 && actual_burst <= 32, "invalid occupancy");\n    const auto bursts = std::stoull(argv[1]);')
    source = replace_once(source, "ns / Size", "ns / actual_burst")
    generated = directory / "dpdk.cpp"
    generated.write_text(source)
    for version in ("before", "after"):
        overlay(directory / version, getattr(args, version), args.runner)
    binaries, commands = {}, []
    for reference in args.references:
        recipe = json.loads((reference / "environment.json").read_text())["command"]
        compiler = Path(recipe[0]).name
        for version in ("before", "after"):
            binary = directory / f"{compiler}-{version}"
            command = [str(generated) if arg == str(ROOT / "bench/io_adapters/dpdk.cpp") else arg for arg in recipe]
            assert str(generated) in command
            command[command.index("-o") + 1] = str(binary)
            command.insert(1, f"-I{directory / version}")
            command += [f"-I{ROOT / 'bench/io_adapters'}"]
            subprocess.run(command, check=True)
            binaries[compiler, version] = str(binary)
            commands.append(command)
    (directory / "environment.json").write_text(json.dumps({"commands": commands, "cpu": args.cpu,
        "count": args.count, "repeats": args.repeats, "capacity": 32,
        "before_sha256": hashlib.sha256(args.before.read_bytes()).hexdigest(),
        "after_sha256": hashlib.sha256(args.after.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(args.runner.read_bytes()).hexdigest()}, indent=2) + "\n")
    with (directory / "results.csv").open("w") as stream, (directory / "runtime.log").open("w") as log:
        writer = csv.writer(stream)
        writer.writerow(["repeat", "compiler", "version", "filled", "mode", "ns_per_burst"])
        for repeat in range(args.repeats):
            for compiler in dict.fromkeys(key[0] for key in binaries):
                for filled in (1, 4, 16, 32):
                    for version in (("before", "after") if repeat % 2 == 0 else ("after", "before")):
                        text = subprocess.check_output([binaries[compiler, version], str(args.count), "1", str(args.cpu)],
                            text=True, stderr=log, timeout=120, env=os.environ | {"FF_BURST_FILL": str(filled)})
                        for row in csv.DictReader(io.StringIO(text)):
                            writer.writerow([repeat, compiler, version, filled, row["mode"], row["ns_per_burst"]])
                        stream.flush()
            print(f"occupancy sweep {repeat + 1}/{args.repeats} completed", flush=True)


if __name__ == "__main__":
    main()
