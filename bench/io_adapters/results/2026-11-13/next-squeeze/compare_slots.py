import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path.cwd()
out = root / 'build/next-squeeze-slots'
out.mkdir(parents=True, exist_ok=True)
commands = []
hashes = {}
for version, header in [('before', 'baseline-uring.h'), ('after', 'compact-uring.h')]:
    overlay = out / version
    (overlay / 'extension').mkdir(parents=True, exist_ok=True)
    for name in ['base', 'memory', 'task', 'utility', 'executor']:
        (overlay / name).symlink_to(root / name, target_is_directory=True)
    (overlay / 'flow').symlink_to(root / 'build/batch-prefix-final/after/flow', target_is_directory=True)
    (overlay / 'extension/detail').symlink_to(root / 'extension/detail', target_is_directory=True)
    data = (root / header).read_bytes()
    (overlay / 'extension/io_uring_awaitable.h').write_bytes(data)
    hashes[version] = hashlib.sha256(data).hexdigest()
    for compiler in ['clang++', 'c++']:
        command = [compiler, '-std=c++14', '-O3', '-DNDEBUG', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-I' + str(overlay), '-I' + str(root), str(root / 'bench/io_adapters/uring.cpp'),
                   '-luring', '-o', str(out / (compiler + '-' + version))]
        subprocess.run(command, check=True)
        commands.append(command)
(out / 'environment.json').write_text(json.dumps(dict(commands=commands, hashes=hashes,
    count=1048576, repeats=3, cpu=3), indent=2) + '\n')
with (out / 'results.csv').open('w') as stream:
    writer = csv.writer(stream)
    writer.writerow(['repeat', 'compiler', 'version', 'path', 'mode', 'batch', 'ns_per_request'])
    for repeat in range(3):
        for compiler in ['clang++', 'c++']:
            for batch in [32, 256, 1024]:
                for path in ['staged', 'kernel']:
                    for mode in ['context', 'fast']:
                        for version in (['before', 'after'] if repeat % 2 == 0 else ['after', 'before']):
                            result = subprocess.check_output([str(out / (compiler + '-' + version)),
                                path, mode, '1048576', str(batch), '3'], text=True)
                            row = next(csv.reader(result.splitlines()))
                            writer.writerow([repeat, compiler, version, path, mode, batch, row[5]])
                            stream.flush()
        print('slot sweep', repeat + 1, flush=True)
