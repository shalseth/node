#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Compare bounded regexp workloads in interleaved fresh Node processes."""

import hashlib
import json
from pathlib import Path
import statistics
import subprocess

root = Path(__file__).resolve().parents[2]
node = root / 'out/Release/node'
digest = hashlib.sha256(node.read_bytes()).hexdigest()
samples = []
reference = None
for sample in range(5):
    modes = ['native', 'interpreter'] if sample % 2 == 0 else ['interpreter', 'native']
    for mode in modes:
        flags = ['--no-regexp-tier-up'] if mode == 'native' else ['--regexp-interpret-all']
        command = [str(node), '--allow-natives-syntax', *flags,
                   'tools/sparc64/benchmark-regexp.cjs']
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stderr
        values = json.loads(result.stdout)
        semantics = [(item['name'], item['count'], item['checksum']) for item in values]
        if reference is None:
            reference = semantics
        assert semantics == reference
        assert all(item['native'] == (mode == 'native') for item in values)
        samples.append(dict(sample=sample, mode=mode, command=command, values=values))
        print(mode, sample, values, flush=True)
summary = []
for name, _, _ in reference:
    times = {}
    for mode in ['native', 'interpreter']:
        times[mode] = statistics.median(item['nanoseconds'] for sample in samples
                                       if sample['mode'] == mode for item in sample['values']
                                       if item['name'] == name)
    summary.append(dict(name=name, median_nanoseconds=times,
                        speedup=times['interpreter'] / times['native']))
evidence = dict(passed=True, binary_sha256=digest, samples=samples, summary=summary)
assert hashlib.sha256(node.read_bytes()).hexdigest() == digest
(root / 'out/sparc64-regexp-benchmark.json').write_text(json.dumps(evidence, indent=2) + '\n')
print('REGEXP_BENCHMARK_OK', json.dumps(summary), flush=True)
