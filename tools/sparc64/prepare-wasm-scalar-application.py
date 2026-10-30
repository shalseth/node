#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Fetch and verify the unchanged pinned nlohmann/json application dependency."""
import hashlib,json,urllib.request
from pathlib import Path
root=Path(__file__).resolve().parents[2]
out=root/'out/wasm-scalar-application';out.mkdir(parents=True,exist_ok=True)
commit='9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03'
records={}
for source,name,expected in [
 ('single_include/nlohmann/json.hpp','include/nlohmann/json.hpp','9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6'),
 ('LICENSE.MIT','LICENSE.MIT','86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14')]:
 url=f'https://raw.githubusercontent.com/nlohmann/json/{commit}/{source}'
 path=out/name
 if not path.exists():
  with urllib.request.urlopen(url,timeout=30) as stream:data=stream.read()
  assert hashlib.sha256(data).hexdigest()==expected
  path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
 assert hashlib.sha256(path.read_bytes()).hexdigest()==expected
 records[name]=dict(url=url,sha256=expected)
(out/'source-manifest.json').write_text(json.dumps(dict(repository='https://github.com/nlohmann/json',tag='v3.11.3',commit=commit,files=records),indent=2)+'\n')
print('WASM_SCALAR_APPLICATION_INPUTS_OK',flush=True)
