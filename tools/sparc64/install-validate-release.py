#!/usr/bin/env python3
"""Extract, relocate and validate one checksummed release in an isolated directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--sha256', required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
archive = args.archive.resolve()
output = args.output.resolve()
assert not output.exists(), 'Use a fresh installation test directory'
actual = hashlib.file_digest(archive.open('rb'), 'sha256').hexdigest()
assert actual == args.sha256
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
output.mkdir(parents=True)
unpack = output / 'unpacked'
unpack.mkdir()
with tarfile.open(archive, 'r:gz') as stream:
    top = {Path(member.name).parts[0] for member in stream.getmembers()}
    assert len(top) == 1
    stream.extractall(unpack, filter='data')
prefix = unpack / top.pop()
assert (prefix / 'bin/node').is_file()
record = {'passed': False, 'archive': str(archive), 'archive_sha256': actual,
          'initial_prefix': str(prefix), 'startup': []}


def save():
    (output / 'installation.json').write_text(json.dumps(record, indent=2) + '\n')


for binary in ['node', 'npm', 'npx']:
    env = dict(os.environ, PATH=str(prefix / 'bin') + ':' + os.defpath,
               npm_config_cache=str(output / 'startup-npm-cache'),
               npm_config_update_notifier='false')
    env.pop('NODE_OPTIONS', None)
    env.pop('NODE_PATH', None)
    result = subprocess.run([str(prefix / 'bin' / binary), '--version'], env=env,
                            capture_output=True, text=True, timeout=30)
    record['startup'].append(dict(binary=binary, exit_code=result.returncode,
                                   stdout=result.stdout, stderr=result.stderr))
    save()
    assert result.returncode == 0
relocated = output / 'relocated' / 'runtime'
relocated.parent.mkdir()
prefix.rename(relocated)
record['relocated_prefix'] = str(relocated)
record['initial_prefix_removed'] = not prefix.exists()
assert record['initial_prefix_removed']
save()
command = ['python3', '-u', str(relocated / 'share/doc/node-sparc64/validation/validate-installed.py'),
           '--prefix', str(relocated), '--output', str(output / 'validation')]
result = subprocess.run(command)
record['validation_exit_code'] = result.returncode
save()
assert result.returncode == 0
validation = json.loads((output / 'validation/validation.json').read_text())
assert validation['passed']
record['node_sha256'] = validation['node_sha256']
record['passed'] = True
save()
print('RELOCATED_RELEASE_VALIDATION_OK', flush=True)
