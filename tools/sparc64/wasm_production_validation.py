# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Source provenance shared by production and inherited Wasm acceptance audits."""
import hashlib
import json
from pathlib import Path
import subprocess

PRODUCTION_SOURCES = {
    'deps/v8/src/builtins/sparc64/builtins-sparc64.cc',
    'deps/v8/src/codegen/sparc64/assembler-sparc64.cc',
    'deps/v8/src/codegen/sparc64/cpu-sparc64.cc',
    'deps/v8/src/codegen/sparc64/simd-helpers-sparc64-inl.h',
    'deps/v8/src/codegen/sparc64/simd-sparc64-inl.h',
    'deps/v8/src/compiler/backend/instruction-selector.cc',
    'deps/v8/src/compiler/backend/sparc64/code-generator-sparc64.cc',
    'deps/v8/src/compiler/backend/sparc64/instruction-selector-sparc64.cc',
    'deps/v8/src/debug/debug-wasm-objects.cc',
    'deps/v8/src/deoptimizer/deoptimizer.cc',
    'deps/v8/src/execution/sparc64/frame-constants-sparc64.h',
    'deps/v8/src/flags/flag-definitions.h',
    'deps/v8/src/wasm/baseline/sparc64/liftoff-assembler-sparc64-inl.h',
    'deps/v8/src/wasm/baseline/sparc64/liftoff-simd-sparc64-inl.h',
    'deps/v8/src/wasm/wasm-debug.cc',
    'deps/v8/src/wasm/wasm-objects.cc',
}
PARENT_MANIFEST_SHA256 = '844dcce1de570469cb76aa98fe1633dc0b3967b59fea01663e45f041157415c5'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_sources(root):
    if (root / 'out/sparc64-node-features-parent.json').exists():
        from node_features_validation import validate_sources as validate_features
        return sorted(PRODUCTION_SOURCES | set(validate_features(root)))
    parent = root.with_name('node-sparc64-jspi')
    checkpoint = Path((parent / 'out/sparc64-jspi-checkpoint-path.txt').read_text().strip())
    assert digest(checkpoint / 'manifest.json') == PARENT_MANIFEST_SHA256
    manifest = json.loads((checkpoint / 'manifest.json').read_text())
    assert manifest == json.loads((root / 'out/sparc64-wasm-production-parent.json').read_text())
    assert manifest['profile'] == 'jspi'
    actual = set()
    for command in [['git', 'diff', '--name-only', '--diff-filter=ACMRT', 'HEAD'],
                    ['git', 'ls-files', '--others', '--exclude-standard']]:
        actual.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
    actual = {name for name in actual if not name.startswith('tools/sparc64/') and (root / name).is_file()}
    inherited = {name: value for name, value in manifest['source_sha256'].items()
                 if not name.startswith('tools/sparc64/')}
    changed = {name for name in actual | set(inherited)
               if name not in inherited or digest(root / name) != inherited[name]}
    assert changed == PRODUCTION_SOURCES, changed ^ PRODUCTION_SOURCES
    expected = dict(inherited)
    for name in PRODUCTION_SOURCES:
        if name in actual:
            expected[name] = digest(root / name)
        else:
            expected.pop(name, None)
    assert actual == set(expected)
    build = json.loads((root / 'out/sparc64-wasm-build.json').read_text())
    assert build and all(item['exit_code'] == 0 for item in build)
    assert build[-1]['source_sha256'] == expected
    assert build[-1]['binary_sha256'] == {name: digest(root / 'out/Release' / name)
                                         for name in ['node', 'd8']}
    return sorted(changed)
