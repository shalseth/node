#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
cd "$(dirname "$0")/../.."
exec timeout 30 out/Release/d8 --allow-natives-syntax --expose-gc \
  --sparkplug --no-always-sparkplug --compact-on-every-full-gc \
  deps/v8/test/mjsunit/mjsunit.js tools/sparc64/test-sparkplug.js
