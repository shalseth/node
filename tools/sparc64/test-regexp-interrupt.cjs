// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const addon = require('../../out/sparc64-regexp-interrupt.node');
for (let round = 0; round < 5; round++) {
  for (const wide of [false, true]) {
    const re = addon.run(wide);
    assert.ok(%RegexpHasNativeCode(re, !wide));
    assert.equal(%RegexpHasBytecode(re, !wide), false);
    assert.equal(re.test('aaaab'), true);
    assert.equal(re.test('ccc'), false);
    gc();
  }
}
console.log('REGEXP_INTERRUPT_GC_NESTING_OK');
