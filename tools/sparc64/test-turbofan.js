// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

const kExecutingTurboFan = 1 << 11;
let lastStatus = 0;
function add(a, b) {
  const result = a + b;
  lastStatus = %GetOptimizationStatus(add);
  return result;
}
function equal(actual, expected) {
  if (!Object.is(actual, expected)) throw new Error(actual + ' != ' + expected);
}
%PrepareFunctionForOptimization(add);
equal(add(4, 7), 11);
equal(add(4, 7), 11);
%OptimizeFunctionOnNextCall(add);
equal(add(4, 7), 11);
print('EXECUTING STATUS', lastStatus);
if (!(lastStatus & kExecutingTurboFan)) throw new Error('Not executing TurboFan code');
equal(add(1.5, 2), 3.5);
print('EAGER DEOPT PASS');
let doDeopt = false;
function trigger() {
  if (doDeopt) {
    %DeoptimizeFunction(lazy);
    gc();
  }
}
%NeverOptimizeFunction(trigger);
function lazy(x) {
  const keep = [x, x + 1];
  const a = x + 2;
  lastStatus = %GetOptimizationStatus(lazy);
  trigger();
  return keep[0] + keep[1] + a;
}
%PrepareFunctionForOptimization(lazy);
equal(lazy(5), 18);
equal(lazy(5), 18);
%OptimizeFunctionOnNextCall(lazy);
equal(lazy(5), 18);
if (!(lastStatus & kExecutingTurboFan)) throw new Error('Lazy test did not execute TurboFan');
doDeopt = true;
equal(lazy(5), 18);
print('LAZY DEOPT AND GC PASS');
let deoptDouble = false;
function triggerDouble() {
  if (deoptDouble) {
    %DeoptimizeFunction(numberState);
    gc();
  }
}
%NeverOptimizeFunction(triggerDouble);
function numberState(a, b) {
  const before = a * 1.25;
  lastStatus = %GetOptimizationStatus(numberState);
  triggerDouble();
  return before + b;
}
%PrepareFunctionForOptimization(numberState);
equal(numberState(2.5, 3.125), 6.25);
equal(numberState(2.5, 3.125), 6.25);
%OptimizeFunctionOnNextCall(numberState);
equal(numberState(2.5, 3.125), 6.25);
if (!(lastStatus & kExecutingTurboFan)) throw new Error('Double test did not execute TurboFan');
deoptDouble = true;
equal(numberState(2.5, 3.125), 6.25);
print('DOUBLE LAZY DEOPT AND GC PASS');
