// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Run with --allow-natives-syntax --expose-gc --verify-heap.
const output = typeof print === 'function' ? print : console.log;
let optimizedEntries = 0;
let lastStatus = 0;
let collections = 0;
let invalidate = false;
function equal(actual, expected) {
  if (!Object.is(actual, expected)) throw new Error(`${actual} != ${expected}`);
}
function collectAndInvalidate(fn) {
  gc({ type: 'minor', execution: 'sync' });
  collections++;
  if (invalidate) {
    %DeoptimizeFunction(fn);
    gc();
    collections++;
  }
}
%NeverOptimizeFunction(collectAndInvalidate);

const old = { field: null, array: new Array(16).fill(null) };
gc();
gc();
function storeAndCall(index, value) {
  const live = { value, nested: { value: value + 1 }, array: [value, value + 2] };
  old.field = live;
  old.array[index & 15] = live.nested;
  lastStatus = %GetOptimizationStatus(storeAndCall);
  collectAndInvalidate(storeAndCall);
  equal(old.field, live);
  equal(old.array[index & 15], live.nested);
  return live.value + live.nested.value + live.array[1];
}

function exceptional(value, callback) {
  const live = { value, text: 'SPARC64: € 🚀' };
  const number = value * 1.25;
  lastStatus = %GetOptimizationStatus(exceptional);
  try {
    callback();
  } catch (error) {
    equal(error, liveError);
    equal(live.text, 'SPARC64: € 🚀');
    return number + live.value;
  }
  return number;
}
const liveError = new Error('callback exception after compacting GC');
let shouldThrow = false;
function callback() {
  collectAndInvalidate(exceptional);
  if (shouldThrow) throw liveError;
}
%NeverOptimizeFunction(callback);

for (let round = 0; round < 30; round++) {
  invalidate = false;
  %PrepareFunctionForOptimization(storeAndCall);
  equal(storeAndCall(round, round), 3 * round + 3);
  equal(storeAndCall(round, round), 3 * round + 3);
  %OptimizeFunctionOnNextCall(storeAndCall);
  equal(storeAndCall(round, round), 3 * round + 3);
  if (!(lastStatus & (1 << 11))) throw new Error('Store/GC must execute TurboFan');
  optimizedEntries++;
  invalidate = true;
  equal(storeAndCall(round, round + 0.5), 3 * (round + 0.5) + 3);
  invalidate = false;
  shouldThrow = false;
  %PrepareFunctionForOptimization(exceptional);
  equal(exceptional(2.5, callback), 3.125);
  equal(exceptional(2.5, callback), 3.125);
  %OptimizeFunctionOnNextCall(exceptional);
  equal(exceptional(2.5, callback), 3.125);
  if (!(lastStatus & (1 << 11))) throw new Error('Exception/GC must execute TurboFan');
  optimizedEntries++;
  invalidate = true;
  shouldThrow = true;
  equal(exceptional(2.5, callback), 5.625);
  if (!(lastStatus & (1 << 11))) throw new Error('Lazy deopt must enter TurboFan');
  optimizedEntries++;
}
equal(optimizedEntries, 90);
equal(collections, 300);
output(`PASS optimized GC/deopt: entries=${optimizedEntries}, collections=${collections}`);
