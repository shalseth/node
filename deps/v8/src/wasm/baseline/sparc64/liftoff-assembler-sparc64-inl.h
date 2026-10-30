// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_WASM_BASELINE_SPARC64_LIFTOFF_ASSEMBLER_SPARC64_INL_H_
#define V8_WASM_BASELINE_SPARC64_LIFTOFF_ASSEMBLER_SPARC64_INL_H_
#include "src/codegen/interface-descriptors-inl.h"
#include "src/compiler/backend/sparc64/code-generator-sparc64-inl.h"
#include "src/wasm/baseline/liftoff-assembler.h"
#include "src/wasm/baseline/parallel-move-inl.h"
#include "src/wasm/baseline/sparc64/liftoff-simd-sparc64-inl.h"
#include "src/wasm/object-access.h"
#include "src/wasm/wasm-linkage.h"
#include "src/wasm/wasm-objects.h"
namespace v8::internal::wasm {
namespace liftoff {
using sparc64::Branch;
using sparc64::MemoryAddress;
inline void PadCall(LiftoffAssembler* assm, int start) {
  // Debugger recompilation must preserve call lengths across literal alignment.
  if ((assm->pc_offset() - start) % kSystemPointerSize == 0) assm->nop();
}
inline void StoreNativeValue(LiftoffAssembler* assm, MemOperand dst,
                             const LiftoffAssembler::VarState& src) {
  if (src.is_reg()) {
    if (src.reg().is_gp())
      assm->MacroAssembler::Move(g1, src.reg().gp());
    else if (src.kind() == kF32)
      assm->MovFloatToInt(g1, src.reg().fp());
    else if (src.kind() == kF64)
      assm->MovDoubleToInt64(g1, src.reg().fp());
    else if (src.kind() == kS128) {
      StoreSimd(assm, src.reg().fp(), dst);
      return;
    } else
      UNREACHABLE();
  } else if (src.is_const()) {
    assm->MacroAssembler::Move(g1, static_cast<int64_t>(src.i32_const()));
  } else {
    if (src.kind() == kS128) {
      for (int half = 0; half < 2; ++half) {
        assm->LoadU64(g1, MemOperand(fp, -src.offset() + half * 8));
        assm->StoreU64(g1, SimdOffset(dst, half * 8));
      }
      return;
    }
    int offset =
        -src.offset() + ((src.kind() == kI32 || src.kind() == kF32) ? 4 : 0);
    assm->LoadUnaligned(g1, MemOperand(fp, offset),
                        value_kind_size(src.kind()));
  }
  assm->StoreUnaligned(g1, dst, value_kind_size(src.kind()));
}
using sparc64::CanonicalizeNaN;
using sparc64::SaturatingFloatToInteger;
inline void AtomicRmw(LiftoffAssembler* assm, Register base, Register offset,
                      uintptr_t immediate, LiftoffRegister value,
                      Register expected, LiftoffRegister result, StoreType type,
                      uint32_t* protected_load_pc, bool offset64,
                      LiftoffAssembler::Endianness endianness,
                      compiler::Sparc64AtomicOp op) {
  LiftoffRegList pinned{base, value, result};
  if (offset != no_reg) pinned.set(offset);
  if (expected != no_reg) pinned.set(expected);
  Register mask = pinned.set(assm->GetUnusedRegister(kGpReg, pinned)).gp();
  Register temp = assm->GetUnusedRegister(kGpReg, pinned).gp();
  MemoryAddress(assm, base, offset, immediate, offset64);
  // Keep adjacent lanes intact when subword operations share a CASX word.
  compiler::EmitSparc64Atomic(
      assm, result.gp(), g2, g0, value.gp(), expected, mask, temp, type.size(),
      false, op, endianness == LiftoffAssembler::kLittle, protected_load_pc);
}
}  // namespace liftoff
constexpr int LiftoffAssembler::StaticStackFrameSize() {
  return WasmLiftoffFrameConstants::kFeedbackVectorOffset;
}
int LiftoffAssembler::SlotSizeForType(ValueKind kind) {
  return kind == kS128 ? 16 : kSystemPointerSize;
}
bool LiftoffAssembler::NeedsAlignment(ValueKind kind) {
  return kind == kS128 || is_reference(kind);
}
int LiftoffAssembler::PrepareStackFrame() {
  Align(kSystemPointerSize);
  int offset = pc_offset();
  LoadLiteral(g1, 0);
  sub(sp, sp, g1);
  nop();
  nop();
  return offset;
}
void LiftoffAssembler::PatchPrepareStackFrame(int offset,
                                              SafepointTableBuilder* safepoints,
                                              bool feedback_vector_slot,
                                              size_t stack_param_slots) {
  int size = GetTotalFrameSize() - 2 * kSystemPointerSize;
  if (feedback_vector_slot) size -= kSystemPointerSize;
  Assembler patcher(AssemblerOptions{},
                    ExternalAssemblerBuffer(buffer_start_ + offset, 80));
  patcher.LoadLiteral(g1, size);
  if (size < 4 * KB) return;

  // Large frames must check the limit before touching their spill slots.
  const int ool_offset = pc_offset();
  patcher.mov(g4, o7);
  patcher.call(ool_offset - (offset + patcher.pc_offset()));
  patcher.mov(o7, g4);
  const int continuation = offset + patcher.pc_offset();
  LoadStackAddress(g2);
  sub(g2, g2, g1);
  LoadU64(g3, MemOperand(kRootRegister, IsolateData::real_jslimit_offset()));
  cmp(g2, g3);
  Label allocate;
  liftoff::Branch(this, greater_equal_unsigned, ConditionCodes::kXcc,
                  &allocate);
  CallBuiltin(Builtin::kWasmStackOverflow);
  safepoints->DefineSafepoint(this);
  Trap();
  bind(&allocate);
  sub(sp, sp, g1);
  mov(g4, o7);
  call(continuation - pc_offset());
  mov(o7, g4);
}

void LiftoffAssembler::FinishCode() {}
void LiftoffAssembler::AbortCompilation() {}
void LiftoffAssembler::CheckStackShrink() {}
void LiftoffAssembler::ResetOSRTarget() {}
void LiftoffAssembler::MaybeOSR() {}
void LiftoffAssembler::CheckTierUp(int declared_func_index, int budget_used,
                                   Label* ool_label, const FreezeCacheState&) {
  LoadInstanceDataFromFrame(g2);
  LoadU64(g2, FieldMemOperand(
                  g2, WasmTrustedInstanceData::kTieringBudgetArrayOffset));
  MacroAssembler::Move(g3, intptr_t{declared_func_index} * kInt32Size);
  add(g2, g2, g3);
  LoadS32(g1, MemOperand(g2));
  MacroAssembler::Move(g3, budget_used);
  sub(g1, g1, g3);
  StoreU32(g1, MemOperand(g2));
  cmp(g1, 0);
  liftoff::Branch(this, less, ConditionCodes::kIcc, ool_label);
}
Register LiftoffAssembler::LoadOldFramePointer() { return fp; }
void LiftoffAssembler::LoadConstant(LiftoffRegister dst, WasmValue value) {
  switch (value.type().kind()) {
    case kI32:
      MacroAssembler::Move(dst.gp(), value.to_i32());
      break;
    case kI64:
      MacroAssembler::Move(dst.gp(), value.to_i64());
      break;
    case kF32:
      MacroAssembler::Move(g1, value.to_f32_boxed().get_bits());
      MovIntToFloat(dst.fp(), g1);
      break;
    case kF64:
      MacroAssembler::Move(g1, value.to_f64_boxed().get_bits());
      MovInt64ToDouble(dst.fp(), g1);
      break;
    default:
      bailout(kUnsupportedArchitecture, "constant type");
  }
}
void LiftoffAssembler::LoadInstanceDataFromFrame(Register dst) {
  LoadU64(dst, MemOperand(fp, -WasmLiftoffFrameConstants::kInstanceDataOffset));
}
void LiftoffAssembler::SpillInstanceData(Register instance) {
  StoreU64(instance,
           MemOperand(fp, -WasmLiftoffFrameConstants::kInstanceDataOffset));
}
void LiftoffAssembler::LoadFromInstance(Register dst, Register instance,
                                        int offset, int size) {
  switch (size) {
    case 1:
      LoadU8(dst, MemOperand(instance, offset));
      break;
    case 2:
      LoadU16(dst, MemOperand(instance, offset));
      break;
    case 4:
      LoadU32(dst, MemOperand(instance, offset));
      break;
    case 8:
      LoadU64(dst, MemOperand(instance, offset));
      break;
    default:
      UNREACHABLE();
  }
}
void LiftoffAssembler::LoadTaggedPointerFromInstance(Register dst,
                                                     Register instance,
                                                     int offset) {
  LoadU64(dst, MemOperand(instance, offset));
}
void LiftoffAssembler::LoadTrustedPointer(Register dst, Register src_addr,
                                          int offset, IndirectPointerTag) {
  LoadU64(dst, MemOperand(src_addr, offset));
}
void LiftoffAssembler::LoadProtectedPointer(Register dst, Register src_addr,
                                            int32_t offset) {
  LoadU64(dst, MemOperand(src_addr, offset));
}
void LiftoffAssembler::LoadFullPointer(Register dst, Register src_addr,
                                       int32_t offset) {
  LoadU64(dst, MemOperand(src_addr, offset));
}
void LiftoffAssembler::Move(Register dst, Register src, ValueKind kind) {
  MacroAssembler::Move(dst, src);
}
void LiftoffAssembler::Move(DoubleRegister dst, DoubleRegister src,
                            ValueKind kind) {
  if (kind == kF32)
    fmovs(dst, src);
  else if (kind == kF64)
    fmovd(dst, src);
  else if (kind == kS128)
    liftoff::MoveSimd(this, dst, src);
  else
    UNREACHABLE();
}
void LiftoffAssembler::Spill(int offset, LiftoffRegister src, ValueKind kind) {
  RecordUsedSpillOffset(offset);
  MemOperand slot(fp, -offset + ((kind == kI32 || kind == kF32) ? 4 : 0));
  if (is_reference(kind) || kind == kI64)
    StoreU64(src.gp(), slot);
  else if (kind == kI32)
    StoreU32(src.gp(), slot);
  else if (kind == kF32)
    StoreF32(src.fp(), slot);
  else if (kind == kF64)
    StoreF64(src.fp(), slot);
  else if (kind == kS128)
    liftoff::StoreSimd(this, src.fp(), slot);
  else
    UNREACHABLE();
}
void LiftoffAssembler::Spill(int offset, WasmValue value) {
  RecordUsedSpillOffset(offset);
  MacroAssembler::Move(
      g1, value.type() == kWasmI32 ? value.to_i32() : value.to_i64());
  if (value.type() == kWasmI32)
    StoreU32(g1, MemOperand(fp, -offset + 4));
  else
    StoreU64(g1, MemOperand(fp, -offset));
}
void LiftoffAssembler::Fill(LiftoffRegister dst, int offset, ValueKind kind) {
  MemOperand slot(fp, -offset + ((kind == kI32 || kind == kF32) ? 4 : 0));
  if (is_reference(kind) || kind == kI64)
    LoadU64(dst.gp(), slot);
  else if (kind == kI32)
    LoadU32(dst.gp(), slot);
  else if (kind == kF32)
    LoadF32(dst.fp(), slot);
  else if (kind == kF64)
    LoadF64(dst.fp(), slot);
  else if (kind == kS128)
    liftoff::LoadSimd(this, dst.fp(), slot);
  else
    UNREACHABLE();
}
void LiftoffAssembler::FillStackSlotsWithZero(int start, int size) {
  DCHECK_EQ(0, size % 8);
  for (int off = start + 8; off <= start + size; off += 8)
    StoreU64(g0, MemOperand(fp, -off));
  RecordUsedSpillOffset(start + size);
}
void LiftoffAssembler::LoadSpillAddress(Register dst, int offset,
                                        ValueKind kind) {
  AddS64(dst, fp, -offset + ((kind == kI32 || kind == kF32) ? 4 : 0));
}
void LiftoffAssembler::MoveStackValue(uint32_t dst, uint32_t src,
                                      ValueKind kind) {
  RecordUsedSpillOffset(dst);
  if (kind == kS128) {
    for (int half = 0; half < 2; ++half) {
      LoadU64(g1, MemOperand(fp, -src + half * 8));
      StoreU64(g1, MemOperand(fp, -dst + half * 8));
    }
    return;
  }
  if (value_kind_size(kind) == 4) {
    LoadU32(g1, MemOperand(fp, -src + (value_kind_size(kind) == 4 ? 4 : 0)));
    StoreU32(g1, MemOperand(fp, -dst + (value_kind_size(kind) == 4 ? 4 : 0)));
  } else {
    LoadU64(g1, MemOperand(fp, -src + (value_kind_size(kind) == 4 ? 4 : 0)));
    StoreU64(g1, MemOperand(fp, -dst + (value_kind_size(kind) == 4 ? 4 : 0)));
  }
}
void LiftoffAssembler::LoadCallerFrameSlot(LiftoffRegister dst, uint32_t slot,
                                           ValueKind kind) {
  int offset = (slot + 1) * kSystemPointerSize;
  if (kind == kI32)
    LoadU32(dst.gp(), MemOperand(fp, offset + 4));
  else if (kind == kI64 || is_reference(kind))
    LoadU64(dst.gp(), MemOperand(fp, offset));
  else if (kind == kF32)
    LoadF32(dst.fp(), MemOperand(fp, offset));
  else if (kind == kF64)
    LoadF64(dst.fp(), MemOperand(fp, offset));
  else if (kind == kS128)
    liftoff::LoadSimd(this, dst.fp(), MemOperand(fp, offset));
  else
    UNREACHABLE();
}
void LiftoffAssembler::LoadReturnStackSlot(LiftoffRegister dst, int offset,
                                           ValueKind kind) {
  MemOperand slot(sp, offset);
  if (kind == kI32)
    LoadU32(dst.gp(), MemOperand(sp, offset + 4));
  else if (kind == kI64 || is_reference(kind))
    LoadU64(dst.gp(), slot);
  else if (kind == kF32)
    LoadF32(dst.fp(), slot);
  else if (kind == kF64)
    LoadF64(dst.fp(), slot);
  else if (kind == kS128)
    liftoff::LoadSimd(this, dst.fp(), slot);
  else
    UNREACHABLE();
}
void LiftoffAssembler::emit_jump(Label* label) { Jump(label); }
void LiftoffAssembler::emit_jump(Register target) { Jump(target); }
void LiftoffAssembler::emit_cond_jump(Condition cond, Label* target,
                                      ValueKind kind, Register lhs,
                                      Register rhs, const FreezeCacheState&) {
  cmp(lhs, rhs == no_reg ? g0 : rhs);
  liftoff::Branch(this, cond,
                  kind == kI32 ? ConditionCodes::kIcc : ConditionCodes::kXcc,
                  target);
}
void LiftoffAssembler::emit_i32_cond_jumpi(Condition cond, Label* target,
                                           Register lhs, int imm,
                                           const FreezeCacheState& frozen) {
  MacroAssembler::Move(g1, imm);
  emit_cond_jump(cond, target, kI32, lhs, g1, frozen);
}
void LiftoffAssembler::emit_ptrsize_cond_jumpi(Condition cond, Label* target,
                                               Register lhs, int32_t imm,
                                               const FreezeCacheState& frozen) {
  MacroAssembler::Move(g1, imm);
  emit_cond_jump(cond, target, kI64, lhs, g1, frozen);
}
void LiftoffAssembler::emit_i32_set_cond(Condition cond, Register dst,
                                         Register lhs, Register rhs) {
  Label yes, done;
  cmp(lhs, rhs);
  liftoff::Branch(this, cond, ConditionCodes::kIcc, &yes);
  MacroAssembler::Move(dst, 0);
  Jump(&done);
  bind(&yes);
  MacroAssembler::Move(dst, 1);
  bind(&done);
}
void LiftoffAssembler::emit_i64_set_cond(Condition cond, Register dst,
                                         LiftoffRegister lhs,
                                         LiftoffRegister rhs) {
  Label yes, done;
  cmp(lhs.gp(), rhs.gp());
  liftoff::Branch(this, cond, ConditionCodes::kXcc, &yes);
  MacroAssembler::Move(dst, 0);
  Jump(&done);
  bind(&yes);
  MacroAssembler::Move(dst, 1);
  bind(&done);
}
void LiftoffAssembler::emit_i32_eqz(Register dst, Register src) {
  emit_i32_set_cond(equal, dst, src, g0);
}
void LiftoffAssembler::emit_i64_eqz(Register dst, LiftoffRegister src) {
  Label zero, done;
  cmp(src.gp(), g0);
  liftoff::Branch(this, equal, ConditionCodes::kXcc, &zero);
  MacroAssembler::Move(dst, 0);
  Jump(&done);
  bind(&zero);
  MacroAssembler::Move(dst, 1);
  bind(&done);
}
void LiftoffAssembler::StackCheck(Label* ool) {
  LoadU64(g1, MemOperand(kRootRegister, IsolateData::jslimit_offset()));
  LoadStackAddress(g2);
  cmp(g2, g1);
  liftoff::Branch(this, less_equal_unsigned, ConditionCodes::kXcc, ool);
}
void LiftoffAssembler::AssertUnreachable(AbortReason reason) { Abort(reason); }
void LiftoffAssembler::AllocateStackSlot(Register addr, uint32_t size) {
  AllocateStackSpace(size);
  LoadStackAddress(addr);
}
void LiftoffAssembler::DeallocateStackSlot(uint32_t size) {
  AddS64(sp, sp, size);
}
void LiftoffAssembler::CallNativeWasmCode(Address addr) {
  int start = pc_offset();
  LoadLiteral(g1, addr, RelocInfo::WASM_CALL);
  liftoff::PadCall(this, start);
  Call(g1);
}
void LiftoffAssembler::TailCallNativeWasmCode(Address addr) {
  LoadLiteral(g1, addr, RelocInfo::WASM_CALL);
  Jump(g1);
}
void LiftoffAssembler::CallBuiltin(Builtin builtin) {
  int start = pc_offset();
  LoadLiteral(g1, static_cast<Address>(builtin), RelocInfo::WASM_STUB_CALL);
  liftoff::PadCall(this, start);
  Call(g1);
}
void LiftoffAssembler::AtomicFence() { membar(kSync); }
void LiftoffAssembler::Pause() { nop(); }
bool LiftoffAssembler::supports_f16_mem_access() { return false; }

void LiftoffAssembler::emit_i32_add(Register dst, Register lhs, Register rhs) {
  add(dst, lhs, rhs);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_sub(Register dst, Register lhs, Register rhs) {
  sub(dst, lhs, rhs);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_mul(Register dst, Register lhs, Register rhs) {
  mulx(dst, lhs, rhs);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_and(Register dst, Register lhs, Register rhs) {
  and_(dst, lhs, rhs);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_or(Register dst, Register lhs, Register rhs) {
  or_(dst, lhs, rhs);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_xor(Register dst, Register lhs, Register rhs) {
  xor_(dst, lhs, rhs);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i64_add(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  add(dst.gp(), lhs.gp(), rhs.gp());
}

void LiftoffAssembler::emit_i64_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  sub(dst.gp(), lhs.gp(), rhs.gp());
}

void LiftoffAssembler::emit_i64_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  mulx(dst.gp(), lhs.gp(), rhs.gp());
}

void LiftoffAssembler::emit_i64_and(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  and_(dst.gp(), lhs.gp(), rhs.gp());
}

void LiftoffAssembler::emit_i64_or(LiftoffRegister dst, LiftoffRegister lhs,
                                   LiftoffRegister rhs) {
  or_(dst.gp(), lhs.gp(), rhs.gp());
}

void LiftoffAssembler::emit_i64_xor(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  xor_(dst.gp(), lhs.gp(), rhs.gp());
}

void LiftoffAssembler::emit_f32_add(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fadds(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, true);
}

void LiftoffAssembler::emit_f32_sub(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fsubs(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, true);
}

void LiftoffAssembler::emit_f32_mul(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fmuls(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, true);
}

void LiftoffAssembler::emit_f32_div(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fdivs(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, true);
}

void LiftoffAssembler::emit_f32_abs(DoubleRegister dst, DoubleRegister src) {
  fabss(dst, src);
}

void LiftoffAssembler::emit_f32_neg(DoubleRegister dst, DoubleRegister src) {
  fnegs(dst, src);
}

void LiftoffAssembler::emit_f32_sqrt(DoubleRegister dst, DoubleRegister src) {
  fsqrts(dst, src);
  liftoff::CanonicalizeNaN(this, dst, true);
}

void LiftoffAssembler::emit_f64_add(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  faddd(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, false);
}

void LiftoffAssembler::emit_f64_sub(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fsubd(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, false);
}

void LiftoffAssembler::emit_f64_mul(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fmuld(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, false);
}

void LiftoffAssembler::emit_f64_div(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  fdivd(dst, lhs, rhs);
  liftoff::CanonicalizeNaN(this, dst, false);
}

void LiftoffAssembler::emit_f64_abs(DoubleRegister dst, DoubleRegister src) {
  fabsd(dst, src);
}

void LiftoffAssembler::emit_f64_neg(DoubleRegister dst, DoubleRegister src) {
  fnegd(dst, src);
}

void LiftoffAssembler::emit_f64_sqrt(DoubleRegister dst, DoubleRegister src) {
  fsqrtd(dst, src);
  liftoff::CanonicalizeNaN(this, dst, false);
}

void LiftoffAssembler::CallFrameSetupStub(int declared_function_index) {
  MacroAssembler::Move(g2, StackFrame::TypeToMarker(StackFrame::WASM));
  PushCommonFrame(g2);
  LoadConstant(LiftoffRegister(kLiftoffFrameSetupFunctionReg),
               WasmValue(declared_function_index));
  CallBuiltin(Builtin::kWasmLiftoffFrameSetup);
}
void LiftoffAssembler::PrepareTailCall(int num_callee_stack_params,
                                       int stack_param_delta) {
  LoadU64(g1, MemOperand(fp, CommonFrameConstants::kCallerPCOffset));
  MacroAssembler::Push(g1);
  LoadU64(g1, MemOperand(fp, CommonFrameConstants::kCallerFPOffset));
  MacroAssembler::Push(g1);
  const int slots = num_callee_stack_params + 2;
  for (int i = slots - 1; i >= 0; --i) {
    LoadU64(g1, MemOperand(sp, i * kSystemPointerSize));
    StoreU64(g1, MemOperand(fp, (i - stack_param_delta) * kSystemPointerSize));
  }
  SubS64(g2, fp, intptr_t{stack_param_delta} * kSystemPointerSize);
  MacroAssembler::Move(sp, g2);
  MacroAssembler::Pop(fp);
  MacroAssembler::Pop(g1);
  sub(o7, g1, kSparcReturnAddressOffset);
}
void LiftoffAssembler::AtomicLoadTaggedPointer(Register dst, Register src_addr,
                                               Register offset_reg,
                                               int32_t offset_imm,
                                               AtomicMemoryOrder memory_order,
                                               uint32_t* protected_load_pc,
                                               bool offset_reg_needs_shift) {
  bailout(kUnsupportedArchitecture, "AtomicLoadTaggedPointer");
}
void LiftoffAssembler::EmitWriteBarrier(Register target_object,
                                        Operand store_location,
                                        Register stored_value,
                                        LiftoffRegList pinned) {
  bailout(kUnsupportedArchitecture, "EmitWriteBarrier");
}
void LiftoffAssembler::AtomicStoreTaggedPointer(
    Register dst_addr, Register offset_reg, int32_t offset_imm, Register src,
    LiftoffRegList pinned, AtomicMemoryOrder memory_order,
    uint32_t* protected_store_pc) {
  bailout(kUnsupportedArchitecture, "AtomicStoreTaggedPointer");
}
void LiftoffAssembler::AtomicLoad(LiftoffRegister dst, Register src_addr,
                                  Register offset_reg, uintptr_t offset_imm,
                                  LoadType type, uint32_t* protected_load_pc,
                                  AtomicMemoryOrder memory_order,
                                  LiftoffRegList pinned, bool i64_offset,
                                  Endianness endianness) {
  liftoff::MemoryAddress(this, src_addr, offset_reg, offset_imm, i64_offset);
  membar(kSync);
  if (protected_load_pc) *protected_load_pc = pc_offset();
  switch (type.size()) {
    case 1:
      LoadU8(dst.gp(), MemOperand(g2));
      break;
    case 2:
      LoadU16(dst.gp(), MemOperand(g2));
      break;
    case 4:
      LoadU32(dst.gp(), MemOperand(g2));
      break;
    case 8:
      LoadU64(dst.gp(), MemOperand(g2));
      break;
    default:
      UNREACHABLE();
  }
  membar(kSync);
  if (endianness == kLittle) {
    compiler::EmitSparc64ByteReverse(this, dst.gp(), dst.gp(), g1, g3,
                                     type.size());
  }
}
void LiftoffAssembler::AtomicStore(Register dst_addr, Register offset_reg,
                                   uintptr_t offset_imm, LiftoffRegister src,
                                   StoreType type, uint32_t* protected_store_pc,
                                   AtomicMemoryOrder memory_order,
                                   LiftoffRegList pinned, bool i64_offset,
                                   Endianness endianness) {
  liftoff::MemoryAddress(this, dst_addr, offset_reg, offset_imm, i64_offset);
  MacroAssembler::Move(g4, src.gp());
  if (endianness == kLittle) {
    compiler::EmitSparc64ByteReverse(this, g4, g4, g1, g3, type.size());
  }
  membar(kSync);
  if (protected_store_pc) *protected_store_pc = pc_offset();
  switch (type.size()) {
    case 1:
      StoreU8(g4, MemOperand(g2));
      break;
    case 2:
      StoreU16(g4, MemOperand(g2));
      break;
    case 4:
      StoreU32(g4, MemOperand(g2));
      break;
    case 8:
      StoreU64(g4, MemOperand(g2));
      break;
    default:
      UNREACHABLE();
  }
  membar(kSync);
}
void LiftoffAssembler::AtomicAdd(Register dst_addr, Register offset_reg,
                                 uintptr_t offset_imm, LiftoffRegister value,
                                 LiftoffRegister result, StoreType type,
                                 uint32_t* protected_load_pc, bool i64_offset,
                                 Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, value, no_reg,
                     result, type, protected_load_pc, i64_offset, endianness,
                     compiler::Sparc64AtomicOp::kAdd);
}
void LiftoffAssembler::AtomicSub(Register dst_addr, Register offset_reg,
                                 uintptr_t offset_imm, LiftoffRegister value,
                                 LiftoffRegister result, StoreType type,
                                 uint32_t* protected_load_pc, bool i64_offset,
                                 Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, value, no_reg,
                     result, type, protected_load_pc, i64_offset, endianness,
                     compiler::Sparc64AtomicOp::kSub);
}
void LiftoffAssembler::AtomicAnd(Register dst_addr, Register offset_reg,
                                 uintptr_t offset_imm, LiftoffRegister value,
                                 LiftoffRegister result, StoreType type,
                                 uint32_t* protected_load_pc, bool i64_offset,
                                 Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, value, no_reg,
                     result, type, protected_load_pc, i64_offset, endianness,
                     compiler::Sparc64AtomicOp::kAnd);
}
void LiftoffAssembler::AtomicOr(Register dst_addr, Register offset_reg,
                                uintptr_t offset_imm, LiftoffRegister value,
                                LiftoffRegister result, StoreType type,
                                uint32_t* protected_load_pc, bool i64_offset,
                                Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, value, no_reg,
                     result, type, protected_load_pc, i64_offset, endianness,
                     compiler::Sparc64AtomicOp::kOr);
}
void LiftoffAssembler::AtomicXor(Register dst_addr, Register offset_reg,
                                 uintptr_t offset_imm, LiftoffRegister value,
                                 LiftoffRegister result, StoreType type,
                                 uint32_t* protected_load_pc, bool i64_offset,
                                 Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, value, no_reg,
                     result, type, protected_load_pc, i64_offset, endianness,
                     compiler::Sparc64AtomicOp::kXor);
}
void LiftoffAssembler::AtomicExchange(Register dst_addr, Register offset_reg,
                                      uintptr_t offset_imm,
                                      LiftoffRegister value,
                                      LiftoffRegister result, StoreType type,
                                      uint32_t* protected_load_pc,
                                      bool i64_offset, Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, value, no_reg,
                     result, type, protected_load_pc, i64_offset, endianness,
                     compiler::Sparc64AtomicOp::kExchange);
}
void LiftoffAssembler::AtomicExchangeTaggedPointer(
    Register dst_addr, Register offset_reg, uintptr_t offset_imm,
    LiftoffRegister value, LiftoffRegister result, uint32_t* protected_load_pc,
    LiftoffRegList pinned) {
  bailout(kUnsupportedArchitecture, "AtomicExchangeTaggedPointer");
}
void LiftoffAssembler::AtomicCompareExchange(
    Register dst_addr, Register offset_reg, uintptr_t offset_imm,
    LiftoffRegister expected, LiftoffRegister new_value, LiftoffRegister result,
    StoreType type, uint32_t* protected_load_pc, bool i64_offset,
    Endianness endianness) {
  liftoff::AtomicRmw(this, dst_addr, offset_reg, offset_imm, new_value,
                     expected.gp(), result, type, protected_load_pc, i64_offset,
                     endianness, compiler::Sparc64AtomicOp::kCompareExchange);
}
void LiftoffAssembler::AtomicCompareExchangeTaggedPointer(
    Register dst_addr, Register offset_reg, uintptr_t offset_imm,
    LiftoffRegister expected, LiftoffRegister new_value, LiftoffRegister result,
    uint32_t* protected_load_pc, LiftoffRegList pinned) {
  bailout(kUnsupportedArchitecture, "AtomicCompareExchangeTaggedPointer");
}
void LiftoffAssembler::FillI64Half(Register, int offset, RegPairHalf) {
  bailout(kUnsupportedArchitecture, "FillI64Half");
}
void LiftoffAssembler::emit_trace_instruction(uint32_t markid) {
  bailout(kUnsupportedArchitecture, "emit_trace_instruction");
}
bool LiftoffAssembler::emit_i32_popcnt(Register dst, Register src) {
  return false;
}
bool LiftoffAssembler::emit_i64_popcnt(LiftoffRegister dst,
                                       LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32_ceil(DoubleRegister dst, DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32_floor(DoubleRegister dst, DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32_trunc(DoubleRegister dst, DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32_nearest_int(DoubleRegister dst,
                                            DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64_ceil(DoubleRegister dst, DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64_floor(DoubleRegister dst, DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64_trunc(DoubleRegister dst, DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64_nearest_int(DoubleRegister dst,
                                            DoubleRegister src) {
  return false;
}
bool LiftoffAssembler::emit_select(LiftoffRegister dst, Register condition,
                                   LiftoffRegister true_value,
                                   LiftoffRegister false_value,
                                   ValueKind kind) {
  return false;
}
void LiftoffAssembler::LoadTransform(LiftoffRegister dst, Register src_addr,
                                     Register offset_reg, uintptr_t offset_imm,
                                     LoadType type,
                                     LoadTransformationKind transform,
                                     uint32_t* protected_load_pc,
                                     bool i64_offset) {
  sparc64::SimdEmitter(this).LoadTransform(
      dst.fp(), src_addr, offset_reg, offset_imm, type.size(),
      transform == LoadTransformationKind::kExtend
          ? sparc64::SimdEmitter::Transform::kExtend
      : transform == LoadTransformationKind::kSplat
          ? sparc64::SimdEmitter::Transform::kSplat
          : sparc64::SimdEmitter::Transform::kZeroExtend,
      type.mem_type().IsSigned(), protected_load_pc, i64_offset);
}
void LiftoffAssembler::LoadLane(LiftoffRegister dst, LiftoffRegister src,
                                Register addr, Register offset_reg,
                                uintptr_t offset_imm, LoadType type,
                                uint8_t lane, uint32_t* protected_load_pc,
                                bool i64_offset) {
  sparc64::SimdEmitter(this).LoadLane(dst.fp(), src.fp(), addr, offset_reg,
                                      offset_imm, type.size(), lane,
                                      protected_load_pc, i64_offset);
}
void LiftoffAssembler::StoreLane(Register dst, Register offset,
                                 uintptr_t offset_imm, LiftoffRegister src,
                                 StoreType type, uint8_t lane,
                                 uint32_t* protected_store_pc,
                                 bool i64_offset) {
  sparc64::SimdEmitter(this).StoreLane(dst, offset, offset_imm, src.fp(),
                                       type.size(), lane, protected_store_pc,
                                       i64_offset);
}
void LiftoffAssembler::emit_i8x16_shuffle(LiftoffRegister dst,
                                          LiftoffRegister lhs,
                                          LiftoffRegister rhs,
                                          const uint8_t shuffle[16],
                                          bool is_swizzle) {
  sparc64::SimdEmitter(this).emit_i8x16_shuffle(dst.fp(), lhs.fp(), rhs.fp(),
                                                shuffle, is_swizzle);
}
void LiftoffAssembler::emit_i8x16_swizzle(LiftoffRegister dst,
                                          LiftoffRegister lhs,
                                          LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_swizzle(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_relaxed_swizzle(LiftoffRegister dst,
                                                  LiftoffRegister lhs,
                                                  LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_relaxed_swizzle(dst.fp(), lhs.fp(),
                                                        rhs.fp());
}
void LiftoffAssembler::emit_i32x4_relaxed_trunc_f32x4_s(LiftoffRegister dst,
                                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_relaxed_trunc_f32x4_s(dst.fp(),
                                                              src.fp());
}
void LiftoffAssembler::emit_i32x4_relaxed_trunc_f32x4_u(LiftoffRegister dst,
                                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_relaxed_trunc_f32x4_u(dst.fp(),
                                                              src.fp());
}
void LiftoffAssembler::emit_i32x4_relaxed_trunc_f64x2_s_zero(
    LiftoffRegister dst, LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_relaxed_trunc_f64x2_s_zero(dst.fp(),
                                                                   src.fp());
}
void LiftoffAssembler::emit_i32x4_relaxed_trunc_f64x2_u_zero(
    LiftoffRegister dst, LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_relaxed_trunc_f64x2_u_zero(dst.fp(),
                                                                   src.fp());
}
void LiftoffAssembler::emit_s128_relaxed_laneselect(LiftoffRegister dst,
                                                    LiftoffRegister src1,
                                                    LiftoffRegister src2,
                                                    LiftoffRegister mask,
                                                    int lane_width) {
  sparc64::SimdEmitter(this).emit_s128_relaxed_laneselect(
      dst.fp(), src1.fp(), src2.fp(), mask.fp(), lane_width);
}
void LiftoffAssembler::emit_i8x16_popcnt(LiftoffRegister dst,
                                         LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i8x16_popcnt(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i8x16_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i8x16_splat(dst.fp(), src.gp());
}
void LiftoffAssembler::emit_i16x8_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_splat(dst.fp(), src.gp());
}
void LiftoffAssembler::emit_i32x4_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_splat(dst.fp(), src.gp());
}
void LiftoffAssembler::emit_i64x2_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_splat(dst.fp(), src.gp());
}
bool LiftoffAssembler::emit_f16x8_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
void LiftoffAssembler::emit_f32x4_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_splat(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f64x2_splat(LiftoffRegister dst,
                                        LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_splat(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i8x16_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_eq(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_ne(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_gt_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_gt_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_gt_u(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_gt_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_ge_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_ge_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_ge_u(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_ge_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_eq(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_ne(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_gt_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_gt_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_gt_u(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_gt_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_ge_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_ge_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_ge_u(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_ge_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_eq(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_ne(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_gt_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_gt_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_gt_u(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_gt_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_ge_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_ge_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_ge_u(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_ge_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_eq(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_ne(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_gt_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_gt_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_ge_s(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_ge_s(dst.fp(), lhs.fp(), rhs.fp());
}
bool LiftoffAssembler::emit_f16x8_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_lt(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_le(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  return false;
}
void LiftoffAssembler::emit_f32x4_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_eq(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_ne(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_lt(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_lt(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_le(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_le(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_eq(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_eq(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_ne(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_ne(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_lt(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_lt(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_le(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_le(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_s128_const(LiftoffRegister dst,
                                       const uint8_t imms[16]) {
  sparc64::SimdEmitter(this).emit_s128_const(dst.fp(), imms);
}
void LiftoffAssembler::emit_s128_not(LiftoffRegister dst, LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_s128_not(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_s128_and(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_s128_and(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_s128_or(LiftoffRegister dst, LiftoffRegister lhs,
                                    LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_s128_or(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_s128_xor(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_s128_xor(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_s128_select(LiftoffRegister dst,
                                        LiftoffRegister src1,
                                        LiftoffRegister src2,
                                        LiftoffRegister mask) {
  sparc64::SimdEmitter(this).emit_s128_select(dst.fp(), src1.fp(), src2.fp(),
                                              mask.fp());
}
void LiftoffAssembler::emit_i8x16_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i8x16_neg(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_v128_anytrue(LiftoffRegister dst,
                                         LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_v128_anytrue(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i8x16_alltrue(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i8x16_alltrue(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i8x16_bitmask(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i8x16_bitmask(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i8x16_shl(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_shl(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i8x16_shli(LiftoffRegister dst, LiftoffRegister lhs,
                                       int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_shli(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i8x16_shr_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_shr_s(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i8x16_shri_s(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_shri_s(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i8x16_shr_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_shr_u(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i8x16_shri_u(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_shri_u(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i8x16_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_add(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_add_sat_s(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_add_sat_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_add_sat_u(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_add_sat_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_sub(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_sub_sat_s(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_sub_sat_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_sub_sat_u(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_sub_sat_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_min_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_min_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_min_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_min_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_max_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_max_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_max_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_max_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_neg(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_alltrue(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_alltrue(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_bitmask(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_bitmask(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_shl(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_shl(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i16x8_shli(LiftoffRegister dst, LiftoffRegister lhs,
                                       int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_shli(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i16x8_shr_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_shr_s(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i16x8_shri_s(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_shri_s(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i16x8_shr_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_shr_u(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i16x8_shri_u(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_shri_u(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i16x8_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_add(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_add_sat_s(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_add_sat_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_add_sat_u(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_add_sat_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_sub(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_sub_sat_s(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_sub_sat_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_sub_sat_u(LiftoffRegister dst,
                                            LiftoffRegister lhs,
                                            LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_sub_sat_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_mul(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_min_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_min_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_min_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_min_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_max_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_max_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_max_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_max_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i16x8_extadd_pairwise_i8x16_s(LiftoffRegister dst,
                                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_extadd_pairwise_i8x16_s(dst.fp(),
                                                                src.fp());
}
void LiftoffAssembler::emit_i16x8_extadd_pairwise_i8x16_u(LiftoffRegister dst,
                                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_extadd_pairwise_i8x16_u(dst.fp(),
                                                                src.fp());
}
void LiftoffAssembler::emit_i16x8_extmul_low_i8x16_s(LiftoffRegister dst,
                                                     LiftoffRegister src1,
                                                     LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_extmul_low_i8x16_s(dst.fp(), src1.fp(),
                                                           src2.fp());
}
void LiftoffAssembler::emit_i16x8_extmul_low_i8x16_u(LiftoffRegister dst,
                                                     LiftoffRegister src1,
                                                     LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_extmul_low_i8x16_u(dst.fp(), src1.fp(),
                                                           src2.fp());
}
void LiftoffAssembler::emit_i16x8_extmul_high_i8x16_s(LiftoffRegister dst,
                                                      LiftoffRegister src1,
                                                      LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_extmul_high_i8x16_s(dst.fp(), src1.fp(),
                                                            src2.fp());
}
void LiftoffAssembler::emit_i16x8_extmul_high_i8x16_u(LiftoffRegister dst,
                                                      LiftoffRegister src1,
                                                      LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_extmul_high_i8x16_u(dst.fp(), src1.fp(),
                                                            src2.fp());
}
void LiftoffAssembler::emit_i16x8_q15mulr_sat_s(LiftoffRegister dst,
                                                LiftoffRegister src1,
                                                LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_q15mulr_sat_s(dst.fp(), src1.fp(),
                                                      src2.fp());
}
void LiftoffAssembler::emit_i16x8_relaxed_q15mulr_s(LiftoffRegister dst,
                                                    LiftoffRegister src1,
                                                    LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_relaxed_q15mulr_s(dst.fp(), src1.fp(),
                                                          src2.fp());
}
void LiftoffAssembler::emit_i16x8_dot_i8x16_i7x16_s(LiftoffRegister dst,
                                                    LiftoffRegister src1,
                                                    LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i16x8_dot_i8x16_i7x16_s(dst.fp(), src1.fp(),
                                                          src2.fp());
}
void LiftoffAssembler::emit_i32x4_dot_i8x16_i7x16_add_s(LiftoffRegister dst,
                                                        LiftoffRegister src1,
                                                        LiftoffRegister src2,
                                                        LiftoffRegister acc) {
  sparc64::SimdEmitter(this).emit_i32x4_dot_i8x16_i7x16_add_s(
      dst.fp(), src1.fp(), src2.fp(), acc.fp());
}
void LiftoffAssembler::emit_i32x4_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_neg(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_alltrue(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_alltrue(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_bitmask(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_bitmask(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_shl(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_shl(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i32x4_shli(LiftoffRegister dst, LiftoffRegister lhs,
                                       int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_shli(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i32x4_shr_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_shr_s(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i32x4_shri_s(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_shri_s(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i32x4_shr_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_shr_u(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i32x4_shri_u(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_shri_u(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i32x4_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_add(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_sub(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_mul(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_min_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_min_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_min_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_min_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_max_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_max_s(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_max_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_max_u(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i32x4_dot_i16x8_s(LiftoffRegister dst,
                                              LiftoffRegister lhs,
                                              LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i32x4_dot_i16x8_s(dst.fp(), lhs.fp(),
                                                    rhs.fp());
}
void LiftoffAssembler::emit_i32x4_extadd_pairwise_i16x8_s(LiftoffRegister dst,
                                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_extadd_pairwise_i16x8_s(dst.fp(),
                                                                src.fp());
}
void LiftoffAssembler::emit_i32x4_extadd_pairwise_i16x8_u(LiftoffRegister dst,
                                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_extadd_pairwise_i16x8_u(dst.fp(),
                                                                src.fp());
}
void LiftoffAssembler::emit_i32x4_extmul_low_i16x8_s(LiftoffRegister dst,
                                                     LiftoffRegister src1,
                                                     LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i32x4_extmul_low_i16x8_s(dst.fp(), src1.fp(),
                                                           src2.fp());
}
void LiftoffAssembler::emit_i32x4_extmul_low_i16x8_u(LiftoffRegister dst,
                                                     LiftoffRegister src1,
                                                     LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i32x4_extmul_low_i16x8_u(dst.fp(), src1.fp(),
                                                           src2.fp());
}
void LiftoffAssembler::emit_i32x4_extmul_high_i16x8_s(LiftoffRegister dst,
                                                      LiftoffRegister src1,
                                                      LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i32x4_extmul_high_i16x8_s(dst.fp(), src1.fp(),
                                                            src2.fp());
}
void LiftoffAssembler::emit_i32x4_extmul_high_i16x8_u(LiftoffRegister dst,
                                                      LiftoffRegister src1,
                                                      LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i32x4_extmul_high_i16x8_u(dst.fp(), src1.fp(),
                                                            src2.fp());
}
void LiftoffAssembler::emit_i64x2_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_neg(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_alltrue(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_alltrue(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_shl(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_shl(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i64x2_shli(LiftoffRegister dst, LiftoffRegister lhs,
                                       int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_shli(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i64x2_shr_s(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_shr_s(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i64x2_shri_s(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_shri_s(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i64x2_shr_u(LiftoffRegister dst,
                                        LiftoffRegister lhs,
                                        LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_shr_u(dst.fp(), lhs.fp(), rhs.gp());
}
void LiftoffAssembler::emit_i64x2_shri_u(LiftoffRegister dst,
                                         LiftoffRegister lhs, int32_t rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_shri_u(dst.fp(), lhs.fp(), rhs);
}
void LiftoffAssembler::emit_i64x2_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_add(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_sub(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i64x2_mul(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i64x2_extmul_low_i32x4_s(LiftoffRegister dst,
                                                     LiftoffRegister src1,
                                                     LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i64x2_extmul_low_i32x4_s(dst.fp(), src1.fp(),
                                                           src2.fp());
}
void LiftoffAssembler::emit_i64x2_extmul_low_i32x4_u(LiftoffRegister dst,
                                                     LiftoffRegister src1,
                                                     LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i64x2_extmul_low_i32x4_u(dst.fp(), src1.fp(),
                                                           src2.fp());
}
void LiftoffAssembler::emit_i64x2_extmul_high_i32x4_s(LiftoffRegister dst,
                                                      LiftoffRegister src1,
                                                      LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i64x2_extmul_high_i32x4_s(dst.fp(), src1.fp(),
                                                            src2.fp());
}
void LiftoffAssembler::emit_i64x2_extmul_high_i32x4_u(LiftoffRegister dst,
                                                      LiftoffRegister src1,
                                                      LiftoffRegister src2) {
  sparc64::SimdEmitter(this).emit_i64x2_extmul_high_i32x4_u(dst.fp(), src1.fp(),
                                                            src2.fp());
}
void LiftoffAssembler::emit_i64x2_bitmask(LiftoffRegister dst,
                                          LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_bitmask(dst.gp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_sconvert_i32x4_low(LiftoffRegister dst,
                                                     LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_sconvert_i32x4_low(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_sconvert_i32x4_high(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_sconvert_i32x4_high(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_uconvert_i32x4_low(LiftoffRegister dst,
                                                     LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_uconvert_i32x4_low(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_uconvert_i32x4_high(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_uconvert_i32x4_high(dst.fp(), src.fp());
}
bool LiftoffAssembler::emit_f16x8_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_sqrt(LiftoffRegister dst,
                                       LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_ceil(LiftoffRegister dst,
                                       LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_floor(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_trunc(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_nearest_int(LiftoffRegister dst,
                                              LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_div(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_min(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_max(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_pmin(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_pmax(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  return false;
}
void LiftoffAssembler::emit_f32x4_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_abs(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f32x4_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_neg(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f32x4_sqrt(LiftoffRegister dst,
                                       LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_sqrt(dst.fp(), src.fp());
}
bool LiftoffAssembler::emit_f32x4_ceil(LiftoffRegister dst,
                                       LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32x4_floor(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32x4_trunc(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32x4_nearest_int(LiftoffRegister dst,
                                              LiftoffRegister src) {
  return false;
}
void LiftoffAssembler::emit_f32x4_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_add(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_sub(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_mul(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_div(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_div(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_min(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_min(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_max(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_max(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_pmin(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_pmin(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_pmax(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_pmax(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f32x4_relaxed_min(LiftoffRegister dst,
                                              LiftoffRegister lhs,
                                              LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_relaxed_min(dst.fp(), lhs.fp(),
                                                    rhs.fp());
}
void LiftoffAssembler::emit_f32x4_relaxed_max(LiftoffRegister dst,
                                              LiftoffRegister lhs,
                                              LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f32x4_relaxed_max(dst.fp(), lhs.fp(),
                                                    rhs.fp());
}
void LiftoffAssembler::emit_f64x2_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_abs(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f64x2_neg(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_neg(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f64x2_sqrt(LiftoffRegister dst,
                                       LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_sqrt(dst.fp(), src.fp());
}
bool LiftoffAssembler::emit_f64x2_ceil(LiftoffRegister dst,
                                       LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64x2_floor(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64x2_trunc(LiftoffRegister dst,
                                        LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f64x2_nearest_int(LiftoffRegister dst,
                                              LiftoffRegister src) {
  return false;
}
void LiftoffAssembler::emit_f64x2_add(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_add(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_sub(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_sub(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_mul(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_mul(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_div(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_div(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_min(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_min(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_max(LiftoffRegister dst, LiftoffRegister lhs,
                                      LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_max(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_pmin(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_pmin(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_pmax(LiftoffRegister dst, LiftoffRegister lhs,
                                       LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_pmax(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_f64x2_relaxed_min(LiftoffRegister dst,
                                              LiftoffRegister lhs,
                                              LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_relaxed_min(dst.fp(), lhs.fp(),
                                                    rhs.fp());
}
void LiftoffAssembler::emit_f64x2_relaxed_max(LiftoffRegister dst,
                                              LiftoffRegister lhs,
                                              LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_f64x2_relaxed_max(dst.fp(), lhs.fp(),
                                                    rhs.fp());
}
void LiftoffAssembler::emit_f64x2_convert_low_i32x4_s(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_convert_low_i32x4_s(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f64x2_convert_low_i32x4_u(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_convert_low_i32x4_u(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_trunc_sat_f64x2_s_zero(LiftoffRegister dst,
                                                         LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_trunc_sat_f64x2_s_zero(dst.fp(),
                                                               src.fp());
}
void LiftoffAssembler::emit_i32x4_trunc_sat_f64x2_u_zero(LiftoffRegister dst,
                                                         LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_trunc_sat_f64x2_u_zero(dst.fp(),
                                                               src.fp());
}
void LiftoffAssembler::emit_f32x4_demote_f64x2_zero(LiftoffRegister dst,
                                                    LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_demote_f64x2_zero(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f64x2_promote_low_f32x4(LiftoffRegister dst,
                                                    LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f64x2_promote_low_f32x4(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_sconvert_f32x4(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_sconvert_f32x4(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_uconvert_f32x4(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_uconvert_f32x4(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f32x4_sconvert_i32x4(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_sconvert_i32x4(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_f32x4_uconvert_i32x4(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_f32x4_uconvert_i32x4(dst.fp(), src.fp());
}
bool LiftoffAssembler::emit_f16x8_demote_f32x4_zero(LiftoffRegister dst,
                                                    LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_demote_f64x2_zero(LiftoffRegister dst,
                                                    LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f32x4_promote_low_f16x8(LiftoffRegister dst,
                                                    LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_i16x8_sconvert_f16x8(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_i16x8_uconvert_f16x8(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_sconvert_i16x8(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_uconvert_i16x8(LiftoffRegister dst,
                                                 LiftoffRegister src) {
  return false;
}
void LiftoffAssembler::emit_i8x16_sconvert_i16x8(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_sconvert_i16x8(dst.fp(), lhs.fp(),
                                                       rhs.fp());
}
void LiftoffAssembler::emit_i8x16_uconvert_i16x8(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_uconvert_i16x8(dst.fp(), lhs.fp(),
                                                       rhs.fp());
}
void LiftoffAssembler::emit_i16x8_sconvert_i32x4(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_sconvert_i32x4(dst.fp(), lhs.fp(),
                                                       rhs.fp());
}
void LiftoffAssembler::emit_i16x8_uconvert_i32x4(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_uconvert_i32x4(dst.fp(), lhs.fp(),
                                                       rhs.fp());
}
void LiftoffAssembler::emit_i16x8_sconvert_i8x16_low(LiftoffRegister dst,
                                                     LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_sconvert_i8x16_low(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_sconvert_i8x16_high(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_sconvert_i8x16_high(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_uconvert_i8x16_low(LiftoffRegister dst,
                                                     LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_uconvert_i8x16_low(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_uconvert_i8x16_high(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_uconvert_i8x16_high(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_sconvert_i16x8_low(LiftoffRegister dst,
                                                     LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_sconvert_i16x8_low(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_sconvert_i16x8_high(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_sconvert_i16x8_high(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_uconvert_i16x8_low(LiftoffRegister dst,
                                                     LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_uconvert_i16x8_low(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_uconvert_i16x8_high(LiftoffRegister dst,
                                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_uconvert_i16x8_high(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_s128_and_not(LiftoffRegister dst,
                                         LiftoffRegister lhs,
                                         LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_s128_and_not(dst.fp(), lhs.fp(), rhs.fp());
}
void LiftoffAssembler::emit_i8x16_rounding_average_u(LiftoffRegister dst,
                                                     LiftoffRegister lhs,
                                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i8x16_rounding_average_u(dst.fp(), lhs.fp(),
                                                           rhs.fp());
}
void LiftoffAssembler::emit_i16x8_rounding_average_u(LiftoffRegister dst,
                                                     LiftoffRegister lhs,
                                                     LiftoffRegister rhs) {
  sparc64::SimdEmitter(this).emit_i16x8_rounding_average_u(dst.fp(), lhs.fp(),
                                                           rhs.fp());
}
void LiftoffAssembler::emit_i8x16_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i8x16_abs(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i16x8_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i16x8_abs(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i32x4_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i32x4_abs(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i64x2_abs(LiftoffRegister dst,
                                      LiftoffRegister src) {
  sparc64::SimdEmitter(this).emit_i64x2_abs(dst.fp(), src.fp());
}
void LiftoffAssembler::emit_i8x16_extract_lane_s(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i8x16_extract_lane_s(dst.gp(), lhs.fp(),
                                                       imm_lane_idx);
}
void LiftoffAssembler::emit_i8x16_extract_lane_u(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i8x16_extract_lane_u(dst.gp(), lhs.fp(),
                                                       imm_lane_idx);
}
void LiftoffAssembler::emit_i16x8_extract_lane_s(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i16x8_extract_lane_s(dst.gp(), lhs.fp(),
                                                       imm_lane_idx);
}
void LiftoffAssembler::emit_i16x8_extract_lane_u(LiftoffRegister dst,
                                                 LiftoffRegister lhs,
                                                 uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i16x8_extract_lane_u(dst.gp(), lhs.fp(),
                                                       imm_lane_idx);
}
void LiftoffAssembler::emit_i32x4_extract_lane(LiftoffRegister dst,
                                               LiftoffRegister lhs,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i32x4_extract_lane(dst.gp(), lhs.fp(),
                                                     imm_lane_idx);
}
void LiftoffAssembler::emit_i64x2_extract_lane(LiftoffRegister dst,
                                               LiftoffRegister lhs,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i64x2_extract_lane(dst.gp(), lhs.fp(),
                                                     imm_lane_idx);
}
bool LiftoffAssembler::emit_f16x8_extract_lane(LiftoffRegister dst,
                                               LiftoffRegister lhs,
                                               uint8_t imm_lane_idx) {
  return false;
}
void LiftoffAssembler::emit_f32x4_extract_lane(LiftoffRegister dst,
                                               LiftoffRegister lhs,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_f32x4_extract_lane(dst.fp(), lhs.fp(),
                                                     imm_lane_idx);
}
void LiftoffAssembler::emit_f64x2_extract_lane(LiftoffRegister dst,
                                               LiftoffRegister lhs,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_f64x2_extract_lane(dst.fp(), lhs.fp(),
                                                     imm_lane_idx);
}
void LiftoffAssembler::emit_i8x16_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i8x16_replace_lane(dst.fp(), src1.fp(),
                                                     src2.gp(), imm_lane_idx);
}
void LiftoffAssembler::emit_i16x8_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i16x8_replace_lane(dst.fp(), src1.fp(),
                                                     src2.gp(), imm_lane_idx);
}
void LiftoffAssembler::emit_i32x4_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i32x4_replace_lane(dst.fp(), src1.fp(),
                                                     src2.gp(), imm_lane_idx);
}
void LiftoffAssembler::emit_i64x2_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_i64x2_replace_lane(dst.fp(), src1.fp(),
                                                     src2.gp(), imm_lane_idx);
}
bool LiftoffAssembler::emit_f16x8_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  return false;
}
void LiftoffAssembler::emit_f32x4_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_f32x4_replace_lane(dst.fp(), src1.fp(),
                                                     src2.fp(), imm_lane_idx);
}
void LiftoffAssembler::emit_f64x2_replace_lane(LiftoffRegister dst,
                                               LiftoffRegister src1,
                                               LiftoffRegister src2,
                                               uint8_t imm_lane_idx) {
  sparc64::SimdEmitter(this).emit_f64x2_replace_lane(dst.fp(), src1.fp(),
                                                     src2.fp(), imm_lane_idx);
}
bool LiftoffAssembler::emit_f16x8_qfma(LiftoffRegister dst,
                                       LiftoffRegister src1,
                                       LiftoffRegister src2,
                                       LiftoffRegister src3) {
  return false;
}
bool LiftoffAssembler::emit_f16x8_qfms(LiftoffRegister dst,
                                       LiftoffRegister src1,
                                       LiftoffRegister src2,
                                       LiftoffRegister src3) {
  return false;
}
void LiftoffAssembler::emit_f32x4_qfma(LiftoffRegister dst,
                                       LiftoffRegister src1,
                                       LiftoffRegister src2,
                                       LiftoffRegister src3) {
  sparc64::SimdEmitter(this).emit_f32x4_qfma(dst.fp(), src1.fp(), src2.fp(),
                                             src3.fp());
}
void LiftoffAssembler::emit_f32x4_qfms(LiftoffRegister dst,
                                       LiftoffRegister src1,
                                       LiftoffRegister src2,
                                       LiftoffRegister src3) {
  sparc64::SimdEmitter(this).emit_f32x4_qfms(dst.fp(), src1.fp(), src2.fp(),
                                             src3.fp());
}
void LiftoffAssembler::emit_f64x2_qfma(LiftoffRegister dst,
                                       LiftoffRegister src1,
                                       LiftoffRegister src2,
                                       LiftoffRegister src3) {
  sparc64::SimdEmitter(this).emit_f64x2_qfma(dst.fp(), src1.fp(), src2.fp(),
                                             src3.fp());
}
void LiftoffAssembler::emit_f64x2_qfms(LiftoffRegister dst,
                                       LiftoffRegister src1,
                                       LiftoffRegister src2,
                                       LiftoffRegister src3) {
  sparc64::SimdEmitter(this).emit_f64x2_qfms(dst.fp(), src1.fp(), src2.fp(),
                                             src3.fp());
}
void LiftoffAssembler::set_trap_on_oob_mem64(Register index, uint64_t max_index,
                                             Label* trap_label) {
  bailout(kUnsupportedArchitecture, "set_trap_on_oob_mem64");
}
void LiftoffAssembler::emit_inc_i32_at(Address address) {
  bailout(kUnsupportedArchitecture, "emit_inc_i32_at");
}
void LiftoffAssembler::TailCallIndirect(
    compiler::CallDescriptor* call_descriptor, Register target) {
  srl(g1, target, 0);
  sllx(g1, g1, kSystemPointerSizeLog2);
  MacroAssembler::Move(g2, ExternalReference::wasm_code_pointer_table());
  add(g1, g2, g1);
  LoadU64(g1, MemOperand(g1));
  Jump(g1);
}
void LiftoffAssembler::Load(LiftoffRegister dst, Register src_addr,
                            Register offset_reg, uintptr_t offset_imm,
                            LoadType type, uint32_t* protected_load_pc,
                            bool is_load_mem, bool i64_offset,
                            bool needs_shift) {
  if (type.value_type().kind() == kS128) {
    sparc64::SimdEmitter(this).LoadVector(dst.fp(), src_addr, offset_reg,
                                          offset_imm, is_load_mem, i64_offset,
                                          needs_shift, protected_load_pc);
    return;
  }
  if (type.value() == LoadType::kF32LoadF16) {
    bailout(kUnsupportedArchitecture, "SIMD/f16 load");
    return;
  }
  liftoff::MemoryAddress(this, src_addr, offset_reg, offset_imm, i64_offset,
                         needs_shift ? type.size_log_2() : 0);
  if (protected_load_pc) *protected_load_pc = pc_offset();
  mov(g1, 0);
  for (int n = 0; n < type.size(); ++n) {
    ldub(g3, g2, n);
    int shift = (is_load_mem ? n : type.size() - 1 - n) * 8;
    if (shift) sllx(g3, g3, shift);
    or_(g1, g1, g3);
  }
  if (type.mem_type().IsSigned() && type.size() < 8) {
    sllx(g1, g1, 64 - type.size() * 8);
    srax(g1, g1, 64 - type.size() * 8);
  }
  switch (type.value_type().kind()) {
    case kI32:
      srl(dst.gp(), g1, 0);
      break;
    case kI64:
      MacroAssembler::Move(dst.gp(), g1);
      break;
    case kF32:
      MovIntToFloat(dst.fp(), g1);
      break;
    case kF64:
      MovInt64ToDouble(dst.fp(), g1);
      break;
    default:
      UNREACHABLE();
  }
}
void LiftoffAssembler::Store(Register dst_addr, Register offset_reg,
                             uintptr_t offset_imm, LiftoffRegister src,
                             StoreType type, LiftoffRegList pinned,
                             uint32_t* protected_store_pc, bool is_store_mem,
                             bool i64_offset) {
  if (type.value_type().kind() == kS128) {
    sparc64::SimdEmitter(this).StoreVector(dst_addr, offset_reg, offset_imm,
                                           src.fp(), is_store_mem, i64_offset,
                                           protected_store_pc);
    return;
  }
  if (type.value() == StoreType::kF32StoreF16) {
    bailout(kUnsupportedArchitecture, "SIMD/f16 store");
    return;
  }
  liftoff::MemoryAddress(this, dst_addr, offset_reg, offset_imm, i64_offset);
  if (protected_store_pc) *protected_store_pc = pc_offset();
  if (src.is_gp())
    MacroAssembler::Move(g1, src.gp());
  else if (type.value() == StoreType::kF32Store)
    MovFloatToInt(g1, src.fp());
  else
    MovDoubleToInt64(g1, src.fp());
  for (int n = 0; n < type.size(); ++n) {
    int shift = (is_store_mem ? n : type.size() - 1 - n) * 8;
    srlx(g3, g1, shift);
    stb(g3, g2, n);
  }
}
void LiftoffAssembler::LoadTaggedPointer(Register dst, Register src_addr,
                                         Register offset_reg,
                                         int32_t offset_imm,
                                         uint32_t* protected_load_pc,
                                         bool needs_shift) {
  liftoff::MemoryAddress(
      this, src_addr, offset_reg,
      static_cast<uintptr_t>(static_cast<intptr_t>(offset_imm)), true,
      needs_shift ? kTaggedSizeLog2 : 0);
  if (protected_load_pc) *protected_load_pc = pc_offset();
  LoadU64(dst, MemOperand(g2));
}

void LiftoffAssembler::PushRegisters(LiftoffRegList regs) {
  for (Register reg : regs.GetGpList()) MacroAssembler::Push(reg);
  for (DoubleRegister reg : regs.GetFpList()) {
    AllocateStackSpace(kSimd128Size);
    liftoff::StoreSimd(this, reg, MemOperand(sp));
  }
}
void LiftoffAssembler::PopRegisters(LiftoffRegList regs) {
  auto fp_regs = regs.GetFpList();
  for (auto it = fp_regs.rbegin(); it != fp_regs.rend(); ++it) {
    liftoff::LoadSimd(this, *it, MemOperand(sp));
    Drop(2);
  }
  auto gp_regs = regs.GetGpList();
  for (auto it = gp_regs.rbegin(); it != gp_regs.rend(); ++it)
    MacroAssembler::Pop(*it);
}
void LiftoffAssembler::RecordSpillsInSafepoint(
    SafepointTableBuilder::Safepoint& safepoint, LiftoffRegList all_spills,
    LiftoffRegList ref_spills, int spill_offset) {
  int bytes = all_spills.GetFpList().Count() * kSimd128Size;
  auto gp = all_spills.GetGpList();
  for (auto it = gp.rbegin(); it != gp.rend(); ++it) {
    if (ref_spills.has(*it)) safepoint.DefineTaggedStackSlot(spill_offset);
    ++spill_offset;
    bytes += kSystemPointerSize;
  }
  RecordOolSpillSpaceSize(bytes);
}
void LiftoffAssembler::AlignFrameSize() {
  RecordUsedSpillOffset(RoundUp(GetTotalFrameSize(), kSparcStackAlignment));
}
void LiftoffAssembler::DropStackSlotsAndRet(uint32_t slots) {
  Drop(slots);
  Ret();
}
void LiftoffAssembler::emit_u32_to_uintptr(Register dst, Register src) {
  srl(dst, src, 0);
}
void LiftoffAssembler::clear_i32_upper_half(Register dst) { srl(dst, dst, 0); }

void LiftoffAssembler::emit_i32_addi(Register dst, Register lhs, int32_t imm) {
  MacroAssembler::Move(g1, imm);
  add(dst, lhs, g1);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_muli(Register dst, Register lhs, int32_t imm) {
  MacroAssembler::Move(g1, imm);
  mulx(dst, lhs, g1);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_andi(Register dst, Register lhs, int32_t imm) {
  MacroAssembler::Move(g1, imm);
  and_(dst, lhs, g1);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_ori(Register dst, Register lhs, int32_t imm) {
  MacroAssembler::Move(g1, imm);
  or_(dst, lhs, g1);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_xori(Register dst, Register lhs, int32_t imm) {
  MacroAssembler::Move(g1, imm);
  xor_(dst, lhs, g1);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_subi(Register dst, Register lhs, int32_t imm) {
  MacroAssembler::Move(g1, imm);
  sub(dst, lhs, g1);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_shl(Register dst, Register src,
                                    Register amount) {
  sll(dst, src, amount);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_shli(Register dst, Register src,
                                     int32_t amount) {
  sll(dst, src, amount & 31);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_sar(Register dst, Register src,
                                    Register amount) {
  sra(dst, src, amount);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_sari(Register dst, Register src,
                                     int32_t amount) {
  sra(dst, src, amount & 31);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_shr(Register dst, Register src,
                                    Register amount) {
  srl(dst, src, amount);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_shri(Register dst, Register src,
                                     int32_t amount) {
  srl(dst, src, amount & 31);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i64_addi(LiftoffRegister dst, LiftoffRegister lhs,
                                     int64_t imm) {
  MacroAssembler::Move(g1, imm);
  add(dst.gp(), lhs.gp(), g1);
}

void LiftoffAssembler::emit_i64_muli(LiftoffRegister dst, LiftoffRegister lhs,
                                     int32_t imm) {
  MacroAssembler::Move(g1, imm);
  mulx(dst.gp(), lhs.gp(), g1);
}

void LiftoffAssembler::emit_i64_andi(LiftoffRegister dst, LiftoffRegister lhs,
                                     int32_t imm) {
  MacroAssembler::Move(g1, imm);
  and_(dst.gp(), lhs.gp(), g1);
}

void LiftoffAssembler::emit_i64_ori(LiftoffRegister dst, LiftoffRegister lhs,
                                    int32_t imm) {
  MacroAssembler::Move(g1, imm);
  or_(dst.gp(), lhs.gp(), g1);
}

void LiftoffAssembler::emit_i64_xori(LiftoffRegister dst, LiftoffRegister lhs,
                                     int32_t imm) {
  MacroAssembler::Move(g1, imm);
  xor_(dst.gp(), lhs.gp(), g1);
}

void LiftoffAssembler::emit_i64_shl(LiftoffRegister dst, LiftoffRegister src,
                                    Register amount) {
  sllx(dst.gp(), src.gp(), amount);
}

void LiftoffAssembler::emit_i64_shli(LiftoffRegister dst, LiftoffRegister src,
                                     int32_t amount) {
  sllx(dst.gp(), src.gp(), amount & 63);
}

void LiftoffAssembler::emit_i64_sar(LiftoffRegister dst, LiftoffRegister src,
                                    Register amount) {
  srax(dst.gp(), src.gp(), amount);
}

void LiftoffAssembler::emit_i64_sari(LiftoffRegister dst, LiftoffRegister src,
                                     int32_t amount) {
  srax(dst.gp(), src.gp(), amount & 63);
}

void LiftoffAssembler::emit_i64_shr(LiftoffRegister dst, LiftoffRegister src,
                                    Register amount) {
  srlx(dst.gp(), src.gp(), amount);
}

void LiftoffAssembler::emit_i64_shri(LiftoffRegister dst, LiftoffRegister src,
                                     int32_t amount) {
  srlx(dst.gp(), src.gp(), amount & 63);
}

void LiftoffAssembler::StoreCallerFrameSlot(LiftoffRegister src, uint32_t slot,
                                            ValueKind kind,
                                            Register frame_pointer) {
  int offset = (slot + 1) * kSystemPointerSize;
  if (kind == kI32)
    StoreU32(src.gp(), MemOperand(frame_pointer, offset + 4));
  else if (kind == kI64 || is_reference(kind))
    StoreU64(src.gp(), MemOperand(frame_pointer, offset));
  else if (kind == kF32)
    StoreF32(src.fp(), MemOperand(frame_pointer, offset));
  else if (kind == kF64)
    StoreF64(src.fp(), MemOperand(frame_pointer, offset));
  else if (kind == kS128)
    liftoff::StoreSimd(this, src.fp(), MemOperand(frame_pointer, offset));
  else
    UNREACHABLE();
}
void LiftoffAssembler::CallIndirect(const ValueKindSig*,
                                    compiler::CallDescriptor*,
                                    Register target) {
  int start = pc_offset();
  srl(g1, target, 0);
  sllx(g1, g1, kSystemPointerSizeLog2);
  MacroAssembler::Move(g2, ExternalReference::wasm_code_pointer_table());
  add(g1, g2, g1);
  LoadU64(g1, MemOperand(g1));
  liftoff::PadCall(this, start);
  Call(g1);
}
void LiftoffStackSlots::Construct(int param_slots) {
  asm_->AllocateStackSpace(param_slots * kSystemPointerSize);
  for (const auto& slot : slots_) {
    const auto& src = slot.src_;
    int offset = slot.dst_slot_ * kSystemPointerSize;
    if (src.is_reg()) {
      if (src.reg().is_gp())
        asm_->StoreU64(src.reg().gp(), MemOperand(sp, offset));
      else if (src.kind() == kF32)
        asm_->StoreF32(src.reg().fp(), MemOperand(sp, offset));
      else if (src.kind() == kF64)
        asm_->StoreF64(src.reg().fp(), MemOperand(sp, offset));
      else if (src.kind() == kS128)
        liftoff::StoreSimd(asm_, src.reg().fp(), MemOperand(sp, offset));
      else
        UNREACHABLE();
    } else if (src.is_const()) {
      asm_->MacroAssembler::Move(g1, static_cast<int64_t>(src.i32_const()));
      asm_->StoreU64(g1, MemOperand(sp, offset));
    } else {
      int src_offset = -static_cast<int>(slot.src_offset_);
      if (src.kind() == kI32) {
        asm_->LoadU32(g1, MemOperand(fp, src_offset + 4));
        asm_->StoreU64(g1, MemOperand(sp, offset));
      } else if (src.kind() == kF32) {
        asm_->LoadF32(kScratchDoubleReg, MemOperand(fp, src_offset + 4));
        asm_->StoreF32(kScratchDoubleReg, MemOperand(sp, offset));
      } else if (src.kind() == kF64 || src.kind() == kI64 ||
                 is_reference(src.kind())) {
        asm_->LoadU64(g1, MemOperand(fp, src_offset));
        asm_->StoreU64(g1, MemOperand(sp, offset));
      } else if (src.kind() == kS128) {
        for (int half = 0; half < 2; ++half) {
          asm_->LoadU64(g1, MemOperand(fp, src_offset + half * 8));
          asm_->StoreU64(g1, MemOperand(sp, offset + half * 8));
        }
      } else
        UNREACHABLE();
    }
  }
}

void LiftoffAssembler::emit_i32_clz(Register dst, Register src) {
  Label loop, nonzero, done;
  srl(g1, src, 0);
  cmp(g1, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &nonzero);
  MacroAssembler::Move(dst, 32);
  Jump(&done);
  bind(&nonzero);
  MacroAssembler::Move(dst, 0);
  bind(&loop);
  srlx(g2, g1, 31);
  cmp(g2, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &done);
  add(dst, dst, 1);
  sllx(g1, g1, 1);
  srl(g1, g1, 0);
  Jump(&loop);
  bind(&done);
}
void LiftoffAssembler::emit_i32_ctz(Register dst, Register src) {
  Label loop, nonzero, done;
  srl(g1, src, 0);
  cmp(g1, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &nonzero);
  MacroAssembler::Move(dst, 32);
  Jump(&done);
  bind(&nonzero);
  MacroAssembler::Move(dst, 0);
  bind(&loop);
  and_(g2, g1, 1);
  cmp(g2, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &done);
  add(dst, dst, 1);
  srlx(g1, g1, 1);
  srl(g1, g1, 0);
  Jump(&loop);
  bind(&done);
}

void LiftoffAssembler::emit_i32_divs(Register dst, Register lhs, Register rhs,
                                     Label* trap_zero, Label* trap_overflow) {
  Label done, normal;
  cmp(rhs, g0);
  liftoff::Branch(this, equal, ConditionCodes::kIcc, trap_zero);
  MacroAssembler::Move(g1, -1);
  cmp(rhs, g1);
  liftoff::Branch(this, not_equal, ConditionCodes::kIcc, &normal);
  MacroAssembler::Move(g1, int32_t{INT32_MIN});
  cmp(lhs, g1);
  liftoff::Branch(this, equal, ConditionCodes::kIcc, trap_overflow);
  bind(&normal);
  sra(g1, lhs, 0);
  sra(g2, rhs, 0);
  sdivx(g3, g1, g2);
  srl(dst, g3, 0);
  bind(&done);
}

void LiftoffAssembler::emit_i32_rems(Register dst, Register lhs, Register rhs,
                                     Label* trap_zero) {
  Label done, normal;
  cmp(rhs, g0);
  liftoff::Branch(this, equal, ConditionCodes::kIcc, trap_zero);
  MacroAssembler::Move(g1, -1);
  cmp(rhs, g1);
  liftoff::Branch(this, not_equal, ConditionCodes::kIcc, &normal);
  MacroAssembler::Move(g1, int32_t{INT32_MIN});
  cmp(lhs, g1);
  liftoff::Branch(this, not_equal, ConditionCodes::kIcc, &normal);
  MacroAssembler::Move(dst, 0);
  Jump(&done);
  bind(&normal);
  sra(g1, lhs, 0);
  sra(g2, rhs, 0);
  sdivx(g3, g1, g2);
  mulx(g3, g3, g2);
  sub(g3, g1, g3);
  srl(dst, g3, 0);
  bind(&done);
}

void LiftoffAssembler::emit_i32_divu(Register dst, Register lhs, Register rhs,
                                     Label* trap_zero) {
  Label done, normal;
  cmp(rhs, g0);
  liftoff::Branch(this, equal, ConditionCodes::kIcc, trap_zero);
  bind(&normal);
  srl(g1, lhs, 0);
  srl(g2, rhs, 0);
  udivx(g3, g1, g2);
  srl(dst, g3, 0);
  bind(&done);
}

void LiftoffAssembler::emit_i32_remu(Register dst, Register lhs, Register rhs,
                                     Label* trap_zero) {
  Label done, normal;
  cmp(rhs, g0);
  liftoff::Branch(this, equal, ConditionCodes::kIcc, trap_zero);
  bind(&normal);
  srl(g1, lhs, 0);
  srl(g2, rhs, 0);
  udivx(g3, g1, g2);
  mulx(g3, g3, g2);
  sub(g3, g1, g3);
  srl(dst, g3, 0);
  bind(&done);
}
void LiftoffAssembler::emit_i64_clz(LiftoffRegister dst, LiftoffRegister src) {
  Label loop, nonzero, done;
  MacroAssembler::Move(g1, src.gp());
  cmp(g1, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &nonzero);
  MacroAssembler::Move(dst.gp(), 64);
  Jump(&done);
  bind(&nonzero);
  MacroAssembler::Move(dst.gp(), 0);
  bind(&loop);
  srlx(g2, g1, 63);
  cmp(g2, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &done);
  add(dst.gp(), dst.gp(), 1);
  sllx(g1, g1, 1);
  Jump(&loop);
  bind(&done);
}
void LiftoffAssembler::emit_i64_ctz(LiftoffRegister dst, LiftoffRegister src) {
  Label loop, nonzero, done;
  MacroAssembler::Move(g1, src.gp());
  cmp(g1, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &nonzero);
  MacroAssembler::Move(dst.gp(), 64);
  Jump(&done);
  bind(&nonzero);
  MacroAssembler::Move(dst.gp(), 0);
  bind(&loop);
  and_(g2, g1, 1);
  cmp(g2, g0);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &done);
  add(dst.gp(), dst.gp(), 1);
  srlx(g1, g1, 1);
  Jump(&loop);
  bind(&done);
}

bool LiftoffAssembler::emit_i64_divs(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs, Label* trap_zero,
                                     Label* trap_overflow) {
  Label done, normal;
  cmp(rhs.gp(), g0);
  liftoff::Branch(this, equal, ConditionCodes::kXcc, trap_zero);
  MacroAssembler::Move(g1, -1);
  cmp(rhs.gp(), g1);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &normal);
  MacroAssembler::Move(g1, int64_t{INT64_MIN});
  cmp(lhs.gp(), g1);
  liftoff::Branch(this, equal, ConditionCodes::kXcc, trap_overflow);
  bind(&normal);
  MacroAssembler::Move(g1, lhs.gp());
  MacroAssembler::Move(g2, rhs.gp());
  sdivx(g3, g1, g2);
  mov(dst.gp(), g3);
  bind(&done);
  return true;
}

bool LiftoffAssembler::emit_i64_rems(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs, Label* trap_zero) {
  Label done, normal;
  cmp(rhs.gp(), g0);
  liftoff::Branch(this, equal, ConditionCodes::kXcc, trap_zero);
  MacroAssembler::Move(g1, -1);
  cmp(rhs.gp(), g1);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &normal);
  MacroAssembler::Move(g1, int64_t{INT64_MIN});
  cmp(lhs.gp(), g1);
  liftoff::Branch(this, not_equal, ConditionCodes::kXcc, &normal);
  MacroAssembler::Move(dst.gp(), 0);
  Jump(&done);
  bind(&normal);
  MacroAssembler::Move(g1, lhs.gp());
  MacroAssembler::Move(g2, rhs.gp());
  sdivx(g3, g1, g2);
  mulx(g3, g3, g2);
  sub(g3, g1, g3);
  mov(dst.gp(), g3);
  bind(&done);
  return true;
}

bool LiftoffAssembler::emit_i64_divu(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs, Label* trap_zero) {
  Label done, normal;
  cmp(rhs.gp(), g0);
  liftoff::Branch(this, equal, ConditionCodes::kXcc, trap_zero);
  bind(&normal);
  MacroAssembler::Move(g1, lhs.gp());
  MacroAssembler::Move(g2, rhs.gp());
  udivx(g3, g1, g2);
  mov(dst.gp(), g3);
  bind(&done);
  return true;
}

bool LiftoffAssembler::emit_i64_remu(LiftoffRegister dst, LiftoffRegister lhs,
                                     LiftoffRegister rhs, Label* trap_zero) {
  Label done, normal;
  cmp(rhs.gp(), g0);
  liftoff::Branch(this, equal, ConditionCodes::kXcc, trap_zero);
  bind(&normal);
  MacroAssembler::Move(g1, lhs.gp());
  MacroAssembler::Move(g2, rhs.gp());
  udivx(g3, g1, g2);
  mulx(g3, g3, g2);
  sub(g3, g1, g3);
  mov(dst.gp(), g3);
  bind(&done);
  return true;
}

bool LiftoffAssembler::emit_type_conversion(WasmOpcode opcode,
                                            LiftoffRegister dst,
                                            LiftoffRegister src, Label* trap) {
  switch (opcode) {
    case kExprI32ConvertI64:
      srl(dst.gp(), src.gp(), 0);
      return true;
    case kExprI64SConvertI32:
      sra(dst.gp(), src.gp(), 0);
      return true;
    case kExprI64UConvertI32:
      srl(dst.gp(), src.gp(), 0);
      return true;
    case kExprI32ReinterpretF32:
      MovFloatToInt(dst.gp(), src.fp());
      return true;
    case kExprI64ReinterpretF64:
      MovDoubleToInt64(dst.gp(), src.fp());
      return true;
    case kExprF32ReinterpretI32:
      MovIntToFloat(dst.fp(), src.gp());
      return true;
    case kExprF64ReinterpretI64:
      MovInt64ToDouble(dst.fp(), src.gp());
      return true;
    case kExprF32ConvertF64:
      fdtos(dst.fp(), src.fp());
      liftoff::CanonicalizeNaN(this, dst.fp(), true);
      return true;
    case kExprF64ConvertF32:
      fstod(dst.fp(), src.fp());
      liftoff::CanonicalizeNaN(this, dst.fp(), false);
      return true;
    case kExprF32SConvertI32:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), true, false,
                                          true);
      return true;
    case kExprF32UConvertI32:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), true, true,
                                          true);
      return true;
    case kExprF32SConvertI64:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), false,
                                          false, true);
      return true;
    case kExprF32UConvertI64:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), false, true,
                                          true);
      return true;
    case kExprF64SConvertI32:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), true, false,
                                          false);
      return true;
    case kExprF64UConvertI32:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), true, true,
                                          false);
      return true;
    case kExprF64SConvertI64:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), false,
                                          false, false);
      return true;
    case kExprF64UConvertI64:
      compiler::EmitSparc64IntegerToFloat(this, dst.fp(), src.gp(), false, true,
                                          false);
      return true;
    case kExprI32SConvertF32:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), true,
                                          false, true);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI32UConvertF32:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), true,
                                          true, true);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI32SConvertF64:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), true,
                                          false, false);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI32UConvertF64:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), true,
                                          true, false);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI64SConvertF32:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), false,
                                          false, true);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI64UConvertF32:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), false,
                                          true, true);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI64SConvertF64:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), false,
                                          false, false);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI64UConvertF64:
      compiler::EmitSparc64FloatToInteger(this, dst.gp(), g4, src.fp(), false,
                                          true, false);
      cmp(g4, g0);
      liftoff::Branch(this, equal, ConditionCodes::kIcc, trap);
      return true;
    case kExprI32SConvertSatF32:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), true, false,
                                        true);
      return true;
    case kExprI32UConvertSatF32:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), true, true,
                                        true);
      return true;
    case kExprI32SConvertSatF64:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), true, false,
                                        false);
      return true;
    case kExprI32UConvertSatF64:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), true, true,
                                        false);
      return true;
    case kExprI64SConvertSatF32:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), false, false,
                                        true);
      return true;
    case kExprI64UConvertSatF32:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), false, true,
                                        true);
      return true;
    case kExprI64SConvertSatF64:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), false, false,
                                        false);
      return true;
    case kExprI64UConvertSatF64:
      liftoff::SaturatingFloatToInteger(this, dst.gp(), src.fp(), false, true,
                                        false);
      return true;
    default:
      return false;
  }
}

void LiftoffAssembler::emit_i32_signextend_i8(Register dst, Register src) {
  sllx(dst, src, 56);
  srax(dst, dst, 56);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i32_signextend_i16(Register dst, Register src) {
  sllx(dst, src, 48);
  srax(dst, dst, 48);
  srl(dst, dst, 0);
}

void LiftoffAssembler::emit_i64_signextend_i8(LiftoffRegister dst,
                                              LiftoffRegister src) {
  sllx(dst.gp(), src.gp(), 56);
  srax(dst.gp(), dst.gp(), 56);
}

void LiftoffAssembler::emit_i64_signextend_i16(LiftoffRegister dst,
                                               LiftoffRegister src) {
  sllx(dst.gp(), src.gp(), 48);
  srax(dst.gp(), dst.gp(), 48);
}

void LiftoffAssembler::emit_i64_signextend_i32(LiftoffRegister dst,
                                               LiftoffRegister src) {
  sllx(dst.gp(), src.gp(), 32);
  srax(dst.gp(), dst.gp(), 32);
}

void LiftoffAssembler::CallCWithStackBuffer(
    const std::initializer_list<VarState> args, const LiftoffRegister* rets,
    ValueKind return_kind, ValueKind out_argument_kind, int stack_bytes,
    ExternalReference ext_ref) {
  int size = RoundUp(stack_bytes, kSystemPointerSize);
  AllocateStackSpace(size);
  int offset = 0;
  for (const VarState& arg : args) {
    liftoff::StoreNativeValue(this, MemOperand(sp, offset), arg);
    offset += value_kind_size(arg.kind());
  }
  LoadStackAddress(o0);
  PrepareCallCFunction(1, g1);
  CallCFunction(ext_ref, 1);
  if (return_kind != kVoid) {
    DCHECK(rets->is_gp());
    MacroAssembler::Move(rets->gp(), kReturnRegister0);
    ++rets;
  }
  if (out_argument_kind != kVoid) {
    switch (out_argument_kind) {
      case kI16:
        LoadS16(rets->gp(), MemOperand(sp));
        break;
      case kI32:
        LoadU32(rets->gp(), MemOperand(sp));
        break;
      case kI64:
      case kRef:
      case kRefNull:
        LoadU64(rets->gp(), MemOperand(sp));
        break;
      case kF32:
        LoadF32(rets->fp(), MemOperand(sp));
        liftoff::CanonicalizeNaN(this, rets->fp(), true);
        break;
      case kF64:
        LoadF64(rets->fp(), MemOperand(sp));
        liftoff::CanonicalizeNaN(this, rets->fp(), false);
        break;
      case kS128:
        liftoff::LoadSimd(this, rets->fp(), MemOperand(sp));
        break;
      default:
        bailout(kUnsupportedArchitecture, "C helper result");
        break;
    }
  }
  Drop(size / kSystemPointerSize);
}
void LiftoffAssembler::CallC(const std::initializer_list<VarState> args,
                             ExternalReference ext_ref) {
  const int count = static_cast<int>(args.size());
  PrepareCallCFunction(count, g1);
  ParallelMove moves(this);
  int index = 0;
  for (const VarState& arg : args) {
    if (index < kRegisterPassedArguments) {
      moves.LoadIntoRegister(LiftoffRegister(kCArgRegs[index]), arg);
    } else {
      liftoff::StoreNativeValue(
          this,
          MemOperand(sp,
                     (index - kRegisterPassedArguments) * kSystemPointerSize +
                         (arg.kind() == kI32   ? 4
                          : arg.kind() == kI16 ? 6
                                               : 0)),
          arg);
    }
    ++index;
  }
  moves.Execute();
  index = 0;
  for (const VarState& arg : args) {
    if (index >= kRegisterPassedArguments) break;
    if (arg.kind() == kI32) srl(kCArgRegs[index], kCArgRegs[index], 0);
    ++index;
  }
  CallCFunction(ext_ref, count);
}

void LiftoffAssembler::emit_f32_min(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  compiler::EmitSparc64FloatMinMax(this, dst, lhs, rhs, true, true);
}

void LiftoffAssembler::emit_f32_max(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  compiler::EmitSparc64FloatMinMax(this, dst, lhs, rhs, true, false);
}

void LiftoffAssembler::emit_f32_copysign(DoubleRegister dst, DoubleRegister lhs,
                                         DoubleRegister rhs) {
  MovFloatToInt(g1, lhs);
  MovFloatToInt(g2, rhs);
  sll(g1, g1, 1);
  srl(g1, g1, 1);
  MacroAssembler::Move(g3, static_cast<intptr_t>(uint64_t{1} << 31));
  and_(g2, g2, g3);
  or_(g1, g1, g2);
  MovIntToFloat(dst, g1);
}

void LiftoffAssembler::emit_f32_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  FloatCondition fc = kFNever;
  switch (cond) {
    case kEqual:
      fc = kFEqual;
      break;
    case kNotEqual:
      fc = kFNotEqual;
      break;
    case kLessThan:
    case kUnsignedLessThan:
      fc = kFLess;
      break;
    case kLessThanEqual:
    case kUnsignedLessThanEqual:
      fc = kFLessOrEqual;
      break;
    case kGreaterThan:
    case kUnsignedGreaterThan:
      fc = kFGreater;
      break;
    case kGreaterThanEqual:
    case kUnsignedGreaterThanEqual:
      fc = kFGreaterOrEqual;
      break;
    default:
      UNREACHABLE();
  }
  Label yes, done;
  fcmps(lhs, rhs);
  fb(fc, 0, &yes);
  nop();
  MacroAssembler::Move(dst, 0);
  Jump(&done);
  bind(&yes);
  MacroAssembler::Move(dst, 1);
  bind(&done);
}

void LiftoffAssembler::emit_f64_min(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  compiler::EmitSparc64FloatMinMax(this, dst, lhs, rhs, false, true);
}

void LiftoffAssembler::emit_f64_max(DoubleRegister dst, DoubleRegister lhs,
                                    DoubleRegister rhs) {
  compiler::EmitSparc64FloatMinMax(this, dst, lhs, rhs, false, false);
}

void LiftoffAssembler::emit_f64_copysign(DoubleRegister dst, DoubleRegister lhs,
                                         DoubleRegister rhs) {
  MovDoubleToInt64(g1, lhs);
  MovDoubleToInt64(g2, rhs);
  sllx(g1, g1, 1);
  srlx(g1, g1, 1);
  MacroAssembler::Move(g3, static_cast<intptr_t>(uint64_t{1} << 63));
  and_(g2, g2, g3);
  or_(g1, g1, g2);
  MovInt64ToDouble(dst, g1);
}

void LiftoffAssembler::emit_f64_set_cond(Condition cond, Register dst,
                                         DoubleRegister lhs,
                                         DoubleRegister rhs) {
  FloatCondition fc = kFNever;
  switch (cond) {
    case kEqual:
      fc = kFEqual;
      break;
    case kNotEqual:
      fc = kFNotEqual;
      break;
    case kLessThan:
    case kUnsignedLessThan:
      fc = kFLess;
      break;
    case kLessThanEqual:
    case kUnsignedLessThanEqual:
      fc = kFLessOrEqual;
      break;
    case kGreaterThan:
    case kUnsignedGreaterThan:
      fc = kFGreater;
      break;
    case kGreaterThanEqual:
    case kUnsignedGreaterThanEqual:
      fc = kFGreaterOrEqual;
      break;
    default:
      UNREACHABLE();
  }
  Label yes, done;
  fcmpd(lhs, rhs);
  fb(fc, 0, &yes);
  nop();
  MacroAssembler::Move(dst, 0);
  Jump(&done);
  bind(&yes);
  MacroAssembler::Move(dst, 1);
  bind(&done);
}

void LiftoffAssembler::StoreTaggedPointer(Register dst_addr,
                                          Register offset_reg,
                                          int32_t offset_imm, Register src,
                                          LiftoffRegList pinned,
                                          uint32_t* protected_store_pc,
                                          SkipWriteBarrier skip_write_barrier) {
  liftoff::MemoryAddress(
      this, dst_addr, offset_reg,
      static_cast<uintptr_t>(static_cast<intptr_t>(offset_imm)), true);
  if (protected_store_pc) *protected_store_pc = pc_offset();
  StoreU64(src, MemOperand(g2));
  if (skip_write_barrier == kSkipWriteBarrier) return;
  mov(g3, g2);
  RecordWrite(dst_addr, g3, src, SaveFPRegsMode::kSave);
}
void LiftoffAssembler::IncrementSmi(LiftoffRegister dst, int offset) {
  LoadU64(g1, MemOperand(dst.gp(), offset));
  MacroAssembler::Move(g2, Smi::FromInt(1));
  add(g1, g1, g2);
  StoreU64(g1, MemOperand(dst.gp(), offset));
}
void LiftoffAssembler::emit_smi_check(Register obj, Label* target,
                                      SmiCheckMode mode,
                                      const FreezeCacheState&) {
  andcc(g0, obj, kSmiTagMask);
  liftoff::Branch(this, mode == kJumpOnSmi ? equal : not_equal,
                  ConditionCodes::kXcc, target);
}

}  // namespace v8::internal::wasm
#endif
