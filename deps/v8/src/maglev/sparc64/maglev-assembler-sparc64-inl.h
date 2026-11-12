// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_MAGLEV_SPARC64_MAGLEV_ASSEMBLER_SPARC64_INL_H_
#define V8_MAGLEV_SPARC64_MAGLEV_ASSEMBLER_SPARC64_INL_H_

#include "src/base/iterator.h"
#include "src/base/numbers/double.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler-inl.h"
#include "src/codegen/sparc64/assembler-sparc64.h"
#include "src/common/globals.h"
#include "src/compiler/compilation-dependencies.h"
#include "src/maglev/maglev-assembler.h"
#include "src/maglev/maglev-basic-block.h"
#include "src/maglev/maglev-code-gen-state.h"

namespace v8 {
namespace internal {
namespace maglev {

constexpr Condition ConditionForFloat64(Operation operation) {
  return ConditionFor(operation);
}

inline int ShiftFromScale(int n) {
  switch (n) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 2;
    case 8:
      return 3;
    default:
      UNREACHABLE();
  }
}

class MaglevAssembler::TemporaryRegisterScope
    : public TemporaryRegisterScopeBase<TemporaryRegisterScope> {
  using Base = TemporaryRegisterScopeBase<TemporaryRegisterScope>;

 public:
  struct SavedData : public Base::SavedData {
    RegList scratch;
    DoubleRegList doubles;
    // Deferred blocks may branch on the incoming flags.
    ConditionState condition;
  };
  explicit TemporaryRegisterScope(MaglevAssembler* masm) : Base(masm) {
    if (prev_scope_) {
      scratch_ = prev_scope_->scratch_;
      doubles_ = prev_scope_->doubles_;
    }
  }
  TemporaryRegisterScope(MaglevAssembler* masm, const SavedData& saved)
      : Base(masm, saved), scratch_(saved.scratch), doubles_(saved.doubles) {
    masm->set_condition_state(saved.condition);
  }
  Register AcquireScratch() {
    CHECK(!scratch_.is_empty());
    return scratch_.PopFirst();
  }
  DoubleRegister AcquireScratchDouble() {
    CHECK(!doubles_.is_empty());
    return doubles_.PopFirst();
  }
  void IncludeScratch(Register reg) { scratch_.set(reg); }
  SavedData CopyForDefer() {
    return {CopyForDeferBase(), scratch_, doubles_, masm_->condition_state()};
  }
  void ResetToDefaultImpl() {
    scratch_ = {i3, i5};
    doubles_ = {d13, d14};
  }

 private:
  RegList scratch_ = {i3, i5};
  DoubleRegList doubles_ = {d13, d14};
};

inline MapCompare::MapCompare(MaglevAssembler* masm, Register object,
                              size_t map_count)
    : masm_(masm), object_(object), map_count_(map_count) {
  map_ = masm_->scratch_register_scope()->Acquire();
  masm_->LoadMap(map_, object_);
  USE(map_count_);
}

void MapCompare::Generate(Handle<Map> map, Condition cond, Label* if_true,
                          Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(masm_);
  Register temp = temps.AcquireScratch();
  masm_->Move(temp, map);
  masm_->CmpS64(map_, temp);
  DCHECK(cond == equal || cond == not_equal);
  masm_->JumpIf(cond, if_true, distance);
}

Register MapCompare::GetMap() { return map_; }

int MapCompare::TemporaryCount(size_t map_count) { return 1; }

namespace detail {

template <typename... Args>
struct PushAllHelper;

template <>
struct PushAllHelper<> {
  static void Push(MaglevAssembler* masm) {}
  static void PushReverse(MaglevAssembler* masm) {}
};

inline void PushInput(MaglevAssembler* masm, ConstInput input) {
  if (input.operand().IsConstant()) {
    MaglevAssembler::TemporaryRegisterScope temps(masm);
    Register scratch = temps.AcquireScratch();
    input.node()->LoadToRegister(masm, scratch);
    masm->Push(scratch);
  } else {
    // TODO(leszeks): Consider special casing the value. (Toon: could possibly
    // be done through Input directly?)
    const compiler::AllocatedOperand& operand =
        compiler::AllocatedOperand::cast(input.operand());
    if (operand.IsRegister()) {
      masm->Push(operand.GetRegister());
    } else {
      DCHECK(operand.IsStackSlot());
      masm->LoadU64(i4, masm->GetStackSlot(operand));
      masm->Push(i4);
    }
  }
}

template <std::ranges::range Range, typename... Args>
inline void PushIterator(MaglevAssembler* masm, Range range, Args... args) {
  for (auto iter = range.begin(), end = range.end(); iter != end; ++iter) {
    masm->Push(*iter);
  }
  PushAllHelper<Args...>::Push(masm, args...);
}

template <typename T, typename... Args>
inline void PushIteratorReverse(MaglevAssembler* masm,
                                base::iterator_range<T> range, Args... args) {
  PushAllHelper<Args...>::PushReverse(masm, args...);
  for (const auto& item : base::Reversed(range)) {
    masm->Push(item);
  }
}

template <typename... Args>
struct PushAllHelper<Input, Args...> {
  static void Push(MaglevAssembler* masm, Input arg, Args... args) {
    PushInput(masm, arg);
    PushAllHelper<Args...>::Push(masm, args...);
  }
  static void PushReverse(MaglevAssembler* masm, Input arg, Args... args) {
    PushAllHelper<Args...>::PushReverse(masm, args...);
    PushInput(masm, arg);
  }
};
template <typename Arg, typename... Args>
struct PushAllHelper<Arg, Args...> {
  static void Push(MaglevAssembler* masm, Arg arg, Args... args) {
    if constexpr (is_iterator_range<Arg>::value || std::ranges::range<Arg>) {
      PushIterator(masm, arg, args...);
    } else {
      masm->MacroAssembler::Push(arg);
      PushAllHelper<Args...>::Push(masm, args...);
    }
  }
  static void PushReverse(MaglevAssembler* masm, Arg arg, Args... args) {
    if constexpr (is_iterator_range<Arg>::value) {
      PushIteratorReverse(masm, arg, args...);
    } else if constexpr (std::ranges::range<Arg>) {
      PushIteratorReverse(
          masm, base::make_iterator_range(arg.begin(), arg.end()), args...);
    } else {
      PushAllHelper<Args...>::PushReverse(masm, args...);
      masm->Push(arg);
    }
  }
};

}  // namespace detail

template <typename... T>
void MaglevAssembler::Push(T... vals) {
  detail::PushAllHelper<T...>::Push(this, vals...);
}

template <typename... T>
void MaglevAssembler::PushReverse(T... vals) {
  detail::PushAllHelper<T...>::PushReverse(this, vals...);
}

inline void MaglevAssembler::BindJumpTarget(Label* label) { bind(label); }

inline void MaglevAssembler::BindBlock(BasicBlock* block) {
  bind(block->label());
}

inline Condition MaglevAssembler::TrySmiTagInt32(Register dst, Register src) {
  CHECK(!SmiValuesAre32Bits());
  AddS32(dst, src, src);
  return no_overflow;
}

inline void MaglevAssembler::CheckInt32IsSmi(Register obj, Label* fail,
                                             Register scratch) {
  CHECK(!SmiValuesAre32Bits());
  if (scratch == Register::no_reg()) {
    scratch = i4;
  }
  mov(scratch, obj);
  AddS32(scratch, scratch);
  JumpIf(overflow, fail);
}

inline void MaglevAssembler::SmiAddConstant(Register dst, Register src,
                                            int value, Label* fail,
                                            Label::Distance distance) {
  AssertSmi(src);
  Move(dst, src);
  if (value != 0) {
    Register scratch = i4;
    Move(scratch, Smi::FromInt(value));
    if (SmiValuesAre31Bits()) {
      AddS32(dst, scratch);
    } else {
      AddS64(dst, scratch);
    }
    JumpIf(overflow, fail, distance);
  }
}

inline void MaglevAssembler::SmiSubConstant(Register dst, Register src,
                                            int value, Label* fail,
                                            Label::Distance distance) {
  AssertSmi(src);
  Move(dst, src);
  if (value != 0) {
    Register scratch = i4;
    Move(scratch, Smi::FromInt(value));
    if (SmiValuesAre31Bits()) {
      SubS32(dst, scratch);
    } else {
      SubS64(dst, scratch);
    }
    JumpIf(overflow, fail, distance);
  }
}

inline void MaglevAssembler::MoveHeapNumber(Register dst, double value) {
  MoveNumber(dst, value);
}

inline Condition MaglevAssembler::IsRootConstant(Input input,
                                                 RootIndex root_index) {
  if (input.operand().IsRegister()) {
    CompareRoot(ToRegister(input), root_index);
  } else {
    DCHECK(input.operand().IsStackSlot());
    TemporaryRegisterScope temps(this);
    Register scratch = temps.AcquireScratch();
    LoadU64(scratch, ToMemOperand(input.operand()), scratch);
    CompareRoot(scratch, root_index);
  }
  return equal;
}

inline MemOperand MaglevAssembler::StackSlotOperand(StackSlot slot) {
  return MemOperand(fp, slot.index);
}

inline Register MaglevAssembler::GetFramePointer() { return fp; }

// TODO(Victorgomes): Unify this to use StackSlot struct.
inline MemOperand MaglevAssembler::GetStackSlot(
    const compiler::AllocatedOperand& operand) {
  return MemOperand(fp, GetFramePointerOffsetForStackSlot(operand));
}

inline MemOperand MaglevAssembler::ToMemOperand(
    const compiler::InstructionOperand& operand) {
  return GetStackSlot(compiler::AllocatedOperand::cast(operand));
}

inline void MaglevAssembler::BuildTypedArrayDataPointer(Register data_pointer,
                                                        Register object) {
  DCHECK_NE(data_pointer, object);
  LoadExternalPointerField(
      data_pointer,
      FieldMemOperand(object, JSTypedArray::kExternalPointerOffset));
  if (JSTypedArray::kMaxSizeInHeap == 0) return;
  // TemporaryRegisterScope temps(this);
  Register base = i4;
  if (COMPRESS_POINTERS_BOOL) {
    LoadU32(base, FieldMemOperand(object, JSTypedArray::kBasePointerOffset));
  } else {
    LoadU64(base, FieldMemOperand(object, JSTypedArray::kBasePointerOffset));
  }
  AddU64(data_pointer, data_pointer, base);
}

inline MemOperand MaglevAssembler::TypedArrayElementOperand(
    Register data_pointer, Register index, int element_size) {
  // TemporaryRegisterScope temps(this);
  Register temp = i4;
  ShiftLeftU64(temp, index, Operand(ShiftFromScale(element_size)));
  AddU64(data_pointer, data_pointer, temp);
  return MemOperand(data_pointer);
}

inline void MaglevAssembler::StoreDataViewElement(Register value,
                                                  Register data_pointer,
                                                  Register index,
                                                  int element_size) {
  MemOperand element_address = MemOperand(data_pointer, index);
  StoreUnaligned(value, element_address, element_size);
}

inline void MaglevAssembler::LoadDataViewElement(Register result,
                                                 Register data_pointer,
                                                 Register index,
                                                 int element_size) {
  MemOperand element_address = MemOperand(data_pointer, index);
  LoadUnaligned(result, element_address, element_size, true);
}

inline void MaglevAssembler::LoadTaggedFieldByIndex(Register result,
                                                    Register object,
                                                    Register index, int scale,
                                                    int offset) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  ShiftLeftU64(scratch, index, Operand(ShiftFromScale(scale)));
  AddU64(scratch, scratch, object);
  MacroAssembler::LoadTaggedField(result, FieldMemOperand(scratch, offset));
}

inline void MaglevAssembler::LoadBoundedSizeFromObject(Register result,
                                                       Register object,
                                                       int offset) {
  Move(result, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::LoadExternalPointerField(Register result,
                                                      MemOperand operand) {
  Move(result, operand);
}

void MaglevAssembler::LoadFixedArrayElement(Register result, Register array,
                                            Register index) {
  if (v8_flags.debug_code) {
    AssertObjectType(array, FIXED_ARRAY_TYPE, AbortReason::kUnexpectedValue);
    CompareInt32AndAssert(index, 0, kUnsignedGreaterThanEqual,
                          AbortReason::kUnexpectedNegativeValue);
  }
  LoadTaggedFieldByIndex(result, array, index, kTaggedSize,
                         OFFSET_OF_DATA_START(FixedArray));
}

inline void MaglevAssembler::LoadTaggedFieldWithoutDecompressing(
    Register result, Register object, int offset) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  MacroAssembler::LoadTaggedFieldWithoutDecompressing(
      result, FieldMemOperand(object, offset), scratch);
}

void MaglevAssembler::LoadFixedArrayElementWithoutDecompressing(
    Register result, Register array, Register index) {
  if (v8_flags.debug_code) {
    AssertObjectType(array, FIXED_ARRAY_TYPE, AbortReason::kUnexpectedValue);
    CompareInt32AndAssert(index, 0, kUnsignedGreaterThanEqual,
                          AbortReason::kUnexpectedNegativeValue);
  }
  int times_tagged_size = (kTaggedSize == 8) ? 3 : 2;
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  Register scratch2 = temps.AcquireScratch();
  ShiftLeftU64(scratch, index, Operand(times_tagged_size));
  MacroAssembler::LoadTaggedFieldWithoutDecompressing(
      result, FieldMemOperand(array, scratch, OFFSET_OF_DATA_START(FixedArray)),
      scratch2);
}

void MaglevAssembler::LoadFixedDoubleArrayElement(DoubleRegister result,
                                                  Register array,
                                                  Register index) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  if (v8_flags.debug_code) {
    AssertObjectType(array, FIXED_DOUBLE_ARRAY_TYPE,
                     AbortReason::kUnexpectedValue);
    CompareInt32AndAssert(index, 0, kUnsignedGreaterThanEqual,
                          AbortReason::kUnexpectedNegativeValue);
  }
  ShiftLeftU64(scratch, index, Operand(kDoubleSizeLog2));
  LoadF64(result, FieldMemOperand(array, scratch,
                                  OFFSET_OF_DATA_START(FixedDoubleArray)));
}

inline void MaglevAssembler::StoreFixedDoubleArrayElement(
    Register array, Register index, DoubleRegister value) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  ShiftLeftU64(scratch, index, Operand(kDoubleSizeLog2));
  StoreF64(value, FieldMemOperand(array, scratch,
                                  OFFSET_OF_DATA_START(FixedDoubleArray)));
}

inline void MaglevAssembler::LoadSignedField(Register result,
                                             MemOperand operand, int size) {
  if (size == 1) {
    LoadS8(result, operand);
  } else if (size == 2) {
    LoadS16(result, operand);
  } else {
    DCHECK_EQ(size, 4);
    LoadS32(result, operand);
  }
}

inline void MaglevAssembler::LoadUnsignedField(Register result,
                                               MemOperand operand, int size) {
  if (size == 1) {
    LoadU8(result, operand);
  } else if (size == 2) {
    LoadU16(result, operand);
  } else {
    DCHECK_EQ(size, 4);
    LoadU32(result, operand);
  }
}

inline void MaglevAssembler::SetSlotAddressForTaggedField(Register slot_reg,
                                                          Register object,
                                                          int offset) {
  mov(slot_reg, object);
  AddS64(slot_reg, Operand(offset - kHeapObjectTag));
}

inline void MaglevAssembler::SetSlotAddressForFixedArrayElement(
    Register slot_reg, Register object, Register index) {
  // TemporaryRegisterScope temps(this);
  Register scratch = i4;
  mov(slot_reg, object);
  AddU64(slot_reg, Operand(OFFSET_OF_DATA_START(FixedArray) - kHeapObjectTag));
  ShiftLeftU64(scratch, index, Operand(kTaggedSizeLog2));
  AddU64(slot_reg, slot_reg, scratch);
}

inline void MaglevAssembler::StoreTaggedFieldNoWriteBarrier(Register object,
                                                            int offset,
                                                            Register value) {
  MacroAssembler::StoreTaggedField(value, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreFixedArrayElementNoWriteBarrier(
    Register array, Register index, Register value) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  ShiftLeftU64(scratch, index, Operand(kTaggedSizeLog2));
  AddU64(scratch, scratch, array);
  MacroAssembler::StoreTaggedField(
      value, FieldMemOperand(scratch, OFFSET_OF_DATA_START(FixedArray)));
}

inline void MaglevAssembler::StoreTaggedSignedField(Register object, int offset,
                                                    Register value) {
  AssertSmi(value);
  MacroAssembler::StoreTaggedField(value, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreTaggedSignedField(Register object, int offset,
                                                    Tagged<Smi> value) {
  TemporaryRegisterScope scope(this);
  Register scratch = i4;
  Move(scratch, value);
  MacroAssembler::StoreTaggedField(scratch, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreInt32Field(Register object, int offset,
                                             int32_t value) {
  TemporaryRegisterScope scope(this);
  Register scratch = i4;
  Move(scratch, value);
  StoreU32(scratch, FieldMemOperand(object, offset));
}

inline void MaglevAssembler::StoreField(MemOperand operand, Register value,
                                        int size) {
  DCHECK(size == 1 || size == 2 || size == 4);
  if (size == 1) {
    StoreU8(value, operand);
  } else if (size == 2) {
    StoreU16(value, operand);
  } else {
    DCHECK_EQ(size, 4);
    StoreU32(value, operand);
  }
}

inline void MaglevAssembler::ReverseByteOrder(Register value, int size) {
  ReverseBytes(value, size);
}

inline void MaglevAssembler::IncrementInt32(Register reg) {
  AddS32(reg, Operand(1));
}

inline void MaglevAssembler::DecrementInt32(Register reg) {
  SubS32(reg, Operand(1));
}

inline void MaglevAssembler::AddInt32(Register reg, int amount) {
  AddS32(reg, Operand(amount));
}

inline void MaglevAssembler::AddInt32(Register reg, Register other) {
  AddS32(reg, other);
  LoadS32(reg, reg);
}

inline void MaglevAssembler::AndInt32(Register reg, int mask) {
  And(reg, Operand(mask));
  LoadU32(reg, reg);
}

inline void MaglevAssembler::OrInt32(Register reg, int mask) {
  Or(reg, Operand(mask));
  LoadU32(reg, reg);
}

inline void MaglevAssembler::AndInt32(Register reg, Register other) {
  And(reg, other);
  LoadU32(reg, reg);
}

inline void MaglevAssembler::OrInt32(Register reg, Register other) {
  Or(reg, other);
  LoadU32(reg, reg);
}

inline void MaglevAssembler::ShiftLeft(Register reg, int amount) {
  ShiftLeftU32(reg, reg, Operand(amount));
}

inline void MaglevAssembler::IncrementAddress(Register reg, int32_t delta) {
  MacroAssembler::AddS64(reg, reg, delta);
}

inline void MaglevAssembler::LoadAddress(Register dst, MemOperand location) {
  MacroAssembler::LoadAddress(dst, location);
}

inline void MaglevAssembler::EmitEnterExitFrame(int extra_slots,
                                                StackFrame::Type frame_type,
                                                Register scratch) {
  EnterExitFrame(scratch, extra_slots, frame_type);
}

inline void MaglevAssembler::Move(StackSlot dst, Register src) {
  StoreU64(src, StackSlotOperand(dst));
}
inline void MaglevAssembler::Move(StackSlot dst, DoubleRegister src) {
  StoreF64(src, StackSlotOperand(dst));
}
inline void MaglevAssembler::Move(Register dst, StackSlot src) {
  LoadU64(dst, StackSlotOperand(src));
}
inline void MaglevAssembler::Move(DoubleRegister dst, StackSlot src) {
  LoadF64(dst, StackSlotOperand(src));
}
inline void MaglevAssembler::Move(MemOperand dst, Register src) {
  StoreU64(src, dst);
}
inline void MaglevAssembler::Move(Register dst, MemOperand src) {
  LoadU64(dst, src);
}
inline void MaglevAssembler::Move(DoubleRegister dst, DoubleRegister src) {
  if (dst != src) {
    MacroAssembler::Move(dst, src);
  }
}
inline void MaglevAssembler::Move(Register dst, Tagged<Smi> src) {
  MacroAssembler::Move(dst, src);
}
inline void MaglevAssembler::Move(Register dst, ExternalReference src) {
  MacroAssembler::Move(dst, src);
}
inline void MaglevAssembler::Move(Register dst, Register src) {
  if (dst != src) {
    MacroAssembler::Move(dst, src);
  }
}
inline void MaglevAssembler::Move(Register dst, Tagged<TaggedIndex> i) {
  mov(dst, Operand(i.ptr()));
}
inline void MaglevAssembler::Move(Register dst, int32_t i) {
  mov(dst, Operand(i));
}
inline void MaglevAssembler::Move(DoubleRegister dst, double n) {
  TemporaryRegisterScope scope(this);
  Register scratch = scope.AcquireScratch();
  MacroAssembler::LoadF64(dst, n, scratch);
}
inline void MaglevAssembler::Move(DoubleRegister dst, Float64 n) {
  TemporaryRegisterScope scope(this);
  Register scratch = scope.AcquireScratch();
  MacroAssembler::LoadF64(dst, n, scratch);
}
inline void MaglevAssembler::Move(Register dst, Handle<HeapObject> obj) {
  MacroAssembler::Move(dst, obj);
}

inline void MaglevAssembler::Move(Register dst, uint32_t i) {
  // Move as a uint32 to avoid sign extension.
  mov(dst, Operand(i));
  LoadU32(dst, dst);
}

inline void MaglevAssembler::Move(Register dst, intptr_t p) {
  mov(dst, Operand(p));
}

void MaglevAssembler::MoveTagged(Register dst, Handle<HeapObject> obj) {
#ifdef V8_COMPRESS_POINTERS
  MacroAssembler::Move(dst, obj, RelocInfo::COMPRESSED_EMBEDDED_OBJECT);
#else
  MacroAssembler::Move(dst, obj);
#endif
}

inline void MaglevAssembler::LoadInt32(Register dst, MemOperand src) {
  LoadU32(dst, src);
}

inline void MaglevAssembler::StoreInt32(MemOperand dst, Register src) {
  StoreU32(src, dst);
}

inline void MaglevAssembler::LoadFloat32(DoubleRegister dst, MemOperand src) {
  MacroAssembler::LoadF32AsF64(dst, src);
}
inline void MaglevAssembler::StoreFloat32(MemOperand dst, DoubleRegister src) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  DoubleRegister double_scratch = temps.AcquireScratchDouble();
  fdtos(double_scratch, src);
  MacroAssembler::StoreF32(double_scratch, dst);
}
inline void MaglevAssembler::LoadFloat64(DoubleRegister dst, MemOperand src) {
  MacroAssembler::LoadF64(dst, src);
}
inline void MaglevAssembler::StoreFloat64(MemOperand dst, DoubleRegister src) {
  MacroAssembler::StoreF64(src, dst);
}

inline void MaglevAssembler::LoadUnalignedFloat64(DoubleRegister dst,
                                                  Register base,
                                                  Register index) {
  LoadUnaligned(i4, MemOperand(base, index), kDoubleSize);
  MovInt64ToDouble(dst, i4);
}
inline void MaglevAssembler::LoadUnalignedFloat64AndReverseByteOrder(
    DoubleRegister dst, Register base, Register index) {
  TemporaryRegisterScope scope(this);
  Register scratch = i4;
  LoadUnaligned(scratch, MemOperand(base, index), kDoubleSize);
  ReverseBytes(scratch, 8);
  MovInt64ToDouble(dst, scratch);
}
inline void MaglevAssembler::StoreUnalignedFloat64(Register base,
                                                   Register index,
                                                   DoubleRegister src) {
  MovDoubleToInt64(i4, src);
  StoreUnaligned(i4, MemOperand(base, index), kDoubleSize);
}
inline void MaglevAssembler::ReverseByteOrderAndStoreUnalignedFloat64(
    Register base, Register index, DoubleRegister src) {
  TemporaryRegisterScope scope(this);
  Register scratch = i4;
  MovDoubleToInt64(scratch, src);
  ReverseBytes(scratch, 8);
  StoreUnaligned(scratch, MemOperand(base, index), kDoubleSize);
}

inline void MaglevAssembler::SignExtend32To64Bits(Register dst, Register src) {
  LoadS32(dst, src);
}
inline void MaglevAssembler::NegateInt32(Register val) {
  LoadS32(val, val);
  sub(val, g0, val);
}

inline void MaglevAssembler::ToUint8Clamped(Register result,
                                            DoubleRegister value, Label* min,
                                            Label* max, Label* done) {
  TemporaryRegisterScope temps(this);
  DoubleRegister scratch = temps.AcquireScratchDouble();
  TemporaryRegisterScope zero_scope(this);
  DoubleRegister zero = zero_scope.AcquireScratchDouble();
  MovInt64ToDouble(zero, g0);
  CmpF64(zero, value);
  // Set to 0 if NaN.
  JumpIf(unordered, min);
  JumpIf(greater_equal, min);
  LoadF64(scratch, 255.0, i4);
  CmpF64(value, scratch);
  JumpIf(greater_equal, max);
  // if value in [0, 255], then round up to the nearest.
  ConvertDoubleToInt32(result, value);
  ConvertIntToDouble(scratch, result);
  LoadF64(zero, 0.5, i4);
  faddd(scratch, scratch, zero);
  CmpF64(value, scratch);
  JumpIf(less, done);
  Label increment;
  JumpIf(greater, &increment);
  And(i4, result, Operand(1));
  JumpIf(equal, done);
  bind(&increment);
  AddS32(result, Operand(1));
  Jump(done);
}

template <typename NodeT>
inline void MaglevAssembler::DeoptIfBufferNotValid(Register array,
                                                   Register scratch,
                                                   TypedArrayAccessMode mode,
                                                   NodeT* node) {
  // A detached buffer leads to megamorphic feedback, so we won't have a deopt
  // loop if we deopt here.
  LoadTaggedField(scratch,
                  FieldMemOperand(array, JSArrayBufferView::kBufferOffset));
  LoadU32(scratch, FieldMemOperand(scratch, JSArrayBuffer::kBitFieldOffset));
  And(i4, scratch, Operand(JSArrayBuffer::NotValidMask(mode)));
  EmitEagerDeoptIf(not_equal, DeoptimizeReason::kArrayBufferWasDetached, node);
}

inline void MaglevAssembler::LoadByte(Register dst, MemOperand src) {
  LoadU8(dst, src);
}

inline Condition MaglevAssembler::IsCallableAndNotUndetectable(
    Register map, Register scratch) {
  LoadU8(scratch, FieldMemOperand(map, Map::kBitFieldOffset));
  And(scratch, Operand(Map::Bits1::IsUndetectableBit::kMask |
                       Map::Bits1::IsCallableBit::kMask));
  CmpU32(scratch, Operand(Map::Bits1::IsCallableBit::kMask));
  return equal;
}

inline Condition MaglevAssembler::IsNotCallableNorUndetactable(
    Register map, Register scratch) {
  LoadU8(scratch, FieldMemOperand(map, Map::kBitFieldOffset));
  And(scratch, Operand(Map::Bits1::IsUndetectableBit::kMask |
                       Map::Bits1::IsCallableBit::kMask));
  return equal;
}

inline void MaglevAssembler::LoadInstanceType(Register instance_type,
                                              Register heap_object) {
  LoadMap(instance_type, heap_object);
  LoadU16(instance_type,
          FieldMemOperand(instance_type, Map::kInstanceTypeOffset));
}

inline void MaglevAssembler::JumpIfObjectType(Register heap_object,
                                              InstanceType type, Label* target,
                                              Label::Distance distance) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  CompareObjectType(heap_object, scratch, scratch, type);
  JumpIf(kEqual, target, distance);
}

inline void MaglevAssembler::JumpIfNotObjectType(Register heap_object,
                                                 InstanceType type,
                                                 Label* target,
                                                 Label::Distance distance) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  CompareObjectType(heap_object, scratch, scratch, type);
  JumpIf(kNotEqual, target, distance);
}

inline void MaglevAssembler::AssertObjectType(Register heap_object,
                                              InstanceType type,
                                              AbortReason reason) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  AssertNotSmi(heap_object);
  CompareObjectType(heap_object, scratch, scratch, type);
  Assert(kEqual, reason);
}

inline void MaglevAssembler::BranchOnObjectType(
    Register heap_object, InstanceType type, Label* if_true,
    Label::Distance true_distance, bool fallthrough_when_true, Label* if_false,
    Label::Distance false_distance, bool fallthrough_when_false) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  CompareObjectType(heap_object, scratch, scratch, type);
  Branch(kEqual, if_true, true_distance, fallthrough_when_true, if_false,
         false_distance, fallthrough_when_false);
}

inline void MaglevAssembler::JumpIfObjectTypeInRange(Register heap_object,
                                                     InstanceType lower_limit,
                                                     InstanceType higher_limit,
                                                     Label* target,
                                                     Label::Distance distance) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  CompareObjectTypeRange(heap_object, scratch, scratch, scratch, lower_limit,
                         higher_limit);
  JumpIf(kUnsignedLessThanEqual, target, distance);
}

inline void MaglevAssembler::JumpIfObjectTypeNotInRange(
    Register heap_object, InstanceType lower_limit, InstanceType higher_limit,
    Label* target, Label::Distance distance) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  CompareObjectTypeRange(heap_object, scratch, scratch, scratch, lower_limit,
                         higher_limit);
  JumpIf(kUnsignedGreaterThan, target, distance);
}

inline void MaglevAssembler::AssertObjectTypeInRange(Register heap_object,
                                                     InstanceType lower_limit,
                                                     InstanceType higher_limit,
                                                     AbortReason reason) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  AssertNotSmi(heap_object);
  CompareObjectTypeRange(heap_object, scratch, scratch, scratch, lower_limit,
                         higher_limit);
  Assert(kUnsignedLessThanEqual, reason);
}

inline void MaglevAssembler::BranchOnObjectTypeInRange(
    Register heap_object, InstanceType lower_limit, InstanceType higher_limit,
    Label* if_true, Label::Distance true_distance, bool fallthrough_when_true,
    Label* if_false, Label::Distance false_distance,
    bool fallthrough_when_false) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  CompareObjectTypeRange(heap_object, scratch, scratch, scratch, lower_limit,
                         higher_limit);
  Branch(kUnsignedLessThanEqual, if_true, true_distance, fallthrough_when_true,
         if_false, false_distance, fallthrough_when_false);
}

inline void MaglevAssembler::JumpIfJSAnyIsNotPrimitive(
    Register heap_object, Label* target, Label::Distance distance) {
  // If the type of the result (stored in its map) is less than
  // FIRST_JS_RECEIVER_TYPE, it is not an object in the ECMA sense.
  static_assert(LAST_JS_RECEIVER_TYPE == LAST_TYPE);
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  MacroAssembler::CompareObjectType<true>(heap_object, scratch, scratch,
                                          FIRST_JS_RECEIVER_TYPE);
  JumpIf(greater_equal, target, distance);
}

inline void MaglevAssembler::CompareMapWithRoot(Register object,
                                                RootIndex index,
                                                Register scratch) {
  LoadMap(scratch, object);
  CompareRoot(scratch, index);
}

inline void MaglevAssembler::CompareInstanceType(Register map,
                                                 InstanceType instance_type) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  MacroAssembler::CompareInstanceType(map, scratch, instance_type);
}

inline Condition MaglevAssembler::CompareInstanceTypeRange(
    Register map, Register instance_type_out, InstanceType lower_limit,
    InstanceType higher_limit) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  MacroAssembler::CompareInstanceTypeRange(map, instance_type_out, scratch,
                                           lower_limit, higher_limit);
  return kUnsignedLessThanEqual;
}

inline void MaglevAssembler::CompareFloat64AndJumpIf(
    DoubleRegister src1, DoubleRegister src2, Condition cond, Label* target,
    Label* nan_failed, Label::Distance distance) {
  CmpF64(src1, src2);
  JumpIf(overflow, nan_failed);
  JumpIf(cond, target, distance);
}

inline void MaglevAssembler::CompareFloat64AndBranch(
    DoubleRegister src1, DoubleRegister src2, Condition cond,
    BasicBlock* if_true, BasicBlock* if_false, BasicBlock* next_block,
    BasicBlock* nan_failed) {
  CmpF64(src1, src2);
  JumpIf(overflow, nan_failed->label());
  Branch(cond, if_true, if_false, next_block);
}

inline void MaglevAssembler::PrepareCallCFunction(int num_reg_arguments,
                                                  int num_double_registers) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  MacroAssembler::PrepareCallCFunction(num_reg_arguments, num_double_registers,
                                       scratch);
}

inline void MaglevAssembler::CallSelf() {
  DCHECK(code_gen_state()->entry_label()->is_bound());
  Call(code_gen_state()->entry_label());
}

inline void MaglevAssembler::Jump(Label* target, Label::Distance) {
  // Any eager deopts should go through JumpIf to enable us to support the
  // `--deopt-every-n-times` stress mode. See EmitEagerDeoptStress.
  DCHECK(!IsDeoptLabel(target));
  b(target);
}

inline void MaglevAssembler::JumpToDeopt(Label* target) {
  DCHECK(IsDeoptLabel(target));
  b(target);
}

inline void MaglevAssembler::EmitEagerDeoptStress(Label* target) {
  // --deopt-every-n-times is not implemented on SPARC64.
}

inline void MaglevAssembler::JumpIf(Condition cond, Label* target,
                                    Label::Distance) {
  b(cond, target);
}

inline void MaglevAssembler::JumpIfRoot(Register with, RootIndex index,
                                        Label* if_equal,
                                        Label::Distance distance) {
  MacroAssembler::JumpIfRoot(with, index, if_equal);
}

inline void MaglevAssembler::JumpIfNotRoot(Register with, RootIndex index,
                                           Label* if_not_equal,
                                           Label::Distance distance) {
  MacroAssembler::JumpIfNotRoot(with, index, if_not_equal);
}

inline void MaglevAssembler::JumpIfSmi(Register src, Label* on_smi,
                                       Label::Distance distance) {
  MacroAssembler::JumpIfSmi(src, on_smi);
}

inline void MaglevAssembler::JumpIfNotSmi(Register src, Label* on_smi,
                                          Label::Distance distance) {
  MacroAssembler::JumpIfNotSmi(src, on_smi);
}

void MaglevAssembler::JumpIfByte(Condition cc, Register value, int32_t byte,
                                 Label* target, Label::Distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  Register scratch = i4;
  mov(scratch, Operand(byte));
  LoadS8(scratch, scratch);
  CmpS32(value, scratch);
  b(cc, target);
}

void MaglevAssembler::Float64SilenceNan(DoubleRegister value) {
  CanonicalizeNaN(value, value);
}

#ifdef V8_ENABLE_UNDEFINED_DOUBLE
void MaglevAssembler::JumpIfUndefinedNan(DoubleRegister value, Register scratch,
                                         Label* target,
                                         Label::Distance distance) {
  // TODO(leszeks): Right now this only accepts Zone-allocated target labels.
  // This works because all callsites are jumping to either a deopt, deferred
  // code, or a basic block. If we ever need to jump to an on-stack label, we
  // have to add support for it here change the caller to pass a ZoneLabelRef.
  DCHECK(compilation_info()->zone()->Contains(target));
  ZoneLabelRef is_undefined = ZoneLabelRef::UnsafeFromLabelPointer(target);
  ZoneLabelRef is_not_undefined(this);
  CmpF64(value, value);
  JumpIf(unordered,
         MakeDeferredCode(
             [](MaglevAssembler* masm, DoubleRegister value, Register scratch,
                ZoneLabelRef is_undefined, ZoneLabelRef is_not_undefined) {
               masm->MovDoubleToInt64(scratch, value);
               masm->ShiftRightU64(scratch, scratch, Operand(32));
               masm->CompareInt32AndJumpIf(scratch, kUndefinedNanUpper32,
                                           kEqual, *is_undefined);
               masm->Jump(*is_not_undefined);
             },
             value, scratch, is_undefined, is_not_undefined));
  bind(*is_not_undefined);
}

void MaglevAssembler::JumpIfUndefinedNan(MemOperand operand, Label* target,
                                         Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  Register upper_bits = temps.AcquireScratch();
  LoadU32(upper_bits, operand, i4);
  CompareInt32AndJumpIf(upper_bits, kUndefinedNanUpper32, kEqual, target,
                        distance);
}

void MaglevAssembler::JumpIfNotUndefinedNan(DoubleRegister value,
                                            Register scratch, Label* target,
                                            Label::Distance distance) {
  JumpIfNotNan(value, target, distance);
  MovDoubleToInt64(scratch, value);
  ShiftRightU64(scratch, scratch, Operand(32));
  CompareInt32AndJumpIf(scratch, kUndefinedNanUpper32, kNotEqual, target,
                        distance);
}
#endif  // V8_ENABLE_UNDEFINED_DOUBLE

void MaglevAssembler::JumpIfHoleNan(DoubleRegister value, Register scratch,
                                    Label* target, Label::Distance distance) {
  // TODO(leszeks): Right now this only accepts Zone-allocated target labels.
  // This works because all callsites are jumping to either a deopt, deferred
  // code, or a basic block. If we ever need to jump to an on-stack label, we
  // have to add support for it here change the caller to pass a ZoneLabelRef.
  DCHECK(compilation_info()->zone()->Contains(target));
  ZoneLabelRef is_hole = ZoneLabelRef::UnsafeFromLabelPointer(target);
  ZoneLabelRef is_not_hole(this);
  CmpF64(value, value);
  JumpIf(unordered,
         MakeDeferredCode(
             [](MaglevAssembler* masm, DoubleRegister value, Register scratch,
                ZoneLabelRef is_hole, ZoneLabelRef is_not_hole) {
               masm->MovDoubleToInt64(scratch, value);
               masm->ShiftRightU64(scratch, scratch, Operand(32));
               masm->CompareInt32AndJumpIf(scratch, kHoleNanUpper32, kEqual,
                                           *is_hole);
               masm->Jump(*is_not_hole);
             },
             value, scratch, is_hole, is_not_hole));
  bind(*is_not_hole);
}

void MaglevAssembler::JumpIfHoleNan(MemOperand operand, Label* target,
                                    Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  Register upper_bits = temps.AcquireScratch();
  LoadU32(upper_bits, operand, i4);
  CompareInt32AndJumpIf(upper_bits, kHoleNanUpper32, kEqual, target, distance);
}

void MaglevAssembler::JumpIfNotHoleNan(DoubleRegister value, Register scratch,
                                       Label* target,
                                       Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  CmpF64(value, value);
  JumpIf(ordered, target, distance);

  MovDoubleToInt64(scratch, value);
  ShiftRightU64(scratch, scratch, Operand(32));
  CompareInt32AndJumpIf(scratch, kHoleNanUpper32, kNotEqual, target, distance);
}

void MaglevAssembler::JumpIfNotHoleNan(MemOperand operand, Label* target,
                                       Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  Register upper_bits = temps.AcquireScratch();
  LoadU32(upper_bits, operand, i4);
  CompareInt32AndJumpIf(upper_bits, kHoleNanUpper32, kNotEqual, target,
                        distance);
}

void MaglevAssembler::JumpIfNan(DoubleRegister value, Label* target,
                                Label::Distance distance) {
  CmpF64(value, value);
  JumpIf(unordered, target, distance);
}

void MaglevAssembler::JumpIfNotNan(DoubleRegister value, Label* target,
                                   Label::Distance distance) {
  CmpF64(value, value);
  JumpIf(ordered, target, distance);
}

void MaglevAssembler::CompareIntPtrAndJumpIf(Register i5, Register o0,
                                             Condition cond, Label* target,
                                             Label::Distance distance) {
  CmpS64(i5, o0);
  b(cond, target);
}

inline void MaglevAssembler::CompareInt32AndJumpIf(Register i5, Register o0,
                                                   Condition cond,
                                                   Label* target,
                                                   Label::Distance distance) {
  CmpS32(i5, o0);
  b(cond, target);
}

inline void MaglevAssembler::CompareIntPtrAndJumpIf(Register i5, int32_t value,
                                                    Condition cond,
                                                    Label* target,
                                                    Label::Distance distance) {
  CmpS64(i5, Operand(value));
  b(cond, target);
}

inline void MaglevAssembler::CompareInt32AndJumpIf(Register i5, int32_t value,
                                                   Condition cond,
                                                   Label* target,
                                                   Label::Distance distance) {
  CmpS32(i5, Operand(value));
  JumpIf(cond, target);
}

inline void MaglevAssembler::CompareInt32AndAssert(Register i5, Register o0,
                                                   Condition cond,
                                                   AbortReason reason) {
  CmpS32(i5, o0);
  Assert(cond, reason);
}

inline void MaglevAssembler::CompareInt32AndAssert(Register i5, int32_t value,
                                                   Condition cond,
                                                   AbortReason reason) {
  CmpS32(i5, Operand(value));
  Assert(cond, reason);
}

inline void MaglevAssembler::CompareInt32AndBranch(
    Register i5, int32_t value, Condition cond, Label* if_true,
    Label::Distance true_distance, bool fallthrough_when_true, Label* if_false,
    Label::Distance false_distance, bool fallthrough_when_false) {
  CmpS32(i5, Operand(value));
  Branch(cond, if_true, true_distance, fallthrough_when_true, if_false,
         false_distance, fallthrough_when_false);
}

inline void MaglevAssembler::CompareInt32AndBranch(
    Register i5, Register o0, Condition cond, Label* if_true,
    Label::Distance true_distance, bool fallthrough_when_true, Label* if_false,
    Label::Distance false_distance, bool fallthrough_when_false) {
  CmpS32(i5, o0);
  Branch(cond, if_true, true_distance, fallthrough_when_true, if_false,
         false_distance, fallthrough_when_false);
}

inline void MaglevAssembler::CompareIntPtrAndBranch(
    Register i5, int32_t value, Condition cond, Label* if_true,
    Label::Distance true_distance, bool fallthrough_when_true, Label* if_false,
    Label::Distance false_distance, bool fallthrough_when_false) {
  CmpS64(i5, Operand(value));
  Branch(cond, if_true, true_distance, fallthrough_when_true, if_false,
         false_distance, fallthrough_when_false);
}

inline void MaglevAssembler::CompareSmiAndJumpIf(Register i5, Tagged<Smi> value,
                                                 Condition cond, Label* target,
                                                 Label::Distance distance) {
  CmpSmiLiteral(i5, value, i4);
  JumpIf(cond, target);
}

inline void MaglevAssembler::CompareSmiAndAssert(Register i5, Tagged<Smi> value,
                                                 Condition cond,
                                                 AbortReason reason) {
  if (!v8_flags.debug_code) return;
  AssertSmi(i5);
  CmpSmiLiteral(i5, value);
  Assert(cond, reason);
}

inline void MaglevAssembler::CompareByteAndJumpIf(MemOperand left, int8_t right,
                                                  Condition cond,
                                                  Register scratch,
                                                  Label* target,
                                                  Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  Register scratch2 = i4;
  LoadS8(scratch, left);
  mov(scratch2, Operand(right));
  LoadS8(scratch2, scratch2);
  CmpS32(scratch, scratch2);
  DCHECK(cond == equal || cond == not_equal);
  JumpIf(cond, target, distance);
}

inline void MaglevAssembler::CompareTaggedAndJumpIf(Register reg,
                                                    Tagged<Smi> value,
                                                    Condition cond,
                                                    Label* target,
                                                    Label::Distance distance) {
  if (COMPRESS_POINTERS_BOOL) {
    CmpSmiLiteral(reg, value, i4);
  } else {
    Move(i4, value);
    CmpS64(reg, i4);
  }
  JumpIf(cond, target);
}

inline void MaglevAssembler::CompareTaggedAndJumpIf(Register reg,
                                                    Handle<HeapObject> obj,
                                                    Condition cond,
                                                    Label* target,
                                                    Label::Distance distance) {
  MaglevAssembler::TemporaryRegisterScope temps(this);
  Register scratch = i4;
  MacroAssembler::Move(scratch, obj);
  CmpTagged(reg, scratch);
  b(cond, target);
}

inline void MaglevAssembler::CompareTaggedAndJumpIf(Register src1,
                                                    Register src2,
                                                    Condition cond,
                                                    Label* target,
                                                    Label::Distance distance) {
  CmpTagged(src1, src2);
  JumpIf(cond, target, distance);
}

inline void MaglevAssembler::CompareDoubleAndJumpIfZeroOrNaN(
    DoubleRegister reg, Label* target, Label::Distance distance) {
  TemporaryRegisterScope zero_scope(this);
  DoubleRegister zero = zero_scope.AcquireScratchDouble();
  MovInt64ToDouble(zero, g0);
  CmpF64(zero, reg);
  JumpIf(equal, target);
  JumpIf(overflow, target);  // NaN check
}

inline void MaglevAssembler::CompareDoubleAndJumpIfZeroOrNaN(
    MemOperand operand, Label* target, Label::Distance distance) {
  TemporaryRegisterScope zero_scope(this);
  DoubleRegister zero = zero_scope.AcquireScratchDouble();
  MovInt64ToDouble(zero, g0);
  DoubleRegister loaded = zero_scope.AcquireScratchDouble();
  LoadF64(loaded, operand);
  CmpF64(zero, loaded);
  JumpIf(equal, target);
  JumpIf(overflow, target);  // NaN check
}

inline void MaglevAssembler::TestInt32AndJumpIfAnySet(
    Register value, int32_t mask, Label* target, Label::Distance distance) {
  And(i4, value, Operand(mask));
  bne(target);
}

inline void MaglevAssembler::TestInt32AndJumpIfAnySet(
    MemOperand operand, int32_t mask, Label* target, Label::Distance distance) {
  LoadU32(i4, operand);
  And(i4, Operand(mask));
  bne(target);
}

inline void MaglevAssembler::TestUint8AndJumpIfAnySet(
    MemOperand operand, uint8_t mask, Label* target, Label::Distance distance) {
  LoadU8(i4, operand);
  And(i4, Operand(mask));
  bne(target, distance);
}

inline void MaglevAssembler::TestInt32AndJumpIfAllClear(
    Register value, int32_t mask, Label* target, Label::Distance distance) {
  And(i4, value, Operand(mask));
  beq(target);
}

inline void MaglevAssembler::TestInt32AndJumpIfAllClear(
    MemOperand operand, int32_t mask, Label* target, Label::Distance distance) {
  LoadU32(i4, operand);
  And(i4, Operand(mask));
  beq(target);
}

inline void MaglevAssembler::TestUint8AndJumpIfAllClear(
    MemOperand operand, uint8_t mask, Label* target, Label::Distance distance) {
  LoadU8(i4, operand);
  And(i4, Operand(mask));
  beq(target, distance);
}

inline void MaglevAssembler::LoadContextCellState(Register state,
                                                  Register cell) {
  LoadU32(state, FieldMemOperand(cell, offsetof(ContextCell, state_)));
}

inline void MaglevAssembler::LoadContextCellInt32Value(Register value,
                                                       Register cell) {
  AssertContextCellState(cell, ContextCell::kInt32);
  LoadU32(value, FieldMemOperand(cell, offsetof(ContextCell, double_value_)));
}

inline void MaglevAssembler::LoadContextCellFloat64Value(DoubleRegister value,
                                                         Register cell) {
  AssertContextCellState(cell, ContextCell::kFloat64);
  LoadFloat64(value,
              FieldMemOperand(cell, offsetof(ContextCell, double_value_)));
}

inline void MaglevAssembler::StoreContextCellInt32Value(Register cell,
                                                        Register value) {
  StoreU32(value, FieldMemOperand(cell, offsetof(ContextCell, double_value_)));
}

inline void MaglevAssembler::StoreContextCellFloat64Value(
    Register cell, DoubleRegister value) {
  StoreF64(value, FieldMemOperand(cell, offsetof(ContextCell, double_value_)));
}

inline void MaglevAssembler::LoadHeapNumberValue(DoubleRegister result,
                                                 Register heap_number) {
  LoadF64(result, FieldMemOperand(heap_number, offsetof(HeapNumber, value_)));
}

inline void MaglevAssembler::Int32ToDouble(DoubleRegister result,
                                           Register src) {
  ConvertIntToDouble(result, src);
}

inline void MaglevAssembler::IntPtrToDouble(DoubleRegister result,
                                            Register src) {
  ConvertInt64ToDouble(result, src);
}

inline void MaglevAssembler::Uint32ToDouble(DoubleRegister result,
                                            Register src) {
  ConvertUnsignedIntToDouble(result, src);
}

inline void MaglevAssembler::Pop(Register dst) { pop(dst); }

inline void MaglevAssembler::AssertStackSizeCorrect() {
  if (v8_flags.slow_debug_code) {
    MacroAssembler::Move(i4, sp);
    AddU64(i4, Operand(code_gen_state()->stack_slots() * kSystemPointerSize +
                       StandardFrameConstants::kFixedFrameSizeFromFp));
    CmpU64(i4, fp);
    Assert(equal, AbortReason::kStackAccessBelowStackPointer);
  }
}

inline Condition MaglevAssembler::FunctionEntryStackCheck(
    int stack_check_offset) {
  TemporaryRegisterScope temps(this);
  Register interrupt_stack_limit = temps.AcquireScratch();
  LoadStackLimit(interrupt_stack_limit, StackLimitKind::kInterruptStackLimit);

  Register stack_cmp_reg = i4;
  MacroAssembler::Move(stack_cmp_reg, sp);
  if (stack_check_offset >= kStackLimitSlackForDeoptimizationInBytes) {
    stack_cmp_reg = i4;
    MacroAssembler::SubS64(stack_cmp_reg, stack_cmp_reg, stack_check_offset);
  }
  CmpU64(stack_cmp_reg, interrupt_stack_limit);
  return greater_equal_unsigned;
}

inline void MaglevAssembler::FinishCode() {}

template <typename NodeT>
inline void MaglevAssembler::EmitEagerDeoptIfNotEqual(DeoptimizeReason reason,
                                                      NodeT* node) {
  EmitEagerDeoptIf(not_equal, reason, node);
}

template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr, Register dst,
                                      Register src) {
  Move(dst, src);
}
template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr, Register dst,
                                      MemOperand src) {
  switch (repr) {
    case MachineRepresentation::kWord32:
      return LoadU64(dst, src);
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTaggedSigned:
    case MachineRepresentation::kWord64:
      return LoadU64(dst, src);
    default:
      UNREACHABLE();
  }
}
template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr,
                                      MemOperand dst, Register src) {
  switch (repr) {
    case MachineRepresentation::kWord32:
      return StoreU64(src, dst);
    case MachineRepresentation::kTagged:
    case MachineRepresentation::kTaggedPointer:
    case MachineRepresentation::kTaggedSigned:
      return StoreU64(src, dst);
    default:
      UNREACHABLE();
  }
}
template <>
inline void MaglevAssembler::MoveRepr(MachineRepresentation repr,
                                      MemOperand dst, MemOperand src) {
  TemporaryRegisterScope temps(this);
  Register scratch = temps.AcquireScratch();
  MoveRepr(repr, scratch, src);
  MoveRepr(repr, dst, scratch);
}

inline void MaglevAssembler::MaybeEmitPlaceHolderForDeopt() {
  // Implemented only for x64.
}

}  // namespace maglev
}  // namespace internal
}  // namespace v8

#endif  // V8_MAGLEV_SPARC64_MAGLEV_ASSEMBLER_SPARC64_INL_H_
