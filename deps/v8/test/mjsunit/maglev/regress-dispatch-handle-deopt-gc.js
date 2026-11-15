// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --allow-natives-syntax --expose-gc --maglev
// Flags: --no-maglev-inlining --no-concurrent-recompilation

let invalidate = false;

function callee(x) {
  if (invalidate) {
    %DeoptimizeFunction(caller);
    // Keep the invalidated caller on the stack while GC visits its relocations.
    gc();
    gc();
  }
  return x + 1;
}
%NeverOptimizeFunction(callee);

function caller(x) {
  return callee(x) + 1;
}

%PrepareFunctionForOptimization(caller);
assertEquals(3, caller(1));
assertEquals(4, caller(2));
%OptimizeMaglevOnNextCall(caller);
assertEquals(5, caller(3));
assertOptimized(caller);

invalidate = true;
assertEquals(6, caller(4));
