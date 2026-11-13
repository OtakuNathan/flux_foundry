"""Post-process single-producer/FIFO-consumer traces without retaining all samples."""
import csv
import gzip
import heapq
from pathlib import Path
import shutil


COUNTS = ("planned", "submitted", "started", "completed")
METRICS = ("e2e", "submit_lag", "pre_dispatch", "dispatch_to_begin", "service",
           "dispatch_call", "submit_call", "producer_gap", "consumer_gap")


def summarize(path):
    path = Path(path)
    windows, spikes = {}, []
    previous_submit_end = previous_end = 0
    total = 0
    maxima = {key: 0 for key in METRICS}

    def window(timestamp):
        index = timestamp // 1000000  # 1 ms bins; original timestamps are ns.
        if index not in windows:
            windows[index] = dict.fromkeys(COUNTS + METRICS, 0)
        return windows[index]

    with path.open(newline="") as source:
        for values in csv.DictReader(source):
            row = {key: int(value) for key, value in values.items()}
            p, ss, se, ds, de, begin, end = (row[key + "_ns"] for key in
                ("planned", "submit_start", "submit_end", "dispatch_start", "dispatch_end", "begin", "end"))
            if not (row["id"] == total and 0 <= p <= ss <= ds <= de <= se and
                    ds <= begin <= end and (total == 0 or
                    (ss >= previous_submit_end and begin >= previous_end))):
                raise RuntimeError("invalid single-producer/FIFO timeline in " + str(path))
            metrics = dict(e2e=end-p, submit_lag=ss-p, pre_dispatch=ds-ss,
                           dispatch_to_begin=begin-ds, service=end-begin,
                           dispatch_call=de-ds, submit_call=se-ss,
                           producer_gap=ss-previous_submit_end if total else 0,
                           consumer_gap=begin-previous_end if total else 0)
            for timestamp, key in ((p, "planned"), (ss, "submitted"), (begin, "started"), (end, "completed")):
                window(timestamp)[key] += 1
            bucket = window(ss)
            for key, value in metrics.items():
                bucket[key] = max(bucket[key], value)
                maxima[key] = max(maxima[key], value)
            spike = (metrics["e2e"], total, {**row, **{k + "_us": v / 1000 for k, v in metrics.items()}})
            if len(spikes) < 16:
                heapq.heappush(spikes, spike)
            else:
                heapq.heappushpop(spikes, spike)
            previous_submit_end, previous_end = se, end
            total += 1
    if not total:
        raise RuntimeError("empty sample trace: " + str(path))

    stem = str(path)[:-len(".samples.csv")]
    with open(stem + ".timeline.csv", "w", newline="") as dest:
        fields = ["window_start_us", *COUNTS, "pending_submit_end", "inflight_end"]
        fields += [key + "_max_us" for key in METRICS]
        writer = csv.DictWriter(dest, fieldnames=fields)
        writer.writeheader()
        planned = submitted = completed = 0
        for index in range(max(windows) + 1):
            bucket = windows.get(index, dict.fromkeys(COUNTS + METRICS, 0))
            planned += bucket["planned"]
            submitted += bucket["submitted"]
            completed += bucket["completed"]
            writer.writerow(dict(window_start_us=index * 1000,
                **{k: bucket[k] for k in COUNTS}, pending_submit_end=planned-submitted,
                inflight_end=submitted-completed,
                **{k + "_max_us": bucket[k] / 1000 for k in METRICS}))
    with open(stem + ".spikes.csv", "w", newline="") as dest:
        writer = csv.DictWriter(dest, fieldnames=list(spikes[0][2]))
        writer.writeheader()
        writer.writerows(item[2] for item in sorted(spikes, reverse=True))
    # Compress only after the benchmark process exits. Keep the raw CSV if this fails.
    with path.open("rb") as source, gzip.open(str(path) + ".gz", "wb", compresslevel=1) as dest:
        shutil.copyfileobj(source, dest)
    path.unlink()
    return "%s | events=%d | max_e2e_us=%.3f | max_producer_gap_us=%.3f | max_consumer_gap_us=%.3f" % (
        Path(stem).name, total, maxima["e2e"] / 1000,
        maxima["producer_gap"] / 1000, maxima["consumer_gap"] / 1000)
