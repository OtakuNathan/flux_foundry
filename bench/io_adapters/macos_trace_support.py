"""Profiler readiness handshake and conservative validation of exported trace data."""
import csv
import ctypes
import gzip
import os
import select
import signal
import subprocess
import time
import uuid
import xml.etree.ElementTree as ET


class TraceStarted:
    def __init__(self):
        self.name = "org.flux-foundry.trace." + uuid.uuid4().hex
        self.lib = ctypes.CDLL("/usr/lib/system/libsystem_notify.dylib")
        register = self.lib.notify_register_file_descriptor
        register.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        register.restype = ctypes.c_uint32
        self.lib.notify_cancel.argtypes = [ctypes.c_int]
        self.lib.notify_cancel.restype = ctypes.c_uint32
        self.fd, self.token = ctypes.c_int(-1), ctypes.c_int(-1)
        status = register(self.name.encode(), ctypes.byref(self.fd), 0, ctypes.byref(self.token))
        if status:
            raise RuntimeError("cannot register profiler-start notification: %d" % status)

    def ready(self):
        if not select.select([self.fd.value], [], [], 0)[0]:
            return False
        data = os.read(self.fd.value, 4)
        token = self.token.value & 0xffffffff
        if len(data) != 4 or token not in (int.from_bytes(data, "big"), int.from_bytes(data, "little")):
            raise RuntimeError("invalid profiler-start notification")
        return True

    def close(self):
        # notify_cancel also closes the descriptor owned by this registration.
        self.lib.notify_cancel(self.token)


def stop_owned(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def record_attached(command, recorder_prefix, out, env, limit, metadata, save, notification_factory=TraceStarted):
    ready_path, go_path = out / "profiled.ready.csv", out / "profiled.go"
    target_env = dict(env, FF_BENCH_READY_FILE=str(ready_path), FF_BENCH_GO_FILE=str(go_path),
                      FF_BENCH_PHASE_FILE=str(out / "profiled.phase.csv"))
    target = recorder = notice = None
    target_record = dict(name="profiled-target", command=command, ready_file=str(ready_path), go_file=str(go_path))
    metadata["commands"].append(target_record)
    save()
    try:
        with (out / "profiled.csv").open("w") as stdout, (out / "profiled.stderr.log").open("w") as stderr, (out / "record.log").open("w") as log:
            target = subprocess.Popen(command, env=target_env, stdout=stdout, stderr=stderr)
            deadline = time.monotonic() + 120
            while not ready_path.exists():
                if target.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("target failed to publish its thread IDs before profiling")
                time.sleep(0.05)
            with ready_path.open() as source:
                rows = list(csv.DictReader(source))
            if len(rows) != 1:
                raise RuntimeError("invalid target readiness record")
            threads = {k: int(v) for k, v in rows[0].items()}
            if (threads.get("pid") != target.pid or threads.get("producer_tid", 0) <= 0 or
                    threads.get("consumer_tid", 0) <= 0 or threads["producer_tid"] == threads["consumer_tid"]):
                raise RuntimeError("invalid target PID/TIDs")
            metadata["target_threads"] = threads
            notice = notification_factory()
            record = recorder_prefix + ["record", "--template", "System Trace", "--output", str(out / "capture.trace"),
                "--time-limit", str(limit) + "s", "--window", str(limit) + "s",
                "--notify-tracing-started", notice.name, "--attach", str(target.pid)]
            record_metadata = dict(name="record", command=record)
            metadata["commands"].append(record_metadata)
            save()
            print("  attach System Trace; waiting for its started notification", flush=True)
            recorder = subprocess.Popen(record, env=env, stdout=log, stderr=subprocess.STDOUT)
            while not notice.ready():
                if target.poll() is not None or recorder.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("profiler did not signal readiness; see record.log")
                time.sleep(0.05)
            # Both named threads existed before xctrace took its initial snapshot.
            metadata["profiler_start_confirmed"] = True
            save()
            go_path.write_text("go\n")
            print("  profiler ready; running measured workload", flush=True)
            deadline = time.monotonic() + limit + 10
            while target.poll() is None:
                if recorder.poll() is not None:
                    try:
                        target.wait(timeout=1)
                    except subprocess.TimeoutExpired as exc:
                        raise RuntimeError("recording ended before the target completed") from exc
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError("recording ended before the target completed")
                time.sleep(0.05)
            target_record["exit_code"] = target.returncode
            if recorder.poll() is None:
                # Ctrl-C is xctrace's documented stop-and-save path. Do not keep
                # recording idle time and risk evicting the measured interval.
                recorder.send_signal(signal.SIGINT)
            try:
                code = recorder.wait(timeout=60)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("profiler did not finish saving within 60 seconds") from exc
            record_metadata["exit_code"] = code
            save()
            if target.returncode or code not in (0, -signal.SIGINT):
                raise RuntimeError("target/recorder failed; see profiled.stderr.log and record.log")
        print((out / "record.log").read_text(), flush=True)
    finally:
        stop_owned(target)
        stop_owned(recorder)
        if notice is not None:
            notice.close()


def identity_rows(path, times=False):
    """Resolve XML IDs needed for PID/TID attribution, without retaining trees."""
    refs, columns, parent = {}, [], None
    relevant = {"tid", "pid", "process", "thread"}
    if times:
        relevant |= {"event-time", "start-time", "duration", "event-type", "signpost-name"}

    def resolve(e):
        if "ref" in e.attrib:
            return refs.get(int(e.attrib["ref"]))
        values = [resolve(c) for c in e]
        if e.tag not in relevant:
            return None
        if e.tag == "process":
            value = {"pid": values[0]}
        elif e.tag == "thread":
            process = values[1] if len(values) > 1 else None
            value = {"tid": values[0], "pid": process.get("pid") if isinstance(process, dict) else None}
        elif e.tag in {"tid", "pid", "event-time", "start-time", "duration"}:
            value = int(e.text) if e.text else None
        else:
            value = e.text or e.get("fmt")
        if "id" in e.attrib:
            refs[int(e.attrib["id"])] = value
        return value

    with gzip.open(path, "rb") as source:
        for event, e in ET.iterparse(source, events=("start", "end")):
            if event == "start" and e.tag == "node":
                parent = e
            elif event == "end" and e.tag == "schema":
                columns = [c.findtext("mnemonic") for c in e.findall("col")]
            elif event == "end" and e.tag == "row":
                yield dict(zip(columns, (resolve(c) for c in e)))
                parent.remove(e)


def audit_exports(out, tables, phase, threads, record_log):
    counts = {str(threads["producer_tid"]): 0}
    if threads.get("consumer_tid"):
        counts[str(threads["consumer_tid"])] = 0
    signposts, seen = [], set()
    for table in tables:
        filename = table.get("export_file")
        key = (table.get("run"), table["schema"])
        if not filename or table["schema"] not in ("thread-state", "os-signpost") or key in seen:
            continue
        seen.add(key)
        for row in identity_rows(out / filename, times=table["schema"] == "os-signpost"):
            thread = row.get("thread")
            if not isinstance(thread, dict) or thread.get("pid") != phase["pid"]:
                continue
            if table["schema"] == "thread-state":
                tid = str(thread.get("tid"))
                if tid in counts:
                    counts[tid] += 1
            elif row.get("name") == "FF measured":
                signposts.append(dict(event=row.get("event-type"), time_ns=row.get("time")))
    warnings = []
    if "lost events" in record_log.lower():
        warnings.append("xctrace reported lost events")
    if any(count == 0 for count in counts.values()):
        warnings.append("target thread-state records are missing; do not infer scheduling from other threads")
    begin = [r["time_ns"] for r in signposts if r["event"] == "Begin"]
    end = [r["time_ns"] for r in signposts if r["event"] == "End"]
    expected = phase["end_ns"] - phase["start_ns"]
    observed = end[0] - begin[0] if len(begin) == len(end) == 1 else None
    if observed is None or abs(observed - expected) > max(1000000, expected * .01):
        warnings.append("signpost interval is missing/truncated or disagrees with the measured clock interval")
    return dict(target_state_rows=counts, signposts=signposts, expected_region_ns=expected,
                observed_region_ns=observed, warnings=warnings, passed_basic_checks=not warnings)
