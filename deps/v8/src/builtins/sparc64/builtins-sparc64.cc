// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api/api-arguments.h"
#include "src/builtins/builtins-inl.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/register-configuration.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/isolate-data.h"
#include "src/heap/heap-inl.h"
#include "src/interpreter/bytecodes.h"
#include "src/objects/api-callbacks.h"
#include "src/objects/bytecode-array.h"
#include "src/objects/contexts.h"
#include "src/objects/feedback-vector.h"
#include "src/objects/fixed-array.h"
#include "src/objects/function-kind.h"
#include "src/objects/js-function.h"
#include "src/objects/js-generator.h"
#include "src/objects/map.h"
#include "src/objects/shared-function-info.h"
#include "src/objects/templates.h"
#include "src/runtime/runtime.h"

#if V8_ENABLE_WEBASSEMBLY
#include "src/wasm/baseline/liftoff-assembler-defs.h"
#include "src/wasm/stacks.h"
#include "src/wasm/wasm-linkage.h"
#include "src/wasm/wasm-objects.h"
#endif

namespace v8::internal {

namespace {

#if V8_ENABLE_WEBASSEMBLY
constexpr int kJspiNativeFpOffset =
    WasmJspiFrameConstants::kLastSpillOffset - kSystemPointerSize;
#endif

void GenerateJSEntryVariant(MacroAssembler* masm, StackFrame::Type type,
                            Builtin trampoline) {
  Label invoke, handler, exit;
  masm->EnterJSEntryFrame(type);
  masm->Jump(&invoke);
  masm->BindExceptionHandler(&handler);
  masm->isolate()->builtins()->SetJSEntryHandlerOffset(handler.pos());
  masm->StoreU64(o0, masm->AsMemOperand(IsolateFieldId::kException));
  masm->LoadRoot(o0, RootIndex::kException);
  masm->Jump(&exit);

  masm->bind(&invoke);
  masm->PushStackHandler();
  for (int n = 0; n < kRegisterPassedArguments; ++n) {
    masm->mov(kCArgRegs[n], Register::from_code(i0.code() + n));
  }
  masm->CallBuiltin(trampoline);
  masm->PopStackHandler();
  masm->bind(&exit);
  masm->LeaveJSEntryFrame();
}

}  // namespace

void Builtins::Generate_Adaptor(MacroAssembler* masm,
                                int formal_parameter_count, Address address) {
  masm->Move(kJavaScriptCallExtraArg1Register,
             ExternalReference::Create(address));
  masm->TailCallBuiltin(AdaptorWithBuiltinExitFrame(formal_parameter_count));
}

void Builtins::Generate_JSEntry(MacroAssembler* masm) {
  GenerateJSEntryVariant(masm, StackFrame::ENTRY, Builtin::kJSEntryTrampoline);
}

void Builtins::Generate_JSConstructEntry(MacroAssembler* masm) {
  GenerateJSEntryVariant(masm, StackFrame::CONSTRUCT_ENTRY,
                         Builtin::kJSConstructEntryTrampoline);
}

void Builtins::Generate_JSRunMicrotasksEntry(MacroAssembler* masm) {
  GenerateJSEntryVariant(masm, StackFrame::ENTRY,
                         Builtin::kRunMicrotasksTrampoline);
}

void Builtins::Generate_RunMicrotasksTrampoline(MacroAssembler* masm) {
  masm->Move(RunMicrotasksDescriptor::MicrotaskQueueRegister(), o1);
  masm->TailCallBuiltin(Builtin::kRunMicrotasks);
}

namespace {

void GenerateJSEntryTrampoline(MacroAssembler* masm, bool construct) {
  Label overflow;
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->LoadU64(kContextRegister,
                  masm->AsMemOperand(IsolateFieldId::kContext));
#ifdef DEBUG
    masm->StoreU64(g0, masm->AsMemOperand(IsolateFieldId::kContext));
#endif
    masm->Push(o2);
    masm->StackOverflowCheck(o4, g3, &overflow);
    masm->PushArguments(o5, o4, g4, ArgumentsElementType::kHandle);
    masm->Push(o3);
    masm->Move(o0, o4);
    masm->Move(o3, o1);
    masm->Move(o1, o2);
    masm->LoadRoot(g1, RootIndex::kUndefinedValue);
    for (Register reg : kJSCallerSaved) {
      if (reg != o0 && reg != o1 && reg != o3 && reg != kContextRegister) {
        masm->Move(reg, g1);
      }
    }
    masm->CallBuiltin(construct ? Builtin::kConstruct : Builtins::Call());
  }
  masm->Ret();
  masm->bind(&overflow);
  {
    FrameScope frame(masm, StackFrame::MANUAL);
    masm->CallRuntime(Runtime::kThrowStackOverflow);
    masm->Trap();
  }
}

}  // namespace

void Builtins::Generate_JSEntryTrampoline(MacroAssembler* masm) {
  GenerateJSEntryTrampoline(masm, false);
}

void Builtins::Generate_JSConstructEntryTrampoline(MacroAssembler* masm) {
  GenerateJSEntryTrampoline(masm, true);
}

namespace {

void GenerateCall(MacroAssembler* masm, ConvertReceiverMode mode,
                  std::optional<RootIndex> error_string_root = std::nullopt) {
  Label non_callable, class_constructor, callable_function, bound_function;
  Label proxy, wrapped;
  masm->andcc(g0, o1, kSmiTagMask);
  masm->b(equal, ConditionCodes::kXcc, &non_callable);
  masm->nop();
  masm->LoadU64(o4, FieldMemOperand(o1, HeapObject::kMapOffset));
  masm->LoadU16(o5, FieldMemOperand(o4, Map::kInstanceTypeOffset));
  masm->SubS64(g1, o5, FIRST_CALLABLE_JS_FUNCTION_TYPE);
  masm->cmp(g1,
            LAST_CALLABLE_JS_FUNCTION_TYPE - FIRST_CALLABLE_JS_FUNCTION_TYPE);
  masm->b(less_equal_unsigned, ConditionCodes::kXcc, &callable_function);
  masm->nop();
  masm->JumpIfEqual(o5, JS_BOUND_FUNCTION_TYPE, &bound_function);
  masm->LoadU8(o4, FieldMemOperand(o4, Map::kBitFieldOffset));
  masm->andcc(g0, o4, Map::Bits1::IsCallableBit::kMask);
  masm->b(equal, ConditionCodes::kXcc, &non_callable);
  masm->nop();
  masm->JumpIfEqual(o5, JS_PROXY_TYPE, &proxy);
  masm->JumpIfEqual(o5, JS_WRAPPED_FUNCTION_TYPE, &wrapped);
  masm->JumpIfEqual(o5, JS_CLASS_CONSTRUCTOR_TYPE, &class_constructor);
  masm->StoreU64(o1, MemOperand(sp));
  masm->LoadU64(o1, FieldMemOperand(kContextRegister, HeapObject::kMapOffset));
  masm->LoadU64(
      o1,
      FieldMemOperand(o1, Map::kConstructorOrBackPointerOrNativeContextOffset));
  masm->LoadU64(o1,
                MemOperand(o1, Context::SlotOffset(
                                   Context::CALL_AS_FUNCTION_DELEGATE_INDEX)));
  masm->TailCallBuiltin(
      Builtins::CallFunction(ConvertReceiverMode::kNotNullOrUndefined));
  masm->bind(&callable_function);
  masm->TailCallBuiltin(Builtins::CallFunction(mode));
  masm->bind(&bound_function);
  masm->TailCallBuiltin(Builtin::kCallBoundFunction);
  masm->bind(&proxy);
  masm->TailCallBuiltin(Builtin::kCallProxy);
  masm->bind(&wrapped);
  masm->TailCallBuiltin(Builtin::kCallWrappedFunction);
  masm->bind(&non_callable);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->Push(o1);
    if (error_string_root.has_value()) {
      masm->LoadRoot(o2, *error_string_root);
      masm->Push(o2);
      masm->CallRuntime(Runtime::kThrowTargetNonFunction);
    } else {
      masm->CallRuntime(Runtime::kThrowCalledNonCallable);
    }
    masm->Trap();
  }
  masm->bind(&class_constructor);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->Push(o1);
    masm->CallRuntime(Runtime::kThrowConstructorNonCallableError);
    masm->Trap();
  }
}

void DropArgumentsAndPushReceiver(MacroAssembler* masm, Register receiver) {
  masm->Drop(o0);
  masm->Push(receiver);
}

}  // namespace

void Builtins::Generate_Call(MacroAssembler* masm, ConvertReceiverMode mode) {
  GenerateCall(masm, mode);
}

void Builtins::Generate_FunctionPrototypeCall(MacroAssembler* masm) {
  masm->Pop(o1);
  Label has_receiver;
  masm->cmp(o0, JSParameterCount(0));
  masm->b(not_equal, ConditionCodes::kXcc, &has_receiver);
  masm->nop();
  masm->LoadRoot(o4, RootIndex::kUndefinedValue);
  masm->Push(o4);
  masm->add(o0, o0, 1);
  masm->bind(&has_receiver);
  masm->sub(o0, o0, 1);
  GenerateCall(masm, ConvertReceiverMode::kAny,
               RootIndex::kFunction_prototype_call_string);
}

void Builtins::Generate_FunctionPrototypeApply(MacroAssembler* masm) {
  masm->LoadRoot(o4, RootIndex::kUndefinedValue);
  masm->Move(o2, o4);
  masm->LoadU64(o1, MemOperand(sp));
  Label loaded, no_arguments;
  masm->JumpIfLessThan(o0, JSParameterCount(1), &loaded);
  masm->LoadU64(o4, MemOperand(sp, kSystemPointerSize));
  masm->JumpIfLessThan(o0, JSParameterCount(2), &loaded);
  masm->LoadU64(o2, MemOperand(sp, 2 * kSystemPointerSize));
  masm->bind(&loaded);
  DropArgumentsAndPushReceiver(masm, o4);
  for (RootIndex root : {RootIndex::kNullValue, RootIndex::kUndefinedValue}) {
    masm->LoadRoot(g1, root);
    masm->cmp(o2, g1);
    masm->b(equal, ConditionCodes::kXcc, &no_arguments);
    masm->nop();
  }
  masm->TailCallBuiltin(Builtin::kCallWithArrayLike);
  masm->bind(&no_arguments);
  masm->Move(o0, JSParameterCount(0));
  GenerateCall(masm, ConvertReceiverMode::kAny,
               RootIndex::kFunction_prototype_apply_string);
}

void Builtins::Generate_ReflectApply(MacroAssembler* masm) {
  masm->LoadRoot(o1, RootIndex::kUndefinedValue);
  masm->Move(o4, o1);
  masm->Move(o2, o1);
  Label loaded;
  masm->JumpIfLessThan(o0, JSParameterCount(1), &loaded);
  masm->LoadU64(o1, MemOperand(sp, kSystemPointerSize));
  masm->JumpIfLessThan(o0, JSParameterCount(2), &loaded);
  masm->LoadU64(o4, MemOperand(sp, 2 * kSystemPointerSize));
  masm->JumpIfLessThan(o0, JSParameterCount(3), &loaded);
  masm->LoadU64(o2, MemOperand(sp, 3 * kSystemPointerSize));
  masm->bind(&loaded);
  DropArgumentsAndPushReceiver(masm, o4);
  masm->TailCallBuiltin(Builtin::kCallWithArrayLike);
}

void Builtins::Generate_ReflectConstruct(MacroAssembler* masm) {
  masm->LoadRoot(o1, RootIndex::kUndefinedValue);
  masm->Move(o2, o1);
  masm->Move(o3, o1);
  masm->Move(o4, o1);
  Label loaded;
  masm->JumpIfLessThan(o0, JSParameterCount(1), &loaded);
  masm->LoadU64(o1, MemOperand(sp, kSystemPointerSize));
  masm->Move(o3, o1);
  masm->JumpIfLessThan(o0, JSParameterCount(2), &loaded);
  masm->LoadU64(o2, MemOperand(sp, 2 * kSystemPointerSize));
  masm->JumpIfLessThan(o0, JSParameterCount(3), &loaded);
  masm->LoadU64(o3, MemOperand(sp, 3 * kSystemPointerSize));
  masm->bind(&loaded);
  DropArgumentsAndPushReceiver(masm, o4);
  masm->TailCallBuiltin(Builtin::kConstructWithArrayLike);
}

void Builtins::Generate_Construct(MacroAssembler* masm) {
  Label non_constructor, function, bound, proxy;
  masm->andcc(g0, o1, kSmiTagMask);
  masm->b(equal, ConditionCodes::kXcc, &non_constructor);
  masm->nop();
  masm->LoadU64(o4, FieldMemOperand(o1, HeapObject::kMapOffset));
  masm->LoadU8(o2, FieldMemOperand(o4, Map::kBitFieldOffset));
  masm->andcc(g0, o2, Map::Bits1::IsConstructorBit::kMask);
  masm->b(equal, ConditionCodes::kXcc, &non_constructor);
  masm->nop();
  masm->LoadU16(o5, FieldMemOperand(o4, Map::kInstanceTypeOffset));
  masm->SubS64(g1, o5, FIRST_JS_FUNCTION_TYPE);
  masm->cmp(g1, LAST_JS_FUNCTION_TYPE - FIRST_JS_FUNCTION_TYPE);
  masm->b(less_equal_unsigned, ConditionCodes::kXcc, &function);
  masm->nop();
  masm->JumpIfEqual(o5, JS_BOUND_FUNCTION_TYPE, &bound);
  masm->JumpIfEqual(o5, JS_PROXY_TYPE, &proxy);
  masm->StoreU64(o1, MemOperand(sp));
  masm->LoadU64(o1, FieldMemOperand(kContextRegister, HeapObject::kMapOffset));
  masm->LoadU64(
      o1,
      FieldMemOperand(o1, Map::kConstructorOrBackPointerOrNativeContextOffset));
  masm->LoadU64(
      o1, MemOperand(o1, Context::SlotOffset(
                             Context::CALL_AS_CONSTRUCTOR_DELEGATE_INDEX)));
  masm->TailCallBuiltin(Builtins::CallFunction());
  masm->bind(&function);
  masm->TailCallBuiltin(Builtin::kConstructFunction);
  masm->bind(&bound);
  masm->TailCallBuiltin(Builtin::kConstructBoundFunction);
  masm->bind(&proxy);
  masm->TailCallBuiltin(Builtin::kConstructProxy);
  masm->bind(&non_constructor);
  masm->TailCallBuiltin(Builtin::kConstructedNonConstructable);
}

void Builtins::Generate_ConstructFunction(MacroAssembler* masm) {
  masm->LoadRoot(o2, RootIndex::kUndefinedValue);
  masm->LoadU64(o4, FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU32(o4, FieldMemOperand(o4, SharedFunctionInfo::kFlagsOffset));
  masm->Move(g1, SharedFunctionInfo::ConstructAsBuiltinBit::kMask);
  masm->andcc(g0, o4, g1);
  Label generic;
  masm->b(equal, ConditionCodes::kXcc, &generic);
  masm->nop();
  masm->TailCallBuiltin(Builtin::kJSBuiltinsConstructStub);
  masm->bind(&generic);
  masm->TailCallBuiltin(Builtin::kJSConstructStubGeneric);
}

void Builtins::Generate_ConstructedNonConstructable(MacroAssembler* masm) {
  FrameScope frame(masm, StackFrame::INTERNAL);
  masm->Push(o1);
  masm->CallRuntime(Runtime::kThrowConstructedNonConstructable);
  masm->Trap();
}

void Builtins::Generate_CallFunction(MacroAssembler* masm,
                                     ConvertReceiverMode mode) {
  masm->LoadU64(o2, FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU64(kContextRegister,
                FieldMemOperand(o1, JSFunction::kContextOffset));
  masm->LoadU32(o3, FieldMemOperand(o2, SharedFunctionInfo::kFlagsOffset));
  masm->Move(g1, SharedFunctionInfo::IsStrictBit::kMask |
                     SharedFunctionInfo::IsNativeBit::kMask);
  masm->andcc(g0, o3, g1);
  Label converted, global_proxy, store_receiver, to_object;
  masm->b(not_equal, ConditionCodes::kXcc, &converted);
  masm->nop();
  if (mode != ConvertReceiverMode::kNullOrUndefined) {
    masm->LoadU64(o3, MemOperand(sp));
    masm->andcc(g0, o3, kSmiTagMask);
    masm->b(equal, ConditionCodes::kXcc, &to_object);
    masm->nop();
    masm->LoadU64(o4, FieldMemOperand(o3, HeapObject::kMapOffset));
    masm->LoadU16(o4, FieldMemOperand(o4, Map::kInstanceTypeOffset));
    static_assert(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
    masm->cmp(o4, FIRST_JS_RECEIVER_TYPE);
    masm->b(greater_equal, ConditionCodes::kXcc, &converted);
    masm->nop();
    if (mode != ConvertReceiverMode::kNotNullOrUndefined) {
      for (RootIndex root :
           {RootIndex::kUndefinedValue, RootIndex::kNullValue}) {
        masm->LoadRoot(g1, root);
        masm->cmp(o3, g1);
        masm->b(equal, ConditionCodes::kXcc, &global_proxy);
        masm->nop();
      }
    }
    masm->bind(&to_object);
    {
      FrameScope frame(masm, StackFrame::INTERNAL);
      masm->sllx(o0, o0, kSmiShiftSize + kSmiTagSize);
      masm->Push(o0, o1, kContextRegister);
      masm->Move(o0, o3);
      masm->CallBuiltin(Builtin::kToObject);
      masm->Move(o3, o0);
      masm->Pop(kContextRegister, o1, o0);
      masm->srax(o0, o0, kSmiShiftSize + kSmiTagSize);
    }
    masm->LoadU64(o2,
                  FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
    masm->Jump(&store_receiver);
  }
  masm->bind(&global_proxy);
  masm->LoadU64(o3, FieldMemOperand(kContextRegister, HeapObject::kMapOffset));
  masm->LoadU64(
      o3,
      FieldMemOperand(o3, Map::kConstructorOrBackPointerOrNativeContextOffset));
  masm->LoadU64(
      o3, MemOperand(o3, Context::SlotOffset(Context::GLOBAL_PROXY_INDEX)));
  masm->bind(&store_receiver);
  masm->StoreU64(o3, MemOperand(sp));
  masm->bind(&converted);
  masm->LoadU16(
      o2, FieldMemOperand(o2, SharedFunctionInfo::kFormalParameterCountOffset));
  masm->InvokeFunctionCode(o1, no_reg, o2, o0, InvokeType::kJump);
}

void Builtins::Generate_RestartFrameTrampoline(MacroAssembler* masm) {
  masm->LoadU64(o1, MemOperand(fp, StandardFrameConstants::kFunctionOffset));
  masm->LoadU64(o0, MemOperand(fp, StandardFrameConstants::kArgCOffset));
  masm->LeaveFrame(StackFrame::INTERPRETED);
  masm->Move(o2, kDontAdaptArgumentsSentinel);
  masm->LoadU64(kContextRegister,
                FieldMemOperand(o1, JSFunction::kContextOffset));
  masm->InvokeFunctionCode(o1, no_reg, o2, o0, InvokeType::kJump);
}

void Builtins::Generate_NotifyDeoptimized(MacroAssembler* masm) {
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->CallRuntime(Runtime::kNotifyDeoptimized);
  }
  masm->Pop(kInterpreterAccumulatorRegister);
  masm->Ret();
}

void Builtins::Generate_InterpreterEntryTrampoline(
    MacroAssembler* masm, InterpreterEntryTrampolineMode mode) {
  const Register array = kInterpreterBytecodeArrayRegister;
  const Register offset = kInterpreterBytecodeOffsetRegister;
  Label bytecode_ready, compile_lazy, feedback_ready, stack_overflow;
  Label is_baseline;
  masm->LoadU64(o4, FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
  masm->StoreU16(g0, FieldMemOperand(o4, SharedFunctionInfo::kAgeOffset));
  masm->LoadU64(array, FieldMemOperand(
                           o4, SharedFunctionInfo::kTrustedFunctionDataOffset));
  masm->LoadU64(o4, FieldMemOperand(array, HeapObject::kMapOffset));
  masm->LoadU16(o4, FieldMemOperand(o4, Map::kInstanceTypeOffset));
  masm->JumpIfEqual(o4, CODE_TYPE, &is_baseline);
  masm->JumpIfEqual(o4, BYTECODE_ARRAY_TYPE, &bytecode_ready);
  masm->cmp(o4, INTERPRETER_DATA_TYPE);
  masm->b(not_equal, ConditionCodes::kXcc, &compile_lazy);
  masm->nop();
  masm->LoadInterpreterDataBytecodeArray(array, array);
  masm->bind(&bytecode_ready);
  masm->LoadU64(o2, FieldMemOperand(o1, JSFunction::kFeedbackCellOffset));
  masm->LoadU64(o2, FieldMemOperand(o2, FeedbackCell::kValueOffset));
  masm->LoadU64(o4, FieldMemOperand(o2, HeapObject::kMapOffset));
  masm->LoadU16(o4, FieldMemOperand(o4, Map::kInstanceTypeOffset));
  Label no_feedback;
  masm->cmp(o4, FEEDBACK_VECTOR_TYPE);
  masm->b(not_equal, ConditionCodes::kXcc, &no_feedback);
  masm->nop();
#ifndef V8_JITLESS
  masm->LoadU8(g1, FieldMemOperand(o2, FeedbackVector::kOsrStateOffset));
  masm->and_(g1, g1, ~FeedbackVector::OsrUrgencyBits::kMask);
  masm->StoreU8(g1, FieldMemOperand(o2, FeedbackVector::kOsrStateOffset));
  masm->LoadU32(g1,
                FieldMemOperand(o2, FeedbackVector::kInvocationCountOffset));
  masm->add(g1, g1, 1);
  masm->StoreU32(g1,
                 FieldMemOperand(o2, FeedbackVector::kInvocationCountOffset));
#endif
  masm->Jump(&feedback_ready);
  masm->bind(&no_feedback);
  masm->LoadRoot(o2, RootIndex::kUndefinedValue);
  masm->bind(&feedback_ready);

  FrameScope frame(masm, StackFrame::MANUAL);
  masm->PushStandardFrame(o1);
  masm->Move(offset, BytecodeArray::kHeaderSize - kHeapObjectTag);
  masm->sllx(o4, offset, kSmiShiftSize + kSmiTagSize);
  masm->Push(array, o4, o2);
  masm->LoadU32(o4, FieldMemOperand(array, BytecodeArray::kFrameSizeOffset));
  masm->srlx(o4, o4, kSystemPointerSizeLog2);
  masm->StackOverflowCheck(o4, g3, &stack_overflow);
  masm->LoadRoot(kInterpreterAccumulatorRegister, RootIndex::kUndefinedValue);
  Label initialized, initialize;
  masm->cmp(o4, 0);
  masm->b(equal, ConditionCodes::kXcc, &initialized);
  masm->nop();
  masm->bind(&initialize);
  masm->Push(kInterpreterAccumulatorRegister);
  masm->sub(o4, o4, 1);
  masm->cmp(o4, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &initialize);
  masm->nop();
  masm->bind(&initialized);
  masm->LoadS32(
      o4,
      FieldMemOperand(
          array, BytecodeArray::kIncomingNewTargetOrGeneratorRegisterOffset));
  Label no_new_target;
  masm->cmp(o4, 0);
  masm->b(equal, ConditionCodes::kXcc, &no_new_target);
  masm->nop();
  masm->sllx(o4, o4, kSystemPointerSizeLog2);
  masm->add(o4, fp, o4);
  masm->StoreU64(o3, MemOperand(o4));
  masm->bind(&no_new_target);

  Label interrupt, dispatch;
  masm->LoadU64(g2, MemOperand(kRootRegister, IsolateData::jslimit_offset()));
  masm->add(g1, sp, kSparcStackBias);
  masm->cmp(g1, g2);
  masm->b(less_unsigned, ConditionCodes::kXcc, &interrupt);
  masm->nop();
  masm->bind(&dispatch);
  masm->Move(
      kInterpreterDispatchTableRegister,
      ExternalReference::interpreter_dispatch_table_address(masm->isolate()));
  masm->add(g1, array, offset);
  masm->LoadU8(o3, MemOperand(g1));
  masm->sllx(o3, o3, kSystemPointerSizeLog2);
  masm->add(g1, kInterpreterDispatchTableRegister, o3);
  masm->LoadU64(o2, MemOperand(g1));
  masm->Call(o2);
  if (mode == InterpreterEntryTrampolineMode::kDefault) {
    masm->isolate()->heap()->SetInterpreterEntryReturnPCOffset(
        masm->pc_offset());
  } else {
    CHECK_EQ(mode, InterpreterEntryTrampolineMode::kForProfiling);
    CHECK_EQ(
        masm->isolate()->heap()->interpreter_entry_return_pc_offset().value(),
        masm->pc_offset());
  }
  masm->LoadU64(
      array, MemOperand(fp, InterpreterFrameConstants::kBytecodeArrayFromFp));
  masm->LoadU64(
      offset, MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  masm->srax(offset, offset, kSmiShiftSize + kSmiTagSize);
  masm->add(g1, array, offset);
  masm->LoadU8(o1, MemOperand(g1));
  masm->Move(o2, ExternalReference::bytecode_size_table_address());
  Label return_value;
  masm->AdvanceBytecodeOffsetOrReturn(array, offset, o1, o2, o3, &return_value);
  masm->Jump(&dispatch);
  masm->bind(&return_value);
  masm->LoadU16(o2,
                FieldMemOperand(array, BytecodeArray::kParameterSizeOffset));
  masm->LoadU64(o3, MemOperand(fp, StandardFrameConstants::kArgCOffset));
  Label drop_args;
  masm->cmp(o2, o3);
  masm->b(greater_equal, ConditionCodes::kXcc, &drop_args);
  masm->nop();
  masm->mov(o2, o3);
  masm->bind(&drop_args);
  masm->LeaveFrame(StackFrame::INTERPRETED);
  masm->Drop(o2);
  masm->Ret();

  masm->bind(&interrupt);
  masm->Move(g1, Smi::FromInt(BytecodeArray::kHeaderSize - kHeapObjectTag +
                              kFunctionEntryBytecodeOffset));
  masm->StoreU64(
      g1, MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  masm->CallRuntime(Runtime::kStackGuard);
  masm->LoadU64(
      array, MemOperand(fp, InterpreterFrameConstants::kBytecodeArrayFromFp));
  masm->Move(offset, BytecodeArray::kHeaderSize - kHeapObjectTag);
  masm->LoadRoot(kInterpreterAccumulatorRegister, RootIndex::kUndefinedValue);
  masm->sllx(g1, offset, kSmiShiftSize + kSmiTagSize);
  masm->StoreU64(
      g1, MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  masm->Jump(&dispatch);

  masm->bind(&compile_lazy);
  {
    FrameScope lazy_frame(masm, StackFrame::INTERNAL);
    masm->sllx(o0, o0, kSmiShiftSize + kSmiTagSize);
    masm->Push(o1, o3, o0, kContextRegister, o1);
    masm->CallRuntime(Runtime::kCompileLazy);
    masm->Pop(kContextRegister, o0, o3, o1);
    masm->srax(o0, o0, kSmiShiftSize + kSmiTagSize);
  }
  masm->JumpJSFunction(o1);
  masm->bind(&stack_overflow);
  masm->CallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();

  masm->bind(&is_baseline);
  {
    FrameScope install_frame(masm, StackFrame::INTERNAL);
    masm->SmiTag(o0);
    masm->Push(o0, o1, o3, o1);
    masm->CallRuntime(Runtime::kInstallBaselineCode, 1);
    masm->Pop(o3, o1, o0);
    masm->SmiUntag(o0);
  }
  masm->JumpJSFunction(o1);
}

namespace {

void PushInterpreterArguments(MacroAssembler* masm, Register count,
                              Register first) {
  masm->sub(g3, count, 1);
  masm->sllx(g3, g3, kSystemPointerSizeLog2);
  masm->sub(first, first, g3);
  Label done, loop;
  masm->cmp(count, 0);
  masm->b(less_equal, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->mov(g3, first);
  masm->mov(g2, count);
  masm->bind(&loop);
  masm->LoadU64(g4, MemOperand(g3));
  masm->Push(g4);
  masm->add(g3, g3, kSystemPointerSize);
  masm->sub(g2, g2, 1);
  masm->cmp(g2, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &loop);
  masm->nop();
  masm->bind(&done);
}

}  // namespace

void Builtins::Generate_InterpreterPushArgsThenCallImpl(
    MacroAssembler* masm, ConvertReceiverMode receiver_mode,
    InterpreterPushArgsMode mode) {
  CHECK_NE(mode, InterpreterPushArgsMode::kArrayFunction);
  if (mode == InterpreterPushArgsMode::kWithFinalSpread) masm->sub(o0, o0, 1);
  masm->SubS64(o3, o0,
               receiver_mode == ConvertReceiverMode::kNullOrUndefined
                   ? kJSArgcReceiverSlots
                   : 0);
  Label overflow;
  masm->StackOverflowCheck(o3, g3, &overflow);
  PushInterpreterArguments(masm, o3, o2);
  if (receiver_mode == ConvertReceiverMode::kNullOrUndefined) {
    masm->LoadRoot(g1, RootIndex::kUndefinedValue);
    masm->Push(g1);
  }
  if (mode == InterpreterPushArgsMode::kWithFinalSpread) {
    masm->LoadU64(o2, MemOperand(o2, -kSystemPointerSize));
    masm->TailCallBuiltin(Builtin::kCallWithSpread);
  } else {
    masm->TailCallBuiltin(Builtins::Call(receiver_mode));
  }
  masm->bind(&overflow);
  masm->TailCallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();
}

void Builtins::Generate_InterpreterPushArgsThenConstructImpl(
    MacroAssembler* masm, InterpreterPushArgsMode mode) {
  Label overflow;
  masm->StackOverflowCheck(o0, g3, &overflow);
  if (mode == InterpreterPushArgsMode::kWithFinalSpread) masm->sub(o0, o0, 1);
  masm->SubS64(o5, o0, kJSArgcReceiverSlots);
  PushInterpreterArguments(masm, o5, o4);
  masm->Push(g0);
  if (mode == InterpreterPushArgsMode::kWithFinalSpread) {
    masm->LoadU64(o2, MemOperand(o4, -kSystemPointerSize));
    masm->TailCallBuiltin(Builtin::kConstructWithSpread);
  } else if (mode == InterpreterPushArgsMode::kArrayFunction) {
    masm->TailCallBuiltin(Builtin::kArrayConstructorImpl);
  } else {
    CHECK_EQ(mode, InterpreterPushArgsMode::kOther);
    masm->TailCallBuiltin(Builtin::kConstruct);
  }
  masm->bind(&overflow);
  masm->TailCallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();
}

namespace {

void EnterInterpreterBytecode(MacroAssembler* masm) {
  const int return_offset =
      masm->isolate()->heap()->interpreter_entry_return_pc_offset().value();
  CHECK_NE(return_offset, 0);
  Label default_trampoline, ready;
  masm->LoadU64(o2, MemOperand(fp, StandardFrameConstants::kFunctionOffset));
  masm->LoadU64(o2, FieldMemOperand(o2, JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU64(
      o2, FieldMemOperand(o2, SharedFunctionInfo::kTrustedFunctionDataOffset));
  masm->LoadU64(g1, FieldMemOperand(o2, HeapObject::kMapOffset));
  masm->LoadU16(g1, FieldMemOperand(g1, Map::kInstanceTypeOffset));
  Label data_ready;
  masm->cmp(g1, CODE_TYPE);
  masm->b(not_equal, ConditionCodes::kXcc, &data_ready);
  masm->nop();
  masm->LoadU64(o2, FieldMemOperand(
                        o2, Code::kDeoptimizationDataOrInterpreterDataOffset));
  masm->LoadMap(g1, o2);
  masm->LoadU16(g1, FieldMemOperand(g1, Map::kInstanceTypeOffset));
  masm->bind(&data_ready);
  masm->cmp(g1, INTERPRETER_DATA_TYPE);
  masm->b(not_equal, ConditionCodes::kXcc, &default_trampoline);
  masm->nop();
  masm->LoadInterpreterDataInterpreterTrampoline(o2, o2);
  masm->LoadU64(o2, FieldMemOperand(o2, Code::kInstructionStartOffset));
  masm->Jump(&ready);
  masm->bind(&default_trampoline);
  masm->Move(o2, ExternalReference::
                     address_of_interpreter_entry_trampoline_instruction_start(
                         masm->isolate()));
  masm->LoadU64(o2, MemOperand(o2));
  masm->bind(&ready);
  masm->AddS64(o7, o2, return_offset - kSparcReturnAddressOffset);
  masm->Move(
      kInterpreterDispatchTableRegister,
      ExternalReference::interpreter_dispatch_table_address(masm->isolate()));
  masm->DispatchInterpreterBytecode();
}

}  // namespace

void Builtins::Generate_InterpreterEnterAtBytecode(MacroAssembler* masm) {
  EnterInterpreterBytecode(masm);
}

void Builtins::Generate_InterpreterEnterAtNextBytecode(MacroAssembler* masm) {
  const Register array = kInterpreterBytecodeArrayRegister;
  const Register offset = kInterpreterBytecodeOffsetRegister;
  masm->LoadU64(
      array, MemOperand(fp, InterpreterFrameConstants::kBytecodeArrayFromFp));
  masm->LoadU64(
      offset, MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  masm->srax(offset, offset, kSmiShiftSize + kSmiTagSize);
  Label function_entry, enter, invalid_return;
  masm->JumpIfEqual(offset,
                    BytecodeArray::kHeaderSize - kHeapObjectTag +
                        kFunctionEntryBytecodeOffset,
                    &function_entry);
  masm->add(g1, array, offset);
  masm->LoadU8(o1, MemOperand(g1));
  masm->Move(o2, ExternalReference::bytecode_size_table_address());
  masm->AdvanceBytecodeOffsetOrReturn(array, offset, o1, o2, o3,
                                      &invalid_return);
  masm->bind(&enter);
  masm->sllx(o2, offset, kSmiShiftSize + kSmiTagSize);
  masm->StoreU64(
      o2, MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  EnterInterpreterBytecode(masm);
  masm->bind(&function_entry);
  masm->Move(offset, BytecodeArray::kHeaderSize - kHeapObjectTag);
  masm->Jump(&enter);
  masm->bind(&invalid_return);
  masm->Trap();
}

void Builtins::Generate_ConstructForwardAllArgsImpl(
    MacroAssembler* masm, ForwardWhichFrame which_frame) {
  masm->Move(o4, fp);
  if (which_frame == ForwardWhichFrame::kParentFrame) {
    masm->LoadU64(o4, MemOperand(o4, StandardFrameConstants::kCallerFPOffset));
  } else {
    CHECK_EQ(which_frame, ForwardWhichFrame::kCurrentFrame);
  }
  masm->LoadU64(o0, MemOperand(o4, StandardFrameConstants::kArgCOffset));
  Label overflow;
  masm->StackOverflowCheck(o0, g3, &overflow);
  masm->AddS64(o4, o4,
               StandardFrameConstants::kCallerSPOffset + kSystemPointerSize);
  masm->PushArguments(o4, o0, o5, ArgumentsElementType::kRaw);
  masm->Push(g0);
  masm->TailCallBuiltin(Builtin::kConstruct);
  masm->bind(&overflow);
  masm->TailCallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();
}

namespace {

void PushBoundArguments(MacroAssembler* masm) {
  Label done, overflow;
  masm->LoadU64(o2,
                FieldMemOperand(o1, JSBoundFunction::kBoundArgumentsOffset));
  masm->LoadU64(o4, FieldMemOperand(o2, offsetof(FixedArray, length_)));
  masm->srax(o4, o4, kSmiShiftSize + kSmiTagSize);
  masm->cmp(o4, 0);
  masm->b(equal, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->StackOverflowCheck(o4, g3, &overflow);
  masm->Pop(o5);
  masm->add(o0, o0, o4);
  masm->add(o4, o4, kJSArgcReceiverSlots);
  masm->AddS64(o2, o2, OFFSET_OF_DATA_START(FixedArray) - kHeapObjectTag);
  masm->PushArguments(o2, o4, g4, ArgumentsElementType::kRaw);
  masm->Push(o5);
  masm->Jump(&done);
  masm->bind(&overflow);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->CallRuntime(Runtime::kThrowStackOverflow);
    masm->Trap();
  }
  masm->bind(&done);
}

}  // namespace

void Builtins::Generate_CallBoundFunctionImpl(MacroAssembler* masm) {
  masm->LoadU64(g1, FieldMemOperand(o1, JSBoundFunction::kBoundThisOffset));
  masm->StoreU64(g1, MemOperand(sp));
  PushBoundArguments(masm);
  masm->LoadU64(
      o1, FieldMemOperand(o1, JSBoundFunction::kBoundTargetFunctionOffset));
  masm->TailCallBuiltin(Builtins::Call());
}

void Builtins::Generate_ConstructBoundFunction(MacroAssembler* masm) {
  PushBoundArguments(masm);
  Label new_target_ready;
  masm->cmp(o1, o3);
  masm->b(not_equal, ConditionCodes::kXcc, &new_target_ready);
  masm->nop();
  masm->LoadU64(
      o3, FieldMemOperand(o1, JSBoundFunction::kBoundTargetFunctionOffset));
  masm->bind(&new_target_ready);
  masm->LoadU64(
      o1, FieldMemOperand(o1, JSBoundFunction::kBoundTargetFunctionOffset));
  masm->TailCallBuiltin(Builtin::kConstruct);
}

void Builtins::Generate_CallOrConstructVarargs(MacroAssembler* masm,
                                               Builtin target_builtin) {
  if (v8_flags.debug_code) {
    Label valid, invalid;
    masm->andcc(g0, o2, kSmiTagMask);
    masm->b(equal, ConditionCodes::kXcc, &invalid);
    masm->nop();
    masm->LoadU64(g1, FieldMemOperand(o2, HeapObject::kMapOffset));
    masm->LoadU16(g1, FieldMemOperand(g1, Map::kInstanceTypeOffset));
    masm->JumpIfEqual(g1, FIXED_ARRAY_TYPE, &valid);
    masm->cmp(g1, FIXED_DOUBLE_ARRAY_TYPE);
    masm->b(not_equal, ConditionCodes::kXcc, &invalid);
    masm->nop();
    masm->JumpIfEqual(o4, 0, &valid);
    masm->bind(&invalid);
    masm->Trap();
    masm->bind(&valid);
  }
  Label overflow, done, copy, store;
  masm->StackOverflowCheck(o4, g3, &overflow);
  masm->AllocateSpaceAndShiftArguments(o4, o0, o5);
  masm->cmp(o4, 0);
  masm->b(equal, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->AddS64(o2, o2, OFFSET_OF_DATA_START(FixedArray) - kHeapObjectTag);
  masm->bind(&copy);
  masm->LoadU64(g4, MemOperand(o2));
  masm->LoadRoot(g3, RootIndex::kTheHoleValue);
  masm->cmp(g4, g3);
  masm->b(not_equal, ConditionCodes::kXcc, &store);
  masm->nop();
  masm->LoadRoot(g4, RootIndex::kUndefinedValue);
  masm->bind(&store);
  masm->StoreU64(g4, MemOperand(o5));
  masm->add(o2, o2, kTaggedSize);
  masm->add(o5, o5, kSystemPointerSize);
  masm->sub(o4, o4, 1);
  masm->cmp(o4, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &copy);
  masm->nop();
  masm->bind(&done);
  masm->TailCallBuiltin(target_builtin);
  masm->bind(&overflow);
  masm->TailCallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();
}

void Builtins::Generate_CallOrConstructForwardVarargs(MacroAssembler* masm,
                                                      CallOrConstructMode mode,
                                                      Builtin target_builtin) {
  Label not_constructor, done, overflow, copy;
  if (mode == CallOrConstructMode::kConstruct) {
    masm->andcc(g0, o3, kSmiTagMask);
    masm->b(equal, ConditionCodes::kXcc, &not_constructor);
    masm->nop();
    masm->LoadU64(g1, FieldMemOperand(o3, HeapObject::kMapOffset));
    masm->LoadU8(g1, FieldMemOperand(g1, Map::kBitFieldOffset));
    masm->andcc(g0, g1, Map::Bits1::IsConstructorBit::kMask);
    masm->b(equal, ConditionCodes::kXcc, &not_constructor);
    masm->nop();
  }
  masm->LoadU64(o4, MemOperand(fp, StandardFrameConstants::kArgCOffset));
  masm->sub(o4, o4, kJSArgcReceiverSlots);
  masm->sub(o4, o4, o2);
  masm->cmp(o4, 0);
  masm->b(less_equal, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->StackOverflowCheck(o4, g3, &overflow);
  masm->sllx(o2, o2, kSystemPointerSizeLog2);
  masm->AddS64(g1, fp,
               StandardFrameConstants::kCallerSPOffset + kSystemPointerSize);
  masm->add(o2, o2, g1);
  masm->AllocateSpaceAndShiftArguments(o4, o0, o5);
  masm->bind(&copy);
  masm->LoadU64(g4, MemOperand(o2));
  masm->StoreU64(g4, MemOperand(o5));
  masm->add(o2, o2, kSystemPointerSize);
  masm->add(o5, o5, kSystemPointerSize);
  masm->sub(o4, o4, 1);
  masm->cmp(o4, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &copy);
  masm->nop();
  masm->bind(&done);
  masm->TailCallBuiltin(target_builtin);
  masm->bind(&overflow);
  masm->TailCallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();
  if (mode == CallOrConstructMode::kConstruct) {
    masm->bind(&not_constructor);
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->Push(o3);
    masm->CallRuntime(Runtime::kThrowNotConstructor);
    masm->Trap();
  }
}

void Builtins::Generate_JSBuiltinsConstructStub(MacroAssembler* masm) {
  Label overflow;
  masm->StackOverflowCheck(o0, g3, &overflow);
  {
    FrameScope frame(masm, StackFrame::CONSTRUCT);
    masm->Push(kContextRegister, o0);
    masm->AddS64(o4, fp,
                 StandardFrameConstants::kCallerSPOffset + kSystemPointerSize);
    masm->PushArguments(o4, o0, o5, ArgumentsElementType::kRaw);
    masm->LoadRoot(g1, RootIndex::kTheHoleValue);
    masm->Push(g1);
    masm->LoadU64(o2,
                  FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
    masm->LoadU16(o2, FieldMemOperand(
                          o2, SharedFunctionInfo::kFormalParameterCountOffset));
    masm->LoadU64(kContextRegister,
                  FieldMemOperand(o1, JSFunction::kContextOffset));
    masm->InvokeFunctionCode(o1, o3, o2, o0, InvokeType::kCall);
    masm->LoadU64(kContextRegister,
                  MemOperand(fp, ConstructFrameConstants::kContextOffset));
    masm->LoadU64(o2, MemOperand(fp, ConstructFrameConstants::kLengthOffset));
  }
  masm->Drop(o2);
  masm->Ret();
  masm->bind(&overflow);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->CallRuntime(Runtime::kThrowStackOverflow);
    masm->Trap();
  }
}

void Builtins::Generate_JSConstructStubGeneric(MacroAssembler* masm) {
  FrameScope frame(masm, StackFrame::MANUAL);
  masm->EnterFrame(StackFrame::CONSTRUCT);
  masm->Push(kContextRegister, o0, o1);
  masm->LoadRoot(g1, RootIndex::kUndefinedValue);
  masm->Push(g1, o3);
  masm->LoadU64(g1, FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU32(g1, FieldMemOperand(g1, SharedFunctionInfo::kFlagsOffset));
  masm->Move(g2, SharedFunctionInfo::FunctionKindBits::kMask);
  masm->and_(g1, g1, g2);
  masm->srlx(g1, g1, SharedFunctionInfo::FunctionKindBits::kShift);
  masm->SubS64(g1, g1,
               static_cast<int>(FunctionKind::kDefaultDerivedConstructor));
  masm->cmp(g1, static_cast<int>(FunctionKind::kDerivedConstructor) -
                    static_cast<int>(FunctionKind::kDefaultDerivedConstructor));
  Label derived, created, overflow;
  masm->b(less_equal_unsigned, ConditionCodes::kXcc, &derived);
  masm->nop();
  masm->CallBuiltin(Builtin::kFastNewObject);
  masm->Jump(&created);
  masm->bind(&derived);
  masm->LoadRoot(o0, RootIndex::kTheHoleValue);
  masm->isolate()->heap()->SetConstructStubCreateDeoptPCOffset(
      masm->pc_offset());
  masm->bind(&created);
  masm->Pop(o3);
  masm->Push(o0);
  masm->Move(o5, o0);
  masm->AddS64(o2, fp,
               StandardFrameConstants::kCallerSPOffset + kSystemPointerSize);
  masm->LoadU64(o1,
                MemOperand(fp, ConstructFrameConstants::kConstructorOffset));
  masm->LoadU64(o0, MemOperand(fp, ConstructFrameConstants::kLengthOffset));
  masm->StackOverflowCheck(o0, g3, &overflow);
  masm->PushArguments(o2, o0, o4, ArgumentsElementType::kRaw);
  masm->Push(o5);
  masm->LoadU64(o2, FieldMemOperand(o1, JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU16(
      o2, FieldMemOperand(o2, SharedFunctionInfo::kFormalParameterCountOffset));
  masm->LoadU64(kContextRegister,
                FieldMemOperand(o1, JSFunction::kContextOffset));
  masm->InvokeFunctionCode(o1, o3, o2, o0, InvokeType::kCall);
  Label use_receiver, leave, throw_non_object;
  masm->LoadRoot(g1, RootIndex::kUndefinedValue);
  masm->cmp(o0, g1);
  masm->b(equal, ConditionCodes::kXcc, &use_receiver);
  masm->nop();
  masm->andcc(g0, o0, kSmiTagMask);
  masm->b(equal, ConditionCodes::kXcc, &use_receiver);
  masm->nop();
  masm->LoadU64(g1, FieldMemOperand(o0, HeapObject::kMapOffset));
  masm->LoadU16(g1, FieldMemOperand(g1, Map::kInstanceTypeOffset));
  masm->cmp(g1, FIRST_JS_RECEIVER_TYPE);
  masm->b(greater_equal, ConditionCodes::kXcc, &leave);
  masm->nop();
  masm->bind(&use_receiver);
  masm->LoadU64(o0, MemOperand(sp));
  masm->LoadRoot(g1, RootIndex::kTheHoleValue);
  masm->cmp(o0, g1);
  masm->b(equal, ConditionCodes::kXcc, &throw_non_object);
  masm->nop();
  masm->bind(&leave);
  masm->LoadU64(o1, MemOperand(fp, ConstructFrameConstants::kLengthOffset));
  masm->LeaveFrame(StackFrame::CONSTRUCT);
  masm->Drop(o1);
  masm->Ret();
  masm->bind(&throw_non_object);
  masm->LoadU64(kContextRegister,
                MemOperand(fp, ConstructFrameConstants::kContextOffset));
  masm->CallRuntime(Runtime::kThrowConstructorReturnedNonObject);
  masm->Trap();
  masm->bind(&overflow);
  masm->LoadU64(kContextRegister,
                MemOperand(fp, ConstructFrameConstants::kContextOffset));
  masm->CallRuntime(Runtime::kThrowStackOverflow);
  masm->Trap();
}

void Builtins::Generate_InterpreterPushArgsThenFastConstructFunction(
    MacroAssembler* masm) {
  masm->LoadRoot(o2, RootIndex::kUndefinedValue);
  Generate_InterpreterPushArgsThenConstructImpl(
      masm, InterpreterPushArgsMode::kOther);

  FrameScope frame(masm, StackFrame::MANUAL);
  masm->isolate()->heap()->SetConstructStubInvokeDeoptPCOffset(
      masm->pc_offset());
  Label use_receiver, leave, throw_non_object;
  masm->LoadRoot(g1, RootIndex::kUndefinedValue);
  masm->cmp(o0, g1);
  masm->b(equal, ConditionCodes::kXcc, &use_receiver);
  masm->nop();
  masm->andcc(g0, o0, kSmiTagMask);
  masm->b(equal, ConditionCodes::kXcc, &use_receiver);
  masm->nop();
  masm->LoadMap(g1, o0);
  masm->LoadU16(g1, FieldMemOperand(g1, Map::kInstanceTypeOffset));
  masm->cmp(g1, FIRST_JS_RECEIVER_TYPE);
  masm->b(greater_equal, ConditionCodes::kXcc, &leave);
  masm->nop();
  masm->bind(&use_receiver);
  masm->LoadU64(
      o0, MemOperand(fp, FastConstructFrameConstants::kImplicitReceiverOffset));
  masm->LoadRoot(g1, RootIndex::kTheHoleValue);
  masm->cmp(o0, g1);
  masm->b(equal, ConditionCodes::kXcc, &throw_non_object);
  masm->nop();
  masm->bind(&leave);
  masm->LoadU64(kContextRegister,
                MemOperand(fp, FastConstructFrameConstants::kContextOffset));
  masm->LeaveFrame(StackFrame::FAST_CONSTRUCT);
  masm->Ret();
  masm->bind(&throw_non_object);
  masm->LoadU64(kContextRegister,
                MemOperand(fp, FastConstructFrameConstants::kContextOffset));
  masm->CallRuntime(Runtime::kThrowConstructorReturnedNonObject);
  masm->Trap();
}

void Builtins::Generate_ResumeGeneratorTrampoline(MacroAssembler* masm) {
  masm->StoreU64(
      o0, FieldMemOperand(o1, JSGeneratorObject::kInputOrDebugPosOffset));
  masm->RecordWriteField(o1, JSGeneratorObject::kInputOrDebugPosOffset, o0,
                         SaveFPRegsMode::kIgnore);
  masm->LoadU64(o4, FieldMemOperand(o1, JSGeneratorObject::kFunctionOffset));
  masm->LoadU64(kContextRegister,
                FieldMemOperand(o4, JSFunction::kContextOffset));
  Label step_function, step_suspended, ready, overflow;
  masm->Move(o5, ExternalReference::debug_hook_on_function_call_address(
                     masm->isolate()));
  masm->LoadU8(o5, MemOperand(o5));
  masm->cmp(o5, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &step_function);
  masm->nop();
  masm->Move(o5, ExternalReference::debug_suspended_generator_address(
                     masm->isolate()));
  masm->LoadU64(o5, MemOperand(o5));
  masm->cmp(o5, o1);
  masm->b(equal, ConditionCodes::kXcc, &step_suspended);
  masm->nop();
  masm->bind(&ready);
  masm->LoadU64(o2, FieldMemOperand(o4, JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU16(
      o0, FieldMemOperand(o2, SharedFunctionInfo::kFormalParameterCountOffset));
  masm->StackOverflowCheck(o0, g3, &overflow);
  masm->LoadU64(o2, FieldMemOperand(
                        o1, JSGeneratorObject::kParametersAndRegistersOffset));
  masm->AddS64(o2, o2, OFFSET_OF_DATA_START(FixedArray) - kHeapObjectTag);
  masm->PushArguments(o2, o0, o5, ArgumentsElementType::kRaw);
  masm->LoadU64(g1, FieldMemOperand(o1, JSGeneratorObject::kReceiverOffset));
  masm->Push(g1);
  if (v8_flags.debug_code) {
    Label has_bytecode;
    masm->LoadU64(g1,
                  FieldMemOperand(o4, JSFunction::kSharedFunctionInfoOffset));
    masm->LoadU64(g1, FieldMemOperand(
                          g1, SharedFunctionInfo::kTrustedFunctionDataOffset));
    masm->LoadU64(g2, FieldMemOperand(g1, HeapObject::kMapOffset));
    masm->LoadU16(g2, FieldMemOperand(g2, Map::kInstanceTypeOffset));
    masm->JumpIfEqual(g2, BYTECODE_ARRAY_TYPE, &has_bytecode);
    masm->JumpIfEqual(g2, INTERPRETER_DATA_TYPE, &has_bytecode);
    masm->Trap();
    masm->bind(&has_bytecode);
  }
  masm->Move(o3, o1);
  masm->Move(o1, o4);
  masm->JumpJSFunction(o1);
  masm->bind(&step_function);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->Push(kContextRegister, o1, o4);
    masm->LoadRoot(g1, RootIndex::kTheHoleValue);
    masm->Push(g1);
    masm->CallRuntime(Runtime::kDebugOnFunctionCall);
    masm->Pop(o1, kContextRegister);
  }
  masm->LoadU64(o4, FieldMemOperand(o1, JSGeneratorObject::kFunctionOffset));
  masm->Jump(&ready);
  masm->bind(&step_suspended);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->Push(kContextRegister, o1);
    masm->CallRuntime(Runtime::kDebugPrepareStepInSuspendedGenerator);
    masm->Pop(o1, kContextRegister);
  }
  masm->LoadU64(o4, FieldMemOperand(o1, JSGeneratorObject::kFunctionOffset));
  masm->Jump(&ready);
  masm->bind(&overflow);
  {
    FrameScope frame(masm, StackFrame::INTERNAL);
    masm->CallRuntime(Runtime::kThrowStackOverflow);
    masm->Trap();
  }
}

namespace {

void CallApiFunctionAndReturn(MacroAssembler* masm, Register function,
                              int c_argc, ExternalReference thunk,
                              bool with_profiling, int slots_to_drop,
                              const MemOperand* argc_slot,
                              MemOperand result_slot, bool interceptor) {
  using ER = ExternalReference;
  auto address = [&](ExternalReference reference) {
    masm->Move(g4, reference);
    return MemOperand(g4);
  };
  masm->Move(l3, function);
  masm->LoadU64(l0, address(ER::handle_scope_next_address(masm->isolate())));
  masm->LoadU64(l1, address(ER::handle_scope_limit_address(masm->isolate())));
  masm->LoadU32(l2, address(ER::handle_scope_level_address(masm->isolate())));
  masm->add(g2, l2, 1);
  masm->StoreU32(g2, MemOperand(g4));
  Label use_thunk, called, result_read, delete_handles, leave, exception;
  if (with_profiling) {
    masm->LoadU8(g1, masm->AsMemOperand(IsolateFieldId::kExecutionMode));
    masm->cmp(g1, 0);
    masm->b(not_equal, ConditionCodes::kXcc, &use_thunk);
    masm->nop();
#ifdef V8_RUNTIME_CALL_STATS
    masm->LoadU32(g1, address(ER::address_of_runtime_stats_flag()));
    masm->cmp(g1, 0);
    masm->b(not_equal, ConditionCodes::kXcc, &use_thunk);
    masm->nop();
#endif
  }
  masm->StoreReturnAddressAndCall(l3, c_argc);
  masm->bind(&called);
  if (interceptor) {
    static_assert(kInterceptedNo == 1 && kInterceptedYes == 0 &&
                  kNotInterceptedSentinel == 1 && kInterceptedSize == 4);
    masm->and_(o0, o0, 1);
    masm->cmp(o0, 0);
    masm->b(not_equal, ConditionCodes::kXcc, &result_read);
    masm->nop();
  }
  masm->LoadU64(o0, result_slot);
  masm->bind(&result_read);
  masm->StoreU64(l0, address(ER::handle_scope_next_address(masm->isolate())));
  masm->Move(g4, ER::handle_scope_level_address(masm->isolate()));
  if (v8_flags.debug_code) {
    Label valid_level;
    masm->LoadU32(g2, MemOperand(g4));
    masm->sub(g2, g2, 1);
    masm->cmp(g2, l2);
    masm->b(equal, ConditionCodes::kXcc, &valid_level);
    masm->nop();
    masm->Trap();
    masm->bind(&valid_level);
  }
  masm->StoreU32(l2, MemOperand(g4));
  masm->LoadU64(g2, address(ER::handle_scope_limit_address(masm->isolate())));
  masm->cmp(g2, l1);
  masm->b(not_equal, ConditionCodes::kXcc, &delete_handles);
  masm->nop();
  masm->bind(&leave);
  if (argc_slot != nullptr) masm->LoadU64(l1, *argc_slot);
  masm->LeaveExitFrame(g1);
  masm->LoadU64(g3, masm->AsMemOperand(IsolateFieldId::kException));
  masm->LoadRoot(g2, RootIndex::kTheHoleValue);
  masm->cmp(g3, g2);
  masm->b(not_equal, ConditionCodes::kXcc, &exception);
  masm->nop();
  masm->Drop(slots_to_drop);
  if (argc_slot != nullptr) masm->Drop(l1);
  masm->Ret();
  if (with_profiling) {
    masm->bind(&use_thunk);
    masm->Move(g3, thunk);
    masm->StoreReturnAddressAndCall(g3, c_argc);
    masm->Jump(&called);
  }
  masm->bind(&exception);
  masm->TailCallRuntime(Runtime::kPropagateException);
  masm->bind(&delete_handles);
  masm->StoreU64(l1, address(ER::handle_scope_limit_address(masm->isolate())));
  masm->Move(l1, o0);
  masm->PrepareCallCFunction(1, g1);
  masm->LoadAddress(o0, masm->AsMemOperand(IsolateFieldId::kIsolateAddress));
  masm->CallCFunction(ER::delete_handle_scope_extensions(), 1,
                      SetIsolateDataSlots::kNo);
  masm->Move(o0, l1);
  masm->Jump(&leave);
}

}  // namespace

void Builtins::Generate_CallApiCallbackImpl(MacroAssembler* masm,
                                            CallApiCallbackMode mode) {
  using FC = ApiCallbackExitFrameConstants;
  using FCA = FunctionCallbackArguments;
  static_assert(FCA::kApiArgsLength == 4);
  static_assert(FCA::ApiArgIndex(FCA::kIsolateIndex) == 0);
  static_assert(FCA::ApiArgIndex(FCA::kReturnValueIndex) == 1);
  static_assert(FCA::ApiArgIndex(FCA::kContextIndex) == 2);
  static_assert(FCA::ApiArgIndex(FCA::kTargetIndex) == 3);
  Register argc = no_reg;
  Register target = no_reg;
  Register script_context = no_reg;
  Register function = no_reg;
  if (mode == CallApiCallbackMode::kGeneric) {
    argc = CallApiCallbackGenericDescriptor::ActualArgumentsCountRegister();
    target = CallApiCallbackGenericDescriptor::FunctionTemplateInfoRegister();
    script_context =
        CallApiCallbackGenericDescriptor::TopmostScriptHavingContextRegister();
    function = o1;
  } else {
    argc = CallApiCallbackOptimizedDescriptor::ActualArgumentsCountRegister();
    target = CallApiCallbackOptimizedDescriptor::FunctionTemplateInfoRegister();
    script_context = kContextRegister;
    function = CallApiCallbackOptimizedDescriptor::ApiFunctionAddressRegister();
  }
  masm->StoreRootRelative(IsolateData::topmost_script_having_context_offset(),
                          script_context);
  masm->LoadRoot(g2, RootIndex::kUndefinedValue);
  masm->LoadAddress(g1, masm->AsMemOperand(IsolateFieldId::kIsolateAddress));
  masm->Push(target, kContextRegister, g2, g1);
  if (mode == CallApiCallbackMode::kGeneric)
    masm->LoadU64(function, FieldMemOperand(
                                target, FunctionTemplateInfo::kCallbackOffset));
  FrameScope frame(masm, StackFrame::MANUAL);
  masm->EnterExitFrame(g1, FC::getExtraSlotsCountFrom<ExitFrameConstants>(),
                       StackFrame::API_CALLBACK_EXIT);
  MemOperand argc_slot(fp, FC::kFCIArgcOffset);
  masm->StoreU64(argc, argc_slot);
  masm->LoadAddress(o0, MemOperand(fp, FC::kFunctionCallbackInfoOffset));
  CallApiFunctionAndReturn(
      masm, function, 1, ExternalReference::invoke_function_callback(mode),
      mode != CallApiCallbackMode::kOptimizedNoProfiling,
      FC::kFunctionCallbackInfoApiArgsLength + kJSArgcReceiverSlots, &argc_slot,
      MemOperand(fp, FC::kReturnValueOffset), false);
}

void Builtins::Generate_CallApiAccessorImpl(MacroAssembler* masm,
                                            bool for_interceptor,
                                            bool for_setter) {
  using FC = ApiAccessorExitFrameConstants;
  using PCA = PropertyCallbackArguments;
  CHECK(for_interceptor || !for_setter);
  const Register callback = CallApiGetterDescriptor::CallbackRegister();
  CHECK_EQ(callback, CallApiSetterDescriptor::CallbackRegister());
  CHECK_EQ(CallApiGetterDescriptor::NameRegister(), o0);
  CHECK_EQ(CallApiSetterDescriptor::NameRegister(), o0);
  static_assert(PCA::kGetterApiArgsLength == 4 &&
                PCA::kSetterApiArgsLength == 6);
  static_assert(PCA::ApiArgIndex(PCA::kIsolateIndex) == 0);
  static_assert(PCA::ApiArgIndex(PCA::kReturnValueIndex) == 1);
  static_assert(PCA::ApiArgIndex(PCA::kCallbackInfoIndex) == 2);
  static_assert(PCA::ApiArgIndex(PCA::kHolderIndex) == 3);
  masm->LoadRoot(
      g2, for_setter ? RootIndex::kTrueValue : RootIndex::kUndefinedValue);
  masm->LoadAddress(g1, masm->AsMemOperand(IsolateFieldId::kIsolateAddress));
  masm->Push(callback, g2, g1);
  FrameScope frame(masm, StackFrame::MANUAL);
  masm->EnterExitFrame(g1, FC::getExtraSlotsCountFrom<ExitFrameConstants>(),
                       StackFrame::API_NAMED_ACCESSOR_EXIT);
  masm->StoreU64(o0, MemOperand(fp, FC::kPropertyKeyOffset));
  Register info = for_setter ? o2 : o1;
  masm->LoadAddress(info, MemOperand(fp, FC::kPropertyCallbackInfoOffset));
#ifdef V8_ENABLE_DIRECT_HANDLE
  if (for_setter) masm->LoadU64(o1, MemOperand(fp, FC::kValueOffset));
#else
  static_assert(PCA::kPropertyKeyIndex == 0);
  masm->Move(o0, info);
  if (for_setter) masm->AddS64(o1, info, PCA::kValueIndex * kSystemPointerSize);
#endif
  ExternalReference thunk;
  if (for_interceptor) {
    thunk = for_setter
                ? ExternalReference::invoke_named_interceptor_setter_callback()
                : ExternalReference::invoke_named_interceptor_getter_callback();
    masm->LoadU64(
        callback,
        FieldMemOperand(callback, for_setter ? InterceptorInfo::kSetterOffset
                                             : InterceptorInfo::kGetterOffset));
  } else {
    thunk = ExternalReference::invoke_accessor_getter_callback();
    masm->LoadU64(callback,
                  FieldMemOperand(callback, AccessorInfo::kGetterOffset));
  }
  CallApiFunctionAndReturn(
      masm, callback, for_setter ? 3 : 2, thunk, true,
      for_setter ? FC::kPropertyCallbackInfoSetterApiArgsLength
                 : FC::kPropertyCallbackInfoGetterApiArgsLength,
      nullptr, MemOperand(fp, FC::kReturnValueOffset), for_interceptor);
}

void Builtins::Generate_ContinueToCodeStubBuiltin(MacroAssembler* masm) {
  masm->ContinueToBuiltin(false, false);
}

void Builtins::Generate_ContinueToCodeStubBuiltinWithResult(
    MacroAssembler* masm) {
  masm->ContinueToBuiltin(false, true);
}

void Builtins::Generate_ContinueToJavaScriptBuiltin(MacroAssembler* masm) {
  masm->ContinueToBuiltin(true, false);
}

void Builtins::Generate_ContinueToJavaScriptBuiltinWithResult(
    MacroAssembler* masm) {
  masm->ContinueToBuiltin(true, true);
}

namespace {

void GenerateDeoptimizationEntry(MacroAssembler* masm, DeoptimizeKind kind) {
  FrameScope frame_scope(masm, StackFrame::MANUAL);
  constexpr int kGeneralSize = Register::kNumRegisters * kSystemPointerSize;
  constexpr int kDoubleSizeTotal = DoubleRegister::kNumRegisters * kSimd128Size;
  constexpr int kSavedSize = kGeneralSize + kDoubleSizeTotal;
  const RegisterConfiguration* config = RegisterConfiguration::Default();
  const RegList restored = kJSCallerSaved | RegList{fp, kRootRegister};

  masm->AllocateStackSpace(kSavedSize);
  // SAVE/RESTORE must retain the native i6/i7 window and g6/g7 unchanged.
  for (int code = 0; code < Register::kNumRegisters; ++code) {
    Register reg = Register::from_code(code);
    if (reg == sp) {
      masm->LoadStackAddress(g1, kSavedSize);
      masm->StoreU64(g1, MemOperand(sp, code * kSystemPointerSize));
    } else {
      masm->StoreU64(reg, MemOperand(sp, code * kSystemPointerSize));
    }
  }
  for (int n = 0; n < config->num_allocatable_double_registers(); ++n) {
    int code = config->GetAllocatableDoubleCode(n);
    masm->StoreF64(DoubleRegister::from_code(code),
                   MemOperand(sp, kGeneralSize + code * kSimd128Size));
    masm->StoreF64(
        DoubleRegister::from_code(code + 16),
        MemOperand(sp, kGeneralSize + code * kSimd128Size + kDoubleSize));
  }
  masm->StoreU64(fp, masm->AsMemOperand(IsolateFieldId::kCEntryFP));

  masm->Move(o0, 0);
  masm->LoadU64(
      g1, MemOperand(fp, CommonFrameConstants::kContextOrFrameTypeOffset));
  Label typed_frame;
  masm->andcc(g0, g1, kSmiTagMask);
  masm->b(equal, ConditionCodes::kXcc, &typed_frame);
  masm->nop();
  masm->LoadU64(o0, MemOperand(fp, StandardFrameConstants::kFunctionOffset));
  masm->bind(&typed_frame);
  masm->Move(o1, static_cast<int>(kind));
  masm->add(o2, o7, kSparcReturnAddressOffset);
  masm->LoadStackAddress(g1, kSavedSize);
  masm->sub(o3, fp, g1);
  masm->Move(o4, ExternalReference::isolate_address());
  {
    AllowExternalCallThatCantCauseGC scope(masm);
    masm->PrepareCallCFunction(5, g2);
    masm->CallCFunction(ExternalReference::new_deoptimizer_function(), 5);
  }
  masm->Move(l0, o0);
  masm->LoadU64(l1, MemOperand(l0, Deoptimizer::input_offset()));
  for (int code = 0; code < Register::kNumRegisters; ++code) {
    masm->LoadU64(g1, MemOperand(sp, code * kSystemPointerSize));
    masm->StoreU64(g1, MemOperand(l1, FrameDescription::registers_offset() +
                                          code * kSystemPointerSize));
  }
  for (int n = 0; n < config->num_allocatable_double_registers(); ++n) {
    int code = config->GetAllocatableDoubleCode(n);
    for (int half = 0; half < 2; ++half) {
      masm->LoadF64(d0, MemOperand(sp, kGeneralSize + code * kSimd128Size +
                                           half * kDoubleSize));
      masm->StoreF64(
          d0, MemOperand(l1, FrameDescription::simd128_registers_offset() +
                                 code * kSimd128Size + half * kDoubleSize));
    }
  }
  masm->StoreU8(g0, masm->AsMemOperand(IsolateFieldId::kStackIsIterable));
  masm->Drop(kSavedSize / kSystemPointerSize);

  masm->LoadU64(l2, MemOperand(l1, FrameDescription::frame_size_offset()));
  masm->LoadStackAddress(g1);
  masm->add(l2, g1, l2);
  masm->AddS64(l3, l1, FrameDescription::frame_content_offset());
  Label copy_input, input_done;
  masm->bind(&copy_input);
  masm->LoadStackAddress(g1);
  masm->cmp(g1, l2);
  masm->b(equal, ConditionCodes::kXcc, &input_done);
  masm->nop();
  masm->Pop(g1);
  masm->StoreU64(g1, MemOperand(l3));
  masm->add(l3, l3, kSystemPointerSize);
  masm->Jump(&copy_input);
  masm->bind(&input_done);

  masm->Move(o0, l0);
  {
    AllowExternalCallThatCantCauseGC scope(masm);
    masm->PrepareCallCFunction(1, g2);
    masm->CallCFunction(ExternalReference::compute_output_frames_function(), 1);
  }
  masm->LoadU64(g1, MemOperand(l0, Deoptimizer::caller_frame_top_offset()));
  masm->Move(sp, g1);
  masm->LoadU32(l2, MemOperand(l0, Deoptimizer::output_count_offset()));
  masm->LoadU64(l3, MemOperand(l0, Deoptimizer::output_offset()));
  masm->sllx(l2, l2, kSystemPointerSizeLog2);
  masm->add(l2, l3, l2);
  Label next_frame, frames_done, copy_output, output_done;
  masm->bind(&next_frame);
  masm->cmp(l3, l2);
  masm->b(equal, ConditionCodes::kXcc, &frames_done);
  masm->nop();
  masm->LoadU64(l4, MemOperand(l3));
  masm->LoadU64(o2, MemOperand(l4, FrameDescription::frame_size_offset()));
  masm->bind(&copy_output);
  masm->cmp(o2, 0);
  masm->b(equal, ConditionCodes::kXcc, &output_done);
  masm->nop();
  masm->sub(o2, o2, kSystemPointerSize);
  masm->add(g1, l4, o2);
  masm->LoadU64(g1, MemOperand(g1, FrameDescription::frame_content_offset()));
  masm->Push(g1);
  masm->Jump(&copy_output);
  masm->bind(&output_done);
  masm->add(l3, l3, kSystemPointerSize);
  masm->Jump(&next_frame);
  masm->bind(&frames_done);

  masm->LoadU64(l1, MemOperand(l0, Deoptimizer::input_offset()));
#if V8_ENABLE_WEBASSEMBLY
  Label fp_source_ready;
  masm->LoadU64(g1, MemOperand(l4, FrameDescription::continuation_offset()));
  masm->cmp(g1, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &fp_source_ready);
  masm->nop();
  masm->Move(l1, l4);
  masm->bind(&fp_source_ready);
#endif
  for (int n = 0; n < config->num_allocatable_double_registers(); ++n) {
    int code = config->GetAllocatableDoubleCode(n);
    masm->LoadF64(DoubleRegister::from_code(code),
                  MemOperand(l1, FrameDescription::simd128_registers_offset() +
                                     code * kSimd128Size));
    masm->LoadF64(DoubleRegister::from_code(code + 16),
                  MemOperand(l1, FrameDescription::simd128_registers_offset() +
                                     code * kSimd128Size + kDoubleSize));
  }
  masm->LoadU64(g3, MemOperand(l4, FrameDescription::pc_offset()));
  masm->LoadU64(g4, MemOperand(l4, FrameDescription::continuation_offset()));
  masm->Move(g5, l4);
  for (Register reg : restored) {
    masm->LoadU64(reg, MemOperand(g5, FrameDescription::registers_offset() +
                                          reg.code() * kSystemPointerSize));
  }
  masm->Move(g2, 1);
  masm->StoreU8(g2, masm->AsMemOperand(IsolateFieldId::kStackIsIterable));
  masm->sub(o7, g3, kSparcReturnAddressOffset);
  Label return_to_pc;
  masm->cmp(g4, 0);
  masm->b(equal, ConditionCodes::kXcc, &return_to_pc);
  masm->nop();
  masm->Jump(g4);
  masm->bind(&return_to_pc);
  masm->Ret();
}

void GenerateOptimizingOSR(MacroAssembler* masm, bool from_interpreter) {
  using D = OnStackReplacementDescriptor;
  static_assert(D::MaybeTargetCodeRegister() == o0);
  Label optimized;
  masm->cmp(o0, 0);
  masm->b(not_equal, ConditionCodes::kXcc, &optimized);
  masm->nop();
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    masm->CallRuntime(Runtime::kCompileOptimizedOSR);
  }
  Label no_code;
  masm->cmp(o0, 0);
  masm->b(equal, ConditionCodes::kXcc, &no_code);
  masm->nop();
  masm->bind(&optimized);
  {
    Label no_trace;
    masm->Move(g1, ExternalReference::address_of_log_or_trace_osr());
    masm->LoadU8(g1, MemOperand(g1));
    masm->cmp(g1, 0);
    masm->b(equal, ConditionCodes::kXcc, &no_trace);
    masm->nop();
    {
      FrameScope scope(masm, StackFrame::INTERNAL);
      masm->Push(o0);
      masm->CallRuntime(Runtime::kLogOrTraceOptimizedOSREntry);
      masm->Pop(o0);
    }
    masm->bind(&no_trace);
  }
  if (from_interpreter) masm->LeaveFrame(StackFrame::STUB);
  static_assert(!V8_ENABLE_SANDBOX_BOOL);
  masm->LoadU64(o1, FieldMemOperand(
                        o0, Code::kDeoptimizationDataOrInterpreterDataOffset));
  masm->LoadU64(
      o1, FieldMemOperand(o1, FixedArray::OffsetOfElementAt(
                                  DeoptimizationData::kOsrPcOffsetIndex)));
  masm->SmiUntag(o1);
  masm->LoadU64(o0, FieldMemOperand(o0, Code::kInstructionStartOffset));
  masm->add(o0, o0, o1);
  masm->Jump(o0);
  masm->bind(&no_code);
  masm->Ret();
}

}  // namespace

void Builtins::Generate_BaselineOnStackReplacement(MacroAssembler* masm) {
  masm->LoadU64(kContextRegister,
                MemOperand(fp, BaselineFrameConstants::kContextOffset));
  GenerateOptimizingOSR(masm, false);
}

void Builtins::Generate_BaselineOutOfLinePrologue(MacroAssembler* masm) {
  const Register closure = kJSFunctionRegister;
  const Register array = kInterpreterBytecodeArrayRegister;
  const Register frame_size = kJavaScriptCallExtraArg1Register;
  const Register cell = o4;
  const Register vector = o5;
  masm->LoadU64(cell,
                FieldMemOperand(closure, JSFunction::kFeedbackCellOffset));
  masm->LoadU64(vector, FieldMemOperand(cell, FeedbackCell::kValueOffset));
  masm->LoadU8(g1, FieldMemOperand(vector, FeedbackVector::kOsrStateOffset));
  masm->and_(g1, g1, ~FeedbackVector::OsrUrgencyBits::kMask);
  masm->StoreU8(g1, FieldMemOperand(vector, FeedbackVector::kOsrStateOffset));
  masm->LoadU32(
      g1, FieldMemOperand(vector, FeedbackVector::kInvocationCountOffset));
  masm->add(g1, g1, 1);
  masm->StoreU32(
      g1, FieldMemOperand(vector, FeedbackVector::kInvocationCountOffset));
  masm->LoadU64(
      g1, FieldMemOperand(closure, JSFunction::kSharedFunctionInfoOffset));
  masm->StoreU16(g0, FieldMemOperand(g1, SharedFunctionInfo::kAgeOffset));

  FrameScope frame_scope(masm, StackFrame::MANUAL);
  masm->Push(kContextRegister, closure, kJavaScriptCallArgCountRegister, array,
             cell, vector);
  Label stack_guard;
  masm->add(g1, sp, kSparcStackBias);
  masm->sub(g1, g1, frame_size);
  masm->LoadU64(g2, MemOperand(kRootRegister, IsolateData::jslimit_offset()));
  masm->cmp(g1, g2);
  masm->b(less_unsigned, ConditionCodes::kXcc, &stack_guard);
  masm->nop();
  masm->LoadRoot(kInterpreterAccumulatorRegister, RootIndex::kUndefinedValue);
  masm->Ret();
  masm->bind(&stack_guard);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    masm->Push(kJavaScriptCallNewTargetRegister);
    masm->SmiTag(frame_size);
    masm->Push(frame_size);
    masm->CallRuntime(Runtime::kStackGuardWithGap);
    masm->Pop(kJavaScriptCallNewTargetRegister);
  }
  masm->LoadRoot(kInterpreterAccumulatorRegister, RootIndex::kUndefinedValue);
  masm->Ret();
}

void Builtins::Generate_BaselineOutOfLinePrologueDeopt(MacroAssembler* masm) {
  masm->LoadU64(kContextRegister,
                MemOperand(fp, StandardFrameConstants::kContextOffset));
  masm->LoadU64(kJavaScriptCallTargetRegister,
                MemOperand(fp, StandardFrameConstants::kFunctionOffset));
  masm->LoadU64(kJavaScriptCallArgCountRegister,
                MemOperand(fp, StandardFrameConstants::kArgCOffset));
  masm->LeaveFrame(StackFrame::BASELINE);
  masm->TailCallBuiltin(Builtin::kInterpreterEntryTrampoline);
}

void Builtins::Generate_DeoptimizationEntry_Eager(MacroAssembler* masm) {
  GenerateDeoptimizationEntry(masm, DeoptimizeKind::kEager);
}

void Builtins::Generate_DeoptimizationEntry_Lazy(MacroAssembler* masm) {
  GenerateDeoptimizationEntry(masm, DeoptimizeKind::kLazy);
}

void Builtins::Generate_DeoptimizationEntry_LazyAfterFastCall(
    MacroAssembler* masm) {
  masm->mov(g3, o7);
  masm->mov(g4, l4);
  masm->mov(g5, l2);
  masm->restore();
  masm->mov(sp, g4);
  masm->Drop(g5);
  masm->mov(o7, g3);
  masm->StoreU64(g0, masm->AsMemOperand(IsolateFieldId::kFastCCallCallerFP));
  Label no_exception;
  masm->LoadU64(g1, masm->AsMemOperand(IsolateFieldId::kException));
  masm->LoadRoot(g2, RootIndex::kTheHoleValue);
  masm->cmp(g1, g2);
  masm->b(equal, ConditionCodes::kXcc, &no_exception);
  masm->nop();
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    const RegList saved = kCalleeSaved - kNativeFramePointer - i7;
    for (Register reg : saved) masm->Push(reg);
    masm->Move(kContextRegister, Context::kNoContext);
    masm->StoreU64(g0, masm->AsMemOperand(IsolateFieldId::kFastCCallCallerFP));
    masm->CallRuntime(Runtime::kPropagateException);
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
      masm->Pop(*it);
    }
  }
  masm->bind(&no_exception);
  masm->LoadRoot(kReturnRegister0, RootIndex::kUndefinedValue);
  masm->TailCallBuiltin(Builtin::kDeoptimizationEntry_Lazy);
}

void Builtins::Generate_InterpreterOnStackReplacement(MacroAssembler* masm) {
  GenerateOptimizingOSR(masm, true);
}

void Builtins::Generate_InterpreterOnStackReplacement_ToBaseline(
    MacroAssembler* masm) {
  Label start, install;
  masm->bind(&start);
  const Register closure = o1;
  const Register code = l3;
  const Register cell = o4;
  const Register vector = o5;
  masm->LoadU64(closure,
                MemOperand(fp, StandardFrameConstants::kFunctionOffset));
  masm->LoadU64(
      code, FieldMemOperand(closure, JSFunction::kSharedFunctionInfoOffset));
  masm->StoreU16(g0, FieldMemOperand(code, SharedFunctionInfo::kAgeOffset));
  masm->LoadU64(
      code,
      FieldMemOperand(code, SharedFunctionInfo::kTrustedFunctionDataOffset));
  masm->LoadU64(cell,
                FieldMemOperand(closure, JSFunction::kFeedbackCellOffset));
  masm->LoadU64(vector, FieldMemOperand(cell, FeedbackCell::kValueOffset));
  masm->LoadMap(g1, vector);
  masm->LoadU16(g1, FieldMemOperand(g1, Map::kInstanceTypeOffset));
  masm->cmp(g1, FEEDBACK_VECTOR_TYPE);
  masm->b(not_equal, ConditionCodes::kXcc, &install);
  masm->nop();
  masm->LoadU64(
      kInterpreterBytecodeOffsetRegister,
      MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  masm->SmiUntag(kInterpreterBytecodeOffsetRegister);
  static_assert(InterpreterFrameConstants::kBytecodeOffsetFromFp ==
                BaselineFrameConstants::kFeedbackCellFromFp);
  masm->StoreU64(cell,
                 MemOperand(fp, BaselineFrameConstants::kFeedbackCellFromFp));
  masm->StoreU64(vector,
                 MemOperand(fp, BaselineFrameConstants::kFeedbackVectorFromFp));
  masm->SubS64(kInterpreterBytecodeOffsetRegister,
               kInterpreterBytecodeOffsetRegister,
               BytecodeArray::kHeaderSize - kHeapObjectTag);
  masm->LoadU64(
      kInterpreterBytecodeArrayRegister,
      MemOperand(fp, InterpreterFrameConstants::kBytecodeArrayFromFp));
  masm->Push(kInterpreterAccumulatorRegister, code);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    masm->Move(o0, code);
    masm->Move(o1, kInterpreterBytecodeOffsetRegister);
    masm->Move(o2, kInterpreterBytecodeArrayRegister);
    masm->PrepareCallCFunction(3, g3);
    masm->CallCFunction(
        ExternalReference::baseline_pc_for_next_executed_bytecode(), 3);
  }
  masm->Pop(code);
  masm->LoadU64(code, FieldMemOperand(code, Code::kInstructionStartOffset));
  masm->add(code, code, kReturnRegister0);
  masm->Pop(kInterpreterAccumulatorRegister);
  masm->Jump(code);

  masm->bind(&install);
  {
    FrameScope scope(masm, StackFrame::INTERNAL);
    masm->Push(kInterpreterAccumulatorRegister, closure);
    masm->CallRuntime(Runtime::kInstallBaselineCode, 1);
    masm->Pop(kInterpreterAccumulatorRegister);
  }
  masm->Jump(&start);
}

void Builtins::Generate_DirectCEntry(MacroAssembler* masm) {
  masm->DirectCEntry();
}

void Builtins::Generate_DoubleToI(MacroAssembler* masm) {
  masm->TruncateDoubleToIOnStack();
  masm->Ret();
}

void Builtins::Generate_CEntry(MacroAssembler* masm, int result_size,
                               ArgvMode argv_mode, bool builtin_exit_frame,
                               bool switch_to_central_stack) {
  CHECK(result_size == 1 || result_size == 2);
#if !V8_ENABLE_WEBASSEMBLY
  USE(switch_to_central_stack);
#endif
  const Register argc = l0;
  const Register target = l1;
  const Register argv = l2;
  masm->Move(argc, o0);
  masm->Move(target, o1);
  if (argv_mode == ArgvMode::kRegister) {
    masm->Move(argv, o2);
  } else {
    masm->LoadStackAddress(argv, -kSystemPointerSize);
    masm->sllx(g1, argc, kSystemPointerSizeLog2);
    masm->add(argv, argv, g1);
  }
  FrameScope frame(masm, StackFrame::MANUAL);
  masm->EnterExitFrame(
      g1, 0, builtin_exit_frame ? StackFrame::BUILTIN_EXIT : StackFrame::EXIT);
#if V8_ENABLE_WEBASSEMBLY
  if (switch_to_central_stack) {
    Label central;
    masm->Move(l3, 0);
    masm->LoadU8(g1, masm->AsMemOperand(IsolateFieldId::kIsOnCentralStackFlag));
    masm->cmp(g1, 0);
    masm->b(not_equal, ConditionCodes::kIcc, &central);
    masm->nop();
    masm->mov(l3, sp);
    masm->Move(o0, ExternalReference::isolate_address(masm->isolate()));
    masm->LoadStackAddress(o1);
    masm->PrepareCallCFunction(2, g1);
    masm->CallCFunction(ExternalReference::wasm_switch_to_the_central_stack(),
                        2, SetIsolateDataSlots::kNo);
    masm->flushw();
    masm->Move(sp, kReturnRegister0);
    masm->AllocateStackSpace(2 * kSystemPointerSize);
    masm->LoadStackAddress(g1, kSystemPointerSize);
    masm->StoreU64(g1, MemOperand(fp, ExitFrameConstants::kSPOffset));
    masm->bind(&central);
  }
#endif
  masm->Move(o0, argc);
  masm->Move(o1, argv);
  masm->LoadAddress(o2, masm->AsMemOperand(IsolateFieldId::kIsolateAddress));
  masm->StoreReturnAddressAndCall(target, 3);
  Label exception, return_ok;
  masm->LoadRoot(g1, RootIndex::kException);
  masm->cmp(o0, g1);
  masm->b(equal, ConditionCodes::kXcc, &exception);
  masm->nop();
#if V8_ENABLE_WEBASSEMBLY
  if (switch_to_central_stack) {
    Label restored;
    masm->cmp(l3, 0);
    masm->b(equal, ConditionCodes::kXcc, &restored);
    masm->nop();
    masm->flushw();
    masm->mov(sp, l3);
    masm->Push(kReturnRegister0, kReturnRegister1);
    masm->Move(o0, ExternalReference::isolate_address(masm->isolate()));
    masm->PrepareCallCFunction(1, g1);
    masm->CallCFunction(ExternalReference::wasm_switch_from_the_central_stack(),
                        1, SetIsolateDataSlots::kNo);
    masm->Pop(kReturnRegister1, kReturnRegister0);
    masm->bind(&restored);
  }
#endif
  if (v8_flags.debug_code) {
    masm->LoadU64(g1, masm->AsMemOperand(IsolateFieldId::kException));
    masm->LoadRoot(g2, RootIndex::kTheHoleValue);
    masm->cmp(g1, g2);
    masm->b(equal, ConditionCodes::kXcc, &return_ok);
    masm->nop();
    masm->Trap();
    masm->bind(&return_ok);
  }
  masm->LeaveExitFrame(g1);
  if (argv_mode == ArgvMode::kStack) masm->Drop(argc);
  masm->Ret();

  masm->bind(&exception);
#if V8_ENABLE_WEBASSEMBLY
  if (switch_to_central_stack) {
    masm->LoadRootRelative(argc, IsolateData::active_stack_offset());
    masm->mov(l4, i6);
    // The unwinder can retire the secondary stack, including its native anchor.
    masm->flushw();
    masm->mov(i6, sp);
    masm->StoreU64(i6,
                   MemOperand(i6, kSparcStackBias + 14 * kSystemPointerSize));
  }
#endif
  masm->PrepareCallCFunction(3, g1);
  masm->Move(o0, 0);
  masm->Move(o1, 0);
  masm->LoadAddress(o2, masm->AsMemOperand(IsolateFieldId::kIsolateAddress));
  masm->CallCFunction(
      ExternalReference::Create(Runtime::kUnwindAndFindExceptionHandler), 3,
      SetIsolateDataSlots::kNo);
#if V8_ENABLE_WEBASSEMBLY
  if (switch_to_central_stack) {
    Label same_stack, restored;
    masm->LoadRootRelative(g1, IsolateData::active_stack_offset());
    masm->cmp(g1, argc);
    masm->b(equal, ConditionCodes::kXcc, &same_stack);
    masm->nop();
    masm->LoadU64(g2, MemOperand(g1, wasm::kStackFpOffset));
    masm->LoadU64(i6, MemOperand(g2, kJspiNativeFpOffset));
    masm->Jump(&restored);
    masm->bind(&same_stack);
    masm->mov(i6, l4);
    masm->bind(&restored);
  }
#endif
  masm->JumpToPendingHandler();
}

#if V8_ENABLE_WEBASSEMBLY
namespace {
void LoadWasmContext(MacroAssembler* masm, Register implicit_arg) {
  Label instance, done;
  masm->LoadMap(g1, implicit_arg);
  masm->LoadU16(g1, MemOperand(g1, Map::kInstanceTypeOffset - kHeapObjectTag));
  masm->Move(g2, WASM_TRUSTED_INSTANCE_DATA_TYPE);
  masm->cmp(g1, g2);
  masm->b(equal, ConditionCodes::kIcc, &instance);
  masm->nop();
  masm->LoadU64(kContextRegister,
                MemOperand(implicit_arg, WasmImportData::kNativeContextOffset -
                                             kHeapObjectTag));
  masm->Jump(&done);
  masm->bind(&instance);
  masm->LoadU64(
      kContextRegister,
      MemOperand(implicit_arg, WasmTrustedInstanceData::kNativeContextOffset -
                                   kHeapObjectTag));
  masm->bind(&done);
}
void ResolveWasmCodePointer(MacroAssembler* masm, Register target) {
  masm->srl(g5, target, 0);
  masm->sllx(g5, g5, kSystemPointerSizeLog2);
  masm->Move(g2, ExternalReference::wasm_code_pointer_table());
  masm->add(target, g2, g5);
  masm->LoadU64(target, MemOperand(target));
}

void EnterJspiFrame(MacroAssembler* masm) {
  masm->EnterFrame(StackFrame::WASM_JSPI);
  masm->AllocateStackSpace((WasmJspiFrameConstants::kNumSpillSlots + 1) *
                           kSystemPointerSize);
  // Native FP is distinct from V8 FP and must follow the suspended stack.
  masm->StoreU64(i6, MemOperand(fp, kJspiNativeFpOffset));
  masm->StoreU64(g0,
                 MemOperand(fp, WasmJspiFrameConstants::kImplicitArgOffset));
  masm->StoreU64(g0,
                 MemOperand(fp, WasmJspiFrameConstants::kResultArrayOffset));
  masm->StoreU64(
      g0, MemOperand(fp, WasmJspiFrameConstants::kGCScanSlotCountOffset));
}

void LoadJspiJumpBuffer(MacroAssembler* masm, Register stack, bool jump) {
  masm->flushw();
  masm->LoadU64(g5, MemOperand(stack, wasm::kStackSpOffset));
  masm->LoadU64(g3, MemOperand(stack, wasm::kStackFpOffset));
  Label empty, restored;
  masm->cmp(g3, 0);
  masm->b(equal, ConditionCodes::kXcc, &empty);
  masm->nop();
  masm->LoadU64(i6, MemOperand(g3, kJspiNativeFpOffset));
  masm->Jump(&restored);
  masm->bind(&empty);
  // A fresh stack still needs a mapped native window anchor.
  masm->SubS64(i6, g5, kSparcStackSlotBias);
  masm->StoreU64(i6, MemOperand(i6, kSparcStackBias + 14 * kSystemPointerSize));
  masm->bind(&restored);
  masm->Move(fp, g3);
  masm->Move(sp, g5);
  if (jump) {
    masm->LoadU64(g5, MemOperand(stack, wasm::kStackPcOffset));
    masm->Jump(g5);
  }
}

void JspiSwitchStacks(MacroAssembler* masm, ExternalReference function,
                      Register target, Label* saved_pc, Register suspender,
                      std::initializer_list<Register> keep) {
  for (Register reg : keep) masm->Push(reg);
  const bool returning =
      function == ExternalReference::wasm_return_jspi_stack();
  int argc = returning ? 2 : suspender.is_valid() ? 6 : 5;
  FrameScope frame(masm, StackFrame::MANUAL);
  masm->PrepareCallCFunction(argc, g1);
  masm->Move(o1, target);
  if (!returning) {
    masm->LoadStackAddress(o2);
    masm->Move(o3, fp);
    int pc = masm->pc_offset();
    masm->rdpc(g4);
    masm->LoadLabelOffset(o4, saved_pc);
    masm->SubS64(o4, o4, pc);
    masm->add(o4, o4, g4);
    if (suspender.is_valid()) masm->Move(o5, suspender);
  }
  masm->Move(o0, ExternalReference::isolate_address(masm->isolate()));
  masm->CallCFunction(function, argc, SetIsolateDataSlots::kNo);
  for (Register reg : base::Reversed(keep)) masm->Pop(reg);
}

void JspiReturnToParent(MacroAssembler* masm) {
  masm->LoadRootRelative(g3, IsolateData::active_stack_offset());
  masm->StoreU64(g0, MemOperand(g3, wasm::kStackSpOffset));
  masm->LoadU64(l2, MemOperand(g3, wasm::kStackParentOffset));
  JspiSwitchStacks(masm, ExternalReference::wasm_return_jspi_stack(), l2,
                   nullptr, no_reg, {l0, l1, l2, kContextRegister});
  LoadJspiJumpBuffer(masm, l2, false);
  masm->LoadRootRelative(g3, IsolateData::active_suspender_offset());
  masm->LoadU64(g3, FieldMemOperand(g3, WasmSuspenderObject::kParentOffset));
  masm->StoreRootRelative(IsolateData::active_suspender_offset(), g3);
}

void JspiFinishPromise(MacroAssembler* masm, bool reject) {
  masm->Move(l0, kReturnRegister0);
  masm->LoadRootRelative(l1, IsolateData::active_suspender_offset());
  masm->LoadU64(l1, FieldMemOperand(l1, WasmSuspenderObject::kPromiseOffset));
  masm->LoadU64(kContextRegister,
                MemOperand(fp, WasmJspiFrameConstants::kImplicitArgOffset));
  LoadWasmContext(masm, kContextRegister);
  JspiReturnToParent(masm);
  masm->Move(g1, 1);
  masm->StoreU64(
      g1, MemOperand(fp, WasmJspiFrameConstants::kGCScanSlotCountOffset));
  masm->Push(l1);
  if (reject) {
    using D = CallInterfaceDescriptorFor<Builtin::kRejectPromise>::type;
    masm->Move(D::GetRegisterParameter(0), l1);
    masm->Move(D::GetRegisterParameter(1), l0);
    masm->LoadRoot(D::GetRegisterParameter(2), RootIndex::kTrueValue);
    masm->CallBuiltin(Builtin::kRejectPromise);
  } else {
    using D = CallInterfaceDescriptorFor<Builtin::kFulfillPromise>::type;
    masm->Move(D::GetRegisterParameter(0), l1);
    masm->Move(D::GetRegisterParameter(1), l0);
    masm->CallBuiltin(Builtin::kFulfillPromise);
  }
  masm->Pop(kReturnRegister0);
}

void GenerateJSToWasmWrapper(MacroAssembler* masm, wasm::Promise mode) {
  using F = JSToWasmWrapperFrameConstants;
  bool switching = mode != wasm::kNoPromise;
  const Register buffer =
      WasmJSToWasmWrapperDescriptor::WrapperBufferRegister();
  if (switching) {
    EnterJspiFrame(masm);
  } else {
    masm->EnterFrame(StackFrame::JS_TO_WASM);
    masm->AllocateStackSpace(WasmJspiFrameConstants::kNumSpillSlots *
                             kSystemPointerSize);
  }
  masm->LoadU64(kWasmImplicitArgRegister,
                MemOperand(fp, F::kImplicitArgOffset));
  Label suspend, return_promise;
  if (switching) {
    masm->Move(l0, fp);
    masm->LoadRootRelative(l1, IsolateData::active_suspender_offset());
    masm->LoadU64(l1, FieldMemOperand(l1, WasmSuspenderObject::kStackOffset));
    JspiSwitchStacks(masm, ExternalReference::wasm_start_stack(), l1, &suspend,
                     no_reg, {l0, buffer, kWasmImplicitArgRegister});
    masm->LoadRootRelative(l1, IsolateData::active_stack_offset());
    LoadJspiJumpBuffer(masm, l1, false);
    EnterJspiFrame(masm);
    masm->StoreU64(kWasmImplicitArgRegister,
                   MemOperand(fp, WasmJspiFrameConstants::kImplicitArgOffset));
    masm->LoadU64(g1, MemOperand(l0, F::kResultArrayParamOffset));
    masm->StoreU64(g1,
                   MemOperand(fp, WasmJspiFrameConstants::kResultArrayOffset));
    masm->AllocateStackSpace(RoundUp(F::kWrapperBufferSize, 16));
    masm->LoadStackAddress(l1);
    for (int offset :
         {static_cast<int>(F::kWrapperBufferReturnCount),
          static_cast<int>(F::kWrapperBufferSigRepresentationArray)}) {
      masm->LoadU64(g1, MemOperand(buffer, offset));
      masm->StoreU64(g1, MemOperand(l1, offset));
    }
    masm->StoreU64(l1, MemOperand(fp, F::kWrapperBufferOffset));
  } else {
    masm->StoreU64(buffer, MemOperand(fp, F::kWrapperBufferOffset));
  }
  masm->LoadU64(g1, MemOperand(buffer, F::kWrapperBufferStackReturnBufferSize));
  masm->sllx(g1, g1, kSystemPointerSizeLog2);
  masm->sub(sp, sp, g1);
  masm->LoadStackAddress(g1);
  masm->LoadU64(l1, MemOperand(fp, F::kWrapperBufferOffset));
  masm->StoreU64(g1, MemOperand(l1, F::kWrapperBufferStackReturnBufferStart));
  masm->LoadU64(g3, MemOperand(buffer, F::kWrapperBufferParamStart));
  masm->LoadU64(g4, MemOperand(buffer, F::kWrapperBufferParamEnd));
  constexpr int register_params_size =
      (arraysize(wasm::kGpParamRegisters) - 1) * kSystemPointerSize +
      arraysize(wasm::kFpParamRegisters) * kDoubleSize;
  masm->AddS64(g5, g3, register_params_size);
  Label loop, done;
  masm->bind(&loop);
  masm->cmp(g4, g5);
  masm->b(less_equal_unsigned, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->SubS64(g4, g4, kSystemPointerSize);
  masm->LoadU64(g1, MemOperand(g4));
  masm->Push(g1);
  masm->ba(&loop);
  masm->nop();
  masm->bind(&done);
  int offset = 0;
  for (size_t i = 1; i < arraysize(wasm::kGpParamRegisters); ++i) {
    masm->LoadU64(wasm::kGpParamRegisters[i], MemOperand(g3, offset));
    offset += kSystemPointerSize;
  }
  for (auto reg : wasm::kFpParamRegisters) {
    masm->LoadF64(reg, MemOperand(g3, offset));
    offset += kDoubleSize;
  }
  masm->StoreU64(
      g0, MemOperand(fp, WasmJspiFrameConstants::kGCScanSlotCountOffset));
  masm->LoadU32(g1, MemOperand(buffer, F::kWrapperBufferCallTarget));
  ResolveWasmCodePointer(masm, g1);
  masm->Call(g1);
  masm->LoadU64(buffer, MemOperand(fp, F::kWrapperBufferOffset));
  masm->StoreF64(wasm::kFpReturnRegisters[0],
                 MemOperand(buffer, F::kWrapperBufferFPReturnRegister1));
  masm->StoreF64(wasm::kFpReturnRegisters[1],
                 MemOperand(buffer, F::kWrapperBufferFPReturnRegister2));
  masm->StoreU64(wasm::kGpReturnRegisters[0],
                 MemOperand(buffer, F::kWrapperBufferGPReturnRegister1));
  masm->StoreU64(wasm::kGpReturnRegisters[1],
                 MemOperand(buffer, F::kWrapperBufferGPReturnRegister2));
  using D = v8::internal::CallInterfaceDescriptorFor<
      Builtin::kJSToWasmHandleReturns>::type;
  masm->LoadU64(
      D::GetRegisterParameter(0),
      MemOperand(fp, switching ? WasmJspiFrameConstants::kImplicitArgOffset
                               : F::kImplicitArgOffset));
  masm->LoadU64(
      D::GetRegisterParameter(1),
      MemOperand(fp, switching ? WasmJspiFrameConstants::kResultArrayOffset
                               : F::kResultArrayParamOffset));
  masm->Move(D::GetRegisterParameter(2), buffer);
  LoadWasmContext(masm, D::GetRegisterParameter(0));
  masm->Move(D::GetRegisterParameter(0), kContextRegister);
  masm->CallBuiltin(Builtin::kJSToWasmHandleReturns);
  if (mode == wasm::kPromise) {
    JspiFinishPromise(masm, false);
  } else if (switching) {
    masm->Move(l0, kReturnRegister0);
    JspiReturnToParent(masm);
    masm->Move(kReturnRegister0, l0);
  }
  masm->bind(&return_promise);
  masm->bind(&suspend);
  masm->LeaveFrame(switching ? StackFrame::WASM_JSPI : StackFrame::JS_TO_WASM,
                   2 * kSystemPointerSize);
  masm->Ret();
  if (mode == wasm::kPromise) {
    int handler = masm->pc_offset();
    JspiFinishPromise(masm, true);
    masm->Jump(&return_promise);
    masm->isolate()->builtins()->SetJSPIPromptHandlerOffset(handler);
  }
}
}  // namespace

void Builtins::Generate_JSToWasmWrapperAsm(MacroAssembler* masm) {
  GenerateJSToWasmWrapper(masm, wasm::kNoPromise);
}

void Builtins::Generate_WasmToJsWrapperAsm(MacroAssembler* masm) {
  for (size_t i = arraysize(wasm::kFpParamRegisters); i > 0; --i) {
    masm->AllocateStackSpace(kDoubleSize);
    masm->StoreF64(wasm::kFpParamRegisters[i - 1], MemOperand(sp));
  }
  for (size_t i = arraysize(wasm::kGpParamRegisters); i > 1; --i)
    masm->Push(wasm::kGpParamRegisters[i - 1]);
  masm->Push(g0);
  masm->TailCallBuiltin(Builtin::kWasmToJsWrapperCSA);
}

void Builtins::Generate_WasmLiftoffFrameSetup(MacroAssembler* masm) {
  Register func_index = wasm::kLiftoffFrameSetupFunctionReg;
  Register vector = g5;
  Label allocate_vector, done;
  masm->LoadU64(
      vector, FieldMemOperand(kWasmImplicitArgRegister,
                              WasmTrustedInstanceData::kFeedbackVectorsOffset));
  masm->sllx(g1, func_index, kTaggedSizeLog2);
  masm->add(vector, vector, g1);
  masm->LoadU64(vector,
                FieldMemOperand(vector, OFFSET_OF_DATA_START(FixedArray)));
  masm->andcc(g0, vector, kSmiTagMask);
  masm->b(equal, ConditionCodes::kXcc, &allocate_vector);
  masm->nop();
  masm->bind(&done);
  masm->LoadU64(g1, FieldMemOperand(vector, OFFSET_OF_DATA_START(FixedArray)));
  masm->Move(g2, Smi::FromInt(1));
  masm->add(g1, g1, g2);
  masm->StoreU64(g1, FieldMemOperand(vector, OFFSET_OF_DATA_START(FixedArray)));
  masm->Push(kWasmImplicitArgRegister);
  masm->Push(vector);
  masm->Ret();

  masm->bind(&allocate_vector);
  masm->Move(g1, StackFrame::TypeToMarker(StackFrame::WASM_LIFTOFF_SETUP));
  masm->StoreU64(g1, MemOperand(fp, TypedFrameConstants::kFrameTypeOffset));
  masm->Push(kWasmImplicitArgRegister);
  for (size_t i = arraysize(wasm::kGpParamRegisters); i > 1; --i) {
    masm->Push(wasm::kGpParamRegisters[i - 1]);
  }
  masm->Push(o7);
  masm->Push(kContextRegister);
  for (auto reg : wasm::kFpParamRegisters) {
    masm->AllocateStackSpace(kSimd128Size);
    masm->StoreF64(reg, MemOperand(sp));
    masm->StoreF64(DoubleRegister::from_code(reg.code() + 16),
                   MemOperand(sp, 8));
  }
  masm->Push(kWasmImplicitArgRegister);
  masm->Move(g1, func_index);
  masm->SmiTag(g1);
  masm->Push(g1);
  masm->Push(g0);
  masm->Move(kContextRegister, Smi::zero());
  masm->CallRuntime(Runtime::kWasmAllocateFeedbackVector, 3);
  masm->Move(vector, kReturnRegister0);
  for (size_t i = arraysize(wasm::kFpParamRegisters); i > 0; --i) {
    DoubleRegister reg = wasm::kFpParamRegisters[i - 1];
    masm->LoadF64(reg, MemOperand(sp));
    masm->LoadF64(DoubleRegister::from_code(reg.code() + 16),
                  MemOperand(sp, 8));
    masm->Drop(2);
  }
  masm->Pop(kContextRegister);
  masm->Pop(o7);
  for (size_t i = 1; i < arraysize(wasm::kGpParamRegisters); ++i) {
    masm->Pop(wasm::kGpParamRegisters[i]);
  }
  masm->Pop(kWasmImplicitArgRegister);
  masm->Move(g1, StackFrame::TypeToMarker(StackFrame::WASM));
  masm->StoreU64(g1, MemOperand(fp, TypedFrameConstants::kFrameTypeOffset));
  masm->ba(&done);
  masm->nop();
}
void Builtins::Generate_WasmDebugBreak(MacroAssembler* masm) {
  using F = WasmDebugBreakFrameConstants;
  HardAbortScope hard_abort(masm);
  masm->EnterFrame(StackFrame::WASM_DEBUG_BREAK);
  masm->AllocateStackSpace(F::kNumPushedGpRegisters * kSystemPointerSize);
  for (Register reg : F::kPushedGpRegs) {
    masm->StoreU64(reg,
                   MemOperand(fp, F::GetPushedGpRegisterOffset(reg.code())));
  }
  masm->AllocateStackSpace(F::kNumPushedFpRegisters * kSimd128Size);
  for (DoubleRegister reg : F::kPushedFpRegs) {
    int offset = F::GetPushedFpRegisterOffset(reg.code());
    masm->StoreF64(reg, MemOperand(fp, offset));
    masm->StoreF64(DoubleRegister::from_code(reg.code() + 16),
                   MemOperand(fp, offset + kDoubleSize));
  }
  masm->Move(kContextRegister, Smi::zero());
  masm->CallRuntime(Runtime::kWasmDebugBreak, 0);
  for (DoubleRegister reg : F::kPushedFpRegs) {
    int offset = F::GetPushedFpRegisterOffset(reg.code());
    masm->LoadF64(reg, MemOperand(fp, offset));
    masm->LoadF64(DoubleRegister::from_code(reg.code() + 16),
                  MemOperand(fp, offset + kDoubleSize));
  }
  for (Register reg : F::kPushedGpRegs) {
    masm->LoadU64(reg,
                  MemOperand(fp, F::GetPushedGpRegisterOffset(reg.code())));
  }
  masm->LeaveFrame(StackFrame::WASM_DEBUG_BREAK);
  masm->Ret();
}
void Builtins::Generate_WasmOnStackReplace(MacroAssembler* masm) {
  masm->Trap();
}
void Builtins::Generate_WasmHandleStackOverflow(MacroAssembler* masm) {
  masm->Trap();
}
void Builtins::Generate_WasmReturnPromiseOnSuspendAsm(MacroAssembler* masm) {
  GenerateJSToWasmWrapper(masm, wasm::kPromise);
}
void Builtins::Generate_JSToWasmStressSwitchStacksAsm(MacroAssembler* masm) {
  GenerateJSToWasmWrapper(masm, wasm::kStressSwitch);
}
void Builtins::Generate_WasmSuspend(MacroAssembler* masm) {
  EnterJspiFrame(masm);
  masm->Move(l0, kReturnRegister0);
  masm->LoadU64(l2, FieldMemOperand(l0, WasmSuspenderObject::kParentOffset));
  masm->LoadU64(l1, FieldMemOperand(l2, WasmSuspenderObject::kStackOffset));
  Label resume;
  JspiSwitchStacks(masm, ExternalReference::wasm_suspend_stack(), l1, &resume,
                   no_reg, {l0, l1, l2});
  masm->StoreRootRelative(IsolateData::active_suspender_offset(), l2);
  masm->LoadU64(kReturnRegister0,
                FieldMemOperand(l0, WasmSuspenderObject::kPromiseOffset));
  LoadJspiJumpBuffer(masm, l1, true);
  masm->bind(&resume);
  masm->LeaveFrame(StackFrame::WASM_JSPI);
  masm->Ret();
}

namespace {
void GenerateWasmResume(MacroAssembler* masm, bool reject) {
  EnterJspiFrame(masm);
  masm->LoadU64(l0, FieldMemOperand(kJSFunctionRegister,
                                    JSFunction::kSharedFunctionInfoOffset));
  masm->LoadU64(l0, FieldMemOperand(
                        l0, SharedFunctionInfo::kUntrustedFunctionDataOffset));
  masm->LoadU64(l1,
                FieldMemOperand(l0, WasmResumeData::kTrustedSuspenderOffset));
  masm->LoadU64(l0, FieldMemOperand(l1, WasmSuspenderObject::kStackOffset));
  Label suspend;
  JspiSwitchStacks(masm, ExternalReference::wasm_resume_jspi_stack(), l0,
                   &suspend, l1, {l0});
  masm->LoadU64(kReturnRegister0, MemOperand(fp, 3 * kSystemPointerSize));
  LoadJspiJumpBuffer(masm, l0, !reject);
  if (reject) {
    masm->LeaveFrame(StackFrame::WASM_JSPI);
    masm->Push(kReturnRegister0);
    masm->CallRuntime(Runtime::kThrow, 1);
  }
  masm->bind(&suspend);
  masm->LeaveFrame(StackFrame::WASM_JSPI, 2 * kSystemPointerSize);
  masm->Ret();
}
}  // namespace

void Builtins::Generate_WasmResume(MacroAssembler* masm) {
  GenerateWasmResume(masm, false);
}
void Builtins::Generate_WasmReject(MacroAssembler* masm) {
  GenerateWasmResume(masm, true);
}
void Builtins::Generate_WasmFXResume(MacroAssembler* masm) { masm->Trap(); }
void Builtins::Generate_WasmFXResumeThrow(MacroAssembler* masm) {
  masm->Trap();
}
void Builtins::Generate_WasmFXSuspend(MacroAssembler* masm) { masm->Trap(); }
void Builtins::Generate_WasmFXReturn(MacroAssembler* masm) { masm->Trap(); }
void Builtins::Generate_WasmTrapHandlerLandingPad(MacroAssembler* masm) {
  masm->Trap();
}

void Builtins::Generate_WasmCompileLazy(MacroAssembler* masm) {
  masm->EnterFrame(StackFrame::INTERNAL);
  for (auto reg : wasm::kFpParamRegisters) {
    masm->AllocateStackSpace(kSimd128Size);
    masm->StoreF64(reg, MemOperand(sp));
    masm->StoreF64(DoubleRegister::from_code(reg.code() + 16),
                   MemOperand(sp, 8));
  }
  for (auto reg : wasm::kGpParamRegisters) masm->Push(reg);
  masm->Push(kWasmImplicitArgRegister);
  masm->Move(g1, kWasmCompileLazyFuncIndexRegister);
  masm->SmiTag(g1);
  masm->Push(g1);
  masm->Move(kContextRegister, 0);
  masm->CallRuntime(Runtime::kWasmCompileLazy, 2);
  masm->SmiUntag(g5, kReturnRegister0);
  for (size_t i = arraysize(wasm::kGpParamRegisters); i > 0; --i)
    masm->Pop(wasm::kGpParamRegisters[i - 1]);
  for (size_t i = arraysize(wasm::kFpParamRegisters); i > 0; --i) {
    DoubleRegister reg = wasm::kFpParamRegisters[i - 1];
    masm->LoadF64(reg, MemOperand(sp));
    masm->LoadF64(DoubleRegister::from_code(reg.code() + 16),
                  MemOperand(sp, 8));
    masm->Drop(2);
  }
  masm->LoadU64(g1, MemOperand(kWasmImplicitArgRegister,
                               WasmTrustedInstanceData::kJumpTableStartOffset -
                                   kHeapObjectTag));
  masm->add(g5, g1, g5);
  masm->LeaveFrame(StackFrame::INTERNAL);
  masm->Jump(g5);
}
#endif

}  // namespace v8::internal
