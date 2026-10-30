#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Write V8 test metadata for the native SPARC64 GCC Release build."""

import argparse
import ast
import json
import re
from pathlib import Path


root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-turbofan', action='store_true')
parser.add_argument('--allow-webassembly', action='store_true')
args = parser.parse_args()
variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
make_path = root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk'
makefile = make_path.read_text()
release = re.search(r'DEFS_Release :=(.*?)(?=\n\n)', makefile, re.S).group(1)
defines = set(re.findall(r'-D([A-Za-z_][A-Za-z_0-9]*)', release))
assert variables['target_arch'] == 'sparc64'
assert 'NDEBUG' in defines and 'DEBUG' not in defines
assert args.allow_turbofan == ('V8_ENABLE_TURBOFAN' in defines)
assert args.allow_webassembly == ('V8_ENABLE_WEBASSEMBLY' in defines)
if 'V8_JITLESS' in defines:
    assert 'V8_LITE_MODE' in defines
else:
    assert 'V8_ENABLE_SPARKPLUG' in defines
    assert 'V8_LITE_MODE' not in defines
    assert 'V8_ENABLE_MAGLEV' not in defines
text = (root / 'deps/v8/BUILD.gn').read_text()
text = text.split('action("v8_dump_build_config") {', 1)[1].split('\n}', 1)[0]
keys = re.findall(r'^    "([A-Za-z_][A-Za-z_0-9]*)=', text, re.M)
disabled_flags = [
    'asan',
    'cet_shadow_stack',
    'cfi',
    'clang_coverage',
    'component_build',
    'dcheck_always_on',
    'debugging_features',
    'full_debug',
    'is_android',
    'is_ios',
    'mips_use_msa',
    'msan',
    'official_build',
    'simd_mips',
    'simulator_run',
    'tsan',
    'ubsan',
    'use_sanitizer',
    'verification_features',
    'wasm_random_fuzzers',
]
config = dict.fromkeys(disabled_flags, False)
for key in ['arch', 'current_cpu', 'target_cpu', 'v8_current_cpu', 'v8_target_cpu']:
    config[key] = 'sparc64'
config['mips_arch_variant'] = ''
macro_flags = {
    'atomic_object_field_writes': 'V8_ATOMIC_OBJECT_FIELD_WRITES',
    'code_comments': 'V8_CODE_COMMENTS',
    'debug_code': 'V8_ENABLE_DEBUG_CODE',
    'DEBUG_defined': 'DEBUG',
    'dict_property_const_tracking': 'V8_DICT_PROPERTY_CONST_TRACKING',
    'direct_handle': 'V8_ENABLE_DIRECT_HANDLE',
    'disassembler': 'ENABLE_DISASSEMBLER',
    'gdbjit': 'ENABLE_GDB_JIT_INTERFACE',
    'has_jitless': 'V8_JITLESS',
    'has_maglev': 'V8_ENABLE_MAGLEV',
    'has_turbofan': 'V8_ENABLE_TURBOFAN',
    'has_webassembly': 'V8_ENABLE_WEBASSEMBLY',
    'has_wasm_interpreter': 'V8_ENABLE_DRUMBRAKE',
    'i18n': 'V8_INTL_SUPPORT',
    'temporal': 'V8_TEMPORAL_SUPPORT',
    'lite_mode': 'V8_LITE_MODE',
    'local_off_stack_check': 'V8_ENABLE_LOCAL_OFF_STACK_CHECK',
    'pointer_compression': 'V8_COMPRESS_POINTERS',
    'pointer_compression_shared_cage': 'V8_COMPRESS_POINTERS_IN_SHARED_CAGE',
    'runtime_call_stats': 'V8_RUNTIME_CALL_STATS',
    'sandbox': 'V8_ENABLE_SANDBOX',
    'sandbox_hardware_support': 'V8_ENABLE_SANDBOX_HARDWARE_SUPPORT',
    'single_generation': 'V8_ENABLE_SINGLE_GENERATION',
    'slow_dchecks': 'ENABLE_SLOW_DCHECKS',
    'v8_cfi': 'V8_ENABLE_CONTROL_FLOW_INTEGRITY',
    'verify_csa': 'ENABLE_VERIFY_CSA',
    'verify_heap': 'VERIFY_HEAP',
    'verify_predictable': 'VERIFY_PREDICTABLE',
    'memory_corruption_api': 'V8_ENABLE_MEMORY_CORRUPTION_API',
    'lower_limits_mode': 'V8_LOWER_LIMITS_MODE',
}
for key, macro in macro_flags.items():
    config[key] = macro in defines
config['clang'] = bool(variables['clang'])
config['concurrent_marking'] = True
config['js_shared_memory'] = 'V8_DISABLE_WRITE_BARRIERS' not in defines
assert set(config) == set(keys), 'Audit metadata when upstream build flags change'
assert not variables['clang'], 'This metadata helper supports the GCC build'
output = root / 'out/Release/v8_build_config.json'
output.write_text(json.dumps(config, indent=2) + '\n')
print('Wrote Release build metadata:', len(config), 'properties')
