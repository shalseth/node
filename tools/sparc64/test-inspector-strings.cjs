// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const inspector = require('node:inspector');
const session = new inspector.Session();
session.connect();
function post(method, params) {
  return new Promise((resolve, reject) => session.post(method, params, (error, result) => {
    if (error) reject(error);
    else resolve(result);
  }));
}
(async () => {
  let cases = 0;
  for (let padding = 0; padding < 32; padding++) {
    for (const value of ['π', '漢字', '😀', '€', '\ud800', '\udfff', 'a\u0000b']) {
      const expected = 'x'.repeat(padding) + value;
      const response = await post('Runtime.evaluate', {
        expression: JSON.stringify(expected), returnByValue: true,
        throwOnSideEffect: true,
      });
      assert.equal(response.exceptionDetails, undefined);
      assert.equal(response.result.value, expected);
      cases++;
    }
    const expected = 'π' + 'x'.repeat(padding);
    const object = await post('Runtime.evaluate', {
      expression: `({[${JSON.stringify(expected)}]: '漢字😀'})`,
    });
    const response = await post('Runtime.getProperties', {
      objectId: object.result.objectId, ownProperties: true,
    });
    assert.ok(response.result.some(item => item.name === expected && item.value.value === '漢字😀'));
    cases++;
  }
  session.disconnect();
  console.log('INSPECTOR_UTF16_ROUNDTRIP_OK', cases);
})().catch(error => { console.error(error); process.exitCode = 1; });
