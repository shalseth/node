#!/usr/bin/env python3
"""Stage upstream installation and export the complete authenticated port."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=root / 'out/release-candidate-package')
args = parser.parse_args()
out = args.output.resolve()
assert not out.exists(), 'Use a new packaging output directory'
assert shutil.disk_usage(out.parent).free > 8 * 1024**3
out.mkdir()
sha = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
report_path = root / 'out/release-candidate-build/build.json'
build = json.loads(report_path.read_text())
assert build['passed'], 'Clean build must pass before packaging'
for name, expected in build['source_sha256'].items():
    assert sha(root / name) == expected, 'Built source changed: ' + name
for name, expected in build['binary_sha256'].items():
    assert sha(root / 'out/Release' / name) == expected, 'Built binary changed: ' + name
names = set(subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0'))
names.update(subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=root).decode().split('\0'))
names.discard('')
names = sorted(name for name in names if (root / name).exists() or (root / name).is_symlink())
extras = set(names) - set(build['source_sha256'])
supplement = {}
for name in sorted(extras):
    if name.startswith('tools/sparc64/'):
        continue
    path = root / name
    data = os.readlink(path).encode() if path.is_symlink() else path.read_bytes()
    expected = subprocess.check_output(['git', 'cat-file', 'blob', 'HEAD:' + name], cwd=root)
    assert data == expected, 'Omitted base entry differs: ' + name
    assert path.is_symlink() or any(ord(c) > 127 for c in name), name
    supplement[name] = {'sha256': hashlib.sha256(data).hexdigest(),
        'reason': 'symlink directory or Git-quoted Unicode path omitted by clean helper',
        'verified_against': 'HEAD:' + name}

version = subprocess.check_output([str(root / 'out/Release/node'), '--version'], text=True).strip()
label = 'node-' + version + '-linux-sparc64-rc1'
stage = out / label
with (out / 'install.log').open('w') as log:
    subprocess.run([sys.executable, 'tools/install.py', 'install', '--dest-dir', str(stage),
                    '--prefix', '/', '--silent'], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
assert sha(stage / 'bin/node') == build['binary_sha256']['node']
shutil.copy2(root / 'LICENSE', stage / 'LICENSE')
doc = stage / 'share/doc/node-sparc64'
doc.mkdir(parents=True, exist_ok=True)
shutil.copy2(root / 'tools/sparc64/README-release.md', doc / 'README.md')
shutil.copytree(root / 'tools/sparc64/release-validation', doc / 'validation',
                ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))


def capture(command):
    result = subprocess.run(command, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == 0, result.stdout
    return result.stdout


def entry(path):
    if path.is_symlink():
        return {'symlink': os.readlink(path)}
    return {'sha256': sha(path), 'bytes': path.stat().st_size, 'mode': oct(path.stat().st_mode & 0o777)}


provenance = {'label': label, 'upstream_version': version, 'base_head': build['base_head'],
    'parent_checkpoint': json.loads((root / 'tools/sparc64/release-candidate-plan.json').read_text()),
    'clean_build_report_sha256': sha(report_path), 'binary_sha256': build['binary_sha256'],
    'sources_added_after_build': sorted(extras - set(supplement)),
    'base_inventory_supplement': supplement,
    'configure_and_make': build['results'], 'configuration': build['configuration'],
    'compile_defines': build['v8_compile_defines'],
    'uname': capture(['uname', '-a']), 'compiler': capture(['gcc', '--version']),
    'rust': capture(['rustc', '--version', '--verbose']), 'cargo': capture(['cargo', '--version']),
    'assembler': capture(['as', '--version']), 'linker': capture(['ld', '--version']),
    'elf': capture(['readelf', '-h', '-d', '--version-info', str(stage / 'bin/node')]),
    'libraries': capture(['ldd', str(stage / 'bin/node')]), 'publication': 'local artifacts only'}
(stage / 'BUILD-PROVENANCE.json').write_text(json.dumps(provenance, indent=2) + '\n')
manifest = {'label': label, 'files': {str(p.relative_to(stage)): entry(p)
    for p in sorted(stage.rglob('*')) if p.is_file() or p.is_symlink()}}
(stage / 'PACKAGE-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
with tarfile.open(out / (label + '.tar.gz'), 'w:gz', compresslevel=6) as archive:
    archive.add(stage, arcname=label)
source_label = label + '-source'
source_manifest = {'base_head': build['base_head'], 'files': {name: entry(root / name) for name in names}}
manifest_path = out / 'SOURCE-MANIFEST.json'
manifest_path.write_text(json.dumps(source_manifest, indent=2) + '\n')
with tarfile.open(out / (source_label + '.tar.gz'), 'w:gz', compresslevel=6) as archive:
    for name in names:
        archive.add(root / name, arcname=source_label + '/' + name, recursive=False)
    archive.add(manifest_path, arcname=source_label + '/SOURCE-MANIFEST.json')
index = out / 'temporary-index'
env = dict(os.environ, GIT_INDEX_FILE=str(index))
try:
    subprocess.run(['git', 'read-tree', 'HEAD'], cwd=root, env=env, check=True)
    subprocess.run(['git', 'add', '-A'], cwd=root, env=env, check=True)
    patch = out / (label + '-complete.patch')
    with patch.open('wb') as stream:
        subprocess.run(['git', 'diff', '--cached', '--binary', '--full-index', 'HEAD'],
                       cwd=root, env=env, stdout=stream, check=True)
    # Applying to the pristine index proves the patch includes every new port file.
    verify_index = out / 'verify-index'
    verify_env = dict(os.environ, GIT_INDEX_FILE=str(verify_index))
    subprocess.run(['git', 'read-tree', 'HEAD'], cwd=root, env=verify_env, check=True)
    subprocess.run(['git', 'apply', '--cached', '--binary', str(patch)], cwd=root, env=verify_env, check=True)
    expected_tree = subprocess.check_output(
        ['git', 'write-tree'], cwd=root, env=env, text=True).strip()
    actual_tree = subprocess.check_output(['git', 'write-tree'], cwd=root, env=verify_env, text=True).strip()
    assert actual_tree == expected_tree
    provenance['complete_patch_tree'] = actual_tree
finally:
    index.unlink(missing_ok=True)
    (out / 'verify-index').unlink(missing_ok=True)
shutil.copy2(report_path, out / 'clean-build.json')
parent_manifest = root / 'out/sparc64-release-parent.json'
assert sha(parent_manifest) == provenance['parent_checkpoint']['parent_manifest_sha256']
shutil.copy2(parent_manifest, out / 'node-features-parent-manifest.json')
(out / 'packaging.json').write_text(json.dumps(dict(passed=True, label=label,
    package_files=len(manifest['files']), source_files=len(names),
    patch_roundtrip_tree=provenance['complete_patch_tree'],
    node_sha256=sha(stage / 'bin/node')), indent=2) + '\n')
artifacts = sorted(p for p in out.iterdir() if p.is_file() and p.name != 'SHA256SUMS')
(out / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.name + '\n' for p in artifacts))
print('RELEASE_CANDIDATE_PACKAGED ' + label, flush=True)
