#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Build real Node feature configurations and retain source/binary provenance."""
import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import resource
import re
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stage', choices=['amaro', 'temporal', 'ffi', 'all'], required=True)
parser.add_argument('--incremental', action='store_true')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert shutil.disk_usage(root).free > 5 * 1024 ** 3
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
parent_file = root / 'out/sparc64-node-features-parent.json'
assert digest(parent_file) == '3df269cf8395ac0673cdeadd8c2813222715a03b44e33f58cc36913cd62c269f'


def sources():
    names = set()
    for command in [['git', 'diff', '--name-only', 'HEAD'],
                    ['git', 'ls-files', '--others', '--exclude-standard']]:
        names.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
    return {name: digest(root / name) for name in sorted(names)
            if (root / name).is_file() and not name.startswith('tools/sparc64/')}


stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
output = root / 'out/node-features-builds' / (stamp + '-' + args.stage)
output.mkdir(parents=True, exist_ok=False)
before = sources()
record = dict(passed=False, stage=args.stage, source_sha256=before,
              parent_manifest_sha256=digest(parent_file), results=[])
target = root / ('out/sparc64-node-features-build-' + args.stage + '.json')
if target.exists():
    shutil.copy2(target, output / 'previous-build.json')


def save():
    data = json.dumps(record, indent=2) + '\n'
    (output / 'build.json').write_text(data)
    target.write_text(data)


save()
commands = [] if args.incremental else [
    ['sh', 'tools/sparc64/configure-node-features.sh', args.stage]]
commands.append(['make', '-j96'])
for command in commands:
    if command[0] == 'make':
        variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
        assert variables['target_arch'] == 'sparc64'
        assert variables['node_use_amaro'] == 'true'
        assert variables['node_use_ffi'] == ('true' if args.stage in ['ffi', 'all'] else 'false')
        assert variables['v8_enable_temporal_support'] == int(args.stage in ['temporal', 'all'])
        assert variables['v8_enable_webassembly'] == 1
        assert variables['v8_enable_verify_heap'] == 1
        assert variables['node_use_node_snapshot'] == 'true'
        record['configuration'] = variables
        record['config_gypi_sha256'] = digest(root / 'config.gypi')
        save()
    start = time.monotonic()
    with (output / (command[0] + '.log')).open('w') as log:
        code = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT).returncode
    item = dict(command=command, exit_code=code, seconds=round(time.monotonic() - start, 3))
    record['results'].append(item)
    save()
    print(json.dumps(item), flush=True)
    if code:
        raise SystemExit(code)
assert sources() == before, 'Compiled sources changed while the build was running'
makefile = (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
release = re.search(r'DEFS_Release :=(.*?)(?=\n\n)', makefile, re.S).group(1)
defines = set(re.findall(r'-D([A-Za-z_][A-Za-z_0-9]*)', release))
assert {'VERIFY_HEAP', 'V8_ENABLE_TURBOFAN', 'V8_ENABLE_SPARKPLUG',
        'V8_ENABLE_WEBASSEMBLY'} <= defines
assert ('V8_TEMPORAL_SUPPORT' in defines) == (args.stage in ['temporal', 'all'])
record['v8_compile_defines'] = sorted(defines)
record['binary_sha256'] = {name: digest(root / 'out/Release' / name)
                           for name in ['node', 'd8', 'cctest', 'node_mksnapshot']}
record['passed'] = True
save()
print('NODE_FEATURES_BUILD_OK', args.stage, str(output), flush=True)
