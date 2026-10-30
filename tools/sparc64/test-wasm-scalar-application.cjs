// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const { WASI } = require('node:wasi');
const { spawnSync } = require('node:child_process');
const crypto = require('node:crypto');
const root = path.resolve(__dirname, '../..');
const output = path.join(root, 'out/wasm-scalar-application');
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const manifest = JSON.parse(fs.readFileSync(path.join(output, 'build-manifest.json')));
const bytes = fs.readFileSync(path.join(output, 'json-report.wasm'));
assert.equal(hash(bytes), manifest.files['json-report.wasm']);
vm.runInThisContext(fs.readFileSync(path.join(root,
  'deps/v8/test/mjsunit/wasm/wasm-module-builder.js'), 'utf8'));
const wasi = new WASI({version: 'preview1'});
let growthNotifications = 0;
const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {
  ...wasi.getImportObject(), env: {
    emscripten_notify_memory_growth() { ++growthNotifications; },
  },
});
wasi.initialize(instance);
const app = instance.exports;
const builder = new WasmModuleBuilder();
const processSig = builder.addType(makeSig([kWasmI32, kWasmI32, kWasmI32], [kWasmI32]));
const dispatchSig = builder.addType(makeSig([
  kWasmI32, kWasmI32, kWasmI32, kWasmI32, wasmRefType(processSig)], [kWasmI32]));
for (const tail of [false, true]) {
  builder.addFunction(tail ? 'tail' : 'ref', dispatchSig).addBody([
    kExprLocalGet, 0, kExprLocalGet, 1, kExprLocalGet, 2, kExprLocalGet, 4,
    tail ? kExprReturnCallRef : kExprCallRef, processSig,
  ]).exportFunc();
}
const deep = builder.addFunction('deep', dispatchSig);
deep.addBody([
  kExprLocalGet, 3, kExprI32Eqz, kExprIf, kWasmI32,
    kExprLocalGet, 0, kExprLocalGet, 1, kExprLocalGet, 2, kExprLocalGet, 4,
    kExprReturnCallRef, processSig,
  kExprElse,
    kExprLocalGet, 0, kExprLocalGet, 1, kExprLocalGet, 2,
    kExprLocalGet, 3, ...wasmI32Const(1), kExprI32Sub, kExprLocalGet, 4,
    kExprReturnCall, ...wasmUnsignedLeb(deep.index),
  kExprEnd,
]).exportFunc();
const adapters = builder.instantiate().exports;
let nativeFunctions = 0;
for (const value of [...Object.values(app), ...Object.values(adapters)]) {
  if (typeof value === 'function') {
    assert.equal(%IsLiftoffFunction(value), true);
    ++nativeFunctions;
  }
}
const cases = [];
for (let index = 0; index < 24; ++index) {
  cases.push({name: `roundtrip-${index}`, mode: 0, input: JSON.stringify({
    name: `SPARC ${index} π 雪`, enabled: index % 2 === 0, count: index,
    values: [null, true, false, -17, 0.125, 1e100, 'a\u0000b'],
    nested: {b: [index, index + 1], a: {unicode: '🚀'}},
  })});
  cases.push({name: `report-${index}`, mode: 1, input: JSON.stringify({
    entries: Array.from({length: index + 1}, (_, n) => ({
      user: `user-${n % 5}`, amount: (n - index) * 19,
    })),
  })});
}
cases.push({name: 'schema', mode: 2, input: '{"name":"server","enabled":true,"ports":[80,443,8080]}'});
for (const [name, mode, input] of [
  ['malformed', 0, '{bad'], ['missing-field', 1, '{}'],
  ['wrong-type', 1, '{"entries":[{"user":7,"amount":12}]}'],
  ['schema-type', 2, '{"name":"server","enabled":7,"ports":[]}'],
  ['application-error', 9, '{}'],
]) cases.push({name, mode, input, error: true});
cases.push({name: 'large-report', mode: 1, input: JSON.stringify({
  entries: Array.from({length: 15000}, (_, i) => ({user: `user-${i % 71}`, amount: i - 7500})),
})});
cases.push({name: 'large-unicode', mode: 0, input: JSON.stringify({text: 'SPARC π 雪 🚀\u0000'.repeat(70000)})});
const referencePath = path.join(output, 'native-reference.json');
let reference;
if (process.env.SCALAR_NATIVE_ORACLE) {
  reference = {};
  for (const item of cases) {
    const result = spawnSync(process.env.SCALAR_NATIVE_ORACLE, [String(item.mode)], {
      input: item.input + '\n', encoding: 'utf8', maxBuffer: 16 * 1024 * 1024,
      timeout: 20000,
    });
    assert.equal(result.status, 0, result.stderr);
    reference[item.name] = JSON.parse(result.stdout);
  }
  fs.writeFileSync(referencePath, JSON.stringify(reference) + '\n');
} else {
  reference = JSON.parse(fs.readFileSync(referencePath));
}
const started = performance.now();
let checks = 0, invocations = 0, errors = 0;
for (const item of cases) {
  const input = Buffer.from(item.input);
  const pointer = app.malloc(input.length);
  assert.notEqual(pointer, 0);
  Buffer.from(app.memory.buffer, pointer, input.length).set(input);
  for (const route of ['ref', 'tail', 'deep']) {
    const before = app.caught_count();
    const status = adapters[route](pointer, input.length, item.mode,
      route === 'deep' ? 20000 : 0, app.process);
    const result = JSON.parse(Buffer.from(app.memory.buffer,
      app.result_data(), app.result_size()).toString());
    assert.deepEqual(result, reference[item.name], item.name + '/' + route);
    assert.equal(status === 0, !item.error);
    assert.equal(app.caught_count(), before + (item.error ? 1 : 0));
    checks += 3; ++invocations;
    if (item.error) ++errors;
    if (typeof global.gc === 'function') global.gc();
  }
  app.free(pointer);
}
assert.equal(errors, 15);
assert.ok(app.caught_count() >= errors);
assert.ok(manifest.exception_instructions.throws > 0 && manifest.exception_instructions.catches > 0);
const result = {passed: true, application: 'nlohmann/json report processor',
  library: manifest.library.commit, cases: cases.length, invocations, checks,
  errors, caught: app.caught_count(), nativeFunctions, tailDepth: 20000,
  growthNotifications, bytes: bytes.length, seconds: (performance.now() - started) / 1000,
  module_sha256: hash(bytes), reference_sha256: hash(fs.readFileSync(referencePath)),
  exception_instructions: manifest.exception_instructions};
fs.writeFileSync(path.join(root, 'out/sparc64-wasm-scalar-application-result.json'),
  JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify(result));
console.log('WASM_SCALAR_APPLICATION_OK');
