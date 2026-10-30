// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';

global.print = console.log;
require('./test-turbofan.js');
const assert = require('node:assert/strict');
let sawTurboFan = false;
let nextValue = 0;
function arithmetic(value) {
  const result = (Math.imul(value, 1664525) + 1013904223) | 0;
  if (%GetOptimizationStatus(arithmetic) & (1 << 11)) sawTurboFan = true;
  return result;
}
function batch() {
  for (let n = 0; n < 1000; n++) {
    const value = nextValue++;
    const expected = Number(BigInt.asIntN(32, BigInt(value) * 1664525n + 1013904223n));
    assert.equal(arithmetic(value), expected);
  }
}
%NeverOptimizeFunction(batch);
async function main() {
  for (let n = 0; n < 40 && !sawTurboFan; n++) {
    batch();
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.ok(sawTurboFan, 'Normal automatic tiering must reach TurboFan');
  batch();
  console.log('PASS automatic TurboFan tiering and arithmetic:', nextValue, 'calls');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
