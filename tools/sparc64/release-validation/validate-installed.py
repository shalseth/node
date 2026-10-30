#!/usr/bin/env python3
"""Validate a relocated installed package without development-tree paths."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--prefix', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
prefix = args.prefix.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
fixtures = Path(__file__).resolve().parent
assert fixtures.is_relative_to(prefix), 'Run the copy bundled with the installed package'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert shutil.disk_usage(output).free > 3 * 1024**3
report = {'passed': False, 'prefix': str(prefix), 'fixture_root': str(fixtures),
          'host': platform.node(), 'uname': list(platform.uname()), 'results': []}
env = dict(os.environ)
for key in ['NODE_OPTIONS', 'NODE_PATH', 'LD_PRELOAD', 'LD_LIBRARY_PATH',
            'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'GYP_DEFINES']:
    env.pop(key, None)
env.update(PATH=str(prefix / 'bin') + os.pathsep + os.defpath + ':/usr/local/bin',
           npm_config_cache=str(output / 'npm-cache'),
           npm_config_userconfig=str(output / 'empty-npmrc'),
           npm_config_globalconfig=str(output / 'empty-global-npmrc'),
           npm_config_prefix=str(output / 'npm-global'),
           npm_config_update_notifier='false', npm_config_audit='false', npm_config_fund='false',
           SPARC64_EXPECT_NODE=str(prefix / 'bin/node'), CCACHE_DISABLE='1')
(output / 'empty-npmrc').touch()
(output / 'empty-global-npmrc').touch()
node = str(prefix / 'bin/node')
npm = str(prefix / 'bin/npm')
npx = str(prefix / 'bin/npx')


def save():
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')


def run(name, command, cwd=None, extra=None, timeout=180, marker=None):
    started = time.monotonic()
    logfile = output / (name + '.log')
    with logfile.open('w') as log:
        process = subprocess.Popen(command, cwd=cwd or output, env={**env, **(extra or {})},
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        timed_out = False
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGKILL)
            code = process.wait()
    result = dict(name=name, command=command, cwd=str(cwd or output),
                  seconds=round(time.monotonic()-started, 3), exit_code=code,
                  timed_out=timed_out, log=logfile.name)
    text = logfile.read_text(errors='replace')
    result['passed'] = code == 0 and not timed_out and (marker is None or marker in text)
    report['results'].append(result)
    save()
    print(json.dumps(result), flush=True)
    assert result['passed'], f'{name} failed: {text[-4000:]}'
    return text


manifest = json.loads((prefix / 'PACKAGE-MANIFEST.json').read_text())
for name, item in manifest['files'].items():
    path = prefix / name
    if 'symlink' in item:
        assert path.is_symlink() and os.readlink(path) == item['symlink'], name
    else:
        assert not path.is_symlink() and path.is_file(), name
        assert hashlib.file_digest(path.open('rb'), 'sha256').hexdigest() == item['sha256'], name
actual = {str(p.relative_to(prefix)) for p in prefix.rglob('*') if p.is_file() or p.is_symlink()}
assert actual == set(manifest['files']) | {'PACKAGE-MANIFEST.json'}
report['package_files_verified'] = len(manifest['files'])
report['node_sha256'] = hashlib.file_digest(Path(node).open('rb'), 'sha256').hexdigest()
save()
run('machine', ['sh', '-c', 'uname -a; getconf PAGESIZE; getconf GNU_LIBC_VERSION; cat /proc/cpuinfo'], timeout=30)
run('elf', ['readelf', '-h', '-d', '--version-info', node], timeout=30)
run('libraries', ['ldd', node], timeout=30)
run('runtime', [node, '-e', "const a=require('node:assert/strict'),o=require('node:os'); a.equal(process.arch,'sparc64');a.equal(o.endianness(),'BE');a.equal(process.execPath,process.env.SPARC64_EXPECT_NODE);a.equal(process.features.typescript,'strip');a.equal(typeof Temporal,'object');a.equal(process.config.variables.node_use_ffi,true);console.log(JSON.stringify({execPath:process.execPath,versions:process.versions,features:process.features,cpus:o.cpus(),memory:process.memoryUsage()}))"], timeout=30)
run('npm-version', [npm, '--version'], timeout=30)
run('npx-version', [npx, '--version'], timeout=30)
run('compiler', ['gcc', '--version'], timeout=30)
run('addon-compiler', ['g++', '--version'], timeout=30)
addon = output / 'addon'
shutil.copytree(fixtures / 'addon', addon)
run('addon-build', [npm, 'install', '--offline', '--nodedir=' + str(prefix), '--no-audit', '--no-fund'], cwd=addon)
run('addon-execute', [node, '--expose-gc', str(addon / 'test.cjs')], marker='INSTALLED_NATIVE_ADDON_OK')
cli = output / 'cli'
shutil.copytree(fixtures / 'cli', cli)
run('cli-pack', [npm, 'pack', '--offline'], cwd=cli, timeout=60)
consumer = output / 'cli-consumer'
consumer.mkdir()
(consumer / 'package.json').write_text('{"name":"installed-cli-consumer","version":"1.0.0","private":true}\n')
run('cli-install', [npm, 'install', '--offline', str(cli / 'sparc64-installed-cli-1.0.0.tgz')], cwd=consumer, timeout=60)
run('npm-exec', [npm, 'exec', '--offline', '--', 'sparc64-installed-cli', 'SPARC64'], cwd=consumer, marker='INSTALLED_NPX_CLI_OK')
run('npx-exec', [npx, '--offline', '--no', 'sparc64-installed-cli', 'SPARC64'], cwd=consumer, marker='INSTALLED_NPX_CLI_OK')
run('typescript-cold', [node, str(fixtures / 'typescript/main.ts')],
    extra={'NODE_COMPILE_CACHE': str(output / 'compile-cache')}, marker='TYPESCRIPT_APPLICATION_OK')
run('typescript-warm', [node, str(fixtures / 'typescript/main.ts')],
    extra={'NODE_COMPILE_CACHE': str(output / 'compile-cache')}, marker='TYPESCRIPT_APPLICATION_OK')
for name, flags in [('normal', []), ('compact', ['--stress-compaction', '--verify-heap'])]:
    run('temporal-' + name, [node, *flags, str(fixtures / 'temporal.cjs')], marker='"passed":true')
run('node-apis', [node, str(fixtures / 'node-apis.cjs')], marker='"passed":true')
run('native-regexp', [node, '--allow-natives-syntax', '--expose-gc', '--verify-heap',
    '--compact-on-every-full-gc', str(fixtures / 'regexp-native.cjs')], marker='SNAPSHOT_REGEXP_FLAGS_OK')
blob = output / 'installed-snapshot.blob'
snapshot_flags = ['--allow-natives-syntax', '--expose-gc', '--verify-heap', '--track-array-buffer-views',
                  '--snapshot-blob=' + str(blob)]
run('snapshot-build', [node, *snapshot_flags, '--build-snapshot', str(fixtures / 'snapshot-keys.cjs')],
    marker='SNAPSHOT_ARRAY_BUFFER_KEYS_BUILT')
for restore in range(2):
    run('snapshot-restore-' + str(restore), [node, *snapshot_flags], marker='SNAPSHOT_ARRAY_BUFFER_KEYS_OK')

for tier, flags in [('liftoff', ['--liftoff-only']), ('optimized', ['--no-liftoff', '--no-liftoff-only'])]:
    for fixture, marker in [('wasm-simd.cjs', 'WASM_SIMD_MATRIX_OK'),
                            ('wasm-undici.cjs', '"passed":true'),
                            ('wasm-gc-jspi.cjs', 'WASM_GC_JSPI_INSTALLED_OK')]:
        run(tier + '-' + fixture, [node, '--allow-natives-syntax', '--expose-gc', '--verify-heap',
            '--no-wasm-lazy-compilation', *flags, str(fixtures / fixture)],
            extra={'WASM_SIMD_TIER': tier}, marker=marker)
oracle = output / 'ffi-oracle.so'
run('ffi-build', ['gcc', '-O2', '-fPIC', '-shared', str(fixtures / 'ffi-call-oracle.c'), '-o', str(oracle), '-ldl'])
run('ffi-native', [node, '--allow-natives-syntax', '--expose-internals', '--expose-gc', '--verify-heap',
                   str(fixtures / 'ffi-fast.cjs')], extra={'SPARC64_FFI_ORACLE': str(oracle)}, marker='FFI_FAST_NATIVE_OK')
run('ffi-memory-callback', [node, '--expose-gc', '--verify-heap', str(fixtures / 'ffi-memory-callback.cjs')],
    extra={'SPARC64_FFI_ORACLE': str(oracle)}, marker='FFI_MEMORY_CALLBACK_INSTALLED_OK')
certs = output / 'certificates'
certs.mkdir()
run('certificates', ['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
    '-keyout', str(certs / 'key.pem'), '-out', str(certs / 'cert.pem'), '-subj', '/CN=localhost',
    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], timeout=60)
apps = output / 'apps'
shutil.copytree(fixtures / 'apps', apps)
run('application-install', [npm, 'ci', '--ignore-scripts', '--omit=optional', '--no-audit', '--no-fund'], cwd=apps, timeout=240)
run('https-websocket', [node, str(apps / 'express-websocket.cjs')], cwd=apps,
    extra={'SPARC64_SMOKE_CERT_DIR': str(certs)}, marker='PASS Express HTTPS')
run('application-soak', [node, '--allow-natives-syntax', '--expose-gc', str(apps / 'turbofan-soak.cjs')],
    cwd=apps, extra={'SPARC64_SMOKE_CERT_DIR': str(certs)}, timeout=260, marker='TURBOFAN_APPLICATION_SOAK_OK')
soak = json.loads((apps / 'turbofan-soak-result.json').read_text())
report['application_baseline'] = {
    'requests': soak['requests'], 'seconds': soak['milliseconds'] / 1000,
    'requests_per_second': soak['requests'] * 1000 / soak['milliseconds'],
    'final_memory': soak['samples'][-1], 'worker_results': soak['workers']}
report['passed'] = True
save()
print('INSTALLED_PACKAGE_VALIDATION_OK', flush=True)
