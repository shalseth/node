// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

function loop() {
  let sum = 0;
  let sawTurboFan = false;
  for (let i = 0; i < 50; i++) {
    if (i === 5) %OptimizeOsr();
    if (i > 5 && (%GetOptimizationStatus(loop) & (1 << 11))) {
      sawTurboFan = true;
    }
    sum += i;
  }
  if (!sawTurboFan) throw new Error('Loop did not execute TurboFan code');
  return sum;
}
%PrepareFunctionForOptimization(loop);
if (loop() !== 1225) throw new Error('Wrong OSR result');
print('INTERPRETER TO TURBOFAN OSR PASS');
