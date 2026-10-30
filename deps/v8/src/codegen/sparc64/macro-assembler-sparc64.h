// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_MACRO_ASSEMBLER_SPARC64_H_
#define V8_CODEGEN_SPARC64_MACRO_ASSEMBLER_SPARC64_H_

#ifndef INCLUDED_FROM_MACRO_ASSEMBLER_H
#error This header must be included via macro-assembler.h
#endif

#include "src/codegen/bailout-reason.h"
#include "src/execution/frame-constants.h"
#include "src/objects/smi.h"

namespace v8::internal {

enum class ArgumentsElementType { kRaw, kHandle };

class MemOperand {
 public:
  explicit MemOperand(Register base, int32_t offset = 0)
      : base_(base), offset_(offset) {}
  Register base() const { return base_; }
  int64_t offset() const {
    return int64_t{offset_} + (base_ == sp ? kSparcStackSlotBias : 0);
  }

 private:
  Register base_;
  int32_t offset_;
};

inline MemOperand FieldMemOperand(Register object, int offset) {
  return MemOperand(object, offset - kHeapObjectTag);
}

class Operand {
 public:
  explicit Operand(Register reg) : reg_(reg), immediate_(0) {}
  explicit Operand(intptr_t immediate) : reg_(no_reg), immediate_(immediate) {}
  bool is_reg() const { return reg_.is_valid(); }
  Register rm() const { return reg_; }
  intptr_t immediate() const { return immediate_; }

 private:
  Register reg_;
  intptr_t immediate_;
};

class V8_EXPORT_PRIVATE MacroAssembler : public MacroAssemblerBase {
 public:
  using MacroAssemblerBase::MacroAssemblerBase;

  void Move(Register dst, Register src);
  void Move(Register dst, MemOperand src) { LoadU64(dst, src); }
  void DebugBreak() { Trap(); }
  void Abort(AbortReason reason);
  void SmiTag(Register reg) { sllx(reg, reg, kSmiShiftSize + kSmiTagSize); }
  void SmiUntag(Register dst, Register src) {
    srax(dst, src, kSmiShiftSize + kSmiTagSize);
  }
  void SmiUntag(Register reg) { SmiUntag(reg, reg); }
  void SmiToInt32(Register dst, Register src) { SmiUntag(dst, src); }
  void SmiToInt32(Register reg) { SmiUntag(reg); }
  void LoadMap(Register dst, Register object) {
    LoadU64(dst, FieldMemOperand(object, HeapObject::kMapOffset));
  }
  void LoadNativeContextSlot(Register dst, int index);
  void Assert(Condition condition, AbortReason reason) {
    if (!v8_flags.debug_code) return;
    Label ok;
    b(condition, ConditionCodes::kXcc, &ok);
    nop();
    Abort(reason);
    bind(&ok);
  }
  void AssertFeedbackCell(Register object, Register scratch);
  template <typename Field>
  void DecodeField(Register reg) {
    srlx(reg, reg, Field::kShift);
    Register scratch = Scratch(reg);
    Move(scratch, Field::kMask >> Field::kShift);
    and_(reg, reg, scratch);
  }
  void Move(Register dst, intptr_t value);
  void Move(Register dst, ExternalReference reference);
  void Move(Register dst, Handle<HeapObject> value);
  void MoveNumber(Register dst, double value);
  void Move(Register dst, Tagged<Smi> value) {
    Move(dst, static_cast<intptr_t>(value.ptr()));
  }
  void AddS64(Register dst, Register src, intptr_t value);
  void SubS64(Register dst, Register src, intptr_t value);
  void LoadU64(Register dst, MemOperand src);
  void StoreU64(Register src, MemOperand dst);
  void LoadU32(Register dst, MemOperand src);
  void LoadS32(Register dst, MemOperand src);
  void LoadU8(Register dst, MemOperand src);
  void LoadS8(Register dst, MemOperand src);
  void LoadU16(Register dst, MemOperand src);
  void LoadS16(Register dst, MemOperand src);
  void StoreU8(Register src, MemOperand dst);
  void StoreU16(Register src, MemOperand dst);
  void StoreU32(Register src, MemOperand dst);
  void LoadF32(FloatRegister dst, MemOperand src);
  void StoreF32(FloatRegister src, MemOperand dst);
  void LoadF64(DoubleRegister dst, MemOperand src);
  void StoreF64(DoubleRegister src, MemOperand dst);
  void MovIntToFloat(FloatRegister dst, Register src);
  void MovFloatToInt(Register dst, FloatRegister src);
  void MovInt64ToDouble(DoubleRegister dst, Register src);
  void MovDoubleToInt64(Register dst, DoubleRegister src);
  void LoadAddress(Register dst, MemOperand src);
  void LoadUnaligned(Register dst, MemOperand src, int bytes,
                     bool is_signed = false);
  void StoreUnaligned(Register src, MemOperand dst, int bytes);

  void Push(Register src);
  void Pop(Register dst);
  template <typename... Registers>
  void Push(Register first, Register second, Registers... rest) {
    Push(first);
    Push(second);
    (Push(rest), ...);
  }
  template <typename... Registers>
  void Pop(Register first, Register second, Registers... rest) {
    Pop(first);
    Pop(second);
    (Pop(rest), ...);
  }
  void AllocateStackSpace(int bytes);
  void Drop(int slots);
  void Drop(Register slots);
  void PushArguments(Register array, Register argc, Register scratch,
                     ArgumentsElementType type);
  void StackOverflowCheck(Register num_args, Register scratch, Label* overflow);
  void LoadStackAddress(Register dst, int32_t offset = 0);
  void PushCommonFrame(Register marker = no_reg);
  void PushStandardFrame(Register function);
  void EnterCFunctionFrame();
  void RetCFunction();
  void RestoreFrameStateForTailCall();
  MemOperand AsMemOperand(IsolateFieldId id);
  void EnterJSEntryFrame(StackFrame::Type type);
  void LeaveJSEntryFrame();
  void PushStackHandler();
  void PopStackHandler();
  void EnterExitFrame(Register scratch, int stack_space,
                      StackFrame::Type frame_type);
  void LeaveExitFrame(Register scratch);
  void JumpToPendingHandler();
  // The call site must be immovable when the callee can trigger GC.
  int StoreReturnAddressAndCall(Register target, int num_arguments);
  void DirectCEntry();
  void EnterFrame(StackFrame::Type type,
                  bool load_constant_pool_pointer_reg = false);
  int LeaveFrame(StackFrame::Type type, int stack_adjustment = 0);
  void Ret();
  void Call(Register target);
  void Jump(Register target);
  void Jump(Label* target);
  void Jump(const ExternalReference& reference);
  void Trap() { ta(1); }
  void jmp(Label* target) { Jump(target); }
  void JumpIfEqual(Register value, int32_t constant, Label* target);
  void JumpIfLessThan(Register value, int32_t constant, Label* target);
  void InitializeRootRegister();
  void CallBuiltin(Builtin target);
  void TailCallBuiltin(Builtin target);
  void CallRuntime(const Runtime::Function* function, int num_arguments);
  void CallRuntime(Runtime::FunctionId id);
  void CallRuntime(Runtime::FunctionId id, int num_arguments);
  void TailCallRuntime(Runtime::FunctionId id);
  void AdaptArguments(Register expected, Register actual, Label* overflow);
  void AllocateSpaceAndShiftArguments(Register count, Register argc,
                                      Register first_free);
  void JumpJSFunction(Register function);
  void InvokeFunctionCode(Register function, Register new_target,
                          Register expected, Register actual, InvokeType type);
  void LoadInterpreterDataBytecodeArray(Register dst, Register data);
  void LoadInterpreterDataInterpreterTrampoline(Register dst, Register data);
  void DispatchInterpreterBytecode();
  void TruncateDoubleBitsToInt32(Register result, Register bits);
  void TruncateDoubleToI(Register result, DoubleRegister input);
  void TruncateDoubleToIOnStack();
  void RecordWriteField(Register object, int offset, Register value,
                        SaveFPRegsMode fp_mode);
  void RecordWrite(Register object, Register slot, Register value,
                   SaveFPRegsMode fp_mode, bool ephemeron = false);
  void ContinueToBuiltin(bool javascript_builtin, bool with_result);
  void AdvanceBytecodeOffsetOrReturn(Register array, Register offset,
                                     Register bytecode, Register size_table,
                                     Register original_offset,
                                     Label* if_return);
  void CallForDeoptimization(Builtin target, int deopt_id, Label* exit,
                             DeoptimizeKind kind, Label* ret,
                             Label* jump_deoptimization_entry_label);

  void LoopHeaderAlign() { Align(16); }
  void SwitchTargetAlign() { Align(kInstrSize); }
  void BranchTargetAlign() { Align(kInstrSize); }
  // SPARC has no additional control-flow integrity entry instructions.
  void CodeEntry() {}
  void ExceptionHandler() {}
  void BindExceptionHandler(Label* target) { bind(target); }

  void PrepareCallCFunction(int num_arguments, Register scratch);
  // The callee must not trigger GC; runtime calls need a CEntry exit frame.
  int CallCFunction(
      Register function, int num_arguments,
      SetIsolateDataSlots set_isolate_data_slots = SetIsolateDataSlots::kYes);
  int CallCFunction(
      ExternalReference function, int num_arguments,
      SetIsolateDataSlots set_isolate_data_slots = SetIsolateDataSlots::kYes);

  void LoadFromConstantsTable(Register dst, int constant_index) override;
  void LoadRootRegisterOffset(Register dst, intptr_t offset) override;
  void LoadRootRelative(Register dst, int32_t offset) override;
  void StoreRootRelative(int32_t offset, Register src) override;
  void LoadRoot(Register dst, RootIndex index) override;

  static int ActivationFrameAlignment() { return kSparcStackAlignment; }

 private:
  static Register Scratch(Register first, Register second = no_reg,
                          Register third = no_reg);
  MemOperand ResolveAddress(MemOperand operand, Register exclude);
};

struct MoveCycleState {
  bool pending_double_scratch_register_use = false;
};

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_MACRO_ASSEMBLER_SPARC64_H_
