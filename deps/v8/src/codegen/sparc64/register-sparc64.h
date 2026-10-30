// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_REGISTER_SPARC64_H_
#define V8_CODEGEN_SPARC64_REGISTER_SPARC64_H_

#include "src/codegen/register-base.h"

namespace v8::internal {

// clang-format off
#define GENERAL_REGISTERS(V)                                             \
  V(g0) V(g1) V(g2) V(g3) V(g4) V(g5) V(g6) V(g7)                       \
  V(o0) V(o1) V(o2) V(o3) V(o4) V(o5) V(o6) V(o7)                       \
  V(l0) V(l1) V(l2) V(l3) V(l4) V(l5) V(l6) V(l7)                       \
  V(i0) V(i1) V(i2) V(i3) V(i4) V(i5) V(i6) V(i7)

#define DOUBLE_REGISTERS(V)                                              \
  V(d0)  V(d1)  V(d2)  V(d3)  V(d4)  V(d5)  V(d6)  V(d7)               \
  V(d8)  V(d9)  V(d10) V(d11) V(d12) V(d13) V(d14) V(d15)              \
  V(d16) V(d17) V(d18) V(d19) V(d20) V(d21) V(d22) V(d23)              \
  V(d24) V(d25) V(d26) V(d27) V(d28) V(d29) V(d30) V(d31)

#define ALLOCATABLE_GENERAL_REGISTERS(V)                                  \
  V(o0) V(o1) V(o2) V(o3) V(o4) V(o5)                                   \
  V(l0) V(l1) V(l2) V(l3) V(l4) V(l6)                                   \
  V(i0) V(i1) V(i2) V(i3) V(i4) V(i5)

// Use one single-precision lane per double, within f0..f31.
#define ALLOCATABLE_DOUBLE_REGISTERS(V)                                   \
  V(d0) V(d1) V(d2) V(d3) V(d4) V(d5) V(d6) V(d7)                       \
  V(d8) V(d9) V(d10) V(d11) V(d12) V(d13) V(d14)

#define FLOAT_REGISTERS DOUBLE_REGISTERS
#define SIMD128_REGISTERS DOUBLE_REGISTERS
#define C_CALL_CALLEE_SAVE_REGISTERS                                      \
  l0, l1, l2, l3, l4, l5, l6, l7, i0, i1, i2, i3, i4, i5, i6, i7
#define C_CALL_CALLEE_SAVE_FP_REGISTERS
// clang-format on

enum RegisterCode {
#define REGISTER_CODE(R) kRegCode_##R,
  GENERAL_REGISTERS(REGISTER_CODE)
#undef REGISTER_CODE
      kRegAfterLast
};

class Register : public RegisterBase<Register, kRegAfterLast> {
 public:
  static constexpr int kMantissaOffset = 4;
  static constexpr int kExponentOffset = 0;

 private:
  friend class RegisterBase;
  explicit constexpr Register(int code) : RegisterBase(code) {}
};

#define DECLARE_REGISTER(R) \
  constexpr Register R = Register::from_code(kRegCode_##R);
GENERAL_REGISTERS(DECLARE_REGISTER)
#undef DECLARE_REGISTER

constexpr Register no_reg = Register::no_reg();
constexpr Register sp = o6;
constexpr Register fp = l5;
constexpr Register kNativeFramePointer = i6;
constexpr Register kDirectCEntryTargetRegister = g5;
constexpr Register kDirectCEntryArgumentCountRegister = g4;
constexpr Register kCArgRegs[] = {o0, o1, o2, o3, o4, o5};
constexpr int kRegisterPassedArguments = arraysize(kCArgRegs);

enum DoubleRegisterCode {
#define REGISTER_CODE(R) kDoubleCode_##R,
  DOUBLE_REGISTERS(REGISTER_CODE)
#undef REGISTER_CODE
      kDoubleAfterLast
};

class DoubleRegister : public RegisterBase<DoubleRegister, kDoubleAfterLast> {
 public:
  static constexpr int kSizeInBytes = 8;
  static constexpr int SupportedRegisterCount() { return kNumRegisters; }
  // V9 encodes f32..f62 in the odd values of a five-bit register field.
  constexpr int encoding() const {
    return ((code() & 15) << 1) | (code() >> 4);
  }
  int single_encoding() const {
    DCHECK_LT(code(), 16);
    return code() << 1;
  }

 private:
  friend class RegisterBase;
  explicit constexpr DoubleRegister(int code) : RegisterBase(code) {}
};

#define DECLARE_REGISTER(R) \
  constexpr DoubleRegister R = DoubleRegister::from_code(kDoubleCode_##R);
DOUBLE_REGISTERS(DECLARE_REGISTER)
#undef DECLARE_REGISTER

constexpr DoubleRegister no_dreg = DoubleRegister::no_reg();

using FloatRegister = DoubleRegister;
using Simd128Register = DoubleRegister;
constexpr AliasingKind kFPAliasing = AliasingKind::kOverlap;
constexpr bool kSimdMaskRegisters = false;
constexpr bool kPadArguments = false;
constexpr int ArgumentPaddingSlots(int argument_count) { return 0; }

constexpr Register kRootRegister = l7;
constexpr Register kContextRegister = l6;
constexpr Register kPtrComprCageBaseRegister = kRootRegister;
constexpr Register kStackPointerRegister = sp;
constexpr Register kReturnRegister0 = o0;
constexpr Register kReturnRegister1 = o1;
constexpr Register kReturnRegister2 = o2;
constexpr Register kJSFunctionRegister = o1;
constexpr Register kAllocateSizeRegister = o1;
constexpr Register kInterpreterAccumulatorRegister = o0;
constexpr Register kInterpreterBytecodeOffsetRegister = l0;
constexpr Register kInterpreterBytecodeArrayRegister = l1;
constexpr Register kInterpreterDispatchTableRegister = l2;
constexpr Register kJavaScriptCallArgCountRegister = o0;
constexpr Register kJavaScriptCallCodeStartRegister = o2;
constexpr Register kJavaScriptCallTargetRegister = kJSFunctionRegister;
constexpr Register kJavaScriptCallNewTargetRegister = o3;
constexpr Register kJavaScriptCallExtraArg1Register = o2;
constexpr Register kJavaScriptCallDispatchHandleRegister = no_reg;
constexpr Register kRuntimeCallFunctionRegister = o1;
constexpr Register kRuntimeCallArgCountRegister = o0;
constexpr Register kRuntimeCallArgvRegister = o2;
constexpr Register kWasmImplicitArgRegister = o4;
constexpr Register kWasmCompileLazyFuncIndexRegister = l0;
constexpr DoubleRegister kFPReturnRegister0 = d0;
constexpr DoubleRegister kScratchDoubleReg = d15;

DEFINE_REGISTER_NAMES(Register, GENERAL_REGISTERS)
DEFINE_REGISTER_NAMES(DoubleRegister, DOUBLE_REGISTERS)

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_REGISTER_SPARC64_H_
