#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
cd "$(dirname "$0")/../.."
exec sh tools/sparc64/configure-turbofan.sh -Dv8_enable_verify_heap=1 "$@"
