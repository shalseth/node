#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run the complete pinned Temporal Test262 selection with upstream expectations."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
inputs = json.loads((root / 'out/sparc64-node-features-test262-inputs.json').read_text())
assert inputs['commit'] == 'dd6138f9bc1aa2c3ba9cbf54452049b9a92c4e13'
assert digest(root / inputs['archive']) == inputs['archive_sha256']
sources = inputs['temporal_selected']
assert len(sources) == inputs['temporal_source_count'] == 6534
suite = root / 'deps/v8/test/test262'
support = ['test262.status', 'testcfg.py', 'harness-adapt.js', 'harness-done.js']
for name in support:
    relative = 'deps/v8/test/test262/' + name
    assert (suite / name).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=root)
for name, expected in sources.items():
    assert digest(suite / 'data/test' / name) == expected, name
config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
assert config['temporal'] and config['has_turbofan'] and config['i18n']
sys.path.insert(0, str(root / 'deps/v8/tools'))
sys.path.insert(0, str(suite / 'data/tools/packaging'))
from testrunner.local import statusfile
from parseTestRecord import parseTestRecord
variables = dict(config)
variables.update(all_arm64_features=False, byteorder=sys.byteorder, deopt_fuzzer=False,
                 device_type=None, endurance_fuzzer=False, gc_fuzzer=False, gc_stress=False,
                 isolates=False, interrupt_fuzzer=False, mode='release', no_harness=False,
                 no_simd_hardware=False, novfp3=False, optimize_for_size=False, system='linux')
status = statusfile.StatusFile(suite / 'test262.status', variables)
expected_executions, upstream_skipped, fixture_sources = {}, {}, []
for name in sources:
    if name.endswith('_FIXTURE.js'):
        fixture_sources.append(name)
        continue
    outcomes = status.get_outcomes(name.removesuffix('.js'), 'default')
    if statusfile.SKIP in outcomes:
        upstream_skipped[name] = sorted(outcomes)
        continue
    record = parseTestRecord((suite / 'data/test' / name).read_text(), name)
    count = 1 if 'noStrict' in record or 'onlyStrict' in record else 2
    if statusfile.FAIL_PHASE_ONLY in outcomes:
        count *= 2
    expected_executions[name] = count
expected_tests = sum(expected_executions.values())
assert expected_tests > 0
binary = root / 'out/Release/d8'
binary_hash = digest(binary)
probe = subprocess.run([str(binary), '-e', 'if(typeof Temporal!=="object")throw Error("Temporal disabled");'
    'print(Temporal.Instant.from("1969-07-20T20:17:00Z").toString())'],
    cwd=root, capture_output=True, timeout=30)
assert probe.returncode == 0 and b'1969-07-20T20:17:00Z' in probe.stdout
prefix = root / ('out/sparc64-node-features-temporal-test262-' + args.label)
assert not prefix.with_suffix('.json').exists(), 'Preserve previous evidence before retrying'
raw = Path(str(prefix) + '-raw.json')
selected = ['test262/' + name.removesuffix('.js') for name in sources]
directories = ['built-ins/Temporal/', 'intl402/Temporal/', 'staging/sm/Temporal/']
selection = ['test262/' + directory + '*' for directory in directories]
selection += ['test262/' + name.removesuffix('.js') for name in sources
              if not any(name.startswith(directory) for directory in directories)]
command = ['python3', '-u', 'tools/run-tests.py', '--outdir=' + str(root / 'out/Release'),
           '--variants=default', '--no-presubmit', '-j16', '-t60',
           '--progress=verbose', '--random-seed=42', '--rerun-failures-count=0',
           '--exit-after-n-failures=0', '--json-test-results=' + str(raw), *selection]
with prefix.with_suffix('.log').open('w') as log:
    try:
        code = subprocess.run(command, cwd=root / 'deps/v8', stdout=log,
                              stderr=subprocess.STDOUT, timeout=1800).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
data = json.loads(raw.read_text()) if raw.exists() else None
assert digest(binary) == binary_hash
assert sources == {name: digest(suite / 'data/test' / name) for name in sources}
result = dict(passed=code == 0 and data is not None and
              data['test_total'] == expected_tests and not data['results'],
              binary_sha256=binary_hash, command=command, exit_code=code,
              source_sha256=sources, input_archive_sha256=inputs['archive_sha256'],
              commit=inputs['commit'], selected=selected,
              support_sha256={name: digest(suite / name) for name in support},
              expected_executions=expected_executions, expected_tests=expected_tests,
              upstream_skipped=upstream_skipped, fixture_sources=fixture_sources,
              tests=data['test_total'] if data else 0, unexpected=data['results'] if data else None,
              upstream_expectations_unchanged=True)
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: result[key] for key in ['passed', 'exit_code', 'tests']}), flush=True)
raise SystemExit(not result['passed'])
