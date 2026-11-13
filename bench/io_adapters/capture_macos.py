#!/usr/bin/env python3
"""Record a focused macOS System Trace, with unprofiled controls before/after."""
import argparse
import csv
import datetime
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run import ROOT, capture, positive
from macos_trace_support import record_attached, audit_exports


def trace_tables(toc):
    """Discover tables from this Xcode version; do not assume schema names."""
    tables, seen = [], set()
    root = ET.fromstring(toc)
    for run in root.findall(".//run"):
        number = run.get("number", "")
        if not number.isdigit():
            continue
        for table in run.findall("./data/table"):
            schema = table.get("schema", "")
            if re.fullmatch(r"[A-Za-z0-9_.:-]+", schema) and (number, schema) not in seen:
                seen.add((number, schema))
                xpath = "/trace-toc/run[@number='%s']/data/table[@schema='%s']" % (number, schema)
                tables.append(dict(run=number, schema=schema, xpath=xpath,
                    selected=bool(re.search(r"thread|sched|context|signpost|syscall|tid-pid", schema, re.I))))
    return tables


def measurement(text):
    rows = [r for r in csv.DictReader(io.StringIO(text)) if r.get("phase") == "measured"]
    required = {"completed_per_s", "e2e_p99_us", "actual_p99_us", "cpu_percent"}
    if len(rows) != 1 or None in rows[0] or not required <= rows[0].keys():
        raise RuntimeError("missing/invalid measured CSV row; recording may have ended before the benchmark completed")
    return rows[0]


def phase_metadata(path):
    try:
        with path.open(newline="") as source:
            rows = list(csv.DictReader(source))
        if len(rows) != 1:
            raise ValueError("expected one measured region")
        data = {key: int(value) for key, value in rows[0].items()}
        if not (data["pid"] > 0 and data["producer_tid"] > 0 and
                data["end_ns"] >= max(data["origin_ns"], data["start_ns"]) and
                data["end_mach_ticks"] >= data["start_mach_ticks"] and
                data["timebase_numer"] > 0 and data["timebase_denom"] > 0):
            raise ValueError("invalid clock anchors")
        return data
    except (OSError, KeyError, TypeError, ValueError) as exc:
        raise RuntimeError("missing/invalid measurement boundary metadata: " + str(path)) from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--mode", choices=("raw", "ordinary-fast", "normal"), default="raw")
    parser.add_argument("--profile", choices=("steady500k", "overload2m"), default="steady500k")
    parser.add_argument("--budget", type=positive, default=16)
    parser.add_argument("--samples", type=positive, default=250000)
    parser.add_argument("--limit-seconds", type=positive, default=30, help="recording safety limit; incomplete runs fail validation")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("run this collector on the Mac with full Xcode installed")
    if args.samples > 1000000:
        parser.error("--samples must be <= 1000000")
    out = (args.out or ROOT / "build" / datetime.datetime.now().strftime("system-trace-%Y%m%d-%H%M%S-%f")).resolve()
    out.mkdir(parents=True, exist_ok=False)
    metadata = dict(platform=platform.platform(), machine=platform.machine(),
        revision=capture(["git", "rev-parse", "HEAD"]), git_status=capture(["git", "status", "--short"]),
        arguments={**vars(args), "out": str(out)}, commands=[], tables=[], status="started",
        affinity="unpinned", policy="adaptive", capacity=1024, work=128, phases={},
        source_sha256={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in
            ("bench/io_adapters/uv_handoff.cpp", "bench/load_probe.h", "bench/system_trace.h", "executor/uv_executor.h")})
    env = os.environ.copy()
    for key in ("FF_BENCH_TRACE_PREFIX", "FF_BENCH_PHASE_FILE", "FF_BENCH_READY_FILE", "FF_BENCH_GO_FILE"):
        env.pop(key, None)

    def save():
        (out / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")

    def run(command, name, check=True, live=False, phase=None):
        print("  " + name, flush=True)
        record = dict(name=name, command=command, phase_file=str(phase) if phase else None)
        metadata["commands"].append(record)
        save()
        child_env = env.copy()
        if phase:
            child_env["FF_BENCH_PHASE_FILE"] = str(phase)
        with (out / (name + ".log")).open("w") as log:
            process = subprocess.Popen(command, cwd=ROOT, env=child_env,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                with process.stdout:
                    for line in process.stdout:
                        log.write(line)
                        log.flush()
                        if live:
                            print(line, end="", flush=True)
                code = process.wait()
            except BaseException:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                raise
        record["exit_code"] = code
        save()
        if code and check:
            raise RuntimeError("%s failed (exit %d); see %s" % (name, code, out / (name + ".log")))
        return (out / (name + ".log")).read_text(), code

    print("Results: " + str(out), flush=True)
    print("Fixed workload, default backoff; control-before -> System Trace -> control-after.", flush=True)
    error = None
    try:
        metadata["xcode"] = run(["xcodebuild", "-version"], "xcode-version")[0]
        metadata["hardware"] = capture(["sysctl", "-n", "machdep.cpu.brand_string"])
        help_text = run(["xcrun", "xctrace", "help", "record"], "record-help", check=False)[0]
        required = ("--template", "--attach", "--output", "--time-limit", "--window", "--notify-tracing-started")
        if any(option not in help_text for option in required):
            raise RuntimeError("installed xctrace does not advertise the required recording options; inspect record-help.log")
        templates = run(["xcrun", "xctrace", "list", "templates"], "templates")[0]
        if "System Trace" not in templates:
            raise RuntimeError("System Trace template unavailable in the selected Xcode")
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        metadata["compiler"] = run(compiler + ["--version"], "compiler-version")[0]
        metadata["libuv"] = capture(["pkg-config", "--modversion", "libuv"])
        flags = shlex.split(capture(["pkg-config", "--cflags", "libuv"]))
        libs = shlex.split(capture(["pkg-config", "--libs", "libuv"]))
        binary = out / "uv-handoff"
        run(compiler + ["-std=c++14", "-O3", "-g", "-fno-omit-frame-pointer", "-DNDEBUG", "-pthread",
            "-I" + str(ROOT), "-DFF_BENCH_SYSTEM_TRACE=1"] + flags +
            shlex.split(os.environ.get("EXTRA_CXXFLAGS", "")) +
            [str(ROOT / "bench/io_adapters/uv_handoff.cpp"), "-o", str(binary)] + libs, "build")
        run(["xcrun", "dsymutil", str(binary), "-o", str(out / "uv-handoff.dSYM")], "symbols")
        command = [str(binary), args.mode, args.profile, str(args.samples), "128", str(args.budget), "-1", "-1", "adaptive"]
        before = measurement(run(command, "control-before", phase=out / "control-before.phase.csv")[0])
        metadata["phases"]["control-before"] = phase_metadata(out / "control-before.phase.csv")
        trace = out / "capture.trace"
        record_attached(command, ["xcrun", "xctrace"], out, env, args.limit_seconds, metadata, save)
        profiled = measurement((out / "profiled.csv").read_text())
        metadata["phases"]["profiled"] = phase_metadata(out / "profiled.phase.csv")
        after = measurement(run(command, "control-after", phase=out / "control-after.phase.csv")[0])
        metadata["phases"]["control-after"] = phase_metadata(out / "control-after.phase.csv")
        observations = [dict(capture=label, **row) for label, row in
                        (("control-before", before), ("profiled", profiled), ("control-after", after))]
        with (out / "results.csv").open("w", newline="") as dest:
            writer = csv.DictWriter(dest, fieldnames=list(observations[0]))
            writer.writeheader()
            writer.writerows(observations)
        toc_path = out / "toc.xml"
        _, toc_code = run(["xcrun", "xctrace", "export", "--input", str(trace), "--toc", "--output", str(toc_path)], "export-toc", check=False)
        if toc_code == 0 and toc_path.is_file():
            metadata["tables"] = trace_tables(toc_path.read_text())
            for index, table in enumerate(metadata["tables"]):
                if not table["selected"]:
                    continue
                dest = out / ("table-%02d-%s.xml" % (index, table["schema"]))
                _, code = run(["xcrun", "xctrace", "export", "--input", str(trace), "--xpath", table["xpath"],
                               "--output", str(dest)], "export-%02d" % index, check=False)
                table["export_exit_code"] = code
                if code == 0 and dest.is_file():
                    with dest.open("rb") as source, gzip.open(str(dest) + ".gz", "wb", compresslevel=1) as target:
                        shutil.copyfileobj(source, target)
                    table["export_file"] = dest.name + ".gz"
                    dest.unlink()
        exported = sum("export_file" in t for t in metadata["tables"])
        print("  validate target thread attribution and measured interval", flush=True)
        quality = audit_exports(out, metadata["tables"], metadata["phases"]["profiled"],
                                metadata["target_threads"], (out / "record.log").read_text())
        metadata["quality"] = quality
        (out / "quality.json").write_text(json.dumps(quality, indent=2) + "\n")
        lines = ["System Trace diagnostic, NOT an uninstrumented performance comparison.",
                 "Threads: ff-producer and ff-uv-consumer. Signpost: FF measured (org.flux-foundry.bench).",
                 "The phase CSV gives clock anchors; the region excludes warmup, setup and quantile sorting.",
                 "Signpost visibility depends on the instruments included in the selected System Trace template.",
                 "Both controls use the same debug-symbol/frame-pointer-enabled binary as the profiled run.",
                 "Inspect scheduling states within the measured region; CPU totals do not establish preemption.",
                 "Exported %d candidate scheduling/signpost/syscall tables; inspect their schemas before drawing conclusions." % exported,
                 "Native trace and symbols remain locally in capture.trace and uv-handoff.dSYM.", "",
                 "capture | completed/s | planned p99 us | actual p99 us | CPU %"]
        for row in observations:
            lines.append(" | ".join(row[k] for k in ("capture", "completed_per_s", "e2e_p99_us", "actual_p99_us", "cpu_percent")))
        if not exported:
            lines.append("No candidate tables exported: open capture.trace in Instruments; the bundle alone cannot establish thread states.")
        lines.append("Target thread-state rows: " + str(quality["target_state_rows"]))
        for warning in quality["warnings"]:
            lines.append("INCOMPLETE EVIDENCE: " + warning)
        lines.append("Passing these basic checks still requires inspecting the actual thread states within the measured region.")
        (out / "summary.txt").write_text("\n".join(lines) + "\n")
        print("\n" + "\n".join(lines), flush=True)
        metadata["status"] = "captured" if quality["passed_basic_checks"] else "captured_incomplete"
    except (OSError, RuntimeError, subprocess.CalledProcessError, ET.ParseError, KeyboardInterrupt) as exc:
        error = exc
        metadata["status"] = "failed"
        metadata["error"] = str(exc) or "interrupted"
        (out / "failure.txt").write_text(metadata["error"] + "\nNative/partial capture and logs are retained.\n")
    finally:
        save()
        bundle = out / "system-trace-diagnostics.tar.gz"
        with tarfile.open(bundle, "w:gz") as archive:
            # Native traces, executable and dSYM stay local; exports are portable.
            for path in sorted(out.iterdir()):
                if path.is_file() and path != bundle and path.name != "uv-handoff":
                    archive.add(path, arcname=path.name)
        print("Share: " + str(bundle), flush=True)
    if error:
        raise RuntimeError(str(error) or "capture interrupted")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("System Trace capture failed: " + str(error), file=sys.stderr)
        sys.exit(1)
