// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

let useDouble = false;
function callback(value) { return useDouble ? value + 0.5 : value; }
%NeverOptimizeFunction(callback);
const statements = 4000;
let body = 'let sum = 0;';
for (let n = 0; n < statements; n++) body += 'sum = (sum + cb(' + Array.from({length: 6}, () => n).join(',') + ')) | 0;';
body += 'return sum;';
const large = eval('(function large(cb) {' + body + '})');
%PrepareFunctionForOptimization(large);
const expected = statements * (statements - 1) / 2;
for (let n = 0; n < 2; n++) {
  if (large(callback) !== expected) throw new Error('Wrong warm-up result');
}
%OptimizeFunctionOnNextCall(large);
if (large(callback) !== expected) throw new Error('Wrong optimized result');
if (!(%GetOptimizationStatus(large) & (1 << 5))) throw new Error('Not TurboFan code');
useDouble = true;
if (large(callback) !== expected) throw new Error('Wrong deopt result');
print('LARGE OPTIMIZED FUNCTION AND EAGER DEOPT PASS');
