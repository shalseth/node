// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_EXECUTION_SPARC64_FRAME_CONSTANTS_SPARC64_H_
#define V8_EXECUTION_SPARC64_FRAME_CONSTANTS_SPARC64_H_

#include "src/base/bits.h"
#include "src/codegen/reglist.h"
#include "src/execution/frame-constants.h"

namespace v8::internal {

class EntryFrameConstants : public AllStatic {
 public:
  static constexpr int kNextExitFrameFPOffset = -3 * kSystemPointerSize;
  static constexpr int kNextFastCallFrameFPOffset = -4 * kSystemPointerSize;
  static constexpr int kNextFastCallFramePCOffset = -5 * kSystemPointerSize;
};

class WasmLiftoffSetupFrameConstants : public TypedFrameConstants {
 public:
  static constexpr int kNumberOfSavedGpParamRegs = 5;
  static constexpr int kNumberOfSavedFpParamRegs = 8;
  static constexpr int kNumberOfSavedAllParamRegs =
      kNumberOfSavedGpParamRegs + kNumberOfSavedFpParamRegs;

  static constexpr int kInstanceSpillOffset =
      TYPED_FRAME_PUSHED_VALUE_OFFSET(0);

  static constexpr int kParameterSpillsOffset[] = {
      TYPED_FRAME_PUSHED_VALUE_OFFSET(5), TYPED_FRAME_PUSHED_VALUE_OFFSET(4),
      TYPED_FRAME_PUSHED_VALUE_OFFSET(3), TYPED_FRAME_PUSHED_VALUE_OFFSET(2),
      TYPED_FRAME_PUSHED_VALUE_OFFSET(1)};

  static constexpr int kWasmInstanceDataOffset = 2 * kSystemPointerSize;
  static constexpr int kDeclaredFunctionIndexOffset = 1 * kSystemPointerSize;
  static constexpr int kNativeModuleOffset = 0;
};

class WasmLiftoffFrameConstants : public TypedFrameConstants {
 public:
  static constexpr int kFeedbackVectorOffset = 3 * kSystemPointerSize;
  static constexpr int kInstanceDataOffset = 2 * kSystemPointerSize;
};

class WasmDebugBreakFrameConstants : public TypedFrameConstants {
 public:
  static constexpr RegList kPushedGpRegs = {o0, o1, o2, o3, o4, o5, l0, l1, l2,
                                            l3, l4, l6, i0, i1, i2, i3, i4, i5};

  static constexpr DoubleRegList kPushedFpRegs = {
      d0, d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12, d13, d14};

  static constexpr int kNumPushedGpRegisters = kPushedGpRegs.Count();
  static constexpr int kNumPushedFpRegisters = kPushedFpRegs.Count();

  static constexpr int kLastPushedGpRegisterOffset =
      -kFixedFrameSizeFromFp - kNumPushedGpRegisters * kSystemPointerSize;
  static constexpr int kLastPushedFpRegisterOffset =
      kLastPushedGpRegisterOffset - kNumPushedFpRegisters * kSimd128Size;

  static int GetPushedGpRegisterOffset(int reg_code) {
    DCHECK_NE(0, kPushedGpRegs.bits() & (1 << reg_code));
    uint32_t lower_regs =
        kPushedGpRegs.bits() & ((uint32_t{1} << reg_code) - 1);
    return kLastPushedGpRegisterOffset +
           base::bits::CountPopulation(lower_regs) * kSystemPointerSize;
  }

  static int GetPushedFpRegisterOffset(int reg_code) {
    DCHECK_NE(0, kPushedFpRegs.bits() & (1 << reg_code));
    uint32_t lower_regs =
        kPushedFpRegs.bits() & ((uint32_t{1} << reg_code) - 1);
    return kLastPushedFpRegisterOffset +
           base::bits::CountPopulation(lower_regs) * kSimd128Size;
  }
};

}  // namespace v8::internal

#endif  // V8_EXECUTION_SPARC64_FRAME_CONSTANTS_SPARC64_H_
