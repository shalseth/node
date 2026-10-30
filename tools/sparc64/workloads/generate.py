#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Generate a bounded typed application and a matching JavaScript lint corpus."""

import json
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
source = root / 'src'
source.mkdir(exist_ok=True)
(root / 'package.json').write_text(json.dumps({'type': 'module'}) + '\n')
config = dict(compilerOptions=dict(target='ES2022', module='NodeNext',
    moduleResolution='NodeNext', strict=True, declaration=True, sourceMap=True,
    incremental=True, rootDir='src', outDir='dist', types=[]), include=['src/**/*.ts'])
(root / 'tsconfig.json').write_text(json.dumps(config, indent=2) + '\n')
imports = []
expected = 0
for index in range(64):
    text = f'''export interface Item{index} {{ kind: 'item'; value: number; label: string }}
export type Group{index} = Record<string, ReadonlyArray<Item{index}>>;
export class Box{index}<T> {{
  constructor(public readonly items: ReadonlyArray<T>) {{}}
  map<U>(fn: (item: T) => U): Box{index}<U> {{ return new Box{index}(this.items.map(fn)); }}
}}
export function compute{index}(size: number): number {{
  const entries: Item{index}[] = Array.from({{length: size}}, (_, i) => ({{
    kind: 'item', value: i % 7 + {index}, label: `item-${{i}}`
  }}));
  const groups: Group{index} = {{ all: entries }};
  return new Box{index}(groups.all).map(item => item.value).items.reduce((a, b) => a + b, 0);
}}
'''
    (source / f'module{index}.ts').write_text(text)
    imports.append(f"import {{ compute{index} }} from './module{index}.js';")
    expected += sum(i % 7 + index for i in range(1000))
main = '\n'.join(imports) + '\nconst checksum = ' + ' + '.join(f'compute{i}(1000)' for i in range(64)) + ';\n'
main += f"if (checksum !== {expected}) throw Error('Bad compiler output: ' + checksum);\n"
main += "console.log('COMPILED_APPLICATION_OK', checksum);\n"
(source / 'main.ts').write_text(main)
(root / 'eslint.config.mjs').write_text('''export default [{
  files: ['dist/**/*.js'],
  languageOptions: {ecmaVersion: 2022, sourceType: 'module', globals: {console: 'readonly'}},
  rules: {'no-undef': 'error', 'no-unused-vars': 'error', 'constructor-super': 'error',
          'valid-typeof': 'error', 'no-unreachable': 'error', 'no-dupe-args': 'error'}
}];
''')
(root / 'expected.json').write_text(json.dumps(dict(modules=64, source_files=65, checksum=expected), indent=2) + '\n')
print('WORKLOAD_PROJECT_GENERATED', expected)
