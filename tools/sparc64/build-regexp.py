#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Build the snapshot/TurboFan/native-RegExp configuration and record inputs."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
output = root / 'out'
output.mkdir(exist_ok=True)
if shutil.disk_usage(root).free < 3 * 1024 ** 3:
    raise SystemExit('At least 3 GiB of free build space is required')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sources():
    paths = set()
    for command in [['git', 'diff', '--name-only', 'HEAD'],
                    ['git', 'ls-files', '--others', '--exclude-standard']]:
        paths.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
    return {name: digest(root / name) for name in sorted(paths)
            if (root / name).is_file() and not name.startswith('tools/sparc64/')}


before = sources()
results = []
with (output / 'sparc64-regexp-build.log').open('w') as log:
    for command in [['sh', 'tools/sparc64/configure-snapshots.sh'], ['make', '-j96']]:
        started = time.monotonic()
        result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
        results.append(dict(command=command, exit_code=result.returncode,
                            seconds=round(time.monotonic() - started, 3)))
        (output / 'sparc64-regexp-build.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(results[-1]), flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
assert sources() == before, 'Sources changed during the build'
results[-1]['source_sha256'] = before
results[-1]['binary_sha256'] = {name: digest(output / 'Release' / name) for name in ['node', 'd8']}
(output / 'sparc64-regexp-build.json').write_text(json.dumps(results, indent=2) + '\n')
print('REGEXP_BUILD_OK', flush=True)
