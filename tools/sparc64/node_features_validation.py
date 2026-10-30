# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Authenticate the exact Node feature delta and its combined native build."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

PARENT_MANIFEST_SHA256 = '3df269cf8395ac0673cdeadd8c2813222715a03b44e33f58cc36913cd62c269f'
FEATURE_SOURCES = {
    'configure.py',
    'deps/crates/crates.gyp',
    'deps/crates/isolate_libm.py',
    'deps/v8/src/compiler/backend/sparc64/code-generator-sparc64.cc',
    'deps/libffi/generate-headers.py',
    'deps/libffi/libffi.gyp',
    'node.gyp',
    'src/ffi/fast.cc',
    'src/ffi/types.cc',
    'src/ffi/platforms/arm64.cc',
    'src/ffi/platforms/sparc64.cc',
}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_sources(root):
    parent = root.with_name('node-sparc64-wasm-production')
    checkpoint = parent / 'out/checkpoints/node-wasm-production-20261029T234057.721541Z'
    assert digest(checkpoint / 'manifest.json') == PARENT_MANIFEST_SHA256
    parent_file = root / 'out/sparc64-node-features-parent.json'
    assert digest(parent_file) == PARENT_MANIFEST_SHA256
    manifest = json.loads(parent_file.read_text())
    assert manifest['profile'] == 'wasm-production'
    actual = set()
    for command in [['git', 'diff', '--name-only', 'HEAD'],
                    ['git', 'ls-files', '--others', '--exclude-standard']]:
        actual.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
    actual = {name: digest(root / name) for name in actual
              if not name.startswith('tools/sparc64/') and (root / name).is_file()}
    inherited = {name: value for name, value in manifest['source_sha256'].items()
                 if not name.startswith('tools/sparc64/')}
    changed = {name for name in set(actual) | set(inherited)
               if actual.get(name) != inherited.get(name)}
    assert changed == FEATURE_SOURCES, changed ^ FEATURE_SOURCES
    build = json.loads((root / 'out/sparc64-node-features-build-all.json').read_text())
    assert build['passed'] and build['stage'] == 'all'
    assert build['parent_manifest_sha256'] == PARENT_MANIFEST_SHA256
    assert build['source_sha256'] == actual
    assert build['binary_sha256'] == {
        name: digest(root / 'out/Release' / name)
        for name in ['node', 'd8', 'cctest', 'node_mksnapshot']}
    config = build['configuration']
    assert config['node_use_amaro'] == config['node_use_ffi'] == 'true'
    assert config['v8_enable_temporal_support'] == config['v8_enable_webassembly'] == 1
    assert config['v8_enable_verify_heap'] == 1
    makefile = (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
    release = re.search(r'DEFS_Release :=(.*?)(?=\n\n)', makefile, re.S).group(1)
    defines = set(re.findall(r'-D([A-Za-z_][A-Za-z_0-9]*)', release))
    assert {'V8_ENABLE_TURBOFAN', 'V8_ENABLE_SPARKPLUG', 'V8_ENABLE_WEBASSEMBLY',
            'V8_TEMPORAL_SUPPORT', 'VERIFY_HEAP'} <= defines
    assert digest(root / 'config.gypi') == build['config_gypi_sha256']
    return sorted(changed)


if __name__ == '__main__':
    print(validate_sources(Path(__file__).resolve().parents[2]))
