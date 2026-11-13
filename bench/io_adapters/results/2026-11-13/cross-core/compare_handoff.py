import csv
import io
import json
from pathlib import Path
import subprocess

root = Path.cwd()
out = root / 'build/cross-core-latency'
out.mkdir(parents=True, exist_ok=True)
commands = []
for compiler in ['clang++', 'c++']:
    for version in ['before', 'after']:
        overlay = root / 'build/cross-core-screen' / version
        command = [compiler, '-std=c++14', '-O3', '-DNDEBUG', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(overlay), '-I' + str(root), str(root / 'bench/io_adapters/uv_handoff.cpp'),
                   '-luv', '-o', str(out / (compiler + '-' + version))]
        subprocess.run(command, check=True)
        commands.append(command)
(out / 'environment.json').write_text(json.dumps(dict(commands=commands, samples=32768,
    repeats=5, owner_cpu=3, producer_cpus=[2, 4], capacity=1024), indent=2) + '\n')
with (out / 'results.csv').open('w') as stream, (out / 'runtime.log').open('w') as log:
    writer = None
    for repeat in range(5):
        for compiler in ['clang++', 'c++']:
            for producer in [2, 4]:
                for budget in [16, 64]:
                    for work in [0, 128]:
                        for profile in ['steady500k', 'overload2m']:
                            for mode in ['raw', 'ordinary-fast']:
                                for version in (['before', 'after'] if repeat % 2 == 0 else ['after', 'before']):
                                    argv = [str(out / (compiler + '-' + version)), mode, profile, '32768',
                                            str(work), str(budget), '3', str(producer)]
                                    result = subprocess.check_output(argv, text=True, stderr=log, timeout=60)
                                    rows = list(csv.DictReader(io.StringIO(result)))
                                    assert len(rows) == 2 and rows[0]['phase'] == 'warmup' and rows[1]['phase'] == 'measured'
                                    row = rows[1]
                                    row['repeat'] = repeat
                                    record = dict(compiler=compiler, version=version, **row)
                                    if writer is None:
                                        writer = csv.DictWriter(stream, fieldnames=list(record))
                                        writer.writeheader()
                                    writer.writerow(record)
                                    stream.flush()
        print('latency sweep', repeat + 1, flush=True)
