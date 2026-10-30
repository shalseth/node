// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const bytes = require(path.join(root, 'deps/undici/src/lib/llhttp/llhttp-wasm.js'));
const moduleObject = new WebAssembly.Module(bytes);
let instance;
let body = '';
let completed = 0;
let status = 0;
const env = Object.fromEntries(WebAssembly.Module.imports(moduleObject).map(({name}) =>
  [name, () => 0]));
env.wasm_on_body = (parser, address, length) => {
  body += Buffer.from(instance.exports.memory.buffer, address, length).toString();
  return 0;
};
env.wasm_on_headers_complete = (parser, code) => { status = code; return 0; };
env.wasm_on_message_complete = () => { ++completed; return 0; };
instance = new WebAssembly.Instance(moduleObject, {env});
const e = instance.exports;
if (e._initialize) e._initialize();
assert(%IsLiftoffFunction(e.llhttp_execute));
assert(%IsLiftoffFunction(e.malloc));
let checks = 0;
for (const [response, expectedBody, expectedStatus] of [
  ['HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello', 'hello', 200],
  ['HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n', 'abcde', 201],
  ['HTTP/1.1 204 No Content\r\n\r\n', '', 204],
]) {
  for (const fragmentSize of [1, 7, response.length]) {
    body = ''; status = 0; completed = 0;
    const parser = e.llhttp_alloc(2);
    assert(parser > 0);
    const buffer = e.malloc(response.length);
    assert(buffer > 0);
    for (let offset = 0; offset < response.length; offset += fragmentSize) {
      const fragment = Buffer.from(response.slice(offset, offset + fragmentSize));
      new Uint8Array(e.memory.buffer, buffer, fragment.length).set(fragment);
      assert.equal(e.llhttp_execute(parser, buffer, fragment.length), 0);
      ++checks;
    }
    assert.equal(body, expectedBody);
    assert.equal(status, expectedStatus);
    assert.equal(completed, 1);
    e.free(buffer);
    e.llhttp_free(parser);
    checks += 3;
  }
}
console.log(JSON.stringify({checks, nativeFunctions: 2, passed: true}));
