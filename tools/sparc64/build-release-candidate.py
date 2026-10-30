#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Record and run a fresh native SPARC64 release-candidate build."""
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert shutil.disk_usage(root).free > 15 * 1024**3
assert not (root / 'out/Release').exists(), 'A fresh build directory is required'
assert not (root / 'config.gypi').exists(), 'A fresh configuration is required'
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()


def sources():
    names = set(subprocess.check_output(['git', 'ls-files'], cwd=root, text=True).splitlines())
    names.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'],
                                         cwd=root, text=True).splitlines())
    return {name: digest(root / name) for name in sorted(names) if (root / name).is_file()}


before = sources()
output = root / 'out/release-candidate-build'
output.mkdir(exist_ok=False)
env = dict(os.environ, CCACHE_DISABLE='1')
record = dict(passed=False, fresh_output=True, copied_build_artifacts=False,
              base_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
              source_sha256=before, results=[],
              environment={key: env.get(key) for key in ['CC', 'CXX', 'RUSTFLAGS',
                  'RUSTC_WRAPPER', 'CARGO_TARGET_DIR', 'GYP_DEFINES', 'CCACHE_DISABLE']})


def save():
    (output / 'build.json').write_text(json.dumps(record, indent=2) + '\n')


save()
for name, command in [('configure', ['sh', 'tools/sparc64/configure-node-features.sh',
                                    'all', '--prefix=/usr/local']),
                       ('make', ['make', '-j96'])]:
    started = time.monotonic()
    with (output / (name + '.log')).open('w') as log:
        code = subprocess.run(command, cwd=root, env=env, stdout=log,
                              stderr=subprocess.STDOUT).returncode
    result = dict(name=name, command=command, exit_code=code,
                  seconds=round(time.monotonic()-started, 3))
    record['results'].append(result)
    save()
    print(json.dumps(result), flush=True)
    if code != 0:
        raise SystemExit(code)
assert before == sources(), 'Sources changed during the clean build'
config = ast.literal_eval((root / 'config.gypi').read_text())['variables']
assert config['target_arch'] == 'sparc64'
assert config['node_use_amaro'] == config['node_use_ffi'] == 'true'
assert config['v8_enable_temporal_support'] == config['v8_enable_verify_heap'] == 1
makefile = (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
release = re.search(r'DEFS_Release :=(.*?)(?=\n\n)', makefile, re.S).group(1)
defines = set(re.findall(r'-D([A-Za-z_][A-Za-z_0-9]*)', release))
assert {'VERIFY_HEAP', 'V8_TEMPORAL_SUPPORT', 'V8_ENABLE_TURBOFAN',
        'V8_ENABLE_SPARKPLUG', 'V8_ENABLE_WEBASSEMBLY'} <= defines
assert (root / 'out/Release/obj/gen/release/libnode_crates.a.sparc64.a.json').is_file()
record.update(passed=True, configuration=config, v8_compile_defines=sorted(defines),
              binary_sha256={name: digest(root / 'out/Release' / name)
                  for name in ['node', 'd8', 'cctest', 'node_mksnapshot']})
save()
print('CLEAN_RELEASE_CANDIDATE_BUILD_OK', flush=True)
