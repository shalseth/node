#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Inventory JIT gaps using the current native Release preprocessor flags."""

import json
import os
from pathlib import Path
import re
import shlex
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    v8 = root / 'deps/v8'
    makefile = root / ('out/tools/v8_gypfiles/'
                       'v8_compiler_for_mksnapshot_source_set.target.mk')
    make = makefile.read_text()

    def make_words(name):
        block = re.search(r'^' + name + r' :=(.*?)(?=\n\n)', make,
                          re.M | re.S)
        if not block:
            raise RuntimeError('Missing GYP flag block: ' + name)
        return shlex.split(block[1].replace('\\\n', ' '))

    defines = make_words('DEFS_Release')
    includes = [value.replace('$(srcdir)', str(root)).replace(
                    '$(obj)', str(root / 'out/Release/obj'))
                for value in make_words('INCS_Release')]
    assert not any('$(' in value for value in includes), includes
    compiler = shlex.split(os.environ.get('CXX', 'g++'))

    def preprocess(relative):
        command = compiler + ['-std=gnu++20', '-E', '-P'] + defines + includes
        command.append(str(v8 / relative))
        result = subprocess.run(command, cwd=root, text=True,
                                stdout=subprocess.PIPE, check=True, timeout=30)
        return result.stdout

    codegen = preprocess('src/compiler/backend/sparc64/code-generator-sparc64.cc')
    enumeration = re.search(r'enum ArchOpcode\s*\{(.*?)\};', codegen, re.S)[1]
    declared = set(re.findall(r'\bk\w+', enumeration.split('kLastArchOpcode')[0]))
    function = re.search(r'CodeGenerator::AssembleArchInstruction\s*\('
                         r'\s*Instruction\*\s*instr\)\s*\{', codegen)
    start = function.end()
    depth = 1
    cursor = start
    while depth:
        depth += (codegen[cursor] == '{') - (codegen[cursor] == '}')
        cursor += 1
    cases = set(re.findall(r'\bcase\s+(k\w+)\s*:', codegen[start:cursor]))
    implemented = sorted(declared & cases)
    missing = sorted(declared - cases)

    selector = preprocess('src/compiler/backend/sparc64/instruction-selector-sparc64.cc')
    selector_source = (v8 / 'src/compiler/backend/sparc64/'
                       'instruction-selector-sparc64.cc').read_text()
    selector_source = re.sub(r'/\*.*?\*/|//[^\n]*', '', selector_source, flags=re.S)
    stubs = [{'method': name, 'guard': guard} for name, guard in re.findall(
        r'InstructionSelector::(\w+)\s*\([^{};]*\)\s*\{\s*'
        r'(UNIMPLEMENTED|UNREACHABLE)\(\);\s*\}', selector_source)
        if re.search(r'InstructionSelector::' + name + r'\s*\(', selector)]

    builtins_path = 'src/builtins/sparc64/builtins-sparc64.cc'
    builtins = (v8 / builtins_path).read_text()
    baseline_required = {'BaselineOutOfLinePrologue',
                         'BaselineOutOfLinePrologueDeopt',
                         'InterpreterOnStackReplacement_ToBaseline'}
    guards = []
    for match in re.finditer(r'void Builtins::Generate_(\w+)\([^{}]*\)\s*'
                             r'\{\s*GenerateJitlessTrap\(masm\);\s*\}', builtins):
        name = match[1]
        guards.append({'builtin': name, 'file': builtins_path,
                       'line': builtins.count('\n', 0, match.start()) + 1,
                       'phase': 'sparkplug' if name in baseline_required
                       else 'optimizing-tier'})

    baseline_files = [
        'src/baseline/sparc64/baseline-assembler-sparc64-inl.h',
        'src/baseline/sparc64/baseline-compiler-sparc64-inl.h',
    ]
    reference = (v8 / 'src/baseline/ppc/baseline-assembler-ppc-inl.h').read_text()
    methods = sorted(set(re.findall(r'BaselineAssembler::(\w+)\s*\(', reference)))
    config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
    inventory = {
        'build_features': {key: config[key] for key in (
            'arch', 'has_jitless', 'has_turbofan', 'has_maglev',
            'has_webassembly', 'i18n')},
        'baseline_files': {name: (v8 / name).exists() for name in baseline_files},
        'jitless_trap_builtins': guards,
        'reference_baseline_assembler_methods': methods,
        'declared_opcode_count': len(declared),
        'emitter_case_count': len(implemented),
        'emitter_cases': implemented,
        'unhandled_declared_opcodes': missing,
        'selector_whole_body_guards': sorted(stubs, key=lambda item: item['method']),
        'interpretation': (
            'Unhandled declarations include SIMD and optional/optimizer operations; '
            'they are not all Sparkplug prerequisites. Whole-body selector guards '
            'can be intentional when machine capabilities force lowering.'),
    }
    output = root / 'out/sparc64-jit-inventory.json'
    output.write_text(json.dumps(inventory, indent=2) + '\n')
    print(f'{len(guards)} guarded builtins; {len(implemented)}/{len(declared)} '
          f'emitter cases; {len(stubs)} whole-body selector guards')
    print(output)


if __name__ == '__main__':
    main()
