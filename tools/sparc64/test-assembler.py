#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Build the initial V8 SPARC64 encoder tests without a working isolate."""

import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys


def ninja_path(path):
    return str(path).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def main():
    root = Path(__file__).resolve().parents[2]
    v8 = root / 'deps/v8'
    overlay_value = os.environ.get('SPARC64_SOURCE_OVERLAY')
    overlay = Path(overlay_value).resolve() if overlay_value else None
    if overlay and not overlay.is_dir():
        sys.exit('SPARC64_SOURCE_OVERLAY must name a V8 source directory')
    gtest = root / 'deps/googletest'
    test_args = sys.argv[1:]
    full = '--full' in test_args
    if full:
        test_args.remove('--full')
        if platform.machine() not in ('sparc64', 'sparcv9'):
            sys.exit('--full requires a native SPARC64 host')
    out = root / ('out/sparc64-assembler-full' if full else
                  'out/sparc64-assembler')
    if overlay:
        out = out.with_name(out.name + '-overlay')
    out.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get('CXX', 'g++'))
    flags = ['-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
             '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-comment',
             '-DV8_TARGET_ARCH_SPARC64=1',
             *(['-I' + str(overlay)] if overlay else []), '-I' + str(v8),
             '-I' + str(v8 / 'include'),
             '-I' + str(v8 / 'third_party/abseil-cpp'),
             '-I' + str(gtest), '-I' + str(gtest / 'include')]
    sources = [v8 / 'src/codegen/sparc64/base-assembler-sparc64.cc',
               v8 / 'test/unittests/assembler/assembler-sparc64-unittest.cc',
               gtest / 'src/gtest-all.cc', gtest / 'src/gtest_main.cc']
    if platform.machine() in ('sparc64', 'sparcv9'):
        sources.append(v8 / 'test/unittests/assembler/sparc64-reference.S')
    libraries = []
    if full:
        makefile = root / ('out/tools/v8_gypfiles/'
                           'v8_base_without_compiler.target.mk')
        if '-DV8_INTL_SUPPORT' in makefile.read_text():
            flags.append('-DV8_INTL_SUPPORT')
        flags[0] = '-std=gnu++20'
        flags.remove('-Wextra')
        flags.append('-Wno-return-type')
        flags += ['-ffunction-sections', '-fdata-sections', '-flto', '-DV8_JITLESS',
                  '-Wno-invalid-offsetof']
        flags += ['-I' + str(v8 / 'third_party/simdutf'),
                  '-I' + str(root / 'out/Release/obj/gen'),
                  '-I' + str(root / 'out/Release/obj/gen/'
                            'generate-bytecode-output-root')]
        sources += [v8 / path for path in (
            'src/codegen/sparc64/assembler-sparc64.cc',
            'src/codegen/sparc64/cpu-sparc64.cc',
            'src/codegen/sparc64/macro-assembler-sparc64.cc',
            'src/codegen/macro-assembler-base.cc',
            'src/codegen/assembler.cc', 'src/codegen/flush-instruction-cache.cc',
            'src/codegen/reloc-info.cc',
            'src/codegen/register-configuration.cc',
            'src/deoptimizer/sparc64/deoptimizer-sparc64.cc',
            'src/heap/base/asm/sparc64/push_registers_asm.cc',
            'src/heap/memory-chunk.cc',
            'src/diagnostics/sparc64/disasm-sparc64.cc',
            'src/flags/flags.cc', 'src/utils/allocation.cc',
            'test/unittests/assembler/assembler-sparc64-buffer-unittest.cc')]
        libdir = root / 'out/Release/obj.target/tools/v8_gypfiles'
        libraries = [str(libdir / 'libv8_libbase.a'),
                     str(libdir / 'libabseil.a')]
    if overlay:
        sources = [
            overlay / source.relative_to(v8)
            if source.is_relative_to(v8) and
            (overlay / source.relative_to(v8)).is_file() else source
            for source in sources
        ]
    link_flags = ['-flto', '-Wl,--gc-sections'] if full else []
    rules = [
        'cxx = ' + shlex.join(compiler).replace('$', '$$'),
        'cxxflags = ' + shlex.join(flags).replace('$', '$$'),
        'linkflags = ' + shlex.join(link_flags + libraries +
                                   (['-ldl', '-lrt', '-latomic'] if full else
                                    [])).replace('$', '$$'),
        'rule cxx',
        '  command = $cxx $cxxflags -MMD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule link',
        '  command = $cxx -pthread $in $linkflags -o $out',
    ]
    objects = []
    for source in sources:
        obj = source.name + '.o'
        rules.append(f'build {obj}: cxx {ninja_path(source)}')
        objects.append(obj)
    binary = out / 'assembler-sparc64-unittests'
    link = f'build {binary.name}: link ' + ' '.join(objects)
    if libraries:
        link += ' | ' + ' '.join(ninja_path(path) for path in libraries)
    rules.append(link)
    (out / 'build.ninja').write_text('\n'.join(rules) + '\n')
    subprocess.run(['ninja', '-C', str(out), '-j', '4', binary.name],
                   check=True)
    return subprocess.run([str(binary)] + test_args).returncode


if __name__ == '__main__':
    sys.exit(main())
