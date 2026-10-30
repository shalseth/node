#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Compile-check the SPARC assembler against generated V8 headers."""

import os
from pathlib import Path
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    compiler = shlex.split(os.environ.get('CXX', 'g++'))
    flags = ['-std=gnu++20', '-fsyntax-only', '-Wno-invalid-offsetof']
    includes = ['deps/v8', 'deps/v8/include',
                'deps/v8/third_party/abseil-cpp',
                'deps/v8/third_party/simdutf', 'out/Release/obj/gen',
                'out/Release/obj/gen/generate-bytecode-output-root']
    flags += ['-I' + str(root / path) for path in includes]
    sources = ['src/codegen/sparc64/assembler-sparc64.cc',
               'src/codegen/sparc64/macro-assembler-sparc64.cc',
               'src/codegen/macro-assembler-base.cc',
               'src/codegen/assembler.cc', 'src/codegen/reloc-info.cc',
               'src/codegen/register-configuration.cc']
    for source in sources:
        print('Checking ' + source, flush=True)
        subprocess.run(compiler + flags + [str(root / 'deps/v8' / source)],
                       check=True)


if __name__ == '__main__':
    main()
