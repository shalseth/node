// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_CONSTANTS_SPARC64_H_
#define V8_CODEGEN_SPARC64_CONSTANTS_SPARC64_H_

#include <stddef.h>
#include <stdint.h>

namespace v8::internal {

using Instr = uint32_t;
constexpr int kInstrSize = 4;
constexpr int kInstrSizeLog2 = 2;
constexpr int kSparcStackBias = 2047;
constexpr int kSparcStackAlignment = 16;
constexpr int kSparcWindowSaveSize = 128;
constexpr int kSparcArgumentSaveSize = 48;
constexpr int kSparcMinimumFrameSize =
    kSparcWindowSaveSize + kSparcArgumentSaveSize;
constexpr int kSparcReturnAddressOffset = 8;
// Logical V8 stack slots stay above the native window and argument areas.
constexpr int kSparcStackSlotBias = kSparcStackBias + kSparcMinimumFrameSize;
constexpr int kRootRegisterBias = 128;
constexpr size_t kMaxPCRelativeCodeRangeInMB = 0;

enum Condition : int {
  never = 0,
  equal = 1,
  less_equal = 2,
  less = 3,
  less_equal_unsigned = 4,
  less_unsigned = 5,
  negative = 6,
  overflow = 7,
  always = 8,
  not_equal = 9,
  greater = 10,
  greater_equal = 11,
  greater_unsigned = 12,
  greater_equal_unsigned = 13,
  positive = 14,
  no_overflow = 15,
  kZero = equal,
  kNotZero = not_equal,
  kEqual = equal,
  kNotEqual = not_equal,
  kLessThan = less,
  kLessThanEqual = less_equal,
  kGreaterThan = greater,
  kGreaterThanEqual = greater_equal,
  kUnsignedLessThan = less_unsigned,
  kUnsignedLessThanEqual = less_equal_unsigned,
  kUnsignedGreaterThan = greater_unsigned,
  kUnsignedGreaterThanEqual = greater_equal_unsigned,

};

enum class ConditionCodes { kIcc = 0, kXcc = 2 };

enum FloatCondition : int {
  kFNever = 0,
  kFNotEqual = 1,
  kFLessOrGreater = 2,
  kFUnorderedOrLess = 3,
  kFLess = 4,
  kFUnorderedOrGreater = 5,
  kFGreater = 6,
  kFUnordered = 7,
  kFAlways = 8,
  kFEqual = 9,
  kFUnorderedOrEqual = 10,
  kFGreaterOrEqual = 11,
  kFUnorderedOrGreaterOrEqual = 12,
  kFLessOrEqual = 13,
  kFUnorderedOrLessOrEqual = 14,
  kFOrdered = 15,
};

enum MembarMask {
  kLoadLoad = 1,
  kStoreLoad = 2,
  kLoadStore = 4,
  kStoreStore = 8,
  kSync = 64,
};

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_CONSTANTS_SPARC64_H_
