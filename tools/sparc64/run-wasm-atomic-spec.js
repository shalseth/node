// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Execute every command emitted by WABT from the unchanged official WAST.
const filename = arguments[0];
const atomicTier = arguments[1] || 'liftoff';
const directory = filename.slice(0, filename.lastIndexOf('/') + 1);
const script = JSON.parse(read(filename));
let instance;
const named = new Map();
const counts = {};
let nativeFunctions = 0;
function check(condition, message) {
  if (!condition) throw new Error(message);
}
function convert(value) {
  if (value.type === 'i32') return Number(BigInt.asIntN(32, BigInt(value.value)));
  if (value.type === 'i64') return BigInt.asIntN(64, BigInt(value.value));
  throw new Error(`Unsupported spec value ${value.type}`);
}
function invoke(action) {
  check(action.type === 'invoke', `Unsupported action ${action.type}`);
  const target = action.module ? named.get(action.module) : instance;
  return target.exports[action.field](...action.args.map(convert));
}
for (const command of script.commands) {
  try {
    if (command.type === 'module') {
      instance = new WebAssembly.Instance(new WebAssembly.Module(
        new Uint8Array(readbuffer(directory + command.filename))));
      if (command.name) named.set(command.name, instance);
      for (const value of Object.values(instance.exports)) {
        if (typeof value === 'function') {
          check(atomicTier === 'optimized' ? %IsTurboFanFunction(value) :
                                          %IsLiftoffFunction(value),
                'Expected native ' + atomicTier + ' function');
          ++nativeFunctions;
        }
      }
    } else if (command.type === 'action') {
      invoke(command.action);
    } else if (command.type === 'assert_return') {
      const result = invoke(command.action);
      const actual = result === undefined ? [] : Array.isArray(result) ? result : [result];
      check(actual.length === command.expected.length, 'Result arity mismatch');
      command.expected.forEach((expected, i) => {
        check(Object.is(actual[i], convert(expected)),
          `Expected ${expected.value}, got ${String(actual[i])}`);
      });
    } else if (command.type === 'assert_trap') {
      let error;
      try { invoke(command.action); } catch (caught) { error = caught; }
      check(error instanceof WebAssembly.RuntimeError, 'Expected Wasm runtime trap');
      const messages = {
        'unaligned atomic': /unaligned/,
        'out of bounds memory access': /out of bounds/,
        'expected shared memory': /Atomics.wait cannot be called in this context/,
      };
      check(messages[command.text]?.test(error.message),
        `Expected ${command.text}, got ${error.message}`);
    } else if (command.type === 'assert_invalid') {
      const bytes = new Uint8Array(readbuffer(directory + command.filename));
      check(!WebAssembly.validate(bytes), 'Expected validation failure');
      let error;
      try { new WebAssembly.Module(bytes); } catch (caught) { error = caught; }
      check(error instanceof WebAssembly.CompileError, 'Expected compile error');
      const messages = {
        'unknown memory': /invalid memory index|memory.*out of bounds|memory index.*exceeds number of declared memories/,
        'atomic alignment must be natural': /invalid alignment for atomic operation/,
      };
      check(messages[command.text]?.test(error.message),
        `Expected ${command.text}, got ${error.message}`);
    } else {
      throw new Error(`Unsupported command ${command.type}`);
    }
    counts[command.type] = (counts[command.type] || 0) + 1;
  } catch (error) {
    throw new Error(`${script.source_filename}:${command.line} ${command.type}: ${error.stack}`);
  }
}
print(JSON.stringify({passed: true, counts, commands: script.commands.length, nativeFunctions}));
