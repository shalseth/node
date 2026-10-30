// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const path = require('node:path');
const vm = require('node:vm');
const tier = process.env.AMARO_WASM_TIER || 'default';
const modules = [], instances = [];
const NativeModule = WebAssembly.Module;
const NativeInstance = WebAssembly.Instance;
WebAssembly.Module = class extends NativeModule {
  constructor(bytes, ...options) {
    super(bytes, ...options);
    modules.push(crypto.createHash('sha256').update(bytes).digest('hex'));
  }
};
WebAssembly.Instance = class extends NativeInstance {
  constructor(...options) { super(...options); instances.push(this); }
};
let amaro;
try {
  amaro = require(path.resolve(__dirname, '../../deps/amaro/dist/index.js'));
} finally {
  WebAssembly.Module = NativeModule;
  WebAssembly.Instance = NativeInstance;
}
assert.equal(modules.length, 1);
assert.equal(instances.length, 1);
let checks = 0;
const cases = [
  ['const answer: number = 42; globalThis.result = answer;', 42],
  ['interface Person { name: string; age: number }\n' +
   'const p: Person = {name:"Ada",age:37}; globalThis.result=p.name+":"+p.age;', 'Ada:37'],
  ['function identity<T>(value: T): T { return value; }\n' +
   'globalThis.result=identity<number>(123);', 123],
  ['const values=[1,2,3] as const; globalThis.result=values.reduce((a,b)=>a+b,0);', 6],
  ['const value = {x: 19} satisfies {x: number}; globalThis.result=value.x;', 19],
  ['const navn: string="blåbær漢字😀"; globalThis.result=navn;', 'blåbær漢字😀'],
];
for (let round = 0; round < 16; ++round) {
  for (const [input, expected] of cases) {
    const {code} = amaro.transformSync(input, {mode:'strip-only'});
    const context = {};
    vm.runInNewContext(code, context);
    assert.equal(context.result, expected);
    assert.equal(code.split('\n').length, input.split('\n').length);
    checks += 2;
  }
  if ((round & 3) === 0) gc();
}
for (const [input, expected] of [['enum Color { Red, Blue }', 'UnsupportedSyntax'],
                                ['const x: number = ;', 'InvalidSyntax']]) {
  assert.throws(() => amaro.transformSync(input, {mode:'strip-only'}),
                error => error.code === expected);
  ++checks;
}
const large = Array.from({length:3072}, (_, n) =>
  `const value${n}: number = ${n};`).join('\n') + '\nglobalThis.result=value3071;';
assert.ok(large.length > 65536);
const context = {};
vm.runInNewContext(amaro.transformSync(large, {mode:'strip-only'}).code, context);
assert.equal(context.result, 3071);
++checks;
const transform = instances[0].exports.transformSync;
assert.equal(typeof transform, 'function');
const liftoff = %IsLiftoffFunction(transform);
const optimized = %IsTurboFanFunction(transform);
assert.ok(liftoff || optimized);
if (tier === 'liftoff') assert.ok(liftoff);
if (tier === 'optimized') assert.ok(optimized);
console.log('AMARO_NATIVE_PARSER_OK ' + JSON.stringify({passed:true, checks,
  calls:99, largeSourceBytes:Buffer.byteLength(large), tier, liftoff, optimized,
  moduleSha256:modules[0]}));
