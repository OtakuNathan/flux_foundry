import csv
import shutil
import io
import json
import hashlib
from pathlib import Path
import subprocess

root = Path.cwd()
out = root / 'build/pool-batch-four-final'
out.mkdir(parents=True, exist_ok=True)
commands = []
hashes = {}
for version in ['before', 'after']:
    overlay = out / version
    # Real directories are essential: directory symlinks followed by ../
    # can escape an overlay and include the baseline pooling header first.
    for folder in ['base', 'memory', 'flow', 'task', 'utility', 'executor']:
        shutil.copytree(root / folder, overlay / folder)
    for target, replacement in [
        ('memory/pooling.h', root / 'build/pool-batch-input' / ('before-pooling.h' if version == 'before' else 'after4-pooling.h')),
        ('flow/flow_runner.h', root / 'after-flow_runner.h'),
        ('executor/uv_executor.h', root / 'after-uv_executor.h')]:
        data = replacement.read_bytes()
        (overlay / target).write_bytes(data)
        hashes[version + '-' + target] = hashlib.sha256(data).hexdigest()
    for compiler in ['clang++', 'c++']:
        command = [compiler, '-std=c++14', '-O3', '-DNDEBUG', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(overlay), '-I' + str(root), str(root / 'bench/io_adapters/uv_handoff.cpp'),
                   '-luv', '-o', str(out / (compiler + '-' + version))]
        subprocess.run(command, check=True)
        commands.append(command)
(out / 'environment.json').write_text(json.dumps(dict(commands=commands, hashes=hashes,
    samples=32768, repeats=7, owner_cpu=3, producer_cpus=[2, 4], capacity=1024, budget=16), indent=2) + '\n')
with (out / 'results.csv').open('w') as stream, (out / 'runtime.log').open('w') as log:
    writer = None
    for repeat in range(7):
        for compiler in ['clang++', 'c++']:
            for producer in [2, 4]:
                for profile in ['steady500k', 'overload2m']:
                    for version in (['before', 'after'] if repeat % 2 == 0 else ['after', 'before']):
                        argv = [str(out / (compiler + '-' + version)), 'ordinary-fast', profile,
                                '32768', '128', '16', '3', str(producer)]
                        result = subprocess.check_output(argv, text=True, stderr=log, timeout=60)
                        rows = list(csv.DictReader(io.StringIO(result)))
                        assert len(rows) == 2 and rows[1]['phase'] == 'measured'
                        row = rows[1]
                        row['repeat'] = repeat
                        record = dict(compiler=compiler, version=version, **row)
                        if writer is None:
                            writer = csv.DictWriter(stream, fieldnames=list(record))
                            writer.writeheader()
                        writer.writerow(record)
                        stream.flush()
        print('pool batch sweep', repeat + 1, flush=True)
