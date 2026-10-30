// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';

const results = [];
let native = 0;
let checks = 0;
function capture(re, text) {
  const value = re.exec(text);
  checks++;
  if ((%RegexpHasNativeCode(re, true) && !%RegexpHasBytecode(re, true)) ||
      (%RegexpHasNativeCode(re, false) && !%RegexpHasBytecode(re, false))) native++;
  return value === null ? null : {
    values: Array.from(value, v => v === undefined ? null : v),
    index: value.index,
    groups: value.groups || null,
    indices: value.indices || null,
    lastIndex: re.lastIndex,
  };
}

const patterns = [
  '(a+)(b*)', '(a|ab)+', '(?:ab|ba)*c?', '^(a|b)*c$',
  '(a*)b', 'a{0,3}?b+', '(a)?b\\1', '(ab|ba)\\1',
  '(?<name>a+)(?<tail>b*)', '(?=(a+))a*b', '(?!ab)a.',
  '(?<=a)(b+)', '(?<!a)b', '(?<=(a+))b\\1',
  '^.*$', '^.$', '\\b\\w+\\b', '\\B.\\B', '[^a-z0-9]+',
  '[a-zA-Z0-9_]+', '[\\s\\S]*?', '(?:)', '$', '^',
  '(.)\\1', '(..+)\\1', '[À-ÿ]+', '(Ä+)\\1', '(Σ+)\\1',
  '[\\u0000-\\u00ff]+', '[\\u0100-\\uffff]+',
  '(\\ud83d\\ude00)+', '[^\\ud800-\\udfff]', 'a|$',
];
let seed = 0x51a7c;
function random(n) {
  seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
  return seed % n;
}
const atoms = ['a', 'b', '.', '[ab]', '[^b]', '\\d', '\\s', '[α-ω]'];
const repeats = ['', '?', '*', '+', '{1,3}', '*?', '+?'];
for (let i = 0; i < 64; i++) {
  const a = atoms[random(atoms.length)] + repeats[random(repeats.length)];
  const b = atoms[random(atoms.length)] + repeats[random(repeats.length)];
  patterns.push(random(2) ? '(' + a + ')(' + b + ')' : '(?:' + a + '|' + b + ')');
}
const subjects = ['', 'a', 'ab', 'aaabbb', 'baba', 'abaac', 'aabaaa',
  'AAbBAa', '0 123\t42', '\n\r\u2028\u2029', 'a\nb', 'ääÄÄ', 'ΣσςΣ',
  'αβγδ', '\u0000a\u00ff', '😀a😀', '\ud800a\udfff', '漢字aa漢字',
  'a'.repeat(64) + 'b', 'ab'.repeat(32) + 'c'];
for (const pattern of patterns) {
  for (const flags of ['', 'i', 'g', 'y', 'd', 'du', 'iu', 'gu', 'im']) {
    const re = new RegExp(pattern, flags);
    for (const text of subjects) {
      for (let warm = 0; warm < 3; warm++) {
        re.lastIndex = 0;
        re.exec(text);
      }
      for (const start of [0, 1, text.length, text.length + 1]) {
        re.lastIndex = start;
        results.push(capture(re, text));
      }
      re.lastIndex = 0;
      results.push(text.replace(re, '<$&:$1>'));
      re.lastIndex = 0;
      results.push(text.split(re, 8));
      if (flags.includes('g')) {
        re.lastIndex = 0;
        results.push(Array.from(text.matchAll(re), m => [Array.from(m), m.index]));
      }
    }
  }
}
for (const [pattern, flags, text] of [
  ['[\\p{Letter}&&[^a-z]]+', 'v', 'abcΣ漢字é'],
  ['[\\q{ab|cd|😀}]', 'gv', 'abcd😀ab'],
  ['(\\p{Script=Greek}+)\\1', 'iu', 'ΣσΣσ'],
  ['^(a|b)*c$', '', 'ab'.repeat(20000) + 'c'],
  ['^(a|b)*c$', '', 'ab'.repeat(20000) + 'd'],
  ['(Ä+)\\1', 'i', 'Ä'.repeat(4000)],
  ['(Σ+)\\1', 'iu', 'Σ'.repeat(4000)],
  ['(?:)', 'gu', '😀'.repeat(16)],
]) {
  const re = new RegExp(pattern, flags);
  for (let i = 0; i < 3; i++) { re.lastIndex = 0; re.exec(text); }
  re.lastIndex = 0;
  results.push(capture(re, text));
}
console.log(JSON.stringify({native, checks, results}));
