// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_INTERFACE_DESCRIPTORS_SPARC64_INL_H_
#define V8_CODEGEN_SPARC64_INTERFACE_DESCRIPTORS_SPARC64_INL_H_

#if V8_TARGET_ARCH_SPARC64

#include "src/codegen/interface-descriptors.h"
#include "src/execution/frames.h"

namespace v8 {
namespace internal {

constexpr auto CallInterfaceDescriptor::DefaultRegisterArray() {
  auto registers = RegisterArray(o0, o1, o2, o3, o4);
  static_assert(registers.size() == kMaxBuiltinRegisterParams);
  return registers;
}

constexpr auto CallInterfaceDescriptor::DefaultDoubleRegisterArray() {
  auto registers = DoubleRegisterArray(d0, d1, d2, d3, d4, d5, d6);
  return registers;
}

constexpr auto CallInterfaceDescriptor::DefaultReturnRegisterArray() {
  auto registers =
      RegisterArray(kReturnRegister0, kReturnRegister1, kReturnRegister2);
  return registers;
}

constexpr auto CallInterfaceDescriptor::DefaultReturnDoubleRegisterArray() {
  // Padding to have as many double return registers as GP return registers.
  auto registers = DoubleRegisterArray(kFPReturnRegister0, no_dreg, no_dreg);
  return registers;
}

#if DEBUG
template <typename DerivedDescriptor>
void StaticCallInterfaceDescriptor<DerivedDescriptor>::
    VerifyArgumentRegisterCount(CallInterfaceDescriptorData* data, int argc) {
  RegList allocatable_regs = data->allocatable_registers();
  if (argc >= 1) DCHECK(allocatable_regs.has(o0));
  if (argc >= 2) DCHECK(allocatable_regs.has(o1));
  if (argc >= 3) DCHECK(allocatable_regs.has(o2));
  if (argc >= 4) DCHECK(allocatable_regs.has(o3));
  if (argc >= 5) DCHECK(allocatable_regs.has(o4));
  if (argc >= 6) DCHECK(allocatable_regs.has(o5));
  if (argc >= 7) DCHECK(allocatable_regs.has(i0));
  if (argc >= 8) DCHECK(allocatable_regs.has(i1));
  // Additional arguments are passed on the stack.
}
#endif  // DEBUG

constexpr auto WriteBarrierDescriptor::registers() {
  return RegisterArray(o1, o5, o4, o2, o0, o3, kContextRegister);
}

constexpr Register LoadDescriptor::ReceiverRegister() { return o1; }
constexpr Register LoadDescriptor::NameRegister() { return o2; }
constexpr Register LoadDescriptor::SlotRegister() { return o0; }

constexpr Register LoadWithVectorDescriptor::VectorRegister() { return o3; }

constexpr Register KeyedLoadBaselineDescriptor::ReceiverRegister() {
  return o1;
}
constexpr Register KeyedLoadBaselineDescriptor::NameRegister() {
  return kInterpreterAccumulatorRegister;
}
constexpr Register KeyedLoadBaselineDescriptor::SlotRegister() { return o2; }

constexpr Register KeyedLoadWithVectorDescriptor::VectorRegister() {
  return o3;
}

constexpr Register EnumeratedKeyedLoadBaselineDescriptor::EnumIndexRegister() {
  return o4;
}

constexpr Register EnumeratedKeyedLoadBaselineDescriptor::CacheTypeRegister() {
  return o5;
}

constexpr Register EnumeratedKeyedLoadBaselineDescriptor::SlotRegister() {
  return o2;
}

constexpr Register KeyedHasICBaselineDescriptor::ReceiverRegister() {
  return kInterpreterAccumulatorRegister;
}
constexpr Register KeyedHasICBaselineDescriptor::NameRegister() { return o1; }
constexpr Register KeyedHasICBaselineDescriptor::SlotRegister() { return o2; }

constexpr Register KeyedHasICWithVectorDescriptor::VectorRegister() {
  return o3;
}

constexpr Register
LoadWithReceiverAndVectorDescriptor::LookupStartObjectRegister() {
  return o4;
}

constexpr Register StoreDescriptor::ReceiverRegister() { return o1; }
constexpr Register StoreDescriptor::NameRegister() { return o2; }
constexpr Register StoreDescriptor::ValueRegister() { return o0; }
constexpr Register StoreDescriptor::SlotRegister() { return o4; }

constexpr Register StoreWithVectorDescriptor::VectorRegister() { return o3; }

constexpr Register DefineKeyedOwnDescriptor::FlagsRegister() { return o5; }

constexpr Register StoreTransitionDescriptor::MapRegister() { return o5; }

constexpr Register CallApiGetterDescriptor::NameRegister() {
  return kCArgRegs[0];
}
constexpr Register CallApiGetterDescriptor::CallbackRegister() { return o3; }

constexpr Register GrowArrayElementsDescriptor::ObjectRegister() { return o0; }
constexpr Register GrowArrayElementsDescriptor::KeyRegister() { return o3; }

constexpr Register BaselineLeaveFrameDescriptor::ParamsSizeRegister() {
  return o3;
}
constexpr Register BaselineLeaveFrameDescriptor::WeightRegister() { return o4; }

constexpr Register TypeConversionDescriptor::ArgumentRegister() { return o0; }

constexpr auto TypeofDescriptor::registers() { return RegisterArray(o0); }

constexpr Register
MaglevOptimizeCodeOrTailCallOptimizedCodeSlotDescriptor::FlagsRegister() {
  return o2;
}
constexpr Register MaglevOptimizeCodeOrTailCallOptimizedCodeSlotDescriptor::
    FeedbackVectorRegister() {
  return o5;
}
constexpr Register
MaglevOptimizeCodeOrTailCallOptimizedCodeSlotDescriptor::TemporaryRegister() {
  return o4;
}

constexpr auto CallTrampolineDescriptor::registers() {
  // o0 : number of arguments
  // o1 : the target to call
  return RegisterArray(o1, o0);
}

constexpr auto CopyDataPropertiesWithExcludedPropertiesDescriptor::registers() {
  // o1 : the source
  // o0 : the excluded property count
  return RegisterArray(o1, o0);
}

constexpr auto
CopyDataPropertiesWithExcludedPropertiesOnStackDescriptor::registers() {
  // o1 : the source
  // o0 : the excluded property count
  // o2 : the excluded property base
  return RegisterArray(o1, o0, o2);
}

constexpr auto CallVarargsDescriptor::registers() {
  // o0 : number of arguments (on the stack)
  // o1 : the target to call
  // o4 : arguments list length (untagged)
  // o2 : arguments list (FixedArray)
  return RegisterArray(o1, o0, o4, o2);
}

constexpr auto CallForwardVarargsDescriptor::registers() {
  // o0 : number of arguments
  // o2 : start index (to support rest parameters)
  // o1 : the target to call
  return RegisterArray(o1, o0, o2);
}

constexpr auto CallFunctionTemplateDescriptor::registers() {
  // o1 : function template info
  // o2 : number of arguments (on the stack)
  return RegisterArray(o1, o2);
}

constexpr auto CallFunctionTemplateGenericDescriptor::registers() {
  // o1 : function template info
  // o2 : number of arguments (on the stack)
  // o3 : topmost script-having context
  return RegisterArray(o1, o2, o3);
}

constexpr auto CallWithSpreadDescriptor::registers() {
  // o0 : number of arguments (on the stack)
  // o1 : the target to call
  // o2 : the object to spread
  return RegisterArray(o1, o0, o2);
}

constexpr auto CallWithArrayLikeDescriptor::registers() {
  // o1 : the target to call
  // o2 : the arguments list
  return RegisterArray(o1, o2);
}

constexpr auto ConstructVarargsDescriptor::registers() {
  // o0 : number of arguments (on the stack)
  // o1 : the target to call
  // o3 : the new target
  // o4 : arguments list length (untagged)
  // o2 : arguments list (FixedArray)
  return RegisterArray(o1, o3, o0, o4, o2);
}

constexpr auto ConstructForwardVarargsDescriptor::registers() {
  // o0 : number of arguments
  // o3 : the new target
  // o2 : start index (to support rest parameters)
  // o1 : the target to call
  return RegisterArray(o1, o3, o0, o2);
}

constexpr auto ConstructWithSpreadDescriptor::registers() {
  // o0 : number of arguments (on the stack)
  // o1 : the target to call
  // o3 : the new target
  // o2 : the object to spread
  return RegisterArray(o1, o3, o0, o2);
}

constexpr auto ConstructWithArrayLikeDescriptor::registers() {
  // o1 : the target to call
  // o3 : the new target
  // o2 : the arguments list
  return RegisterArray(o1, o3, o2);
}

constexpr auto ConstructStubDescriptor::registers() {
  // o0 : number of arguments
  // o1 : the target to call
  // o3 : the new target
  return RegisterArray(o1, o3, o0);
}

constexpr auto AbortDescriptor::registers() { return RegisterArray(o1); }

constexpr auto CompareDescriptor::registers() { return RegisterArray(o1, o0); }

constexpr auto Compare_BaselineDescriptor::registers() {
  return RegisterArray(o1, o0, o2);
}

constexpr auto Compare_WithEmbeddedFeedbackOffsetDescriptor::registers() {
  return RegisterArray(o1, o0, o2);
}

constexpr auto BinaryOpDescriptor::registers() { return RegisterArray(o1, o0); }

constexpr auto BinaryOp_BaselineDescriptor::registers() {
  return RegisterArray(o1, o0, o2);
}

constexpr auto BinarySmiOp_BaselineDescriptor::registers() {
  return RegisterArray(o0, o1, o2);
}

constexpr Register
CallApiCallbackOptimizedDescriptor::ApiFunctionAddressRegister() {
  return o1;
}
constexpr Register
CallApiCallbackOptimizedDescriptor::ActualArgumentsCountRegister() {
  return o2;
}
constexpr Register
CallApiCallbackOptimizedDescriptor::FunctionTemplateInfoRegister() {
  return o3;
}

constexpr Register
CallApiCallbackGenericDescriptor::ActualArgumentsCountRegister() {
  return o2;
}
constexpr Register
CallApiCallbackGenericDescriptor::TopmostScriptHavingContextRegister() {
  return o1;
}
constexpr Register
CallApiCallbackGenericDescriptor::FunctionTemplateInfoRegister() {
  return o3;
}

constexpr auto InterpreterDispatchDescriptor::registers() {
  return RegisterArray(
      kInterpreterAccumulatorRegister, kInterpreterBytecodeOffsetRegister,
      kInterpreterBytecodeArrayRegister, kInterpreterDispatchTableRegister);
}

constexpr auto InterpreterPushArgsThenCallDescriptor::registers() {
  return RegisterArray(o0,   // argument count
                       o2,   // address of first argument
                       o1);  // the target callable to be call
}

constexpr auto InterpreterPushArgsThenConstructDescriptor::registers() {
  return RegisterArray(
      o0,   // argument count
      o4,   // address of the first argument
      o1,   // constructor to call
      o3,   // new target
      o2);  // allocation site feedback if available, undefined otherwise
}

constexpr auto ConstructForwardAllArgsDescriptor::registers() {
  return RegisterArray(o1,   // constructor to call
                       o3);  // new target
}

constexpr auto ResumeGeneratorDescriptor::registers() {
  return RegisterArray(o0,   // the value to pass to the generator
                       o1);  // the JSGeneratorObject to resume
}

constexpr auto RunMicrotasksEntryDescriptor::registers() {
  return RegisterArray(o0, o1);
}

constexpr auto WasmJSToWasmWrapperDescriptor::registers() {
  // Arbitrarily picked register.
  return RegisterArray(i2);
}
}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_SPARC64

#endif  // V8_CODEGEN_SPARC64_INTERFACE_DESCRIPTORS_SPARC64_INL_H_
