#!/usr/bin/env python3
"""Validate and summarize load-probe CSV. Run-to-run medians, not pooled quantiles."""
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
    if float(r['dispatch_p99_us']) > float(r['dispatch_max_us']):
        raise ValueError('invalid dispatch quantile order')
    groups[(r['profile'], r['mode'], int(r['capacity']))].append(r)
lines = [
    '# Queue capacity load-probe summary', '',
    f'Source: `{a.csv}`. {len(rows)} completed cases; {sum(int(r["samples"]) for r in rows):,} validated events.', '',
    'Values below are **medians of per-run metrics**; p99 is not a pooled percentile. The p99 range exposes run-to-run variability. All latency columns are microseconds.', '',
    '| Profile | Mode | Capacity | Runs | Completed/s | E2E p50 | E2E p99 | p99 min–max | E2E p99.9 | Submit lag p99 | Dispatch p99 | Peak outstanding | Schedule drain ms |',
    '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|',
]
for (profile, mode, capacity), group in sorted(groups.items()):
    def med(k):
        return statistics.median(float(r[k]) for r in group)
    tails = [float(r['e2e_p99_us']) for r in group]
    lines.append(
        f'| {profile} | {mode} | {capacity} | {len(group)} | {med("completed_per_s"):.0f} | '
        f'{med("e2e_p50_us"):.3f} | {med("e2e_p99_us"):.3f} | {min(tails):.3f}–{max(tails):.3f} | '
        f'{med("e2e_p999_us"):.3f} | {med("submit_lag_p99_us"):.3f} | {med("dispatch_p99_us"):.3f} | '
        f'{med("peak_outstanding"):.0f} | {med("drain_schedule_ms"):.3f} |'
    )
lines += ['', 'Interpretation and metric limitations: see `../README.md`; Pi-specific analysis: `pi4b-20260930-notes.md`.', '']
text = '\n'.join(lines)
if a.output:
    with open(a.output, 'w', encoding='utf-8') as f:
        f.write(text)
    print(f'Validated {len(rows)} cases; wrote {a.output}')
else:
    print(text)
