// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// SAVE makes the caller's locals/inputs an older window; FLUSHW spills all
// older windows into their native save areas. The marker is an unbiased SP.
asm(".text\n"
    ".align 4\n"
    ".global PushAllRegistersAndIterateStack\n"
    ".hidden PushAllRegistersAndIterateStack\n"
    ".type PushAllRegistersAndIterateStack, #function\n"
    "PushAllRegistersAndIterateStack:\n"
    "  save %sp, -176, %sp\n"
    "  flushw\n"
    "  mov %i0, %o0\n"
    "  mov %i1, %o1\n"
    "  add %sp, 2047, %o2\n"
    "  jmpl %i2, %o7\n"
    "   nop\n"
    "  ret\n"
    "   restore\n"
    ".size PushAllRegistersAndIterateStack, "
    ".-PushAllRegistersAndIterateStack\n");
