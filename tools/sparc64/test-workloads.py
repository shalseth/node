#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Compile, execute, incrementally rebuild and lint a multi-module application."""

import hashlib
import json
from pathlib import Path
import resource
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
binary_hash = hashlib.sha256(node.read_bytes()).hexdigest()
work = root / 'out/sparc64-workloads'
work.mkdir(exist_ok=True)
for name in ['package.json', 'package-lock.json']:
    shutil.copy2(root / 'tools/sparc64/workloads' / name, work / name)
results = []


def run(name, command, cwd=work, expected_exit=0, marker=None, timeout=120):
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
        code, log = result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired as error:
        code, log = 'timeout', error.stdout or b''
    (root / 'out' / ('sparc64-hardening-workload-' + name + '.log')).write_bytes(log)
    passed = code == expected_exit and (marker is None or marker.encode() in log)
    item = dict(name=name, command=list(map(str, command)), cwd=str(cwd), exit_code=code,
                expected_exit=expected_exit, marker=marker, passed=passed,
                optimizing_compilations=log.count(b'completed compiling'),
                seconds=round(time.monotonic() - started, 3))
    results.append(item)
    print(json.dumps(item), flush=True)
    assert passed, log[-12000:].decode(errors='replace')
    return item


run('install', [str(node), str(root / 'deps/npm/bin/npm-cli.js'), 'ci',
                '--ignore-scripts', '--omit=optional', '--no-audit', '--no-fund'])
project = work / 'project'
if project.exists():
    assert (project / 'expected.json').is_file()
    shutil.rmtree(project)
run('generate', ['python3', str(root / 'tools/sparc64/workloads/generate.py'), str(project)],
    marker='WORKLOAD_PROJECT_GENERATED')
tsc = work / 'node_modules/typescript/bin/tsc'
eslint = work / 'node_modules/eslint/bin/eslint.js'
flags = [str(node), '--trace-opt', '--trace-deopt']
first = run('typescript-clean', [*flags, str(tsc), '--project', '.', '--extendedDiagnostics'],
            cwd=project)
assert first['optimizing_compilations'] > 0
expected = json.loads((project / 'expected.json').read_text())
assert len(list((project / 'dist').glob('*.js'))) == expected['source_files']
run('execute', [str(node), 'dist/main.js'], cwd=project,
    marker='COMPILED_APPLICATION_OK ' + str(expected['checksum']))
outputs = {p.name: p.stat().st_mtime_ns for p in (project / 'dist').glob('*.js')}
run('typescript-incremental', [*flags, str(tsc), '--project', '.', '--extendedDiagnostics'], cwd=project)
assert outputs == {p.name: p.stat().st_mtime_ns for p in (project / 'dist').glob('*.js')}
bad = project / 'src/module31.ts'
original = bad.read_text()
try:
    bad.write_text(original + "\nexport const invalid: number = 'wrong';\n")
    run('typescript-negative', [str(node), str(tsc), '--project', '.', '--noEmit'],
        cwd=project, expected_exit=2, marker='TS2322')
finally:
    bad.write_text(original)
run('typescript-recovery', [*flags, str(tsc), '--project', '.'], cwd=project)
run('execute-rebuilt', [str(node), 'dist/main.js'], cwd=project,
    marker='COMPILED_APPLICATION_OK ' + str(expected['checksum']))
lint = run('eslint', [*flags, str(eslint), 'dist', '--format', 'json', '--output-file', 'eslint.json'], cwd=project)
assert lint['optimizing_compilations'] > 0
lint_results = json.loads((project / 'eslint.json').read_text())
assert len(lint_results) == expected['source_files']
assert all(item['errorCount'] == item['warningCount'] == 0 for item in lint_results)
negative = project / 'dist/lint-negative.js'
try:
    negative.write_text('missingGlobal();\n')
    run('eslint-negative', [str(node), str(eslint), str(negative), '--format', 'json',
                           '--output-file', 'eslint-negative.json'], cwd=project, expected_exit=1)
    assert any(message['ruleId'] == 'no-undef'
               for item in json.loads((project / 'eslint-negative.json').read_text())
               for message in item['messages'])
finally:
    negative.unlink(missing_ok=True)
assert hashlib.sha256(node.read_bytes()).hexdigest() == binary_hash
result = dict(passed=True, binary_sha256=binary_hash, expected=expected, results=results,
              lockfile_sha256=hashlib.sha256((work / 'package-lock.json').read_bytes()).hexdigest(),
              versions={name: json.loads((work / 'node_modules' / name / 'package.json').read_text())['version']
                        for name in ['typescript', 'eslint']})
(root / 'out/sparc64-hardening-workloads.json').write_text(json.dumps(result, indent=2) + '\n')
print('APPLICATION_WORKLOADS_OK', flush=True)
