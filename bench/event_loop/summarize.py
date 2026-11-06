#!/usr/bin/env python3
"""Summarize per-run event-loop metrics; never merge unlike backends/budgets."""
import argparse
import collections
import csv
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('csv')
p.add_argument('--output')
a = p.parse_args()
with open(a.csv, newline='', encoding='utf-8') as f:
    rows = list(csv.DictReader(f))
if not rows:
    raise ValueError('empty CSV')
groups = collections.defaultdict(list)
for r in rows:
    if None in r or any(v is None for v in r.values()):
        raise ValueError('CSV shape mismatch')
    q = [float(r[k]) for k in ('e2e_p50_us','e2e_p95_us','e2e_p99_us','e2e_p999_us','e2e_max_us')]
    if q != sorted(q) or q[0] < 0:
        raise ValueError('invalid E2E quantile order')
    if not 0 <= float(r['consumer_cpu_pct']) <= 102:
        raise ValueError('invalid consumer CPU measurement')
    key = (r['profile'],r['mode'],r['backend'],int(r['budget']),int(r['capacity']))
    groups[key].append(r)
lines = [
    '# Event-loop probe summary', '',
    f'Source: `{a.csv}`. {len(rows)} completed cases; {sum(int(r["samples"]) for r in rows):,} validated events.', '',
    '**Medians of per-run metrics**, not pooled percentiles. Latencies are microseconds; CPU is percent of one consumer core. Range is the minimum–maximum per-run p99.', '',
    '| Profile | Lane | Backend | Budget | Capacity | Runs | Completed/s | p50 | p99 | p99 range | p99.9 | Lag p99 | Consumer CPU % | Peak outstanding | Schedule drain ms |',
    '|---|---|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|',
]
for (profile,lane,backend,budget,capacity),group in sorted(groups.items()):
    def med(k):
        return statistics.median(float(r[k]) for r in group)
    tails = [float(r['e2e_p99_us']) for r in group]
    lines.append(
        f'| {profile} | {lane} | {backend} | {budget} | {capacity} | {len(group)} | '
        f'{med("completed_per_s"):.0f} | {med("e2e_p50_us"):.3f} | {med("e2e_p99_us"):.3f} | '
        f'{min(tails):.3f}–{max(tails):.3f} | {med("e2e_p999_us"):.3f} | '
        f'{med("submit_lag_p99_us"):.3f} | {med("consumer_cpu_pct"):.3f} | '
        f'{med("peak_outstanding"):.0f} | {med("drain_schedule_ms"):.3f} |'
    )
lines += ['', 'Metric limitations and reproduction: ../README.md. Pi-specific interpretation: pi4b-20260930-notes.md.', '']
text = '\n'.join(lines)
if a.output:
    with open(a.output,'w',encoding='utf-8') as f:
        f.write(text)
    print(f'Validated {len(rows)} cases; wrote {a.output}')
else:
    print(text)
