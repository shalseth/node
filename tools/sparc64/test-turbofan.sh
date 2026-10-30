#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
ulimit -c 0
cd "$(dirname "$0")/../.."
timeout 15 out/Release/d8 --allow-natives-syntax --expose-gc \
  --no-concurrent-recompilation --trace-opt --trace-deopt \
  tools/sparc64/test-turbofan.js
exec timeout 15 out/Release/d8 --allow-natives-syntax --no-sparkplug \
  --no-concurrent-recompilation --trace-opt --trace-osr \
  tools/sparc64/test-turbofan-osr.js
