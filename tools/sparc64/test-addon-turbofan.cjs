// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const addon = require(path.resolve(process.argv[2]));
let entries = 0;
let lastStatus = 0;
let invalidating = false;
function callback(box) {
  if (invalidating) %DeoptimizeFunction(invoke);
  gc();
  box.afterGc = { payload: box.payload };
  gc({ type: 'minor', execution: 'sync' });
  return box.payload;
}
%NeverOptimizeFunction(callback);
function invoke(payload) {
  const live = { payload, number: payload.number * 1.25 };
  lastStatus = %GetOptimizationStatus(invoke);
  const result = addon.roundTrip(callback, payload);
  assert.strictEqual(result.box.payload, live.payload);
  assert.strictEqual(result.box.afterGc.payload, payload);
  assert.strictEqual(result.result, payload);
  return live.number;
}
for (let round = 0; round < 12; round++) {
  invalidating = false;
  const payload = { number: round + 0.5, values: Array.from({ length: 64 }, (_, n) => ({ n })) };
  %PrepareFunctionForOptimization(invoke);
  assert.equal(invoke(payload), payload.number * 1.25);
  assert.equal(invoke(payload), payload.number * 1.25);
  %OptimizeFunctionOnNextCall(invoke);
  assert.equal(invoke(payload), payload.number * 1.25);
  assert.ok(lastStatus & (1 << 11), 'Addon caller must execute TurboFan');
  entries++;
  invalidating = true;
  %OptimizeFunctionOnNextCall(invoke);
  assert.equal(invoke(payload), payload.number * 1.25);
  assert.ok(lastStatus & (1 << 11), 'Invalidated addon caller must enter TurboFan');
  entries++;
}
console.log(`PASS optimized native addon: ${entries} entries, 48 major/minor GC pairs`);
