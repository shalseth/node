// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const cases = [
  ['route', /^\/api\/([a-z]+)\/(\d+)(?:\?(.+))?$/, '/api/items/123456?order=desc'],
  ['email', /^[a-z0-9._%+-]+@[a-z0-9.-]+\.[a-z]{2,}$/i, 'sparc.port+test@example.org'],
  ['search', /(?:error|warn|info):\s+([^\n]+)/gi, 'prefix '.repeat(16) + 'WARN: sparc ready\n'],
  ['unicode', /\p{Letter}+(?:\s+\p{Letter}+)*/u, 'Σπαρκ JavaScript 漢字 engine'],
  ['backreference', /([a-z]+)-\1/i, 'prefix Engine-ENGINE suffix'],
];
const results = [];
for (const [name, re, text] of cases) {
  let checksum = 0;
  const count = 100000;
  for (let i = 0; i < 2000; i++) { re.lastIndex = 0; re.exec(text); }
  const start = process.hrtime.bigint();
  for (let i = 0; i < count; i++) {
    re.lastIndex = 0;
    const match = re.exec(text);
    if (match) checksum += match[0].length;
  }
  const ns = Number(process.hrtime.bigint() - start);
  const native = (%RegexpHasNativeCode(re, true) && !%RegexpHasBytecode(re, true)) ||
    (%RegexpHasNativeCode(re, false) && !%RegexpHasBytecode(re, false));
  results.push({name, count, checksum, native, nanoseconds: ns});
}
console.log(JSON.stringify(results));
