#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Build the unchanged upstream inspector shell against the current V8 libraries."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import resource
import shlex
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert shutil.disk_usage(root).free > 1024 ** 3
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
binary_hashes = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
makefile = root / 'out/tools/v8_gypfiles/d8.target.mk'
text = makefile.read_text()


def expand(value):
    return value.replace('$(srcdir)', str(root)).replace('$(obj)', str(root / 'out/Release/obj'))


def variable(name, content=text):
    match = re.search(r'^' + re.escape(name) + r' :=([^\n]*)', content.replace('\\\n', ' '), re.M)
    assert match, name
    return shlex.split(expand(match[1].replace('\\\n', ' ')))


flags = sum((variable(name) for name in ['DEFS_Release', 'INCS_Release',
                                        'CFLAGS_Release', 'CFLAGS_CC_Release']), [])
base = (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
flags += variable('INCS_Release', base)
sources = ['inspector-test.cc', 'isolate-data.cc', 'task-runner.cc', 'tasks.cc', 'utils.cc']
output = root / 'out/sparc64-jspi-inspector-build'
output.mkdir(parents=True, exist_ok=True)
source_hashes = {}
commands = []
for name in sources:
    source = 'deps/v8/test/inspector/' + name
    assert (root / source).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
    source_hashes[source] = digest(root / source)
    commands.append(['g++', *flags, '-c', source, '-o', str(output / (name + '.o'))])


def compile_one(command):
    run = subprocess.run(command, cwd=root, capture_output=True, timeout=180)
    (output / (Path(command[-1]).name + '.log')).write_bytes(run.stdout + run.stderr)
    print(json.dumps(dict(source=command[-3], exit_code=run.returncode)), flush=True)
    return run.returncode


with ThreadPoolExecutor(max_workers=5) as pool:
    codes = list(pool.map(compile_one, commands))
result = dict(passed=False, binary_sha256=binary_hashes, source_sha256=source_hashes,
              compile_commands=commands, compile_exit_codes=codes, makefile_sha256=digest(makefile))
record = root / 'out/sparc64-jspi-inspector-build.json'
record.write_text(json.dumps(result, indent=2) + '\n')
assert not any(codes), 'See inspector build logs'
match = re.search(r'^\$\(builddir\)/d8: LD_INPUTS := (.*)$', text, re.M)
libraries = shlex.split(expand(match[1].replace('$(OBJS)', '')))
binary = root / 'out/Release/inspector-test'
link = ['g++', '-o', str(binary), *[str(output / (name + '.o')) for name in sources],
        '-Wl,--start-group', *libraries, '-Wl,--end-group', *variable('LDFLAGS_Release'), *variable('LIBS')]
run = subprocess.run(link, cwd=root, capture_output=True, timeout=180)
(output / 'link.log').write_bytes(run.stdout + run.stderr)
result.update(link_command=link, link_exit_code=run.returncode)
record.write_text(json.dumps(result, indent=2) + '\n')
assert run.returncode == 0, 'See inspector link log'
assert all(digest(root / 'out/Release' / name) == value for name, value in binary_hashes.items())
result.update(passed=True, inspector_sha256=digest(binary))
record.write_text(json.dumps(result, indent=2) + '\n')
print('JSPI_INSPECTOR_BUILD_OK', flush=True)
