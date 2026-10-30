#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Keep Rust's weak math fallbacks from replacing the platform C libm."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def symbols(command):
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    output = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and len(fields[1]) == 1:
            output.setdefault(fields[0].split('@')[0], set()).add(fields[1])
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    assert args.archive.resolve() != args.output.resolve()
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    nm = shlex.split(os.environ.get('NM', 'nm'))
    objcopy = shlex.split(os.environ.get('OBJCOPY', 'objcopy'))
    libm = Path(subprocess.check_output(
        [*compiler, '-print-file-name=libm.so.6'], text=True).strip()).resolve()
    assert libm.is_file(), str(libm)
    before = symbols([*nm, '-g', '--defined-only', '--format=posix', str(args.archive)])
    platform = symbols([*nm, '-D', '--defined-only', '--format=posix', str(libm)])
    selected = sorted(name for name, kinds in before.items()
                      if kinds == {'W'} and name in platform)
    source_hash = digest(args.archive)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
        staged = Path(temporary) / args.output.name
        command = [*objcopy, *['--localize-symbol=' + name for name in selected],
                   str(args.archive), str(staged)]
        subprocess.run(command, check=True)
        after = symbols([*nm, '-g', '--defined-only', '--format=posix', str(staged)])
        assert after == {name: kinds for name, kinds in before.items() if name not in selected}
        assert digest(args.archive) == source_hash
        report = dict(passed=True, raw_archive=str(args.archive), raw_sha256=source_hash,
                      isolated_archive=str(args.output), isolated_sha256=digest(staged),
                      localized_math_symbols=selected, platform_libm=str(libm),
                      platform_libm_sha256=digest(libm), script_sha256=digest(Path(__file__)),
                      objcopy_version=subprocess.check_output(
                          [*objcopy, '--version'], text=True).splitlines()[0])
        staged.replace(args.output)
        metadata = Path(temporary) / 'isolation.json'
        metadata.write_text(json.dumps(report, indent=2) + '\n')
        metadata.replace(Path(str(args.output) + '.json'))
    print('RUST_LIBM_ISOLATED', len(selected), flush=True)


if __name__ == '__main__':
    main()
