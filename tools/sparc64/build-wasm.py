#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Build the snapshot/TurboFan/Wasm configuration and record inputs."""

from datetime import datetime, timezone
import argparse
import ast
import hashlib
import json
from pathlib import Path
import resource
import shutil
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--incremental', action='store_true',
                    help='Reuse the existing verified Wasm configuration')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
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


attempt = output / 'wasm-build-attempts' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
attempt.mkdir(parents=True)
for extension in ['log', 'json']:
    previous = output / f'sparc64-wasm-build.{extension}'
    if previous.exists():
        shutil.copy2(previous, attempt / f'previous-build.{extension}')
before = sources()
(attempt / 'source-sha256.json').write_text(json.dumps(before, indent=2) + '\n')
commands = [['sh', 'tools/sparc64/configure-wasm.sh'], ['make', '-j96']]
configuration = None
if args.incremental:
    variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
    assert variables['target_arch'] == 'sparc64'
    assert variables['v8_enable_webassembly'] == 1
    assert variables['node_use_node_snapshot'] == 'true'
    makefile = (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
    for feature in ['V8_ENABLE_WEBASSEMBLY', 'V8_ENABLE_SPARKPLUG', 'V8_ENABLE_TURBOFAN']:
        assert '-D' + feature in makefile, feature
    configuration = dict(config_gypi_sha256=digest(root / 'config.gypi'),
                         makefile_sha256=digest(root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk'))
    commands = [['make', '-j96']]
results = []
with (output / 'sparc64-wasm-build.log').open('w') as log:
    for command in commands:
        started = time.monotonic()
        result = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT)
        results.append(dict(command=command, exit_code=result.returncode,
                            seconds=round(time.monotonic() - started, 3),
                            reused_configuration=configuration))
        (output / 'sparc64-wasm-build.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(results[-1]), flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
assert sources() == before, 'Sources changed during the build'
results[-1]['source_sha256'] = before
results[-1]['binary_sha256'] = {name: digest(output / 'Release' / name) for name in ['node', 'd8']}
(output / 'sparc64-wasm-build.json').write_text(json.dumps(results, indent=2) + '\n')
print('WASM_BUILD_OK', flush=True)
