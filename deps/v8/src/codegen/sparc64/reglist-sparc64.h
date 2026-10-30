// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_REGLIST_SPARC64_H_
#define V8_CODEGEN_SPARC64_REGLIST_SPARC64_H_

#include "src/codegen/register-arch.h"
#include "src/codegen/reglist-base.h"

namespace v8::internal {

using RegList = RegListBase<Register>;
using DoubleRegList = RegListBase<DoubleRegister>;
ASSERT_TRIVIALLY_COPYABLE(RegList);
ASSERT_TRIVIALLY_COPYABLE(DoubleRegList);

#define LIST_REG(R) R,
constexpr RegList kJSCallerSaved = {ALLOCATABLE_GENERAL_REGISTERS(LIST_REG)};
#undef LIST_REG
constexpr int kNumJSCallerSaved = kJSCallerSaved.Count();
constexpr RegList kCalleeSaved = {C_CALL_CALLEE_SAVE_REGISTERS};
constexpr int kNumCalleeSaved = kCalleeSaved.Count();

#define LIST_REG(R) R,
constexpr DoubleRegList kCallerSavedDoubles = {DOUBLE_REGISTERS(LIST_REG)};
#undef LIST_REG
constexpr int kNumCallerSavedDoubles = kCallerSavedDoubles.Count();
constexpr DoubleRegList kCalleeSavedDoubles = {};
constexpr int kNumCalleeSavedDoubles = 0;

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_REGLIST_SPARC64_H_
