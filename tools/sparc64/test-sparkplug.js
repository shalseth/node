// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

function baselineCheck(fn, args, expected) {
  assertEquals(expected, fn(...args));
  %CompileBaseline(fn);
  assertTrue(isBaseline(fn));
  assertEquals(expected, fn(...args));
}

baselineCheck(function constant() { return 42; }, [], 42);
baselineCheck(function arithmetic(a, b) { return a * 3 + b - 2; }, [7, 4], 23);
baselineCheck(function branch(a) { return a < 0 ? 'negative' : 'positive'; },
              [-1], 'negative');
baselineCheck(function argumentsCount(a, b) {
  return [a, b, arguments.length];
}, [1, 2, 3], [1, 2, 3]);
baselineCheck(function missingArgument(a, b) { return b; }, [1], undefined);
baselineCheck(function loop(n) {
  let sum = 0;
  for (let i = 0; i < n; ++i) sum += i;
  return sum;
}, [30], 435);
baselineCheck(function call(fn, value) { return fn(value); },
              [x => x + 1, 41], 42);
baselineCheck(function caught(fn) {
  try { fn(); } catch (error) { return error.message; }
}, [() => { throw new Error('caught'); }], 'caught');

const largeLoop = new Function('n',
    'let sum = 0; for (let i = 0; i < n; ++i) {' +
    'sum += 1;'.repeat(18000) + '} return sum;');
baselineCheck(largeLoop, [2], 36000);

function executingBaseline() {
  return (%GetOptimizationStatus(executingBaseline) & (1 << 16)) !== 0;
}
executingBaseline();
%CompileBaseline(executingBaseline);
assertTrue(executingBaseline());

function compacted(value) {
  const object = { value };
  gc();
  object.next = { value: value + 1 };
  gc();
  return object.value + object.next.value;
}
baselineCheck(compacted, [20], 41);

function baselineOsr() {
  let sum = 0;
  for (let i = 0; i < 20; ++i) {
    if (i === 5) %BaselineOsr();
    if (i > 5) {
      assertTrue((%GetOptimizationStatus(baselineOsr) & (1 << 16)) !== 0);
    }
    sum += i;
  }
  return sum;
}
assertEquals(190, baselineOsr());
print('PASS Sparkplug: forced baseline execution, calls, branches, loops, ' +
      'wide backedges, exceptions, compaction and interpreter-to-baseline OSR');
