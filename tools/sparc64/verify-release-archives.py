#!/usr/bin/env python3
"""Check outer hashes, exact tar contents, member hashes, modes and symlinks."""
import argparse
import hashlib
import json
from pathlib import Path
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
root = args.directory.resolve()
for line in (root / 'SHA256SUMS').read_text().splitlines():
    expected, name = line.split('  ', 1)
    assert Path(name).name == name
    with (root / name).open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected, name
report = {'passed': False, 'archives': []}
for path in sorted(root.glob('*.tar.gz')):
    with tarfile.open(path, 'r:gz') as archive:
        members = archive.getmembers()
        names = [member.name for member in members]
        assert len(names) == len(set(names))
        prefix_set = {Path(name).parts[0] for name in names}
        assert len(prefix_set) == 1
        prefix = prefix_set.pop()
        for name in names:
            assert not Path(name).is_absolute() and '..' not in Path(name).parts, name
        source = prefix.endswith('-source')
        manifest_name = 'SOURCE-MANIFEST.json' if source else 'PACKAGE-MANIFEST.json'
        manifest = json.load(archive.extractfile(prefix + '/' + manifest_name))
        actual = {str(Path(member.name).relative_to(prefix)) for member in members if not member.isdir()}
        assert actual == set(manifest['files']) | {manifest_name}
        for name, item in manifest['files'].items():
            member = archive.getmember(prefix + '/' + name)
            if 'symlink' in item:
                assert member.issym() and member.linkname == item['symlink'], name
            else:
                assert member.isfile(), name
                with archive.extractfile(member) as stream:
                    assert hashlib.file_digest(stream, 'sha256').hexdigest() == item['sha256'], name
                assert member.size == item['bytes'], name
                assert oct(member.mode & 0o777) == item['mode'], name
        report['archives'].append({'name': path.name, 'files': len(manifest['files']), 'passed': True})
        print('ARCHIVE_VERIFIED', path.name, len(manifest['files']), flush=True)
assert len(report['archives']) == 2
report['passed'] = True
print(json.dumps(report), flush=True)
