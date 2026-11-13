import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess

root = Path.cwd()
parser = argparse.ArgumentParser()
parser.add_argument('--out', default='build/cross-core-screen')
parser.add_argument('--repeats', type=int, default=3)
args = parser.parse_args()
out = root / args.out
out.mkdir(parents=True, exist_ok=True)
commands, hashes = [], {}
for version in ['before', 'after']:
    overlay = out / version
    (overlay / 'executor').mkdir(parents=True, exist_ok=True)
    for name in ['base', 'memory', 'task', 'utility']:
        (overlay / name).symlink_to(root / name, target_is_directory=True)
    # The latency driver also consumes this overlay. Keep the runner-factory
    # optimization identical on both sides, as in the measured setup.
    (overlay / 'flow').symlink_to(root / 'build/batch-prefix-final/after/flow', target_is_directory=True)
    data = (root / (version + '-uv_executor.h')).read_bytes()
    (overlay / 'executor/uv_executor.h').write_bytes(data)
    hashes[version] = hashlib.sha256(data).hexdigest()
    for compiler in ['clang++', 'c++']:
        command = [compiler, '-std=c++14', '-O3', '-DNDEBUG', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(overlay), '-I' + str(root), str(root / 'bench/io_adapters/uv.cpp'),
                   '-luv', '-o', str(out / (compiler + '-' + version))]
        subprocess.run(command, check=True)
        commands.append(command)
(out / 'environment.json').write_text(json.dumps(dict(commands=commands, hashes=hashes,
    count=1048576, repeats=args.repeats, cpu=3), indent=2) + '\n')
with (out / 'results.csv').open('w') as stream:
    writer = csv.writer(stream)
    writer.writerow(['repeat', 'compiler', 'version', 'producers', 'first_cpu', 'budget', 'ns_per_task', 'close_us', 'retries'])
    for repeat in range(args.repeats):
        for compiler in ['clang++', 'c++']:
            for producers, first in [(0, 0), (1, 2), (1, 4), (3, 0), (3, 4)]:
                for budget in [16, 64]:
                    for version in (['before', 'after'] if repeat % 2 == 0 else ['after', 'before']):
                        result = subprocess.check_output([str(out / (compiler + '-' + version)), version,
                            str(producers), '1048576', '1024', str(budget), '3', str(first)], text=True)
                        row = next(csv.reader(io.StringIO(result)))
                        writer.writerow([repeat, compiler, version, producers, first, budget, *row[6:9]])
                        stream.flush()
        print('UV screening', repeat + 1, flush=True)
