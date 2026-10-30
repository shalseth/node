// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

const re = /^(a|b)*c$/;
const text = 'ab'.repeat(20000) + 'c';
const match = re.exec(text);
if (!match || match[0] !== text || match[1] !== 'b') throw Error('Bad capture');
if (!%RegexpHasNativeCode(re, true) || %RegexpHasBytecode(re, true))
  throw Error('Native regexp required');
console.log('REGEXP_STACK_GROWTH_OK');
