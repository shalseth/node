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
  MemOperand(Register base, Register index, int32_t offset = 0)
      : base_(base), index_(index), offset_(offset) {}
  Register base() const { return base_; }
  Register index() const { return index_; }
  int64_t offset() const {
    return int64_t{offset_} + (base_ == sp ? kSparcStackSlotBias : 0);
  }

 private:
  Register base_;
  Register index_ = no_reg;
  int32_t offset_;
};

inline MemOperand FieldMemOperand(Register object, int offset) {
  return MemOperand(object, offset - kHeapObjectTag);
}

inline MemOperand FieldMemOperand(Register object, Register index, int offset) {
  return MemOperand(object, index, offset - kHeapObjectTag);
}

enum class StackLimitKind { kInterruptStackLimit, kRealStackLimit };

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

#ifdef V8_ENABLE_MAGLEV
  using Assembler::b;
  using Assembler::mov;
  Register OperandRegister(Operand value, Register first = no_reg,
                           Register second = no_reg) {
    if (value.is_reg()) return value.rm();
    if (value.immediate() == 0) return g0;
    Register reg = Scratch(first, second);
    Move(reg, value.immediate());
    return reg;
  }
  template <typename T>
  void Push(Handle<T> value) {
    Move(g1, value);
    Push(g1);
  }
  template <typename T>
  void Push(Tagged<T> value) {
    Move(g1, static_cast<intptr_t>(value.ptr()));
    Push(g1);
  }
  Condition CheckSmi(Register value) {
    andcc(g1, value, kSmiTagMask);
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
    return equal;
  }
  void DecompressTagged(Register dst, Register src) {
    static_assert(!COMPRESS_POINTERS_BOOL);
    Move(dst, src);
  }
  void MaybeJumpIfReadOnlyOrSmallSmi(Register value, Label* target) {
    JumpIfSmi(value, target);
  }
  void CheckPageFlag(Register object, Register scratch, int mask, Condition cc,
                     Label* target);
  void CallRecordWriteStub(Register object, Register slot, SaveFPRegsMode mode);
  void AssertMap(Register object);
  void AssertFeedbackVector(Register object, Register scratch);
  void CallJSDispatchEntry(JSDispatchHandle handle, uint16_t argument_count);
  void TryLoadOptimizedOsrCode(Register result, CodeKind min_opt_level,
                               Register feedback, FeedbackSlot slot,
                               Label* on_result, Label::Distance = Label::kFar);
  void Switch(Register scratch, Register value, int base, Label** labels,
              int count) {
    for (int n = 0; n < count; ++n) {
      CmpS32(value, Operand(static_cast<intptr_t>(base) + n));
      BranchCondition(equal, labels[n]);
    }
  }
  struct ConditionState {
    ConditionCodes codes;
    bool floating_point;
  };
  ConditionState condition_state() const {
    return {condition_codes_, float_condition_};
  }
  void set_condition_state(ConditionState state) {
    condition_codes_ = state.codes;
    float_condition_ = state.floating_point;
  }
  void BranchCondition(Condition condition, Label* target);
  void Call(Label* target) {
    call(target);
    nop();
  }
  void JumpIfSmi(Register value, Label* target) {
    andcc(g1, value, kSmiTagMask);
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
    BranchCondition(equal, target);
  }
  void JumpIfNotSmi(Register value, Label* target) {
    andcc(g1, value, kSmiTagMask);
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
    BranchCondition(not_equal, target);
  }
  void JumpIfRoot(Register value, RootIndex root, Label* target) {
    CompareRoot(value, root);
    BranchCondition(equal, target);
  }
  void JumpIfNotRoot(Register value, RootIndex root, Label* target) {
    CompareRoot(value, root);
    BranchCondition(not_equal, target);
  }
  void CompareTaggedRoot(Register value, RootIndex root) {
    CompareRoot(value, root);
  }
  template <typename T>
  void Cmp(Register lhs, T rhs) {
    CmpS64(lhs, rhs);
  }
  template <typename T>
  void CompareTagged(Register lhs, T rhs) {
    CmpS64(lhs, rhs);
  }
  void CmpSmiLiteral(Register lhs, Tagged<Smi> rhs, Register) {
    CmpSmiLiteral(lhs, rhs);
  }
  void SmiUntagField(Register dst, MemOperand src) {
    LoadU64(dst, src);
    SmiUntag(dst);
  }
  void LoadTaggedFieldWithoutDecompressing(Register dst, MemOperand src,
                                           Register) {
    LoadU64(dst, src);
  }
  void LoadU32(Register dst, MemOperand src, Register) { LoadU32(dst, src); }
  void LoadS8(Register dst, Register src) {
    sll(dst, src, 24);
    sra(dst, dst, 24);
  }
  void LoadS16(Register dst, Register src) {
    sll(dst, src, 16);
    sra(dst, dst, 16);
  }
  void MulS32(Register dst, Register lhs, Register rhs) {
    mulx(dst, lhs, rhs);
    sra(dst, dst, 0);
  }
  void MulS64(Register dst, Register rhs) { mulx(dst, dst, rhs); }
  void MulHighS32(Register dst, Register lhs, Register rhs) {
    sra(g1, lhs, 0);
    sra(g2, rhs, 0);
    mulx(dst, g1, g2);
    srax(dst, dst, 32);
  }
  void DivS32(Register dst, Register lhs, Register rhs) {
    sra(g1, lhs, 0);
    sra(g2, rhs, 0);
    sdivx(g3, g1, g2);
    mulx(g4, g3, g2);
    sub(i4, g1, g4);
    sra(dst, g3, 0);
  }
  void ModU32(Register dst, Register lhs, Register rhs) {
    srl(g1, lhs, 0);
    srl(g2, rhs, 0);
    udivx(g3, g1, g2);
    mulx(g3, g3, g2);
    sub(dst, g1, g3);
    srl(dst, dst, 0);
  }
  void Not32(Register dst, Register src) {
    xor_(dst, src, -1);
    srl(dst, dst, 0);
  }
  void AddF64(DoubleRegister dst, DoubleRegister lhs, DoubleRegister rhs) {
    faddd(dst, lhs, rhs);
  }
  void SubF64(DoubleRegister dst, DoubleRegister lhs, DoubleRegister rhs) {
    fsubd(dst, lhs, rhs);
  }
  void MulF64(DoubleRegister dst, DoubleRegister lhs, DoubleRegister rhs) {
    fmuld(dst, lhs, rhs);
  }
  void DivF64(DoubleRegister dst, DoubleRegister lhs, DoubleRegister rhs) {
    fdivd(dst, lhs, rhs);
  }
  void DoubleMin(DoubleRegister dst, DoubleRegister lhs, DoubleRegister rhs);
  void DoubleMax(DoubleRegister dst, DoubleRegister lhs, DoubleRegister rhs);
  void ReverseBytes(Register value, int bytes);
  void CompareInstanceType(Register map, Register type, InstanceType expected);
  void CompareInstanceTypeRange(Register map, Register type, Register scratch,
                                InstanceType first, InstanceType last);
  void PrepareCallCFunction(int gp, int fp, Register scratch) {
    CHECK_LE(gp + fp, kRegisterPassedArguments);
    PrepareCallCFunction(gp, scratch);
  }
  int CallCFunction(ExternalReference function, int gp, int fp) {
    CHECK_LE(gp + fp, kRegisterPassedArguments);
    return CallCFunction(function, gp);
  }

  void b(Label* target) { Jump(target); }
  void b(Condition condition, Label* target) {
    BranchCondition(condition, target);
  }
  void beq(Label* target, Label::Distance = Label::kFar) {
    BranchCondition(equal, target);
  }
  void bne(Label* target, Label::Distance = Label::kFar) {
    BranchCondition(not_equal, target);
  }
  void blt(Label* target, Label::Distance = Label::kFar) {
    BranchCondition(less, target);
  }
  void ble(Label* target, Label::Distance = Label::kFar) {
    BranchCondition(less_equal, target);
  }
  void bgt(Label* target, Label::Distance = Label::kFar) {
    BranchCondition(greater, target);
  }
  void bge(Label* target, Label::Distance = Label::kFar) {
    BranchCondition(greater_equal, target);
  }
  void mov(Register dst, Operand src) {
    if (src.is_reg())
      Move(dst, src.rm());
    else
      Move(dst, src.immediate());
  }
  void Move(MemOperand dst, Register src) { StoreU64(src, dst); }
  void Move(DoubleRegister dst, DoubleRegister src) { fmovd(dst, src); }
  void push(Register reg) { Push(reg); }
  void pop(Register reg) { Pop(reg); }
  void SmiTag(Register dst, Register src) {
    sllx(dst, src, kSmiShiftSize + kSmiTagSize);
  }
  template <typename T>
  void CmpS32(Register lhs, T rhs) {
    cmp(lhs, OperandRegister(Operand(rhs), lhs));
    condition_codes_ = ConditionCodes::kIcc;
    float_condition_ = false;
  }
  template <typename T>
  void CmpU32(Register lhs, T rhs) {
    CmpS32(lhs, rhs);
  }
  template <typename T>
  void CmpS64(Register lhs, T rhs) {
    cmp(lhs, OperandRegister(Operand(rhs), lhs));
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void CmpU64(Register lhs, T rhs) {
    CmpS64(lhs, rhs);
  }
  template <typename T>
  void CmpTagged(Register lhs, T rhs) {
    CmpS64(lhs, rhs);
  }
  void CmpSmiLiteral(Register lhs, Tagged<Smi> rhs) {
    CmpS64(lhs, Operand(static_cast<intptr_t>(rhs.ptr())));
  }
  void CmpF64(DoubleRegister lhs, DoubleRegister rhs) {
    fcmpd(lhs, rhs);
    float_condition_ = true;
  }
  void LoadS32(Register dst, Register src) { sra(dst, src, 0); }
  void LoadU32(Register dst, Register src) { srl(dst, src, 0); }
  void LoadU64(Register dst, MemOperand src, Register) { LoadU64(dst, src); }
  void LoadTaggedField(Register dst, MemOperand src) { LoadU64(dst, src); }
  void LoadTaggedFieldWithoutDecompressing(Register dst, MemOperand src) {
    LoadU64(dst, src);
  }
  void StoreTaggedField(Register src, MemOperand dst) { StoreU64(src, dst); }
  void LoadTaggedRoot(Register dst, RootIndex root) { LoadRoot(dst, root); }
  template <typename T>
  void LoadF64(DoubleRegister dst, T value, Register scratch) {
    static_assert(sizeof(T) == kDoubleSize);
    Move(scratch, static_cast<intptr_t>(base::bit_cast<uint64_t>(value)));
    MovInt64ToDouble(dst, scratch);
  }
  void LoadF32AsF64(DoubleRegister dst, MemOperand src) {
    LoadF32(dst, src);
    fstod(dst, dst);
  }
  void PushAll(RegList registers);
  void PopAll(RegList registers);
  void PushAll(DoubleRegList registers, int size = kDoubleSize);
  void PopAll(DoubleRegList registers, int size = kDoubleSize);
  void CompareRoot(Register value, RootIndex root);
  void LoadStackLimit(Register dst, StackLimitKind kind);
  void Check(Condition condition, AbortReason reason);
  void AssertSmi(Register value);
  void AssertNotSmi(Register value);
  void AssertZeroExtended(Register value);
  void AssertNotDeoptimized(Register scratch);
  template <bool = false>
  void CompareObjectType(Register object, Register map, Register type,
                         InstanceType expected);
  void CompareObjectTypeRange(Register object, Register map, Register type,
                              Register scratch, InstanceType first,
                              InstanceType last);
  MemOperand ExternalReferenceAsOperand(IsolateFieldId field) {
    return AsMemOperand(field);
  }
  MemOperand ExternalReferenceAsOperand(ExternalReference reference,
                                        Register scratch = g5) {
    Move(scratch, reference);
    return MemOperand(scratch);
  }
  void ConvertIntToDouble(DoubleRegister dst, Register src);
  void ConvertUnsignedIntToDouble(DoubleRegister dst, Register src);
  void ConvertInt64ToDouble(DoubleRegister dst, Register src) {
    MovInt64ToDouble(dst, src);
    fxtod(dst, dst);
  }
  void ConvertDoubleToInt32(Register dst, DoubleRegister src);
  void ConvertDoubleToUnsignedInt32(Register dst, DoubleRegister src);
  void CountLeadingZerosU32(Register dst, Register src);
  void CanonicalizeNaN(DoubleRegister dst, DoubleRegister src);

  template <typename T>
  void AddS32(Register dst, Register lhs, T rhs) {
    addcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    sra(dst, dst, 0);
    condition_codes_ = ConditionCodes::kIcc;
    float_condition_ = false;
  }
  template <typename T>
  void AddS32(Register dst, T rhs) {
    AddS32(dst, dst, rhs);
  }
  template <typename T>
  void SubS32(Register dst, Register lhs, T rhs) {
    subcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    sra(dst, dst, 0);
    condition_codes_ = ConditionCodes::kIcc;
    float_condition_ = false;
  }
  template <typename T>
  void SubS32(Register dst, T rhs) {
    SubS32(dst, dst, rhs);
  }
  template <typename T>
  void AddU32(Register dst, Register lhs, T rhs) {
    addcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    sra(dst, dst, 0);
    condition_codes_ = ConditionCodes::kIcc;
    float_condition_ = false;
  }
  template <typename T>
  void AddU32(Register dst, T rhs) {
    AddU32(dst, dst, rhs);
  }
  template <typename T>
  void AddU64(Register dst, Register lhs, T rhs) {
    addcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void AddU64(Register dst, T rhs) {
    AddU64(dst, dst, rhs);
  }
  void AddS64(Register dst, Register lhs, Register rhs) {
    AddS64(dst, lhs, Operand(rhs));
  }
  void AddS64(Register dst, Register lhs, Operand rhs) {
    addcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void AddS64(Register dst, T rhs) {
    AddS64(dst, dst, rhs);
  }
  void SubS64(Register dst, Register lhs, Register rhs) {
    SubS64(dst, lhs, Operand(rhs));
  }
  void SubS64(Register dst, Register lhs, Operand rhs) {
    subcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void SubS64(Register dst, T rhs) {
    SubS64(dst, dst, rhs);
  }
  template <typename T>
  void And(Register dst, Register lhs, T rhs) {
    andcc(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void And(Register dst, T rhs) {
    And(dst, dst, rhs);
  }
  template <typename T>
  void Or(Register dst, Register lhs, T rhs) {
    or_(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    cmp(dst, 0);
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void Or(Register dst, T rhs) {
    Or(dst, dst, rhs);
  }
  template <typename T>
  void Xor(Register dst, Register lhs, T rhs) {
    xor_(dst, lhs, OperandRegister(Operand(rhs), dst, lhs));
    cmp(dst, 0);
    condition_codes_ = ConditionCodes::kXcc;
    float_condition_ = false;
  }
  template <typename T>
  void Xor(Register dst, T rhs) {
    Xor(dst, dst, rhs);
  }
  template <typename T>
  void ShiftLeftU32(Register dst, Register src, T rhs) {
    sll(dst, src, OperandRegister(Operand(rhs), dst, src));
  }
  template <typename T>
  void ShiftLeftU32(Register dst, T rhs) {
    ShiftLeftU32(dst, dst, rhs);
  }
  template <typename T>
  void ShiftLeftU64(Register dst, Register src, T rhs) {
    sllx(dst, src, OperandRegister(Operand(rhs), dst, src));
  }
  template <typename T>
  void ShiftLeftU64(Register dst, T rhs) {
    ShiftLeftU64(dst, dst, rhs);
  }
  template <typename T>
  void ShiftRightU32(Register dst, Register src, T rhs) {
    srl(dst, src, OperandRegister(Operand(rhs), dst, src));
  }
  template <typename T>
  void ShiftRightU32(Register dst, T rhs) {
    ShiftRightU32(dst, dst, rhs);
  }
  template <typename T>
  void ShiftRightS32(Register dst, Register src, T rhs) {
    sra(dst, src, OperandRegister(Operand(rhs), dst, src));
  }
  template <typename T>
  void ShiftRightS32(Register dst, T rhs) {
    ShiftRightS32(dst, dst, rhs);
  }
  template <typename T>
  void ShiftRightU64(Register dst, Register src, T rhs) {
    srlx(dst, src, OperandRegister(Operand(rhs), dst, src));
  }
  template <typename T>
  void ShiftRightU64(Register dst, T rhs) {
    ShiftRightU64(dst, dst, rhs);
  }
  template <typename T>
  void ShiftRightS64(Register dst, Register src, T rhs) {
    srax(dst, src, OperandRegister(Operand(rhs), dst, src));
  }
  template <typename T>
  void ShiftRightS64(Register dst, T rhs) {
    ShiftRightS64(dst, dst, rhs);
  }
#endif

 private:
#ifdef V8_ENABLE_MAGLEV
  ConditionCodes condition_codes_ = ConditionCodes::kXcc;
  bool float_condition_ = false;
#endif
  static Register Scratch(Register first, Register second = no_reg,
                          Register third = no_reg);
  MemOperand ResolveAddress(MemOperand operand, Register exclude);
};

struct MoveCycleState {
  bool pending_double_scratch_register_use = false;
};

#ifdef V8_ENABLE_MAGLEV
void CallApiFunctionAndReturn(MacroAssembler* masm, bool with_profiling,
                              Register function_address,
                              ExternalReference thunk_ref, Register thunk_arg,
                              int slots_to_drop_on_return,
                              MemOperand* argc_operand,
                              MemOperand return_value_operand,
                              bool handle_interceptor_result);
#endif

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_MACRO_ASSEMBLER_SPARC64_H_
