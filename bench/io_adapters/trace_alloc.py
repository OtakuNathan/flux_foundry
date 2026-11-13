#!/usr/bin/env python3
"""Count direct executable allocator calls; instrumentation timings are ignored."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnostic", type=Path, required=True)
    parser.add_argument("--cpu", type=int, default=3)
    args = parser.parse_args()
    directory = args.diagnostic.resolve()
    metadata = json.loads((directory / "environment.json").read_text())
    backend = metadata["backend"]
    command = metadata["commands"][0][:]
    source = (directory / "current.cpp").read_text()
    start = "const auto start = probe::clock::now();"
    end = ("elapsed = probe::ns(start, probe::clock::now()) / count;" if backend == "uring" else
           "const auto elapsed = probe::ns(start, probe::clock::now()) / bursts;")
    assert source.count(start) == source.count(end) == 1
    source = source.replace(start, "ff_alloc_begin();\n" + start).replace(end, end + "\nff_alloc_end();")
    generated = directory / "alloc.cpp"
    generated.write_text(source)
    command[command.index(str(directory / "current.cpp"))] = str(generated)
    command[command.index("-o") + 1] = str(directory / "alloc")
    command += ["-include", str(ROOT / "bench/io_adapters/diagnostic_alloc.h"),
                str(ROOT / "bench/io_adapters/diagnostic_alloc.cpp")]
    for symbol in ("malloc", "posix_memalign", "aligned_alloc", "_Znwm", "_ZnwmRKSt9nothrow_t"):
        command.append(f"-Wl,--wrap={symbol}")
    subprocess.run(command, check=True)
    with (directory / "alloc.log").open("w") as log:
        for mode in (("raw", "context", "normal", "fast") if backend == "uring" else ("fast",)):
            log.write(f"mode={mode}; each measure logs separately (warmup, then timed workload)\n")
            log.flush()
            argv = (["staged", mode, "1048576", "32", str(args.cpu)] if backend == "uring" else
                    ["1048576", "1", str(args.cpu)])
            subprocess.run([str(directory / "alloc")] + argv, stdout=log, stderr=log, check=True)


if __name__ == "__main__":
    main()
