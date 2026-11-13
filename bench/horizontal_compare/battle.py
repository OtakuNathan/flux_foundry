#!/usr/bin/env python3
"""Reproducible success-path Asio/FF comparison; see README.md."""
import argparse
import csv
import datetime
import hashlib
import io
import itertools
import json
import os
from pathlib import Path
import platform
import random
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ASIO_TAG = 'asio-1-39-0'
ASIO_COMMIT = '190a8d8daf1cc62909b4e0c72ff491ba264ca98e'
LANES = ('raw', 'erased', 'coroutine', 'ff-normal', 'ff-fast-normal', 'ff-fast-fast')
KEYS = ('group', 'backend', 'lane', 'profile', 'policy', 'hops')
METRICS = ('completed_per_s', 'e2e_p50_us', 'e2e_p99_us', 'e2e_p999_us', 'e2e_max_us',
           'actual_p99_us', 'admitted_p99_us', 'submit_lag_p99_us', 'admission_wait_p99_us',
           'cpu_percent', 'peak_inflight', 'admission_retries')


def output(cmd):
    return subprocess.check_output(cmd, cwd=ROOT, text=True, stderr=subprocess.STDOUT).strip()


def optional(cmd):
    try:
        return output(cmd)
    except (OSError, subprocess.CalledProcessError) as error:
        return str(error)


def summarize(rows, directory):
    groups = {}
    for row in rows:
        groups.setdefault(tuple(row[k] for k in KEYS), []).append(row)
    summary = []
    for key, values in sorted(groups.items()):
        for metric in METRICS:
            numbers = [float(row[metric]) for row in values]
            summary.append(dict(zip(KEYS, key), metric=metric, trials=len(numbers),
                                median=statistics.median(numbers), min=min(numbers), max=max(numbers)))
    with (directory / 'summary.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(KEYS) + ['metric', 'trials', 'median', 'min', 'max'])
        writer.writeheader()
        writer.writerows(summary)
    with (directory / 'summary.txt').open('w') as f:
        f.write('Medians across independent processes; ranges are trial variability.\n'
                'Only saturated completed_per_s measures capacity. Steady500k is offered load.\n'
                'CPU is process CPU / wall time: 200% means two busy cores.\n'
                'group | backend | lane | profile | policy | hops | metric | median | min | max\n')
        for row in summary:
            f.write(' | '.join(str(row[k]) for k in KEYS) + ' | ' + row['metric'] +
                    ' | %.3f | %.3f | %.3f\n' % (row['median'], row['min'], row['max']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--asio', type=Path, help='existing checkout at the pinned Asio commit')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'))
    parser.add_argument('--trials', type=int, default=6)
    parser.add_argument('--samples', type=int, default=131072)
    parser.add_argument('--work', type=int, default=128)
    parser.add_argument('--capacity', type=int, default=1024)
    parser.add_argument('--owner-cpu', type=int, default=-1)
    parser.add_argument('--producer-cpu', type=int, default=-1)
    parser.add_argument('--groups', nargs='+', choices=['chain', 'handoff'], default=['chain', 'handoff'])
    parser.add_argument('--lanes', nargs='+', choices=LANES, default=list(LANES))
    parser.add_argument('--policies', nargs='+', choices=['blocking', 'poll', 'hybrid-2000'], default=['blocking', 'poll'])
    parser.add_argument('--profiles', nargs='+', choices=['saturated', 'steady500k'], default=['saturated', 'steady500k'])
    parser.add_argument('--hops', nargs='+', type=int, choices=[1, 4], default=[1, 4])
    args = parser.parse_args()
    if not (args.trials > 0 and 0 < args.samples <= 1000000 and 0 <= args.work <= 1000000 and 0 < args.capacity <= 1024):
        parser.error('invalid trials/sample/work/capacity bounds')
    if platform.system() != 'Linux' and ('hybrid-2000' in args.policies or args.owner_cpu != -1 or args.producer_cpu != -1):
        parser.error('hybrid and CPU pinning are Linux-only')
    directory = (args.out or ROOT / 'build' / ('asio-battle-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    (directory / 'raw').mkdir()
    print('Results:', directory, flush=True)
    asio = args.asio.resolve() if args.asio else directory / 'asio'
    if not args.asio:
        with (directory / 'fetch.log').open('w') as f:
            subprocess.run(['git', 'clone', '--depth', '1', '--branch', ASIO_TAG,
                            'https://github.com/chriskohlhoff/asio.git', str(asio)], stdout=f, stderr=f, check=True)
    revision = output(['git', '-C', str(asio), 'rev-parse', 'HEAD'])
    if revision != ASIO_COMMIT or output(['git', '-C', str(asio), 'status', '--porcelain']):
        raise RuntimeError('Asio checkout must be clean at ' + ASIO_COMMIT)
    uvflags = shlex.split(output(['pkg-config', '--cflags', '--libs', 'libuv']))
    binary = directory / 'battle'
    command = [args.cxx, '-std=c++20', '-O3', '-DNDEBUG', '-pthread', '-I' + str(ROOT),
               '-I' + str(asio / 'asio/include'), str(ROOT / 'bench/horizontal_compare/battle.cpp'),
               *uvflags, '-o', str(binary)]
    tracked = output(['git', 'ls-files', '*.h', '*.hpp']).splitlines()
    sources = tracked + ['bench/horizontal_compare/battle.cpp', 'bench/horizontal_compare/battle.py']
    metadata = {'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'platform': platform.platform(), 'machine': platform.machine(),
                'revision': output(['git', 'rev-parse', 'HEAD']), 'tracked_diff': output(['git', 'diff', 'HEAD']),
                'source_sha256': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
                'asio_tag': ASIO_TAG, 'asio_commit': revision, 'compiler': output([args.cxx, '--version']),
                'libuv': output(['pkg-config', '--modversion', 'libuv']), 'compile_command': command,
                'args': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                'commands': [], 'seed': 20261113,
                'host': optional(['sysctl', '-n', 'machdep.cpu.brand_string']) if platform.system() == 'Darwin' else optional(['lscpu']),
                'power_before': optional(['pmset', '-g', 'batt']) if platform.system() == 'Darwin' else optional(['cat', '/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor'])}
    def save_metadata():
        (directory / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n')
    save_metadata()
    with (directory / 'compile.log').open('w') as f:
        subprocess.run(command, cwd=ROOT, stdout=f, stderr=f, check=True)
    cells = []
    for group, hops, profile, policy in itertools.product(args.groups, args.hops, args.profiles, args.policies):
        if group == 'handoff' and hops != 1:
            continue
        variants = [(backend, lane) for backend in (['asio'] if group == 'chain' else ['asio', 'uv'])
                    for lane in args.lanes if group == 'chain' or lane != 'coroutine']
        if variants:
            cells.append((group, hops, profile, policy, variants))
    if not cells:
        raise RuntimeError('empty comparison matrix')
    rows = []
    total = args.trials * sum(len(cell[-1]) for cell in cells)
    rng = random.Random(metadata['seed'])
    with (directory / 'results.csv').open('w', newline='') as f:
        writer = None
        for trial in range(args.trials):
            order = list(cells)
            rng.shuffle(order)
            for group, hops, profile, policy, variants in order:
                # Rotate every variant through positions, reverse blocks on alternating trials.
                offset = trial % len(variants)
                ordered = variants[offset:] + variants[:offset]
                if trial % 2:
                    ordered.reverse()
                for backend, lane in ordered:
                    cmd = [str(binary), group, backend, lane, str(hops), profile, policy,
                           str(args.samples), str(args.work), str(args.capacity), str(args.owner_cpu), str(args.producer_cpu)]
                    index = len(rows)
                    metadata['commands'].append({'trial': trial + 1, 'index': index, 'argv': cmd})
                    save_metadata()
                    process = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
                    (directory / 'raw' / ('%04d.csv' % index)).write_text(process.stdout)
                    (directory / 'raw' / ('%04d.stderr' % index)).write_text(process.stderr)
                    process.check_returncode()
                    records = list(csv.DictReader(io.StringIO(process.stdout)))
                    if len(records) != 2 or [r['phase'] for r in records] != ['warmup', 'measured']:
                        raise RuntimeError('invalid warmup/measured output')
                    row = records[1]
                    expected = dict(group=group, backend=backend, lane=lane, profile=profile, policy=policy,
                                    hops=str(hops), samples=str(args.samples), work=str(args.work), capacity=str(args.capacity),
                                    owner_cpu=str(args.owner_cpu), producer_cpu=str(args.producer_cpu))
                    if any(row[k] != value for k, value in expected.items()):
                        raise RuntimeError('command/result mismatch')
                    row = dict(trial=trial + 1, index=index, **row)
                    if writer is None:
                        writer = csv.DictWriter(f, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    f.flush()
                    rows.append(row)
                    print('%d/%d trial=%d %s %s %s h%d %s %s p99=%sus rate=%s/s' %
                          (len(rows), total, trial+1, group, backend, lane, hops, profile, policy,
                           row['e2e_p99_us'], row['completed_per_s']), flush=True)
    summarize(rows, directory)
    metadata['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    metadata['power_after'] = optional(['pmset', '-g', 'batt']) if platform.system() == 'Darwin' else metadata['power_before']
    metadata['completed_processes'] = len(rows)
    save_metadata()
    print('Complete:', directory, flush=True)


if __name__ == '__main__':
    main()
