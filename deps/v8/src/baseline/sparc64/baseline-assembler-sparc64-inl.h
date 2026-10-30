// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_BASELINE_SPARC64_BASELINE_ASSEMBLER_SPARC64_INL_H_
#define V8_BASELINE_SPARC64_BASELINE_ASSEMBLER_SPARC64_INL_H_

#include "src/baseline/baseline-assembler.h"
#include "src/codegen/interface-descriptors.h"
#include "src/codegen/sparc64/assembler-sparc64-inl.h"
#include "src/codegen/sparc64/register-sparc64.h"
#include "src/objects/literal-objects-inl.h"

namespace v8 {
namespace internal {
namespace baseline {

static constexpr Condition kZero = equal;
static constexpr Condition kNotZero = not_equal;
static constexpr Condition kEqual = equal;
static constexpr Condition kNotEqual = not_equal;
static constexpr Condition kLessThan = less;
static constexpr Condition kLessThanEqual = less_equal;
static constexpr Condition kGreaterThan = greater;
static constexpr Condition kGreaterThanEqual = greater_equal;
static constexpr Condition kUnsignedLessThan = less_unsigned;
static constexpr Condition kUnsignedLessThanEqual = less_equal_unsigned;
static constexpr Condition kUnsignedGreaterThan = greater_unsigned;
static constexpr Condition kUnsignedGreaterThanEqual = greater_equal_unsigned;

namespace detail {

static constexpr Register kScratchRegisters[] = {i3, i4, i5};
static constexpr int kNumScratchRegisters = arraysize(kScratchRegisters);

#ifdef DEBUG
inline bool Clobbers(Register target, MemOperand op) {
  return op.base() == target;
}
#endif
}  // namespace detail

class BaselineAssembler::ScratchRegisterScope {
 public:
  explicit ScratchRegisterScope(BaselineAssembler* assembler)
      : assembler_(assembler),
        prev_scope_(assembler->scratch_register_scope_),
        registers_used_(prev_scope_ == nullptr ? 0
                                               : prev_scope_->registers_used_) {
    assembler_->scratch_register_scope_ = this;
  }
  ~ScratchRegisterScope() { assembler_->scratch_register_scope_ = prev_scope_; }

  Register AcquireScratch() {
    CHECK_LT(registers_used_, detail::kNumScratchRegisters);
    return detail::kScratchRegisters[registers_used_++];
  }

 private:
  BaselineAssembler* assembler_;
  ScratchRegisterScope* prev_scope_;
  int registers_used_;
};

static void JumpToLabel(MacroAssembler* assm, Label* target) {
  // CALL's 30-bit displacement reaches across large baseline functions.
  // Preserve the link register in the delay slot; g4 is macro scratch.
  assm->mov(g4, o7);
  assm->call(target);
  assm->mov(o7, g4);
}

static void BranchToLabel(MacroAssembler* assm, Condition cc, Label* target) {
  Label skip;
  assm->b(static_cast<Condition>(cc ^ 8), ConditionCodes::kXcc, &skip);
  assm->nop();
  JumpToLabel(assm, target);
  assm->bind(&skip);
}

static void JumpIfHelper(MacroAssembler* assm, Condition cc, Register lhs,
                         Register rhs, Label* target) {
  assm->cmp(lhs, rhs);
  BranchToLabel(assm, cc, target);
}

#define __ masm_->

MemOperand BaselineAssembler::RegisterFrameOperand(
    interpreter::Register interpreter_register) {
  return MemOperand(fp, interpreter_register.ToOperand() * kSystemPointerSize);
}
void BaselineAssembler::RegisterFrameAddress(
    interpreter::Register interpreter_register, Register rscratch) {
  __ AddS64(rscratch, fp,
            interpreter_register.ToOperand() * kSystemPointerSize);
}
MemOperand BaselineAssembler::FeedbackVectorOperand() {
  return MemOperand(fp, BaselineFrameConstants::kFeedbackVectorFromFp);
}
MemOperand BaselineAssembler::FeedbackCellOperand() {
  return MemOperand(fp, BaselineFrameConstants::kFeedbackCellFromFp);
}

void BaselineAssembler::Bind(Label* label) { __ bind(label); }

void BaselineAssembler::JumpTarget() {}

void BaselineAssembler::Jump(Label* target, Label::Distance distance) {
  JumpToLabel(masm_, target);
}

void BaselineAssembler::JumpIfRoot(Register value, RootIndex index,
                                   Label* target, Label::Distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ LoadRoot(scratch, index);
  JumpIfHelper(masm_, equal, value, scratch, target);
}

void BaselineAssembler::JumpIfNotRoot(Register value, RootIndex index,
                                      Label* target, Label::Distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ LoadRoot(scratch, index);
  JumpIfHelper(masm_, not_equal, value, scratch, target);
}

void BaselineAssembler::JumpIfSmi(Register value, Label* target,
                                  Label::Distance) {
  __ andcc(g0, value, kSmiTagMask);
  BranchToLabel(masm_, equal, target);
}

void BaselineAssembler::JumpIfImmediate(Condition cc, Register left, int right,
                                        Label* target,
                                        Label::Distance distance) {
  ASM_CODE_COMMENT(masm_);
  JumpIf(cc, left, Operand(right), target, distance);
}

void BaselineAssembler::JumpIfNotSmi(Register value, Label* target,
                                     Label::Distance) {
  __ andcc(g0, value, kSmiTagMask);
  BranchToLabel(masm_, not_equal, target);
}

void BaselineAssembler::TestAndBranch(Register value, int mask, Condition cc,
                                      Label* target, Label::Distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ Move(scratch, mask);
  __ andcc(g0, value, scratch);
  BranchToLabel(masm_, cc, target);
}

void BaselineAssembler::JumpIf(Condition cc, Register lhs, const Operand& rhs,
                               Label* target, Label::Distance) {
  if (rhs.is_reg()) {
    JumpIfHelper(masm_, cc, lhs, rhs.rm(), target);
  } else {
    DCHECK_NE(lhs, g5);
    __ Move(g5, rhs.immediate());
    JumpIfHelper(masm_, cc, lhs, g5, target);
  }
}

void BaselineAssembler::JumpIfObjectTypeFast(Condition cc, Register object,
                                             InstanceType instance_type,
                                             Label* target,
                                             Label::Distance distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ LoadMap(scratch, object);
  __ LoadU16(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
  JumpIf(cc, scratch, Operand(instance_type), target, distance);
}

void BaselineAssembler::JumpIfObjectType(Condition cc, Register object,
                                         InstanceType instance_type,
                                         Register map, Label* target,
                                         Label::Distance) {
  ScratchRegisterScope temps(this);
  Register type = temps.AcquireScratch();
  __ LoadMap(map, object);
  __ LoadU16(type, FieldMemOperand(map, Map::kInstanceTypeOffset));
  JumpIf(cc, type, Operand(instance_type), target);
}

void BaselineAssembler::JumpIfInstanceType(Condition cc, Register map,
                                           InstanceType instance_type,
                                           Label* target, Label::Distance) {
  ScratchRegisterScope temps(this);
  Register type = temps.AcquireScratch();
  __ LoadU16(type, FieldMemOperand(map, Map::kInstanceTypeOffset));
  JumpIf(cc, type, Operand(instance_type), target);
}

void BaselineAssembler::JumpIfPointer(Condition cc, Register value,
                                      MemOperand operand, Label* target,
                                      Label::Distance) {
  ASM_CODE_COMMENT(masm_);
  ScratchRegisterScope temps(this);
  Register tmp = temps.AcquireScratch();
  __ LoadU64(tmp, operand);
  JumpIfHelper(masm_, cc, value, tmp, target);
}

void BaselineAssembler::JumpIfSmi(Condition cc, Register value, Tagged<Smi> smi,
                                  Label* target, Label::Distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ Move(scratch, smi);
  JumpIfHelper(masm_, cc, value, scratch, target);
}

void BaselineAssembler::JumpIfSmi(Condition cc, Register lhs, Register rhs,
                                  Label* target, Label::Distance) {
  JumpIfHelper(masm_, cc, lhs, rhs, target);
}

void BaselineAssembler::JumpIfTagged(Condition cc, Register value,
                                     MemOperand operand, Label* target,
                                     Label::Distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ LoadU64(scratch, operand);
  JumpIfHelper(masm_, cc, value, scratch, target);
}

void BaselineAssembler::JumpIfTagged(Condition cc, MemOperand operand,
                                     Register value, Label* target,
                                     Label::Distance) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ LoadU64(scratch, operand);
  JumpIfHelper(masm_, cc, scratch, value, target);
}

void BaselineAssembler::JumpIfByte(Condition cc, Register value, int32_t byte,
                                   Label* target, Label::Distance) {
  ASM_CODE_COMMENT(masm_);
  JumpIf(cc, value, Operand(byte), target);
}

void BaselineAssembler::Move(interpreter::Register output, Register source) {
  ASM_CODE_COMMENT(masm_);
  Move(RegisterFrameOperand(output), source);
}

void BaselineAssembler::Move(Register output, Tagged<TaggedIndex> value) {
  ASM_CODE_COMMENT(masm_);
  __ Move(output, static_cast<intptr_t>(value.ptr()));
}

void BaselineAssembler::Move(MemOperand output, Register source) {
  ASM_CODE_COMMENT(masm_);
  __ StoreU64(source, output);
}

void BaselineAssembler::Move(Register output, ExternalReference reference) {
  ASM_CODE_COMMENT(masm_);
  __ Move(output, reference);
}

void BaselineAssembler::Move(Register output, Handle<HeapObject> value) {
  ASM_CODE_COMMENT(masm_);
  __ Move(output, value);
}

void BaselineAssembler::Move(Register output, int32_t value) {
  ASM_CODE_COMMENT(masm_);
  __ Move(output, value);
}

void BaselineAssembler::MoveMaybeSmi(Register output, Register source) {
  ASM_CODE_COMMENT(masm_);
  __ Move(output, source);
}

void BaselineAssembler::MoveSmi(Register output, Register source) {
  ASM_CODE_COMMENT(masm_);
  __ Move(output, source);
}

namespace detail {

template <typename Arg>
inline Register ToRegister(BaselineAssembler* basm,
                           BaselineAssembler::ScratchRegisterScope* scope,
                           Arg arg) {
  Register reg = scope->AcquireScratch();
  basm->Move(reg, arg);
  return reg;
}
inline Register ToRegister(BaselineAssembler* basm,
                           BaselineAssembler::ScratchRegisterScope* scope,
                           Register reg) {
  return reg;
}

template <typename... Args>
struct PushAllHelper;
template <>
struct PushAllHelper<> {
  static int Push(BaselineAssembler* basm) { return 0; }
  static int PushReverse(BaselineAssembler* basm) { return 0; }
};
template <typename Arg>
struct PushAllHelper<Arg> {
  static int Push(BaselineAssembler* basm, Arg arg) {
    BaselineAssembler::ScratchRegisterScope scope(basm);
    basm->masm()->Push(ToRegister(basm, &scope, arg));
    return 1;
  }
  static int PushReverse(BaselineAssembler* basm, Arg arg) {
    return Push(basm, arg);
  }
};
template <typename Arg, typename... Args>
struct PushAllHelper<Arg, Args...> {
  static int Push(BaselineAssembler* basm, Arg arg, Args... args) {
    PushAllHelper<Arg>::Push(basm, arg);
    return 1 + PushAllHelper<Args...>::Push(basm, args...);
  }
  static int PushReverse(BaselineAssembler* basm, Arg arg, Args... args) {
    int nargs = PushAllHelper<Args...>::PushReverse(basm, args...);
    PushAllHelper<Arg>::Push(basm, arg);
    return nargs + 1;
  }
};
template <>
struct PushAllHelper<interpreter::RegisterList> {
  static int Push(BaselineAssembler* basm, interpreter::RegisterList list) {
    for (int reg_index = 0; reg_index < list.register_count(); ++reg_index) {
      PushAllHelper<interpreter::Register>::Push(basm, list[reg_index]);
    }
    return list.register_count();
  }
  static int PushReverse(BaselineAssembler* basm,
                         interpreter::RegisterList list) {
    for (int reg_index = list.register_count() - 1; reg_index >= 0;
         --reg_index) {
      PushAllHelper<interpreter::Register>::Push(basm, list[reg_index]);
    }
    return list.register_count();
  }
};

template <typename... T>
struct PopAllHelper;
template <>
struct PopAllHelper<> {
  static void Pop(BaselineAssembler* basm) {}
};
template <>
struct PopAllHelper<Register> {
  static void Pop(BaselineAssembler* basm, Register reg) {
    basm->masm()->Pop(reg);
  }
};
template <typename... T>
struct PopAllHelper<Register, T...> {
  static void Pop(BaselineAssembler* basm, Register reg, T... tail) {
    PopAllHelper<Register>::Pop(basm, reg);
    PopAllHelper<T...>::Pop(basm, tail...);
  }
};

}  // namespace detail

template <typename... T>
int BaselineAssembler::Push(T... vals) {
  return detail::PushAllHelper<T...>::Push(this, vals...);
}

template <typename... T>
void BaselineAssembler::PushReverse(T... vals) {
  detail::PushAllHelper<T...>::PushReverse(this, vals...);
}

template <typename... T>
void BaselineAssembler::Pop(T... registers) {
  detail::PopAllHelper<T...>::Pop(this, registers...);
}

void BaselineAssembler::LoadTaggedField(Register output, Register source,
                                        int offset) {
  ASM_CODE_COMMENT(masm_);
  __ LoadU64(output, FieldMemOperand(source, offset));
}

void BaselineAssembler::LoadTaggedSignedField(Register output, Register source,
                                              int offset) {
  ASM_CODE_COMMENT(masm_);
  __ LoadU64(output, FieldMemOperand(source, offset));
}

void BaselineAssembler::LoadTaggedSignedFieldAndUntag(Register output,
                                                      Register source,
                                                      int offset) {
  LoadTaggedSignedField(output, source, offset);
  SmiUntag(output);
}

void BaselineAssembler::LoadWord16FieldZeroExtend(Register output,
                                                  Register source, int offset) {
  ASM_CODE_COMMENT(masm_);
  __ LoadU16(output, FieldMemOperand(source, offset));
}

void BaselineAssembler::LoadWord8Field(Register output, Register source,
                                       int offset) {
  ASM_CODE_COMMENT(masm_);
  __ LoadU8(output, FieldMemOperand(source, offset));
}

void BaselineAssembler::StoreTaggedSignedField(Register target, int offset,
                                               Tagged<Smi> value) {
  ASM_CODE_COMMENT(masm_);
  ScratchRegisterScope temps(this);
  Register tmp = temps.AcquireScratch();
  __ Move(tmp, value);
  __ StoreU64(tmp, FieldMemOperand(target, offset));
}

void BaselineAssembler::StoreTaggedFieldWithWriteBarrier(Register target,
                                                         int offset,
                                                         Register value) {
  __ StoreU64(value, FieldMemOperand(target, offset));
  __ RecordWriteField(target, offset, value, SaveFPRegsMode::kIgnore);
}
void BaselineAssembler::StoreTaggedFieldNoWriteBarrier(Register target,
                                                       int offset,
                                                       Register value) {
  __ StoreU64(value, FieldMemOperand(target, offset));
}

void BaselineAssembler::TryLoadOptimizedOsrCode(Register scratch_and_result,
                                                Register feedback_vector,
                                                FeedbackSlot slot,
                                                Label* on_result,
                                                Label::Distance) {
  Label fallthrough, clear_slot;
  LoadTaggedField(scratch_and_result, feedback_vector,
                  FeedbackVector::OffsetOfElementAt(slot.ToInt()));
  __ srl(g1, scratch_and_result, 0);
  __ cmp(g1, kClearedWeakHeapObjectLower32);
  __ b(equal, ConditionCodes::kIcc, &fallthrough);
  __ nop();
  __ and_(scratch_and_result, scratch_and_result,
          ~static_cast<int>(kWeakHeapObjectMask));
  __ LoadU64(scratch_and_result,
             FieldMemOperand(scratch_and_result, CodeWrapper::kCodeOffset));
  __ LoadU32(g1, FieldMemOperand(scratch_and_result, Code::kFlagsOffset));
  __ Move(g2, intptr_t{1} << Code::kMarkedForDeoptimizationBit);
  __ andcc(g0, g1, g2);
  __ b(not_equal, ConditionCodes::kIcc, &clear_slot);
  __ nop();
  Jump(on_result);
  __ bind(&clear_slot);
  __ Move(g1, kClearedWeakHeapObjectLower32);
  StoreTaggedFieldNoWriteBarrier(
      feedback_vector, FeedbackVector::OffsetOfElementAt(slot.ToInt()), g1);
  __ bind(&fallthrough);
  Move(scratch_and_result, 0);
}

void BaselineAssembler::AddToInterruptBudgetAndJumpIfNotExceeded(
    int32_t weight, Label* skip_interrupt_label) {
  ScratchRegisterScope temps(this);
  Register cell = temps.AcquireScratch();
  Register budget = temps.AcquireScratch();
  LoadFeedbackCell(cell);
  __ LoadS32(budget,
             FieldMemOperand(cell, FeedbackCell::kInterruptBudgetOffset));
  __ AddS64(budget, budget, weight);
  __ sra(budget, budget, 0);
  __ StoreU32(budget,
              FieldMemOperand(cell, FeedbackCell::kInterruptBudgetOffset));
  if (skip_interrupt_label) {
    __ cmp(budget, 0);
    BranchToLabel(masm_, greater_equal, skip_interrupt_label);
  }
}

void BaselineAssembler::AddToInterruptBudgetAndJumpIfNotExceeded(
    Register weight, Label* skip_interrupt_label) {
  ScratchRegisterScope temps(this);
  Register cell = temps.AcquireScratch();
  Register budget = temps.AcquireScratch();
  LoadFeedbackCell(cell);
  __ LoadS32(budget,
             FieldMemOperand(cell, FeedbackCell::kInterruptBudgetOffset));
  __ add(budget, budget, weight);
  __ sra(budget, budget, 0);
  __ StoreU32(budget,
              FieldMemOperand(cell, FeedbackCell::kInterruptBudgetOffset));
  if (skip_interrupt_label) {
    __ cmp(budget, 0);
    BranchToLabel(masm_, greater_equal, skip_interrupt_label);
  }
}

void BaselineAssembler::LdaContextSlotNoCell(Register context, uint32_t index,
                                             uint32_t depth,
                                             CompressionMode compression_mode) {
  ASM_CODE_COMMENT(masm_);
  for (; depth > 0; --depth) {
    LoadTaggedField(context, context, Context::kPreviousOffset);
  }
  LoadTaggedField(kInterpreterAccumulatorRegister, context,
                  Context::OffsetOfElementAt(index));
}

void BaselineAssembler::StaContextSlotNoCell(Register context, Register value,
                                             uint32_t index, uint32_t depth) {
  ASM_CODE_COMMENT(masm_);
  for (; depth > 0; --depth) {
    LoadTaggedField(context, context, Context::kPreviousOffset);
  }
  StoreTaggedFieldWithWriteBarrier(context, Context::OffsetOfElementAt(index),
                                   value);
}

void BaselineAssembler::LdaModuleVariable(Register context, int cell_index,
                                          uint32_t depth) {
  ASM_CODE_COMMENT(masm_);
  for (; depth > 0; --depth) {
    LoadTaggedField(context, context, Context::kPreviousOffset);
  }
  LoadTaggedField(context, context, Context::kExtensionOffset);
  if (cell_index > 0) {
    LoadTaggedField(context, context, SourceTextModule::kRegularExportsOffset);
    // The actual array index is (cell_index - 1).
    cell_index -= 1;
  } else {
    LoadTaggedField(context, context, SourceTextModule::kRegularImportsOffset);
    // The actual array index is (-cell_index - 1).
    cell_index = -cell_index - 1;
  }
  LoadFixedArrayElement(context, context, cell_index);
  LoadTaggedField(kInterpreterAccumulatorRegister, context, Cell::kValueOffset);
}

void BaselineAssembler::StaModuleVariable(Register context, Register value,
                                          int cell_index, uint32_t depth) {
  ASM_CODE_COMMENT(masm_);
  for (; depth > 0; --depth) {
    LoadTaggedField(context, context, Context::kPreviousOffset);
  }
  LoadTaggedField(context, context, Context::kExtensionOffset);
  LoadTaggedField(context, context, SourceTextModule::kRegularExportsOffset);

  // The actual array index is (cell_index - 1).
  cell_index -= 1;
  LoadFixedArrayElement(context, context, cell_index);
  StoreTaggedFieldWithWriteBarrier(context, Cell::kValueOffset, value);
}

void BaselineAssembler::IncrementSmi(MemOperand lhs) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ LoadU64(scratch, lhs);
  __ SmiUntag(scratch);
  __ AddS64(scratch, scratch, 1);
  __ SmiTag(scratch);
  __ StoreU64(scratch, lhs);
}

void BaselineAssembler::Switch(Register reg, int case_value_base,
                               Label** labels, int num_labels) {
  for (int i = 0; i < num_labels; ++i) {
    JumpIf(kEqual, reg, Operand(static_cast<intptr_t>(case_value_base) + i),
           labels[i]);
  }
}

void BaselineAssembler::Word32And(Register output, Register lhs, int rhs) {
  ScratchRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  __ Move(scratch, rhs);
  __ and_(output, lhs, scratch);
  __ srl(output, output, 0);
}

#undef __

#define __ basm.

void BaselineAssembler::EmitReturn(MacroAssembler* masm) {
  ASM_CODE_COMMENT(masm);
  BaselineAssembler basm(masm);

  Register weight = BaselineLeaveFrameDescriptor::WeightRegister();
  Register params_size = BaselineLeaveFrameDescriptor::ParamsSizeRegister();

  {
    ASM_CODE_COMMENT_STRING(masm, "Update Interrupt Budget");

    Label skip_interrupt_label;
    __ AddToInterruptBudgetAndJumpIfNotExceeded(weight, &skip_interrupt_label);
    {
      __ masm() -> SmiTag(params_size);
      __ Push(params_size, kInterpreterAccumulatorRegister);

      __ LoadContext(kContextRegister);
      __ LoadFunction(kJSFunctionRegister);
      __ Push(kJSFunctionRegister);
      __ CallRuntime(Runtime::kBytecodeBudgetInterrupt_Sparkplug, 1);

      __ Pop(kInterpreterAccumulatorRegister, params_size);
      __ masm() -> SmiUntag(params_size);
    }

    __ Bind(&skip_interrupt_label);
  }

  BaselineAssembler::ScratchRegisterScope temps(&basm);
  Register actual_params_size = temps.AcquireScratch();
  // Compute the size of the actual parameters + receiver.
  __ Move(actual_params_size,
          MemOperand(fp, StandardFrameConstants::kArgCOffset));

  // If actual is bigger than formal, then we should use it to free up the stack
  // arguments.
  Label corrected_args_count;
  JumpIfHelper(__ masm(), kGreaterThanEqual, params_size, actual_params_size,
               &corrected_args_count);
  __ masm() -> Move(params_size, actual_params_size);
  __ Bind(&corrected_args_count);

  // Leave the frame (also dropping the register file).
  __ masm() -> LeaveFrame(StackFrame::BASELINE);

  // Drop receiver + arguments.
  __ masm() -> Drop(params_size);
  __ masm() -> Ret();
}

#undef __

inline void EnsureAccumulatorPreservedScope::AssertEqualToAccumulator(
    Register reg) {
  assembler_->masm()->cmp(reg, kInterpreterAccumulatorRegister);
  assembler_->masm()->Assert(equal, AbortReason::kAccumulatorClobbered);
}

}  // namespace baseline
}  // namespace internal
}  // namespace v8

#endif  // V8_BASELINE_SPARC64_BASELINE_ASSEMBLER_SPARC64_INL_H_
