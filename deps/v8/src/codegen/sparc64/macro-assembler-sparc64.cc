// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>

#include "src/base/iterator.h"
#include "src/builtins/builtins-inl.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/register-configuration.h"
#include "src/execution/frames.h"
#include "src/execution/isolate-data.h"
#include "src/heap/memory-chunk.h"
#include "src/interpreter/bytecodes.h"
#include "src/numbers/conversions-inl.h"
#include "src/objects/contexts.h"
#include "src/objects/fixed-array.h"
#include "src/objects/js-function.h"
#include "src/objects/map.h"
#include "src/objects/shared-function-info.h"
#include "src/sandbox/js-dispatch-table.h"

namespace v8::internal {

namespace {
bool IsSimm13(int64_t value) { return value >= -4096 && value <= 4095; }
}  // namespace

Register MacroAssembler::Scratch(Register first, Register second,
                                 Register third) {
  for (Register scratch : {g1, g2, g3, g4}) {
    if (scratch != first && scratch != second && scratch != third)
      return scratch;
  }
  UNREACHABLE();
}

void MacroAssembler::Move(Register dst, Register src) {
  if (dst == src) return;
  if (src == sp) {
    LoadStackAddress(dst);
  } else if (dst == sp) {
    SubS64(dst, src, kSparcStackSlotBias);
  } else {
    mov(dst, src);
  }
}

void MacroAssembler::Move(Register dst, intptr_t value) {
  if (IsSimm13(value)) {
    mov(dst, static_cast<int32_t>(value));
  } else {
    LoadLiteral(dst, static_cast<uint64_t>(value));
  }
}

void MacroAssembler::AddS64(Register dst, Register src, intptr_t value) {
  if (IsSimm13(value)) {
    add(dst, src, static_cast<int32_t>(value));
  } else {
    Register scratch = Scratch(dst, src);
    Move(scratch, value);
    add(dst, src, scratch);
  }
}

void MacroAssembler::Move(Register dst, ExternalReference reference) {
  if (root_array_available()) {
    if (reference.IsIsolateFieldId()) {
      AddS64(dst, kRootRegister, reference.offset_from_root_register());
      return;
    }
    if (options().isolate_independent_code) {
      IndirectLoadExternalReference(dst, reference);
      return;
    }
  }
  CHECK(!reference.IsIsolateFieldId());
  LoadLiteral(dst, reference.address(), RelocInfo::EXTERNAL_REFERENCE);
}

void MacroAssembler::Move(Register dst, Handle<HeapObject> value) {
  if (root_array_available() && options().isolate_independent_code) {
    IndirectLoadConstant(dst, value);
    return;
  }
  LoadLiteral(dst, value.address(), RelocInfo::FULL_EMBEDDED_OBJECT);
}

void MacroAssembler::Abort(AbortReason reason) {
  if (!v8_flags.debug_code || v8_flags.trap_on_abort) {
    Trap();
    return;
  }
  Move(AbortDescriptor::GetRegisterParameter(0),
       Smi::FromInt(static_cast<int>(reason)));
  TailCallBuiltin(Builtin::kAbort);
}

void MacroAssembler::LoadNativeContextSlot(Register dst, int index) {
  LoadMap(dst, kContextRegister);
  LoadU64(dst, FieldMemOperand(
                   dst, Map::kConstructorOrBackPointerOrNativeContextOffset));
  LoadU64(dst, FieldMemOperand(dst, Context::OffsetOfElementAt(index)));
}

void MacroAssembler::AssertFeedbackCell(Register object, Register scratch) {
  if (!v8_flags.debug_code) return;
  LoadMap(scratch, object);
  LoadU16(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
  cmp(scratch, FEEDBACK_CELL_TYPE);
  Assert(equal, AbortReason::kExpectedFeedbackCell);
}

void MacroAssembler::MoveNumber(Register dst, double value) {
  int32_t smi;
  if (DoubleToSmiInteger(value, &smi)) {
    Move(dst, Smi::FromInt(smi));
    return;
  }
  LoadLiteral(dst, 0, RelocInfo::FULL_EMBEDDED_OBJECT);
  heap_number_requests_.emplace_front(value, pc_offset() - kSystemPointerSize);
}

void MacroAssembler::SubS64(Register dst, Register src, intptr_t value) {
  if (IsSimm13(value)) {
    sub(dst, src, static_cast<int32_t>(value));
  } else {
    Register scratch = Scratch(dst, src);
    Move(scratch, value);
    sub(dst, src, scratch);
  }
}

MemOperand MacroAssembler::ResolveAddress(MemOperand operand,
                                          Register exclude) {
  if (IsSimm13(operand.offset())) return operand;
  Register scratch = Scratch(operand.base(), exclude);
  Move(scratch, operand.offset());
  add(scratch, operand.base(), scratch);
  return MemOperand(scratch);
}

#define LOAD_METHOD(name, instruction)                                \
  void MacroAssembler::name(Register dst, MemOperand src) {           \
    src = ResolveAddress(src, dst);                                   \
    instruction(dst, src.base(), static_cast<int32_t>(src.offset())); \
  }
LOAD_METHOD(LoadU64, ldx)
LOAD_METHOD(LoadU32, lduw)
LOAD_METHOD(LoadS32, ldsw)
LOAD_METHOD(LoadU8, ldub)
LOAD_METHOD(LoadS8, ldsb)
LOAD_METHOD(LoadU16, lduh)
LOAD_METHOD(LoadS16, ldsh)
#undef LOAD_METHOD

#define STORE_METHOD(name, instruction)                               \
  void MacroAssembler::name(Register src, MemOperand dst) {           \
    dst = ResolveAddress(dst, src);                                   \
    instruction(src, dst.base(), static_cast<int32_t>(dst.offset())); \
  }
STORE_METHOD(StoreU64, stx)
STORE_METHOD(StoreU32, stw)
STORE_METHOD(StoreU8, stb)
STORE_METHOD(StoreU16, sth)
#undef STORE_METHOD

#define FP_MEMORY_METHOD(name, instruction)                             \
  void MacroAssembler::name(DoubleRegister value, MemOperand mem) {     \
    mem = ResolveAddress(mem, no_reg);                                  \
    instruction(value, mem.base(), static_cast<int32_t>(mem.offset())); \
  }
FP_MEMORY_METHOD(LoadF32, ldf)
FP_MEMORY_METHOD(StoreF32, stf)
FP_MEMORY_METHOD(LoadF64, ldd)
FP_MEMORY_METHOD(StoreF64, std)
#undef FP_MEMORY_METHOD

void MacroAssembler::MovIntToFloat(FloatRegister dst, Register src) {
  AllocateStackSpace(kSystemPointerSize);
  StoreU32(src, MemOperand(sp));
  LoadF32(dst, MemOperand(sp));
  Drop(1);
}

void MacroAssembler::MovFloatToInt(Register dst, FloatRegister src) {
  AllocateStackSpace(kSystemPointerSize);
  StoreF32(src, MemOperand(sp));
  LoadU32(dst, MemOperand(sp));
  Drop(1);
}

void MacroAssembler::MovInt64ToDouble(DoubleRegister dst, Register src) {
  AllocateStackSpace(kSystemPointerSize);
  StoreU64(src, MemOperand(sp));
  LoadF64(dst, MemOperand(sp));
  Drop(1);
}

void MacroAssembler::MovDoubleToInt64(Register dst, DoubleRegister src) {
  AllocateStackSpace(kSystemPointerSize);
  StoreF64(src, MemOperand(sp));
  LoadU64(dst, MemOperand(sp));
  Drop(1);
}

void MacroAssembler::LoadAddress(Register dst, MemOperand src) {
  if (IsSimm13(src.offset())) {
    add(dst, src.base(), static_cast<int32_t>(src.offset()));
  } else if (dst != src.base()) {
    Move(dst, src.offset());
    add(dst, src.base(), dst);
  } else {
    AddS64(dst, src.base(), src.offset());
  }
}

void MacroAssembler::LoadUnaligned(Register dst, MemOperand src, int bytes,
                                   bool is_signed) {
  CHECK(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8);
  Register address = Scratch(dst, src.base());
  Register byte = Scratch(dst, address, src.base());
  LoadAddress(address, src);
  ldub(dst, address, 0);
  for (int n = 1; n < bytes; ++n) {
    sllx(dst, dst, 8);
    ldub(byte, address, n);
    or_(dst, dst, byte);
  }
  if (is_signed && bytes < kSystemPointerSize) {
    const int shift = (kSystemPointerSize - bytes) * 8;
    sllx(dst, dst, shift);
    srax(dst, dst, shift);
  }
}

void MacroAssembler::StoreUnaligned(Register src, MemOperand dst, int bytes) {
  CHECK(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8);
  Register address = Scratch(src, dst.base());
  Register byte = Scratch(src, address, dst.base());
  LoadAddress(address, dst);
  for (int n = 0; n < bytes; ++n) {
    srlx(byte, src, (bytes - n - 1) * 8);
    stb(byte, address, n);
  }
}

void MacroAssembler::Push(Register src) {
  CHECK_NE(src, sp);
  sub(sp, sp, kSystemPointerSize);
  StoreU64(src, MemOperand(sp));
}

void MacroAssembler::Pop(Register dst) {
  CHECK_NE(dst, sp);
  LoadU64(dst, MemOperand(sp));
  add(sp, sp, kSystemPointerSize);
}

void MacroAssembler::AllocateStackSpace(int bytes) {
  CHECK_GE(bytes, 0);
  CHECK_EQ(bytes % kSystemPointerSize, 0);
  SubS64(sp, sp, bytes);
}

void MacroAssembler::Drop(int slots) {
  CHECK_GE(slots, 0);
  AddS64(sp, sp, intptr_t{slots} * kSystemPointerSize);
}

void MacroAssembler::Drop(Register slots) {
  Register scratch = Scratch(slots);
  sllx(scratch, slots, kSystemPointerSizeLog2);
  add(sp, sp, scratch);
}

void MacroAssembler::PushArguments(Register array, Register argc,
                                   Register scratch,
                                   ArgumentsElementType type) {
  CHECK_NE(array, argc);
  CHECK_NE(array, scratch);
  CHECK_NE(argc, scratch);
  Register value = Scratch(array, argc, scratch);
  Label loop, done;
  SubS64(scratch, argc, kJSArgcReceiverSlots);
  cmp(scratch, 0);
  b(less_equal, ConditionCodes::kXcc, &done);
  nop();
  sllx(scratch, scratch, kSystemPointerSizeLog2);
  add(scratch, array, scratch);
  bind(&loop);
  sub(scratch, scratch, kSystemPointerSize);
  LoadU64(value, MemOperand(scratch));
  if (type == ArgumentsElementType::kHandle) LoadU64(value, MemOperand(value));
  Push(value);
  cmp(scratch, array);
  b(not_equal, ConditionCodes::kXcc, &loop);
  nop();
  bind(&done);
}

void MacroAssembler::StackOverflowCheck(Register num_args, Register scratch,
                                        Label* overflow) {
  CHECK_NE(num_args, scratch);
  Register limit = Scratch(num_args, scratch);
  Register address = Scratch(num_args, scratch, limit);
  Move(address, IsolateData::real_jslimit_offset());
  add(address, kRootRegister, address);
  ldx(limit, address, 0);
  add(scratch, sp, kSparcStackBias);
  sub(scratch, scratch, limit);
  sllx(limit, num_args, kSystemPointerSizeLog2);
  cmp(scratch, limit);
  b(less_equal, ConditionCodes::kXcc, overflow);
  nop();
}

void MacroAssembler::LoadStackAddress(Register dst, int32_t offset) {
  AddS64(dst, sp, int64_t{offset} + kSparcStackSlotBias);
}

void MacroAssembler::PushCommonFrame(Register marker) {
  CHECK_NE(marker, g1);
  add(g1, o7, kSparcReturnAddressOffset);
  Push(g1, fp);
  LoadStackAddress(fp);
  if (marker.is_valid()) Push(marker);
}

void MacroAssembler::PushStandardFrame(Register function) {
  PushCommonFrame();
  Push(kContextRegister);
  if (function.is_valid()) Push(function);
  Push(kJavaScriptCallArgCountRegister);
}

void MacroAssembler::EnterCFunctionFrame() {
  mov(g4, fp);
  mov(g5, kRootRegister);
  save(sp, sp, -kSparcMinimumFrameSize);
  mov(fp, g4);
  mov(kRootRegister, g5);
  mov(o7, i7);
  for (int n = 0; n < kRegisterPassedArguments; ++n) {
    mov(kCArgRegs[n], Register::from_code(i0.code() + n));
  }
  PushCommonFrame();
}

void MacroAssembler::RetCFunction() {
  mov(i0, o0);
  mov(i1, o1);
  ret();
  restore();
}

void MacroAssembler::RestoreFrameStateForTailCall() {
  LoadU64(g1, MemOperand(fp, CommonFrameConstants::kCallerPCOffset));
  LoadU64(fp, MemOperand(fp, CommonFrameConstants::kCallerFPOffset));
  sub(o7, g1, kSparcReturnAddressOffset);
}

MemOperand MacroAssembler::AsMemOperand(IsolateFieldId id) {
  DCHECK(root_array_available());
  return MemOperand(kRootRegister, IsolateData::GetOffset(id));
}

void MacroAssembler::EnterJSEntryFrame(StackFrame::Type type) {
  CHECK(type == StackFrame::ENTRY || type == StackFrame::CONSTRUCT_ENTRY);
  save(sp, sp, -kSparcMinimumFrameSize);
  mov(kRootRegister, i0);
  Move(g1, -1);
  Push(g1);
  Move(g1, StackFrame::TypeToMarker(type));
  Push(g1, g1);
  for (IsolateFieldId id :
       {IsolateFieldId::kCEntryFP, IsolateFieldId::kFastCCallCallerFP,
        IsolateFieldId::kFastCCallCallerPC}) {
    LoadU64(g1, AsMemOperand(id));
    Push(g1);
    StoreU64(g0, AsMemOperand(id));
  }
  LoadStackAddress(fp, -EntryFrameConstants::kNextFastCallFramePCOffset);
  Label inner, pushed;
  LoadU64(g1, AsMemOperand(IsolateFieldId::kJSEntrySP));
  cmp(g1, 0);
  b(not_equal, ConditionCodes::kXcc, &inner);
  nop();
  StoreU64(fp, AsMemOperand(IsolateFieldId::kJSEntrySP));
  Move(g1, StackFrame::OUTERMOST_JSENTRY_FRAME);
  Jump(&pushed);
  bind(&inner);
  Move(g1, StackFrame::INNER_JSENTRY_FRAME);
  bind(&pushed);
  Push(g1);
}

void MacroAssembler::LeaveJSEntryFrame() {
  Label inner;
  Pop(g1);
  cmp(g1, StackFrame::OUTERMOST_JSENTRY_FRAME);
  b(not_equal, ConditionCodes::kIcc, &inner);
  nop();
  StoreU64(g0, AsMemOperand(IsolateFieldId::kJSEntrySP));
  bind(&inner);
  for (IsolateFieldId id :
       {IsolateFieldId::kFastCCallCallerPC, IsolateFieldId::kFastCCallCallerFP,
        IsolateFieldId::kCEntryFP}) {
    Pop(g1);
    StoreU64(g1, AsMemOperand(id));
  }
  Drop(3);  // Entry markers and the deliberately invalid FP.
  RetCFunction();
}

void MacroAssembler::PushStackHandler() {
  static_assert(StackHandlerConstants::kSize == 2 * kSystemPointerSize);
  static_assert(StackHandlerConstants::kNextOffset == 0);
  Push(g0);
  LoadU64(g1, AsMemOperand(IsolateFieldId::kHandler));
  Push(g1);
  LoadStackAddress(g2);
  StoreU64(g2, AsMemOperand(IsolateFieldId::kHandler));
}

void MacroAssembler::PopStackHandler() {
  Pop(g1);
  StoreU64(g1, AsMemOperand(IsolateFieldId::kHandler));
  Drop(1);
}

void MacroAssembler::EnterExitFrame(Register scratch, int stack_space,
                                    StackFrame::Type frame_type) {
  CHECK_GE(stack_space, 0);
  CHECK(frame_type == StackFrame::EXIT ||
        frame_type == StackFrame::BUILTIN_EXIT ||
        frame_type == StackFrame::API_NAMED_ACCESSOR_EXIT ||
        frame_type == StackFrame::API_INDEXED_ACCESSOR_EXIT ||
        frame_type == StackFrame::API_CALLBACK_EXIT);
  EnterFrame(frame_type);
  AllocateStackSpace(ExitFrameConstants::kFixedFrameSizeFromFp -
                     TypedFrameConstants::kFixedFrameSizeFromFp);
  AllocateStackSpace((stack_space + 1) * kSystemPointerSize);
  StoreU64(g0, MemOperand(sp));
  LoadStackAddress(g1, kSystemPointerSize);
  StoreU64(g1, MemOperand(fp, ExitFrameConstants::kSPOffset));
  StoreU64(kContextRegister, AsMemOperand(IsolateFieldId::kContext));
  StoreU64(fp, AsMemOperand(IsolateFieldId::kCEntryFP));
}

void MacroAssembler::LeaveExitFrame(Register scratch) {
  LoadU64(kContextRegister, AsMemOperand(IsolateFieldId::kContext));
#ifdef DEBUG
  Move(scratch, Context::kNoContext);
  StoreU64(scratch, AsMemOperand(IsolateFieldId::kContext));
#endif
  StoreU64(g0, AsMemOperand(IsolateFieldId::kCEntryFP));
  LeaveFrame(StackFrame::EXIT);
}

void MacroAssembler::JumpToPendingHandler() {
  LoadU64(kContextRegister,
          AsMemOperand(IsolateFieldId::kPendingHandlerContext));
  LoadU64(g5, AsMemOperand(IsolateFieldId::kPendingHandlerSP));
  Label keep_sp, non_js;
  cmp(g5, 0);
  b(equal, ConditionCodes::kXcc, &keep_sp);
  nop();
  Move(sp, g5);
  bind(&keep_sp);  // FP-only handlers must still leave a valid window spill
                   // area.
  LoadU64(fp, AsMemOperand(IsolateFieldId::kPendingHandlerFP));
  cmp(kContextRegister, 0);
  b(equal, ConditionCodes::kXcc, &non_js);
  nop();
  StoreU64(kContextRegister,
           MemOperand(fp, StandardFrameConstants::kContextOffset));
  bind(&non_js);
  StoreU64(g0, AsMemOperand(IsolateFieldId::kCEntryFP));
  LoadU64(g1, AsMemOperand(IsolateFieldId::kPendingHandlerEntrypoint));
  Jump(g1);
}

int MacroAssembler::StoreReturnAddressAndCall(Register target,
                                              int num_arguments) {
  CHECK_GE(num_arguments, 0);
  CHECK_LE(num_arguments, kMaxCParameters);
  DCHECK(has_frame());
  mov(g5, target);
  const int pc = pc_offset();
  rdpc(g3);
  const int pc_add = pc_offset();
  add(g3, g3, 0);
  StoreU64(g3, MemOperand(sp));
  mov(g1, g5);
  mov(g5, kRootRegister);
  mov(g4, sp);
  add(g3, sp, kSparcStackBias);
  and_(g3, g3, -kSparcStackAlignment);
  sub(sp, g3, kSparcStackBias);
  const int stack_arguments =
      std::max(0, num_arguments - kRegisterPassedArguments);
  const int native_size =
      RoundUp(kSparcMinimumFrameSize + stack_arguments * kSystemPointerSize,
              kSparcStackAlignment);
  save(sp, sp, -native_size);
  mov(l4, g4);
  mov(l3, g1);
  mov(kRootRegister, g5);
  for (int n = 0; n < std::min(num_arguments, kRegisterPassedArguments); ++n) {
    mov(kCArgRegs[n], Register::from_code(i0.code() + n));
  }
  for (int n = 0; n < stack_arguments; ++n) {
    LoadU64(g2,
            MemOperand(l4, kSparcStackSlotBias + (n + 1) * kSystemPointerSize));
    StoreU64(g2, MemOperand(sp, n * kSystemPointerSize));
  }
  Call(l3);
  const int continuation = pc_offset();
  const int delta = continuation - pc;
  CHECK(IsSimm13(delta));
  instr_at_put(pc_add, (instr_at(pc_add) & ~Instr{0x1fff}) | delta);
  mov(i0, o0);
  mov(i1, o1);
  mov(g4, l4);
  restore();
  mov(sp, g4);
  return continuation;
}

void MacroAssembler::DirectCEntry() {
  static_assert(kDirectCEntryTargetRegister == g5);
  static_assert(kDirectCEntryArgumentCountRegister == g4);
  add(g1, o7, kSparcReturnAddressOffset);
  StoreU64(g1, MemOperand(sp));
  mov(g3, sp);
  sub(g1, g4, kRegisterPassedArguments);
  Label size_ready, copied, copy;
  cmp(g1, 0);
  b(greater, ConditionCodes::kXcc, &size_ready);
  nop();
  mov(g1, 0);
  bind(&size_ready);
  sllx(g1, g1, kSystemPointerSizeLog2);
  add(g1, g1, kSparcMinimumFrameSize + kSparcStackAlignment - 1);
  and_(g1, g1, -kSparcStackAlignment);
  sub(g1, g0, g1);
  add(g2, sp, kSparcStackBias);
  and_(g2, g2, -kSparcStackAlignment);
  sub(sp, g2, kSparcStackBias);
  mov(g2, kRootRegister);
  save(sp, sp, g1);
  mov(l4, g3);
  mov(l3, g5);
  mov(l2, g4);
  mov(kRootRegister, g2);
  for (int n = 0; n < kRegisterPassedArguments; ++n)
    mov(kCArgRegs[n], Register::from_code(i0.code() + n));
  sub(l1, l2, kRegisterPassedArguments);
  cmp(l1, 0);
  b(less_equal, ConditionCodes::kXcc, &copied);
  nop();
  AddS64(g2, l4, kSparcStackSlotBias + kSystemPointerSize);
  LoadStackAddress(g3);
  bind(&copy);
  LoadU64(g4, MemOperand(g2));
  StoreU64(g4, MemOperand(g3));
  add(g2, g2, kSystemPointerSize);
  add(g3, g3, kSystemPointerSize);
  sub(l1, l1, 1);
  cmp(l1, 0);
  b(not_equal, ConditionCodes::kXcc, &copy);
  nop();
  bind(&copied);
  Call(l3);
  mov(i0, o0);
  mov(i1, o1);
  mov(g4, l4);
  restore();
  mov(sp, g4);
  LoadU64(g1, MemOperand(sp));
  sub(o7, g1, kSparcReturnAddressOffset);
  Ret();
}

void MacroAssembler::EnterFrame(StackFrame::Type type,
                                bool load_constant_pool_pointer_reg) {
  if (StackFrame::IsJavaScript(type)) {
    PushCommonFrame();
  } else {
    Move(g2, StackFrame::TypeToMarker(type));
    PushCommonFrame(g2);
  }
#if V8_ENABLE_WEBASSEMBLY
  if (type == StackFrame::WASM) Push(kWasmImplicitArgRegister);
#endif
}

int MacroAssembler::LeaveFrame(StackFrame::Type type, int stack_adjustment) {
  ldx(g1, fp, CommonFrameConstants::kCallerPCOffset);
  ldx(g2, fp, CommonFrameConstants::kCallerFPOffset);
  sub(o7, g1, kSparcReturnAddressOffset);
  const int frame_ends = pc_offset();
  AddS64(sp, fp,
         int64_t{CommonFrameConstants::kCallerSPOffset} + stack_adjustment -
             kSparcStackSlotBias);
  mov(fp, g2);
  return frame_ends;
}

void MacroAssembler::Ret() {
  retl();
  nop();
}

void MacroAssembler::Call(Register target) {
  CHECK_NE(target, o7);
  jmpl(o7, target, 0);
  nop();
}

void MacroAssembler::Jump(Register target) {
  jmpl(g0, target, 0);
  nop();
}

void MacroAssembler::Jump(Label* target) {
  // CALL reaches the whole code buffer; its delay slot restores the link.
  mov(g4, o7);
  call(target);
  mov(o7, g4);
}

void MacroAssembler::Jump(const ExternalReference& reference) {
  Move(g1, reference);
  Jump(g1);
}

void MacroAssembler::JumpIfEqual(Register value, int32_t constant,
                                 Label* target) {
  if (IsSimm13(constant)) {
    cmp(value, constant);
  } else {
    Register scratch = Scratch(value);
    Move(scratch, constant);
    cmp(value, scratch);
  }
  Label skip;
  b(not_equal, ConditionCodes::kIcc, &skip);
  nop();
  Jump(target);
  bind(&skip);
}

void MacroAssembler::JumpIfLessThan(Register value, int32_t constant,
                                    Label* target) {
  if (IsSimm13(constant)) {
    cmp(value, constant);
  } else {
    Register scratch = Scratch(value);
    Move(scratch, constant);
    cmp(value, scratch);
  }
  Label skip;
  b(greater_equal, ConditionCodes::kIcc, &skip);
  nop();
  Jump(target);
  bind(&skip);
}

void MacroAssembler::InitializeRootRegister() {
  LoadLiteral(kRootRegister,
              ExternalReference::isolate_root(isolate()).address(),
              RelocInfo::EXTERNAL_REFERENCE);
}

void MacroAssembler::CallBuiltin(Builtin target) {
  LoadRootRelative(g1, IsolateData::BuiltinEntrySlotOffset(target));
  Call(g1);
}

void MacroAssembler::TailCallBuiltin(Builtin target) {
  LoadRootRelative(g1, IsolateData::BuiltinEntrySlotOffset(target));
  Jump(g1);
}

void MacroAssembler::CallRuntime(const Runtime::Function* function,
                                 int num_arguments) {
  CHECK(function->nargs < 0 || function->nargs == num_arguments);
  Move(kRuntimeCallArgCountRegister, num_arguments);
  Move(kRuntimeCallFunctionRegister, ExternalReference::Create(function));
  CallBuiltin(
      Builtins::RuntimeCEntry(function->result_size, options().is_wasm));
}

void MacroAssembler::CallRuntime(Runtime::FunctionId id) {
  const Runtime::Function* function = Runtime::FunctionForId(id);
  CallRuntime(function, function->nargs);
}

void MacroAssembler::CallRuntime(Runtime::FunctionId id, int num_arguments) {
  CallRuntime(Runtime::FunctionForId(id), num_arguments);
}

void MacroAssembler::TailCallRuntime(Runtime::FunctionId id) {
  const Runtime::Function* function = Runtime::FunctionForId(id);
  DCHECK_EQ(function->result_size, 1);
  if (function->nargs >= 0) Move(kRuntimeCallArgCountRegister, function->nargs);
  Move(kRuntimeCallFunctionRegister, ExternalReference::Create(function));
  TailCallBuiltin(Builtins::RuntimeCEntry(1, options().is_wasm));
}

void MacroAssembler::AllocateSpaceAndShiftArguments(Register count,
                                                    Register argc,
                                                    Register first_free) {
  CHECK_EQ(count, o4);
  CHECK_EQ(argc, o0);
  CHECK_EQ(first_free, o5);
  LoadStackAddress(g2);
  sllx(g1, count, kSystemPointerSizeLog2);
  sub(sp, sp, g1);
  LoadStackAddress(first_free);
  mov(g3, argc);
  Label copy, done;
  cmp(g3, 0);
  b(less_equal, ConditionCodes::kXcc, &done);
  nop();
  bind(&copy);
  LoadU64(g4, MemOperand(g2));
  StoreU64(g4, MemOperand(first_free));
  add(g2, g2, kSystemPointerSize);
  add(first_free, first_free, kSystemPointerSize);
  sub(g3, g3, 1);
  cmp(g3, 0);
  b(not_equal, ConditionCodes::kXcc, &copy);
  nop();
  bind(&done);
  add(argc, argc, count);
}

void MacroAssembler::AdaptArguments(Register expected, Register actual,
                                    Label* overflow) {
  CHECK_EQ(expected, o2);
  CHECK_EQ(actual, o0);
  Label done, copied, copy, fill;
  sub(expected, expected, actual);
  cmp(expected, 0);
  b(less_equal, ConditionCodes::kXcc, &done);
  nop();
  StackOverflowCheck(expected, g3, overflow);
  LoadStackAddress(o4);
  sllx(g1, expected, kSystemPointerSizeLog2);
  sub(sp, sp, g1);
  LoadStackAddress(o5);
  mov(g3, actual);
  cmp(g3, 0);
  b(less_equal, ConditionCodes::kXcc, &copied);
  nop();
  bind(&copy);
  LoadU64(g4, MemOperand(o4));
  StoreU64(g4, MemOperand(o5));
  add(o4, o4, kSystemPointerSize);
  add(o5, o5, kSystemPointerSize);
  sub(g3, g3, 1);
  cmp(g3, 0);
  b(not_equal, ConditionCodes::kXcc, &copy);
  nop();
  bind(&copied);
  LoadRoot(g4, RootIndex::kUndefinedValue);
  bind(&fill);
  StoreU64(g4, MemOperand(o5));
  add(o5, o5, kSystemPointerSize);
  sub(expected, expected, 1);
  cmp(expected, 0);
  b(not_equal, ConditionCodes::kXcc, &fill);
  nop();
  bind(&done);
}

void MacroAssembler::JumpJSFunction(Register function) {
  CHECK_EQ(function, kJavaScriptCallTargetRegister);
  static_assert(!V8_ENABLE_SANDBOX_BOOL);
  LoadU64(g2, AsMemOperand(IsolateFieldId::kJSDispatchTable));
  LoadU32(g1, FieldMemOperand(function, JSFunction::kDispatchHandleOffset));
  srlx(o2, g1, kJSDispatchHandleShift);
  sllx(o2, o2, kJSDispatchTableEntrySizeLog2);
  add(g2, g2, o2);
  LoadU64(o2, MemOperand(g2, JSDispatchEntry::kEntrypointOffset));
  Jump(o2);
}

void MacroAssembler::LoadInterpreterDataBytecodeArray(Register dst,
                                                      Register data) {
  LoadU64(dst,
          FieldMemOperand(data, offsetof(InterpreterData, bytecode_array_)));
}

void MacroAssembler::LoadInterpreterDataInterpreterTrampoline(Register dst,
                                                              Register data) {
  LoadU64(dst, FieldMemOperand(
                   data, offsetof(InterpreterData, interpreter_trampoline_)));
}

void MacroAssembler::DispatchInterpreterBytecode() {
  LoadU64(kInterpreterBytecodeArrayRegister,
          MemOperand(fp, InterpreterFrameConstants::kBytecodeArrayFromFp));
  LoadU64(kInterpreterBytecodeOffsetRegister,
          MemOperand(fp, InterpreterFrameConstants::kBytecodeOffsetFromFp));
  srax(kInterpreterBytecodeOffsetRegister, kInterpreterBytecodeOffsetRegister,
       kSmiShiftSize + kSmiTagSize);
  add(g1, kInterpreterBytecodeArrayRegister,
      kInterpreterBytecodeOffsetRegister);
  LoadU8(g1, MemOperand(g1));
  sllx(g1, g1, kSystemPointerSizeLog2);
  add(g1, kInterpreterDispatchTableRegister, g1);
  LoadU64(kJavaScriptCallCodeStartRegister, MemOperand(g1));
  Jump(kJavaScriptCallCodeStartRegister);
}

void MacroAssembler::TruncateDoubleBitsToInt32(Register result, Register bits) {
  CHECK_EQ(result, g2);
  CHECK_EQ(bits, g1);
  Label zero, right, sign, positive, done;
  srlx(g2, g1, 52);
  and_(g2, g2, 0x7ff);
  sub(g2, g2, 1023);
  cmp(g2, 0);
  b(less, ConditionCodes::kXcc, &zero);
  nop();
  // At 2^84 every representable finite value is a multiple of 2^32.
  cmp(g2, 84);
  b(greater_equal, ConditionCodes::kXcc, &zero);
  nop();
  sllx(g4, g1, 12);
  srlx(g4, g4, 12);
  mov(g3, 1);
  sllx(g3, g3, 52);
  or_(g4, g4, g3);
  cmp(g2, 52);
  b(less, ConditionCodes::kXcc, &right);
  nop();
  sub(g3, g2, 52);
  sllx(g2, g4, g3);
  Jump(&sign);
  bind(&right);
  mov(g3, 52);
  sub(g3, g3, g2);
  srlx(g2, g4, g3);
  bind(&sign);
  cmp(g1, 0);
  b(greater_equal, ConditionCodes::kXcc, &positive);
  nop();
  sub(g2, g0, g2);
  bind(&positive);
  sra(g2, g2, 0);
  Jump(&done);
  bind(&zero);
  mov(g2, 0);
  bind(&done);
}

void MacroAssembler::TruncateDoubleToI(Register result, DoubleRegister input) {
  MovDoubleToInt64(g1, input);
  TruncateDoubleBitsToInt32(g2, g1);
  Move(result, g2);
}

void MacroAssembler::TruncateDoubleToIOnStack() {
  LoadU64(g1, MemOperand(sp));
  TruncateDoubleBitsToInt32(g2, g1);
  StoreU64(g2, MemOperand(sp));
}

void MacroAssembler::RecordWriteField(Register object, int offset,
                                      Register value, SaveFPRegsMode fp_mode) {
  CHECK_NE(object, g3);
  CHECK_NE(value, g3);
  DCHECK(IsAligned(offset, kTaggedSize));
  AddS64(g3, object, offset - kHeapObjectTag);
  RecordWrite(object, g3, value, fp_mode);
}

void MacroAssembler::RecordWrite(Register object, Register slot, Register value,
                                 SaveFPRegsMode fp_mode, bool ephemeron) {
  CHECK_NE(object, g1);
  CHECK_NE(object, g2);
  CHECK_NE(slot, g1);
  CHECK_NE(slot, g2);
  CHECK_NE(value, g1);
  CHECK_NE(value, g2);
  if (v8_flags.disable_write_barriers) return;
  Label done;
  andcc(g0, value, kSmiTagMask);
  b(equal, ConditionCodes::kXcc, &done);
  nop();
  auto skip_uninteresting_page = [&](Register pointer, intptr_t mask) {
    srlx(g1, pointer, kPageSizeBits);
    sllx(g1, g1, kPageSizeBits);
    LoadU64(g1, MemOperand(g1, MemoryChunk::FlagsOffset()));
    Move(g2, mask);
    andcc(g0, g1, g2);
    b(equal, ConditionCodes::kXcc, &done);
    nop();
  };
  skip_uninteresting_page(
      value,
      static_cast<intptr_t>(MemoryChunk::kPointersToHereAreInterestingMask));
  skip_uninteresting_page(
      object,
      static_cast<intptr_t>(MemoryChunk::kPointersFromHereAreInterestingMask));
  RegList saved = WriteBarrierDescriptor::ComputeSavedRegisters(object, slot);
  Push(o7);
  for (Register reg : saved) Push(reg);
  Push(object, slot);
  Pop(WriteBarrierDescriptor::SlotAddressRegister(),
      WriteBarrierDescriptor::ObjectRegister());
  CallBuiltin(ephemeron ? Builtins::EphemeronKeyBarrier(fp_mode)
                        : Builtins::RecordWrite(fp_mode));
  for (Register reg : base::Reversed(saved)) Pop(reg);
  Pop(o7);
  bind(&done);
}

void MacroAssembler::ContinueToBuiltin(bool javascript_builtin,
                                       bool with_result) {
  using FC = BuiltinContinuationFrameConstants;
  const RegisterConfiguration* config = RegisterConfiguration::Default();
  int count = config->num_allocatable_general_registers();
  if (with_result) {
    if (javascript_builtin) {
      mov(g5, o0);
    } else {
      StoreU64(
          o0, MemOperand(sp, count * kSystemPointerSize + FC::kFixedFrameSize));
    }
  }
  for (int n = count - 1; n >= 0; --n) {
    Register reg = Register::from_code(config->GetAllocatableGeneralCode(n));
    Pop(reg);
    if (javascript_builtin && reg == kJavaScriptCallArgCountRegister)
      srax(reg, reg, kSmiShiftSize + kSmiTagSize);
  }
  if (javascript_builtin && with_result) {
    constexpr int result_offset = FC::kFixedSlotCount - kJSArgcReceiverSlots;
    AddS64(g1, o0, result_offset);
    sllx(g1, g1, kSystemPointerSizeLog2);
    LoadStackAddress(g2);
    add(g1, g1, g2);
    StoreU64(g5, MemOperand(g1));
  }
  LoadU64(fp, MemOperand(sp, FC::kFixedFrameSizeFromFp));
  Pop(g3);
  Drop(FC::kFixedSlotCountFromFp);
  Pop(g4);
  sub(o7, g4, kSparcReturnAddressOffset);
  srax(g3, g3, kSmiShiftSize + kSmiTagSize);
  sllx(g3, g3, kSystemPointerSizeLog2);
  AddS64(g1, kRootRegister, IsolateData::builtin_entry_table_offset());
  add(g1, g1, g3);
  LoadU64(g1, MemOperand(g1));
  Jump(g1);
}

void MacroAssembler::AdvanceBytecodeOffsetOrReturn(
    Register array, Register offset, Register bytecode, Register size_table,
    Register original_offset, Label* if_return) {
  DCHECK(!AreAliased(array, offset, bytecode, size_table, original_offset, g1));
  mov(original_offset, offset);
  Label process, extra_wide, end, advance;
  static_assert(static_cast<int>(interpreter::Bytecode::kWide) == 0);
  static_assert(static_cast<int>(interpreter::Bytecode::kExtraWide) == 1);
  static_assert(static_cast<int>(interpreter::Bytecode::kDebugBreakWide) == 2);
  static_assert(static_cast<int>(interpreter::Bytecode::kDebugBreakExtraWide) ==
                3);
  cmp(bytecode, 3);
  b(greater, ConditionCodes::kXcc, &process);
  nop();
  andcc(g0, bytecode, 1);
  b(not_equal, ConditionCodes::kXcc, &extra_wide);
  nop();
  add(offset, offset, 1);
  add(g1, array, offset);
  LoadU8(bytecode, MemOperand(g1));
  AddS64(size_table, size_table, interpreter::Bytecodes::kBytecodeCount);
  Jump(&process);
  bind(&extra_wide);
  add(offset, offset, 1);
  add(g1, array, offset);
  LoadU8(bytecode, MemOperand(g1));
  AddS64(size_table, size_table, 2 * interpreter::Bytecodes::kBytecodeCount);
  bind(&process);
#define RETURN_IF_BYTECODE(name)                                          \
  JumpIfEqual(bytecode, static_cast<int>(interpreter::Bytecode::k##name), \
              if_return);
  RETURN_BYTECODE_LIST(RETURN_IF_BYTECODE)
#undef RETURN_IF_BYTECODE
  cmp(bytecode, static_cast<int>(interpreter::Bytecode::kJumpLoop));
  b(not_equal, ConditionCodes::kXcc, &advance);
  nop();
  mov(offset, original_offset);
  Jump(&end);
  bind(&advance);
  add(g1, size_table, bytecode);
  LoadU8(bytecode, MemOperand(g1));
  add(offset, offset, bytecode);
  bind(&end);
}

void MacroAssembler::InvokeFunctionCode(Register function, Register new_target,
                                        Register expected, Register actual,
                                        InvokeType type) {
  CHECK_EQ(function, o1);
  CHECK_EQ(expected, o2);
  CHECK_EQ(actual, o0);
  CHECK(!new_target.is_valid() || new_target == o3);
  DCHECK_IMPLIES(type == InvokeType::kCall, has_frame());
  Label no_hook;
  Move(o4, ExternalReference::debug_hook_on_function_call_address(isolate()));
  LoadU8(o4, MemOperand(o4));
  cmp(o4, 0);
  b(equal, ConditionCodes::kXcc, &no_hook);
  nop();
  LoadU64(o4, MemOperand(sp));
  {
    FrameScope frame(
        this, has_frame() ? StackFrame::NO_FRAME_TYPE : StackFrame::INTERNAL);
    sllx(expected, expected, kSmiShiftSize + kSmiTagSize);
    sllx(actual, actual, kSmiShiftSize + kSmiTagSize);
    Push(expected, actual, kContextRegister);
    if (new_target.is_valid()) Push(new_target);
    Push(function, function, o4);
    CallRuntime(Runtime::kDebugOnFunctionCall);
    Pop(function);
    if (new_target.is_valid()) Pop(new_target);
    Pop(kContextRegister, actual, expected);
    srax(actual, actual, kSmiShiftSize + kSmiTagSize);
    srax(expected, expected, kSmiShiftSize + kSmiTagSize);
  }
  bind(&no_hook);
  if (!new_target.is_valid()) LoadRoot(o3, RootIndex::kUndefinedValue);
  Label overflow, done;
  AdaptArguments(expected, actual, &overflow);
  if (type == InvokeType::kJump) {
    JumpJSFunction(function);
  } else {
    LoadU64(g2, AsMemOperand(IsolateFieldId::kJSDispatchTable));
    LoadU32(g1, FieldMemOperand(function, JSFunction::kDispatchHandleOffset));
    srlx(o2, g1, kJSDispatchHandleShift);
    sllx(o2, o2, kJSDispatchTableEntrySizeLog2);
    add(g2, g2, o2);
    LoadU64(o2, MemOperand(g2, JSDispatchEntry::kEntrypointOffset));
    Call(o2);
    Jump(&done);
  }
  bind(&overflow);
  {
    FrameScope frame(
        this, has_frame() ? StackFrame::NO_FRAME_TYPE : StackFrame::INTERNAL);
    CallRuntime(Runtime::kThrowStackOverflow);
    Trap();
  }
  bind(&done);
}

void MacroAssembler::CallForDeoptimization(Builtin target, int deopt_id,
                                           Label* exit, DeoptimizeKind kind,
                                           Label* ret, Label*) {
  CHECK_LE(target, Builtins::kLastTier0);
  const int offset = IsolateData::BuiltinEntrySlotOffset(target);
  CHECK(IsSimm13(offset));
  ldx(g1, kRootRegister, offset);
  Call(g1);
}

void MacroAssembler::PrepareCallCFunction(int num_arguments, Register scratch) {
  CHECK_GE(num_arguments, 0);
  CHECK_LE(num_arguments, kMaxCParameters);
  const int stack_arguments =
      std::max(0, num_arguments - kRegisterPassedArguments);
  AllocateStackSpace(stack_arguments * kSystemPointerSize);
}

int MacroAssembler::CallCFunction(ExternalReference function, int num_arguments,
                                  SetIsolateDataSlots set_isolate_data_slots) {
  Move(g1, function);
  return CallCFunction(g1, num_arguments, set_isolate_data_slots);
}

int MacroAssembler::CallCFunction(Register function, int num_arguments,
                                  SetIsolateDataSlots set_isolate_data_slots) {
  CHECK_GE(num_arguments, 0);
  CHECK_LE(num_arguments, kMaxCParameters);
  const int stack_arguments =
      std::max(0, num_arguments - kRegisterPassedArguments);
  DCHECK(has_frame());
  mov(g5, function);
  int pc_offset = 0;
  int pc_add_offset = 0;
  if (set_isolate_data_slots == SetIsolateDataSlots::kYes) {
    CHECK(root_array_available());
    pc_offset = this->pc_offset();
    rdpc(g3);
    pc_add_offset = this->pc_offset();
    add(g3, g3, 0);
    StoreRootRelative(IsolateData::fast_c_call_caller_pc_offset(), g3);
    StoreRootRelative(IsolateData::fast_c_call_caller_fp_offset(), fp);
  }
  mov(g1, g5);
  mov(g5, kRootRegister);
  mov(g4, sp);
  add(g3, sp, kSparcStackBias);
  and_(g3, g3, -kSparcStackAlignment);
  sub(sp, g3, kSparcStackBias);
  // Stack arguments must remain below the parent window's spill area.
  const int native_frame_size =
      RoundUp(kSparcMinimumFrameSize + stack_arguments * kSystemPointerSize,
              kSparcStackAlignment);
  save(sp, sp, -native_frame_size);
  mov(l4, g4);
  mov(l3, g1);
  mov(l2, stack_arguments);
  mov(kRootRegister, g5);
  for (int n = 0; n < std::min(num_arguments, kRegisterPassedArguments); ++n) {
    mov(kCArgRegs[n], Register::from_code(i0.code() + n));
  }
  for (int n = 0; n < stack_arguments; ++n) {
    LoadU64(g2, MemOperand(l4, kSparcStackSlotBias + n * kSystemPointerSize));
    StoreU64(g2, MemOperand(sp, n * kSystemPointerSize));
  }
  Call(l3);
  const int return_offset = this->pc_offset();
  if (set_isolate_data_slots == SetIsolateDataSlots::kYes) {
    const int delta = return_offset - pc_offset;
    CHECK(IsSimm13(delta));
    instr_at_put(pc_add_offset,
                 (instr_at(pc_add_offset) & ~Instr{0x1fff}) | delta);
    // Fast-call deoptimization performs cleanup in its entry builtin.
    nop();
    nop();
    nop();
  }
  mov(i0, o0);
  mov(i1, o1);
  mov(g4, l4);
  restore();
  mov(sp, g4);
  Drop(stack_arguments);
  if (set_isolate_data_slots == SetIsolateDataSlots::kYes) {
    StoreRootRelative(IsolateData::fast_c_call_caller_fp_offset(), g0);
  }
  return return_offset;
}

void MacroAssembler::LoadRootRelative(Register dst, int32_t offset) {
  LoadU64(dst, MemOperand(kRootRegister, offset));
}

void MacroAssembler::StoreRootRelative(int32_t offset, Register src) {
  StoreU64(src, MemOperand(kRootRegister, offset));
}

void MacroAssembler::LoadRootRegisterOffset(Register dst, intptr_t offset) {
  AddS64(dst, kRootRegister, offset);
}

void MacroAssembler::LoadRoot(Register dst, RootIndex index) {
  LoadRootRelative(dst, IsolateData::root_slot_offset(index));
}

void MacroAssembler::LoadFromConstantsTable(Register dst, int constant_index) {
  LoadRoot(dst, RootIndex::kBuiltinsConstantsTable);
  LoadU64(dst,
          FieldMemOperand(dst, FixedArray::OffsetOfElementAt(constant_index)));
}

}  // namespace v8::internal
