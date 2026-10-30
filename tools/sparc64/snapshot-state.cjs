// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const { startupSnapshot } = require('node:v8');
assert.ok(startupSnapshot.isBuildingSnapshot());
const state = {
  delta: 17, bigint: -(2n ** 192n) + 1337n,
  map: new Map([['text', 'SPARC64: € 🚀']]),
  numbers: new Float64Array([-0, NaN, Infinity, -Infinity, 0.125]),
  bytes: new Uint8Array([1, 2, 3, 4, 5]), current: null,
};
state.self = state;
let lastStatus = 0;
function add(value) {
  lastStatus = %GetOptimizationStatus(add);
  return value + state.delta;
}
%CollectGarbage(null);
startupSnapshot.setDeserializeMainFunction(() => {
  assert.ok(!startupSnapshot.isBuildingSnapshot());
  assert.equal(process.env.SPARC64_SNAPSHOT_PHASE, 'restore');
  assert.strictEqual(state.self, state);
  assert.equal(state.current, null);
  assert.equal(state.bigint, -(2n ** 192n) + 1337n);
  assert.equal(state.map.get('text'), 'SPARC64: € 🚀');
  assert.ok(Object.is(state.numbers[0], -0));
  assert.ok(Number.isNaN(state.numbers[1]));
  assert.equal(state.numbers[2], Infinity);
  assert.equal(state.numbers[3], -Infinity);
  assert.equal(state.numbers[4], 0.125);
  assert.deepEqual([...state.bytes], [1, 2, 3, 4, 5]);
  %PrepareFunctionForOptimization(add);
  assert.equal(add(2), 19);
  assert.equal(add(3), 20);
  %OptimizeFunctionOnNextCall(add);
  assert.equal(add(4), 21);
  assert.ok(lastStatus & (1 << 11), 'Restored function must execute TurboFan');
  assert.equal(add(2.5), 19.5);
  for (let round = 0; round < 8; round++) {
    state.current = { round, live: [{ value: round + 0.25 }] };
    %CollectGarbage(null);
    assert.equal(state.current.live[0].value, round + 0.25);
    assert.equal(state.map.get('text'), 'SPARC64: € 🚀');
    assert.strictEqual(state.self, state);
  }
  console.log('SNAPSHOT_STATE_TURBOFAN_OK');
});
console.log('SNAPSHOT_STATE_BUILT');
