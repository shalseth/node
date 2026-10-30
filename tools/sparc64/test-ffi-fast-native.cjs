'use strict';

const assert = require('node:assert/strict');
const { internalBinding } = require('internal/test/binding');
const binding = internalBinding('ffi');
const rawGetFunction = binding.DynamicLibrary.prototype.getFunction;
const ffi = require('node:ffi');
const { lib, functions } = ffi.dlopen(process.env.SPARC64_FFI_ORACLE, {
  weighted_two: { arguments: ['u64', 'u64'], return: 'u64' },
  weighted_eight: { arguments: Array(8).fill('u64'), return: 'u64' },
  call_was_jit: { arguments: [], return: 'i32' },
  observed_argument: { arguments: ['u32'], return: 'u64' },
});
const rawLibrary = new binding.DynamicLibrary(process.env.SPARC64_FFI_ORACLE);
const target = rawGetFunction.call(rawLibrary, 'weighted_eight', {
  arguments: Array(8).fill('u64'), return: 'u64',
});
function callEight(a, b, c, d, e, f, g, h) {
  return target(a, b, c, d, e, f, g, h);
}
const weights = [3n, 5n, 7n, 11n, 13n, 17n, 19n, 23n];
function expected(values) {
  return BigInt.asUintN(64, values.reduce((sum, value, i) => sum + value * weights[i], 0n));
}
const initial = [1n, 2n, 3n, 4n, 5n, 6n, 7n, 8n];
assert.equal(callEight(...initial), expected(initial));
assert.equal(functions.call_was_jit(), 0, 'unoptimized control must call through libffi');
eval('%PrepareFunctionForOptimization(callEight)');
callEight(...initial);
callEight(...initial);
eval('%OptimizeFunctionOnNextCall(callEight)');
const firstOptimized = callEight(...initial);
if (firstOptimized !== expected(initial)) {
  console.error('NATIVE_ARGUMENTS', initial.map((_, i) => functions.observed_argument(i).toString()));
}
assert.equal(firstOptimized, expected(initial));
assert.equal(eval('%GetOptimizationStatus(callEight)') & 40, 40, 'caller must stay TurboFan optimized');
assert.equal(functions.call_was_jit(), 1, 'native target must return directly to JIT code');
for (let round = 0; round < 64; round++) {
  const values = weights.map((weight, i) => BigInt.asUintN(64,
    0xfedcba9876543210n + BigInt(round) * weight + BigInt(i)));
  assert.equal(callEight(...values), expected(values));
  assert.equal(functions.call_was_jit(), 1);
  if (round === 31) global.gc();
}
assert.equal(eval('%GetOptimizationStatus(callEight)') & 40, 40);
const pair = functions.weighted_two;
function callTwo(a, b) {
  return pair(a, b);
}
assert.equal(callTwo(17n, 23n), 166n);
assert.equal(functions.call_was_jit(), 0);
eval('%PrepareFunctionForOptimization(callTwo)');
eval('%PrepareFunctionForOptimization(pair)');
callTwo(17n, 23n);
callTwo(17n, 23n);
eval('%OptimizeFunctionOnNextCall(callTwo)');
eval('%OptimizeFunctionOnNextCall(pair)');
assert.equal(callTwo(17n, 23n), 166n);
assert.equal(eval('%GetOptimizationStatus(callTwo)') & 40, 40);
assert.equal(functions.call_was_jit(), 1, 'public wrapper must use the native fast path');
for (let round = 0n; round < 64n; round++) {
  assert.equal(callTwo(round, round + 7n), round * 8n + 35n);
  assert.equal(functions.call_was_jit(), 1);
}
lib.close();
assert.throws(() => functions.weighted_eight(...initial), { code: 'ERR_FFI_LIBRARY_CLOSED' });
assert.throws(() => callTwo(17n, 23n), { code: 'ERR_FFI_LIBRARY_CLOSED' });
rawLibrary.close();
console.log('FFI_FAST_NATIVE_OK ' + JSON.stringify({ passed: true, calls: 128,
  eightArguments: true, publicWrapper: true, nativeJitReturnAddress: true,
  gc: true, closedLibrary: true }));
