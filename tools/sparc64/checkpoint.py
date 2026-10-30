#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Preserve the dirty port sources, working binaries and validation evidence."""

import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['jitless', 'sparkplug', 'turbofan',
                                            'backend', 'runtime', 'snapshots', 'regexp', 'hardening', 'wasm', 'wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'],
                        default='jitless')
    profile = parser.parse_args().profile
    root = Path(__file__).resolve().parents[2]

    def run(*args):
        return subprocess.check_output(args, cwd=root, timeout=15)

    def sources():
        changed = run('git', 'diff', '--name-only', '--diff-filter=ACMRT',
                      '-z', 'HEAD').decode().split('\0')
        untracked = run('git', 'ls-files', '--others', '--exclude-standard',
                        '-z').decode().split('\0')
        return sorted({name for name in changed + untracked
                       if name and (root / name).is_file()})

    run('git', 'diff', '--check')
    if profile == 'turbofan':
        selected = json.loads(
            (root / 'out/sparc64-turbofan-selected.json').read_text())
        if not selected or not all(item['pass'] for item in selected):
            raise RuntimeError('Selected optimizing validation did not pass')
        config = json.loads(
            (root / 'out/Release/v8_build_config.json').read_text())
        if not config['has_turbofan']:
            raise RuntimeError('TurboFan is not enabled in this build')
    if profile == 'backend':
        full = json.loads((root / 'out/sparc64-turbofan-default-full.json').read_text())
        large = json.loads((root / 'out/sparc64-turbofan-backend-large.json').read_text())
        if full['test_total'] < 6000 or full['results'] or not large['pass']:
            raise RuntimeError('Backend full-suite/large-function validation failed')
        config = json.loads(
            (root / 'out/Release/v8_build_config.json').read_text())
        if not config['has_turbofan']:
            raise RuntimeError('TurboFan is not enabled in this build')
    if profile in ('runtime', 'snapshots'):
        config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
        if not config['has_turbofan'] or not config['verify_heap']:
            raise RuntimeError('Runtime validation requires the heap-verification build')
        current = {name: sha256(root / 'out/Release' / name) for name in ['node', 'd8']}
        groups = ['core', 'applications', 'directed']
        if profile == 'runtime':
            groups.append('stress')
        for group in groups:
            evidence = json.loads((root / 'out' /
                                  f'sparc64-turbofan-runtime-{group}.json').read_text())
            if not evidence['passed'] or evidence['binary_sha256'] != current:
                raise RuntimeError('Runtime evidence failed or binaries changed: ' + group)
        if profile == 'runtime':
            stress = json.loads((root / 'out/sparc64-turbofan-stress-verified.json').read_text())
            if len(stress) != 4 or not all(item['passed'] and item['tests'] >= 40 for item in stress):
                raise RuntimeError('Incomplete upstream stress evidence')
        core = json.loads((root / 'out/sparc64-turbofan-node-core.json').read_text())
        soak = json.loads((root / 'out/sparc64-turbofan-soak.json').read_text())
        if not core['passed'] or core['tests'] < 990 or core['failures']:
            raise RuntimeError('Incomplete Node core evidence')
        if (not soak['passed'] or soak['milliseconds'] < 120000 or
                soak['requests'] < 6000 or soak['handlerOptimized'] < 1 or
                soak['optimized'] < 1 or len(soak['samples']) != 8 or
                len(soak['workers']) != 2 or
                not all(item['optimized'] > 0 for item in soak['workers'])):
            raise RuntimeError('Incomplete application soak evidence')
        if profile == 'snapshots':
            variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
            for key in ['node_use_node_snapshot', 'node_use_node_code_cache']:
                if variables[key] != 'true':
                    raise RuntimeError('Required feature disabled: ' + key)
            upstream = json.loads((root / 'out/sparc64-snapshots-upstream.json').read_text())
            if len(upstream) != 2 or not all(item['passed'] and item['binary_sha256'] == current['node'] for item in upstream):
                raise RuntimeError('Snapshot upstream evidence failed or binaries changed')
            state = json.loads((root / 'out/sparc64-snapshots-state.json').read_text())
            benchmark = json.loads((root / 'out/sparc64-snapshots-startup-benchmark.json').read_text())
            if not state['passed'] or state['binary_sha256'] != current['node'] or len(state['results']) != 12:
                raise RuntimeError('Incomplete snapshot/cache state validation')
            if not benchmark['passed'] or benchmark['node_sha256'] != current['node']:
                raise RuntimeError('Incomplete startup comparison')
    if profile == 'wasm':
        subprocess.run(['python3', 'tools/sparc64/audit-wasm.py'], cwd=root, check=True)
    if profile == 'wasm-atomics':
        subprocess.run(['python3', 'tools/sparc64/audit-wasm-atomics.py'], cwd=root, check=True)
    if profile == 'wasm-production':
        subprocess.run(['python3', 'tools/sparc64/audit-wasm-production.py'], cwd=root, check=True)
    if profile == 'node-features':
        subprocess.run(['python3', 'tools/sparc64/audit-node-features.py'], cwd=root, check=True)
    if profile in ('wasm-scalar', 'jspi'):
        subprocess.run(['python3', 'tools/sparc64/audit-jspi.py' if profile == 'jspi' else 'tools/sparc64/audit-wasm-scalar.py'], cwd=root, check=True)
    if profile == 'hardening':
        subprocess.run(['python3', 'tools/sparc64/audit-hardening.py'], cwd=root, check=True)
    if profile == 'regexp':
        subprocess.run(['python3', 'tools/sparc64/audit-regexp.py'], cwd=root, check=True)
    files = sources()
    source_hashes = {name: sha256(root / name) for name in files}
    node = root / 'out/Release/node'
    d8 = root / 'out/Release/d8'
    binary_size = d8.stat().st_size
    if profile != 'turbofan':
        binary_size += node.stat().st_size
    if profile in ('snapshots', 'regexp', 'hardening', 'wasm', 'wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        binary_size += (root / 'out/Release/node_mksnapshot').stat().st_size
    if profile in ('hardening', 'wasm', 'wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        binary_size += (root / 'out/Release/cctest').stat().st_size
    if profile in ('jspi', 'wasm-production', 'node-features'):
        binary_size += (root / 'out/Release/inspector-test').stat().st_size
    required = binary_size * 2 + 64 * 1024 * 1024
    if shutil.disk_usage(root).free < required:
        raise RuntimeError('Insufficient free space for a binary checkpoint')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    prefix = {'jitless': 'pre-sparkplug-', 'sparkplug': 'sparkplug-smoke-',
              'turbofan': 'turbofan-smoke-', 'backend': 'turbofan-backend-',
              'runtime': 'turbofan-runtime-', 'snapshots': 'node-snapshots-', 'regexp': 'node-regexp-', 'hardening': 'node-hardening-', 'wasm': 'node-wasm-', 'wasm-atomics': 'node-wasm-atomics-', 'wasm-scalar': 'node-wasm-scalar-', 'jspi': 'node-jspi-', 'wasm-production': 'node-wasm-production-', 'node-features': 'node-features-'}[profile]
    output = root / 'out/checkpoints' / (prefix + stamp)
    output.mkdir(parents=True, exist_ok=False)
    (output / 'tracked.patch').write_bytes(run('git', 'diff', '--binary', 'HEAD'))
    (output / 'tracked.diffstat').write_bytes(run('git', 'diff', '--stat', 'HEAD'))
    (output / 'git-status.txt').write_bytes(run('git', 'status', '--short'))
    (output / 'BASE_HEAD').write_bytes(run('git', 'rev-parse', 'HEAD'))
    with tarfile.open(output / 'source-overlay.tar.gz', 'w:gz') as archive:
        for name in files:
            archive.add(root / name, arcname=name, recursive=False)

    artifacts = {
        'd8': d8,
        'config.gypi': root / 'config.gypi',
        'v8_build_config.json': root / 'out/Release/v8_build_config.json',
    }
    if profile != 'turbofan':
        artifacts['node'] = node
    if profile in ('jitless', 'sparkplug'):
        artifacts['addon-gc.node'] = root / 'out/sparc64-addon-gc/addon-gc.node'
    if profile == 'runtime':
        artifacts['addon-gc.node'] = root / 'out/sparc64-addon-gc/addon-gc.node'
        parent = Path((root / 'out/sparc64-backend-checkpoint-path.txt').read_text().strip())
        artifacts['backend-parent-manifest.json'] = parent / 'manifest.json'
    if profile == 'snapshots':
        artifacts.update({
            'node_mksnapshot': root / 'out/Release/node_mksnapshot',
            'node_snapshot.cc': root / 'out/Release/obj/gen/node_snapshot.cc',
            'runtime-parent-manifest.json': root / 'out/sparc64-snapshot-parent.json',
            'addon-gc.node': root / 'out/sparc64-addon-gc/addon-gc.node',
            'snapshot-state.blob': root / 'out/snapshot-state.blob',
            'state-code.cache': root / 'out/sparc64-state-code.cache',
        })
    if profile == 'regexp':
        artifacts.update({
            'node_mksnapshot': root / 'out/Release/node_mksnapshot',
            'node_snapshot.cc': root / 'out/Release/obj/gen/node_snapshot.cc',
            'snapshots-parent-manifest.json': root / 'out/sparc64-regexp-parent.json',
            'regexp-interrupt.node': root / 'out/sparc64-regexp-interrupt.node',
            'snapshot-state.blob': root / 'out/snapshot-state.blob',
            'state-code.cache': root / 'out/sparc64-state-code.cache',
        })
        for path in (root / 'out/sparc64-regexp-differential').iterdir():
            artifacts['validation/differential/' + path.name] = path
    if profile == 'hardening':
        artifacts.update({
            'node_mksnapshot': root / 'out/Release/node_mksnapshot',
            'node_snapshot.cc': root / 'out/Release/obj/gen/node_snapshot.cc',
            'cctest': root / 'out/Release/cctest',
            'regexp-parent-manifest.json': root / 'out/sparc64-hardening-parent.json',
            'regexp-interrupt.node': root / 'out/sparc64-regexp-interrupt.node',
            'snapshot-state.blob': root / 'out/snapshot-state.blob',
            'state-code.cache': root / 'out/sparc64-state-code.cache',
        })
        for directory in ['sparc64-inspector-strings', 'sparc64-hardening-platform',
                          'sparc64-hardening-zip', 'sparc64-regexp-differential']:
            for path in (root / 'out' / directory).rglob('*'):
                if path.is_file():
                    artifacts['validation/' + str(path.relative_to(root / 'out'))] = path
        for path in (root / 'out/sparc64-workloads/project').rglob('*'):
            if path.is_file():
                artifacts['workload-project/' + str(path.relative_to(root / 'out/sparc64-workloads/project'))] = path
    if profile in ('wasm', 'wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        artifacts.update({
            'node_mksnapshot': root / 'out/Release/node_mksnapshot',
            'node_snapshot.cc': root / 'out/Release/obj/gen/node_snapshot.cc',
            'cctest': root / 'out/Release/cctest',
            'hardening-parent-manifest.json': root / 'out/sparc64-wasm-parent.json',
            'snapshot-state.blob': root / 'out/snapshot-state.blob',
            'state-code.cache': root / 'out/sparc64-state-code.cache',
        })
        for directory in ['sparc64-wasm-baseline', 'sparc64-wasm-integration',
                          'sparc64-wasm-platform', 'sparc64-wasm-spec',
                          'wasm-upstream-inputs', 'sparc64-hardening-platform',
                          'sparc64-hardening-zip']:
            for path in (root / 'out' / directory).rglob('*'):
                if path.is_file():
                    artifacts['validation/' + str(path.relative_to(root / 'out'))] = path
        parent = root.with_name('node-sparc64-regexp')
        parent_checkpoint = Path((parent / 'out/sparc64-regexp-checkpoint-path.txt').read_text().strip())
        artifacts['validation/parent-js-selection.log'] = (
            parent_checkpoint / 'validation/sparc64-regexp-upstream-full.log')
    if profile in ('wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        artifacts['wasm-parent-manifest.json'] = root / 'out/sparc64-wasm-atomics-parent.json'
        for directory in ['sparc64-wasm-atomics', 'sparc64-wasm-atomics-attempts',
                          'wasm-threads-spec', 'wasm-atomics-workload']:
            for path in (root / 'out' / directory).rglob('*'):
                if path.is_file():
                    artifacts['validation/' + str(path.relative_to(root / 'out'))] = path
    if profile in ('wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        artifacts['wasm-atomics-parent-manifest.json'] = root / 'out/sparc64-wasm-scalar-parent.json'
        for directory in ['sparc64-wasm-scalar-baseline', 'sparc64-wasm-scalar-baseline-exnref',
                          'sparc64-wasm-scalar-current', 'sparc64-wasm-scalar-acceptance',
                          'wasm-scalar-application',
                          'sparc64-snapshot-before-fix',
                          'sparc64-snapshot-array-buffer-keys-baseline',
                          'sparc64-snapshot-array-buffer-keys-current']:
            for path in (root / 'out' / directory).rglob('*'):
                if path.is_file():
                    artifacts['validation/' + str(path.relative_to(root / 'out'))] = path
        debug_log = root / 'out/scalar-ref-baseline-gdb.log'
        if debug_log.exists():
            artifacts['validation/scalar-ref-baseline-gdb.log'] = debug_log
    if profile in ('jspi', 'wasm-production', 'node-features'):
        artifacts['inspector-test'] = root / 'out/Release/inspector-test'
        artifacts['wasm-scalar-parent-manifest.json'] = root / 'out/sparc64-jspi-parent.json'
        for directory in sorted((root / 'out').iterdir()):
            if directory.is_dir() and (directory.name.startswith('sparc64-jspi-') or directory.name == 'jspi-attempts'):
                for path in directory.rglob('*'):
                    if path.is_file():
                        artifacts['validation/' + str(path.relative_to(root / 'out'))] = path
        for path in sorted((root / 'out').glob('sparc64-jspi-*')):
            if path.is_file() and path.suffix in ['.json', '.stdout', '.stderr', '.log'] and path.name != 'sparc64-jspi-progress.json':
                artifacts['validation/' + path.name] = path
    if profile in ('wasm-production', 'node-features'):
        artifacts['jspi-parent-manifest.json'] = root / 'out/sparc64-wasm-production-parent.json'
        prefixes = ('sparc64-wasm-production-', 'sparc64-wasm-simd-application-',
                    'sparc64-wasm-debug-simd-', 'sparc64-wasm-inlining-proof-')
        directories = {'wasm-production-attempts', 'wasm-build-attempts',
                       'wasm-production-tool-history', 'wasm-simd-undici-application'}
        for path in sorted((root / 'out').iterdir()):
            if path.name == 'sparc64-wasm-production-progress.json':
                continue
            if path.name.startswith(prefixes) or path.name in directories:
                for item in ([path] if path.is_file() else path.rglob('*')):
                    if item.is_file():
                        artifacts['validation/' + str(item.relative_to(root / 'out'))] = item
    if profile == 'node-features':
        artifacts['wasm-production-parent-manifest.json'] = root / 'out/sparc64-node-features-parent.json'
        artifacts['libnode_crates.a'] = root / 'out/Release/obj/gen/release/libnode_crates.a'
        artifacts['libnode_crates.a.sparc64.a'] = root / 'out/Release/obj/gen/release/libnode_crates.a.sparc64.a'
        artifacts['libnode_crates.a.sparc64.a.json'] = root / 'out/Release/obj/gen/release/libnode_crates.a.sparc64.a.json'
        artifacts['ffi-test-library.so'] = root / 'test/ffi/fixture_library/build/Release/ffi_test_library.so'
        directories = {'node-features-attempts', 'node-features-builds',
                       'ffi-trampoline-probe', 'test262-inputs', 'rust-libm-probe'}
        for path in sorted((root / 'out').iterdir()):
            if path.name == 'sparc64-node-features-progress.json':
                continue
            if path.name.startswith('sparc64-node-features-') or path.name in directories:
                for item in ([path] if path.is_file() else path.rglob('*')):
                    if item.is_file():
                        artifacts['validation/' + str(item.relative_to(root / 'out'))] = item
    logs = (
        'sparc64-mjsunit-jitless.log', 'sparc64-mjsunit-jitless.json',
        'sparc64-full-assembler-test.log', 'sparc64-node-core-smoke.log',
        'sparc64-node-app-smoke.log', 'sparc64-node-configure.log',
        'sparc64-node-build.log', 'sparc64-addon-gc.log',
        'sparc64-jit-inventory.json',
    )
    if profile == 'sparkplug':
        logs = (
            'sparc64-sparkplug-address-build.log',
            'sparc64-address-after-final.log',
            'sparc64-sparkplug-smoke-final.log',
            'sparc64-sparkplug-upstream-final.log',
            'sparc64-sparkplug-assembler-final.log',
            'sparc64-sparkplug-node-core-final.log',
            'sparc64-worker-check-final.log',
            'sparc64-sparkplug-apps-final.log',
            'sparc64-sparkplug-addon-final.log',
            'sparc64-sparkplug-addon-forced-final.log',
            'sparc64-mjsunit-baseline-full.log',
            'sparc64-mjsunit-baseline-full.json',
            'sparc64-mjsunit-forced-baseline.log',
            'sparc64-mjsunit-forced-baseline.json',
            'sparc64-extra-baseline-clean-flags.log',
            'sparc64-extra-baseline-clean-flags.json',
            'sparc64-feedback-x64-control.log',
            'sparc64-icu-control.cc',
            'sparc64-icu-control.log',
            'sparc64-runner-control.log',
            'sparc64-jit-inventory.json',
        )
    if profile == 'turbofan':
        logs = (
            'sparc64-turbofan-configure.log',
            'sparc64-turbofan-final-build.log',
            'sparc64-turbofan-smoke-final.log',
            'sparc64-turbofan-upstream-final.log',
            'sparc64-turbofan-selected.json',
            'sparc64-turbofan-compiler-deopt-simple-eager.log',
            'sparc64-turbofan-compiler-deopt-simple-lazy.log',
            'sparc64-turbofan-compiler-deopt-followed-by-gc.log',
            'sparc64-turbofan-compiler-deopt-simple-try-catch.log',
            'sparc64-turbofan-baseline-test-osr.log',
            'sparc64-turbofan-baseline-final.log',
            'sparc64-turbofan-baseline-upstream-final.log',
            'sparc64-turbofan-native-final.log',
            'sparc64-turbofan-negative-control.log',
            'sparc64-turbofan-parent.json',
            'sparc64-jit-inventory.json',
        )
    if profile == 'backend':
        logs = (
            'sparc64-turbofan-coverage-build.log',
            'sparc64-turbofan-default-initial.log',
            'sparc64-turbofan-default-initial.json',
            'sparc64-turbofan-default-full.log',
            'sparc64-turbofan-default-full.json',
            'sparc64-turbofan-fixed-failures.log',
            'sparc64-turbofan-fixed-failures.json',
            'sparc64-turbofan-backend-native-current.log',
            'sparc64-turbofan-backend-matrix-final.log',
            'sparc64-turbofan-backend-large.log',
            'sparc64-turbofan-backend-large.json',
            'sparc64-turbofan-branch-negative-control.log',
            'sparc64-rotate-constant-before.log',
            'sparc64-turbofan-node-coverage-final.log',
            'sparc64-turbofan-baseline-backend-final.log',
            'sparc64-turbofan-parent.json',
            'sparc64-jit-inventory.json',
        )
    if profile == 'runtime':
        logs = tuple(path.name for pattern in [
            'sparc64-turbofan-runtime-*.log', 'sparc64-turbofan-runtime-*.json',
            'sparc64-turbofan-stress-verified*.log', 'sparc64-turbofan-stress-verified*.json',
        ] for path in sorted((root / 'out').glob(pattern))) + (
            'sparc64-turbofan-stress-configure.log',
            'sparc64-turbofan-stress-build.log',
            'sparc64-turbofan-node-core.tap', 'sparc64-turbofan-node-core.json',
            'sparc64-turbofan-node-core-initial.tap',
            'sparc64-turbofan-serialization-gdb.log',
            'sparc64-turbofan-soak.json', 'sparc64-turbofan-soak-initial.log',
        )
    if profile == 'snapshots':
        logs = tuple(path.name for pattern in [
            'sparc64-snapshots-*.log', 'sparc64-snapshots-*.json',
            'sparc64-snapshots-*.tap', 'sparc64-turbofan-runtime-*.log',
            'sparc64-turbofan-runtime-*.json',
        ] for path in sorted((root / 'out').glob(pattern))) + (
            'sparc64-turbofan-node-core.tap', 'sparc64-turbofan-node-core.json',
            'sparc64-turbofan-soak.json',
        )
    if profile == 'regexp':
        logs = tuple(path.name for pattern in [
            'sparc64-regexp-*.log', 'sparc64-regexp-*.json',
            'sparc64-snapshots-state*.log', 'sparc64-snapshots-state*.json',
            'sparc64-turbofan-runtime-*.log', 'sparc64-turbofan-runtime-*.json',
        ] for path in sorted((root / 'out').glob(pattern))) + (
            'sparc64-turbofan-node-core.tap', 'sparc64-turbofan-node-core.json',
            'sparc64-turbofan-soak.json',
        )
    if profile == 'hardening':
        logs = tuple(path.name for pattern in [
            'sparc64-hardening-*.log', 'sparc64-hardening-*.json', 'sparc64-hardening-*.tap',
            'sparc64-snapshots-state*.log', 'sparc64-snapshots-state*.json',
            'sparc64-regexp-*.log', 'sparc64-regexp-*.json',
            'sparc64-turbofan-runtime-*.log', 'sparc64-turbofan-runtime-*.json',
        ] for path in sorted((root / 'out').glob(pattern))) + ('sparc64-turbofan-soak.json',)
    if profile in ('wasm', 'wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        logs = tuple(path.name for pattern in [
            'sparc64-wasm-*.json', 'sparc64-wasm-*.log', 'sparc64-wasm-*.tap',
            'sparc64-snapshots-state*.json', 'sparc64-snapshots-state*.log',
            'sparc64-snapshot-array-buffer-keys-*.json',
            'sparc64-regexp-upstream*.json', 'sparc64-regexp-upstream*.log',
            'sparc64-turbofan-runtime-*.json', 'sparc64-turbofan-runtime-*.log',
            'sparc64-hardening-workload*.json', 'sparc64-hardening-workload*.log',
            'sparc64-hardening-regexp-flags*.json', 'sparc64-hardening-regexp-flags*.log',
            'sparc64-hardening-platform.json', 'sparc64-hardening-zip.json',
        ] for path in sorted((root / 'out').glob(pattern))
                     if path.name not in ('sparc64-wasm-progress.json', 'sparc64-wasm-atomics-progress.json', 'sparc64-wasm-scalar-progress.json', 'sparc64-wasm-production-progress.json')) + ('sparc64-turbofan-soak.json',)
    for name in logs:
        artifacts['validation/' + name] = root / 'out' / name
    if profile == 'wasm-production':
        required = sum(path.stat().st_size for path in artifacts.values()) + 256 * 1024 ** 2
        if shutil.disk_usage(root).free < required:
            raise RuntimeError('Insufficient free space for all production evidence')
    for name, original in artifacts.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, destination)
        if sha256(original) != sha256(destination):
            raise RuntimeError('Checkpoint copy mismatch: ' + name)

    if profile == 'turbofan':
        versions = {'arch': 'sparc64',
                    'd8_version': run(str(d8), '--version').decode().strip()}
    else:
        versions = json.loads(run(str(node), '-p',
                                 'JSON.stringify({arch:process.arch,versions:process.versions})'))
    binary = d8 if profile == 'turbofan' else node
    (output / 'shared-libraries.txt').write_bytes(run('ldd', str(binary)))
    if profile in ('runtime', 'snapshots', 'regexp', 'hardening', 'wasm', 'wasm-atomics', 'wasm-scalar', 'jspi', 'wasm-production', 'node-features'):
        commands = [['uname', '-a'], ['gcc', '--version'], ['g++', '--version'],
                    ['ld', '--version'], ['make', '--version'],
                    ['python3', '--version'], ['ldd', '--version']]
        toolchain = bytearray()
        for command in commands:
            toolchain.extend(('$ ' + ' '.join(command) + '\n').encode())
            toolchain.extend(run(*command))
        (output / 'build-toolchain.txt').write_bytes(toolchain)
    if sources() != files or any(sha256(root / name) != value
                                for name, value in source_hashes.items()):
        raise RuntimeError('Sources changed while creating the checkpoint')
    artifact_hashes = {str(path.relative_to(output)): sha256(path)
                       for path in sorted(output.rglob('*')) if path.is_file()}
    manifest = {
        'created_utc': stamp,
        'profile': profile,
        'base_head': run('git', 'rev-parse', 'HEAD').decode().strip(),
        'branch': run('git', 'branch', '--show-current').decode().strip(),
        'runtime': versions,
        'source_file_count': len(files),
        'source_sha256': source_hashes,
        'artifact_sha256': artifact_hashes,
        'external_dependencies': 'Shared OpenSSL and ICU are recorded, not bundled.',
        'validation_scope': (
            'Amaro native TypeScript loading and parser execution in both Wasm tiers; '
            'Rust Temporal with timezone/worker/compaction and full pinned Temporal Test262; '
            'native FFI calls, callbacks, memory access, shared-buffer byte order and '
            'TurboFan fast-call stack arguments with native JIT return-address proof. '
            'All three features enabled together, with the complete production Wasm, '
            'JavaScript JIT, Node and application regression chain retained. Upstream '
            'FFI big-endian skip guards and obsolete arity expectation are preserved '
            'alongside separately derived passing controls; the inherited Wasm '
            'instance-gc host-endian fixture limitation also remains explicit. '
            'Exact scope is gated by audit-node-features.py and its inherited audit.'
            if profile == 'node-features' else
            'SPARC64 production Wasm: standard and relaxed SIMD, Wasm GC, '
            'optimizing compilation, tier transitions and deoptimization, '
            'JS/Wasm wrappers and supported body inlining, and inspector debugging. '
            'Pinned production specification scripts retain upstream expectations '
            'in Liftoff, optimizing and default modes. Distinct SIMD lanes and live '
            'Wasm GC references survive concurrent JSPI suspension, GC and tiering. '
            'Debugger scope inspection, GC while paused and resume preserve vectors. '
            'The unchanged instance-gc test has a host-endian write assumption; '
            'its failure is retained and a separate explicit-little-endian copy '
            'passes all three compiler modes. '
            'All inherited JSPI/scalar/atomics/Node/JIT/snapshot/RegExp/application '
            'gates are retained through audit-wasm-production.py and inherited audits. '
            'The verified JSPI parent remains unchanged. Experimental WasmFX '
            'and growable stacks remain outside scope.'
            if profile == 'wasm-production' else
            'SPARC64 JSPI promise wrappers, stack suspension/resumption/rejection, '
            'central-stack calls and cross-stack exception/termination unwinding. '
            'All five unchanged core JSPI suites and 13 supported-mode upstream '
            'regressions, unchanged Node parallel/test-wasm and inspector async-stack '
            'test pass. Three heap-verification/GC profiles cover repeated, concurrent '
            'and nested suspension, rejection, stack reuse and live externref roots; '
            '20 repeated pairs cover worker termination and the full stack pool. '
            'All inherited scalar/atomics/Node/JIT/snapshot/RegExp/application gates '
            'are retained through audit-jspi.py. The scalar parent is verified unchanged. '
            'Optimizing Wasm, SIMD, comprehensive GC proposal execution, WasmFX, '
            'experimental growable stacks and additional debugger/tiering support '
            'remain outside this milestone.'
            if profile == 'jspi' else
            'Scalar Liftoff Wasm tail calls, typed function references and exception '
            'handling, with 175 unchanged scalar specification scripts and upstream '
            'expected failures retained, 15 unchanged feature suites and a mixed-type '
            'tail-call/GC/large-frame matrix. A real pinned nlohmann/json C++ Wasm '
            'application uses native Wasm exceptions and typed/tail-call adapters, '
            'with target-native and cross-host C++ oracles, error recovery and memory '
            'growth. Eight baseline unexpected outcomes are fixed. Existing atomics, '
            'Node/JIT, GC/deopt, snapshots/cache, RegExp, addons and application '
            'regressions are gated by audit-wasm-scalar.py and inherited audits. '
            'The verified atomics parent is unchanged. Optimizing Wasm, SIMD, '
            'comprehensive GC proposal execution, JSPI and debugger/tiering '
            'infrastructure remain deferred.'
            if profile == 'wasm-scalar' else
            'Scalar Liftoff Wasm atomic loads/stores/RMW/CAS at 8/16/32/64 bits, '
            'with little-endian memory semantics and sequential consistency; '
            'unchanged official threads atomic assertions and V8 CAS/shared-memory '
            'tests; compiled xxHash atomic queue across four Node workers against '
            'an independent native oracle; neighboring-lane contention, memory '
            'ordering and 32/64-bit wait/notify interoperability. Existing baseline '
            'Wasm, Node applications, JavaScript JIT, GC/deopt, native addons, '
            'snapshots/cache and RegExp regressions are retained. Exact evidence '
            'is gated by audit-wasm-atomics.py and audit-wasm.py. The verified '
            'baseline Wasm source tree, binaries and checkpoint are unchanged. '
            'Optimizing Wasm, SIMD, GC proposal execution and JSPI remain deferred.'
            if profile == 'wasm-atomics' else
            'SPARC64 baseline Liftoff Wasm execution with scalar specification '
            'and core JS API validation, native compiler proof, actual Undici '
            'llhttp parsing and Node fetch/WebSocket exchanges. JavaScript '
            'Sparkplug/TurboFan, native RegExp and snapshots/cache are retained '
            'and regression-tested, including the preserved 6544-case ordinary '
            'JavaScript selection and Node conformance with disabled-Wasm '
            'exclusions re-enabled except the explicitly deferred JSPI test. Native '
            'ABI alignment and self-referencing signature controls are included. '
            'Optimizing Wasm, SIMD, Wasm '
            'atomics, GC proposal execution and stack switching are unfinished. '
            'Exact evidence and scope are gated by audit-wasm.py. The hardened '
            'parent sources, binaries and checkpoint are verified unchanged.'
            if profile == 'wasm' else
            'Optimized SPARC64 Node hardening: consumer RegExp snapshot flags '
            'and unaligned inspector UTF-16 fixed; broad Node conformance, '
            'native addons/Node-API and C++ tests with explicit disabled-feature '
            'and independently verified platform/tool exclusions; TypeScript '
            'compilation, incremental rebuild, application execution and ESLint '
            'with positive and negative controls; native RegExp, snapshot/cache '
            'and optimized GC/deoptimization regressions; npm, HTTPS/WebSocket '
            'application soak. Exact coverage and limitations are documented '
            'in README and gated audit evidence. The RegExp parent is unchanged.'
            if profile == 'hardening' else
            'SPARC64 native RegExp: scalar Latin-1/UTF-16 code generation; '
            'upstream normal, forced-JIT and compaction profiles and full default '
            'mjsunit; interpreter differential with native-code proof; actual '
            'backtrack-stack growth; GC interrupts, nested matching and '
            'termination; bounded interleaved benchmarks; snapshot/cache, '
            'TurboFan, Node core, npm and application regressions. Exact '
            'counts, exclusions and the debug flag/snapshot verification '
            'caveat are recorded in README and validation evidence. '
            'The working snapshot/TurboFan parent is hash-verified unchanged.'
            if profile == 'regexp' else
            'Node embedded startup snapshots and builtin code cache enabled; '
            'actual cache use and no-snapshot fallback verified; upstream '
            'snapshot/VM/module cache tests in main and worker contexts; '
            'restored cyclic/BigInt/typed-array state, GC, TurboFan and deopt; '
            'fresh-process cache acceptance and corrupt/source mismatch rejection; '
            'Node core and application/runtime regressions; startup comparison '
            'against the preserved runtime build. Disabled features remain '
            'outside this milestone.'
            if profile == 'snapshots' else
            'TurboFan-enabled Node: targeted upstream compaction, incremental '
            'marking, concurrent allocation and inlining with heap verification; '
            'optimized GC/deoptimization, OSR and native-addon reentry; '
            'buffer/stream/network/TLS/worker/child-process suites with explicit '
            'disabled-Wasm exclusions; npm and published application smoke; '
            'two-minute concurrent HTTPS/WebSocket/worker soak with real '
            'automatic optimized execution and post-GC memory samples. '
            'BigInt serialized-buffer alignment fixed. This is bounded runtime '
            'validation of the current feature configuration, not all Node '
            'suites, long-term reliability or disabled-feature support.'
            if profile == 'runtime' else
            'Ordinary-JS backend: full default mjsunit (6539 cases, zero '
            'unexpected); 100 native tests including long integer/FP/switch '
            'branches and deopt patches; 71 optimized arithmetic/data-access '
            'vectors; optimized >1 MiB function with eager deoptimization; '
            'negative branch control; forced and normal concurrent TurboFan '
            'execution inside Node. Other features, broader GC stress and '
            'Node/application conformance remain outside this milestone.'
            if profile == 'backend' else
            'TurboFan d8: verified execution tier, eager/lazy deoptimization, '
            'live objects and doubles with GC, interpreter/baseline OSR, '
            'selected upstream optimizing cases, and bounded baseline/native '
            'regressions. Optimizing full suites and Node remain pending.'
            if profile == 'turbofan' else
            'Sparkplug: full applicable mjsunit (6145 cases, zero unexpected); '
            'forced baseline (6145 cases, one array-feedback failure); '
            'Intl/message/debugger (943 cases, two ICU output differences); '
            'bounded native, baseline, GC/OSR, Node core, worker, addon '
            'and application checks. See README and independent controls.'
            if profile == 'sparkplug' else
            'Full mjsunit/native checks cover the minimal jitless configuration; '
            'the OpenSSL/ICU Node build has bounded core, application and addon/GC checks.'),
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    artifact_hashes['manifest.json'] = sha256(output / 'manifest.json')
    (output / 'SHA256SUMS').write_text(''.join(
        f'{value}  {name}\n' for name, value in sorted(artifact_hashes.items())))
    pointer = {'jitless': 'sparc64-prejit-checkpoint-path.txt',
               'sparkplug': 'sparc64-sparkplug-checkpoint-path.txt',
               'turbofan': 'sparc64-turbofan-checkpoint-path.txt',
               'backend': 'sparc64-backend-checkpoint-path.txt',
               'runtime': 'sparc64-runtime-checkpoint-path.txt',
               'snapshots': 'sparc64-snapshots-checkpoint-path.txt',
               'regexp': 'sparc64-regexp-checkpoint-path.txt',
               'hardening': 'sparc64-hardening-checkpoint-path.txt',
               'wasm': 'sparc64-wasm-checkpoint-path.txt',
               'wasm-atomics': 'sparc64-wasm-atomics-checkpoint-path.txt',
               'wasm-scalar': 'sparc64-wasm-scalar-checkpoint-path.txt',
               'jspi': 'sparc64-jspi-checkpoint-path.txt',
               'wasm-production': 'sparc64-wasm-production-checkpoint-path.txt',
               'node-features': 'sparc64-node-features-checkpoint-path.txt'}[profile]
    (root / 'out' / pointer).write_text(str(output) + '\n')
    size = sum(path.stat().st_size for path in output.rglob('*') if path.is_file())
    print(f'Checkpoint: {output}')
    print(f'{len(files)} source files; {size / (1024 * 1024):.1f} MiB total')
    print('Binaries copied and hashes verified; no Git commit created')


if __name__ == '__main__':
    main()
