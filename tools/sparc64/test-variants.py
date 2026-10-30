#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

import sys
from pathlib import Path
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'deps/v8/tools'))
from testrunner.local.testsuite import VariantsGenerator
from testrunner.local.variants import ALL_VARIANT_FLAGS


class NoWasmVariantsTest(unittest.TestCase):
    def generate(self, has_wasm):
        test = SimpleNamespace(
            only_standard_variant=False,
            suite=SimpleNamespace(statusfile=SimpleNamespace(
                variables={'has_webassembly': has_wasm})))
        return list(VariantsGenerator(['nooptimization']).gen(test))[0][1]

    def test_no_wasm_preserves_javascript_variant(self):
        self.assertEqual(self.generate(False), ['--disable-optimizing-compilers'])

    def test_wasm_flags_are_not_mutated(self):
        expected = list(ALL_VARIANT_FLAGS['nooptimization'][0])
        self.generate(False)
        self.assertEqual(self.generate(True), expected)
        self.assertIn('--no-wasm-lazy-compilation', expected)


if __name__ == '__main__':
    unittest.main()
