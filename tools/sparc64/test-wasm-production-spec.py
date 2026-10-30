#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run complete pinned production Wasm specification groups without editing tests."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import tarfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--feature', choices=['simd', 'gc', 'production'], default='simd')
parser.add_argument('--tier', choices=['liftoff', 'optimized', 'default'], default='liftoff')
parser.add_argument('--build', type=Path, default=root / 'out/Release')
parser.add_argument('--label', default='current')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
sha1 = (root / 'deps/v8/test/wasm-spec-tests/tests.tar.gz.sha1').read_text().strip()
archive = root / ('out/wasm-upstream-inputs/wasm-spec-tests-' + sha1 + '.tar.gz')
assert hashlib.sha1(archive.read_bytes()).hexdigest() == sha1
selected, excluded, sources = [], {}, {}
with tarfile.open(archive) as stream:
    for member in stream:
        if not member.isfile() or not member.name.endswith('.js'):
            continue
        relative = Path(member.name).relative_to('tests')
        group = relative.parts[0] if len(relative.parts) > 1 else 'scalar'
        if group == 'proposals':
            excluded[str(relative)] = 'Experimental WasmFX or custom-descriptors proposal; outside production goal'
            continue
        if args.feature == 'simd' and group not in ['simd', 'relaxed-simd']:
            continue
        if args.feature == 'gc' and group != 'gc':
            continue
        path = root / 'deps/v8/test/wasm-spec-tests/tests' / relative
        assert path.read_bytes() == stream.extractfile(member).read()
        selected.append('wasm-spec-tests/' + str(relative.with_suffix('')))
        sources[str(relative)] = digest(path)
assert len(selected) == {'simd': 66, 'gc': 17, 'production': 258}[args.feature], len(selected)
binary_hash = digest(args.build / 'd8')
prefix = root / ('out/sparc64-wasm-production-spec-' + args.feature + '-' + args.tier + '-' + args.label)
assert not prefix.with_suffix('.json').exists(), 'Preserve previous evidence before retrying'
raw = Path(str(prefix) + '-raw.json')
command = ['python3', '-u', 'tools/run-tests.py', '--outdir=' + str(args.build),
           '--variants=default', '--no-presubmit', '-j8', '-t120', '--progress=verbose',
           '--random-seed=42', '--rerun-failures-count=0', '--exit-after-n-failures=0',
           '--extra-flags=' + {'liftoff': '--liftoff-only', 'optimized': '--no-liftoff-only --no-liftoff', 'default': ''}[args.tier] +
           ' --no-wasm-lazy-compilation', '--json-test-results=' + str(raw), *sorted(selected)]
with prefix.with_suffix('.log').open('w') as log:
    try:
        code = subprocess.run(command, cwd=root / 'deps/v8', stdout=log,
                              stderr=subprocess.STDOUT, timeout=1800).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
data = json.loads(raw.read_text()) if raw.exists() else None
assert digest(args.build / 'd8') == binary_hash
status = 'deps/v8/test/wasm-spec-tests/wasm-spec-tests.status'
assert (root / status).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + status], cwd=root)
result = dict(passed=code == 0 and data is not None and data['test_total'] == len(selected) and not data['results'],
              binary_sha256=binary_hash, tier=args.tier, feature=args.feature,
              command=command, exit_code=code, selected=sorted(selected), source_sha256=sources,
              archive_sha1=sha1, archive_sha256=digest(archive), excluded_experimental=excluded,
              status_sha256=digest(root / status), tests=data['test_total'] if data else 0,
              unexpected=data['results'] if data else None)
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: result[key] for key in ['passed', 'tier', 'feature', 'exit_code', 'tests']}), flush=True)
raise SystemExit(not result['passed'])
