// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/base/iterator.h"
#include "src/base/numbers/double.h"
#include "src/builtins/builtins-inl.h"
#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/codegen/sparc64/simd-sparc64-inl.h"
#include "src/compiler/backend/code-generator-impl.h"
#include "src/compiler/backend/code-generator.h"
#include "src/compiler/backend/gap-resolver.h"
#include "src/compiler/backend/sparc64/code-generator-sparc64-inl.h"
#include "src/objects/code.h"
#include "src/objects/instruction-stream.h"
#include "src/objects/js-function.h"
#include "src/sandbox/js-dispatch-table.h"

namespace v8::internal::compiler {

#define __ masm()->

class Sparc64OperandConverter final : public InstructionOperandConverter {
 public:
  Sparc64OperandConverter(CodeGenerator* gen, Instruction* instr)
      : InstructionOperandConverter(gen, instr) {}

  MemOperand MemoryOperand(size_t* next) {
    const size_t index = *next;
    switch (AddressingModeField::decode(instr_->opcode())) {
      case kMode_MRI:
        *next += 2;
        return MemOperand(InputRegister(index), InputInt32(index + 1));
      case kMode_MRR:
        *next += 2;
        gen_->masm()->add(g4, InputRegister(index), InputRegister(index + 1));
        return MemOperand(g4);
      case kMode_Root:
        *next += 1;
        gen_->masm()->add(g4, kRootRegister, InputRegister(index));
        return MemOperand(g4);
      case kMode_None:
        UNREACHABLE();
    }
    UNREACHABLE();
  }

  MemOperand ToMemOperand(InstructionOperand* operand) const {
    DCHECK(operand->IsStackSlot() || operand->IsFPStackSlot());
    return SlotToMemOperand(AllocatedOperand::cast(operand)->index());
  }

  MemOperand SlotToMemOperand(int slot) const {
    FrameOffset offset = frame_access_state()->GetFrameOffset(slot);
    int displacement = offset.offset();
    if (slot < 0 &&
        gen_->linkage()->GetIncomingDescriptor()->IsCFunctionCall()) {
      displacement += kSparcMinimumFrameSize;
    }
    return MemOperand(offset.from_stack_pointer() ? sp : fp, displacement);
  }
};

namespace {

const char* Sparc64OpcodeName(ArchOpcode opcode) {
  switch (opcode) {
#define OPCODE_NAME(name) \
  case k##name:           \
    return #name;
    ARCH_OPCODE_LIST(OPCODE_NAME)
#undef OPCODE_NAME
  }
  UNREACHABLE();
}

RegList SavedRegisters(const CallDescriptor* descriptor) {
  // SAVE/RESTORE preserves the native caller's entire local/input window.
  if (descriptor->IsCFunctionCall()) return {};
  return descriptor->CalleeSavedRegisters() - fp - kNativeFramePointer;
}

RegList CallerSavedRegisters() {
  return kJSCallerSaved - RegList{kReturnRegister0};
}

int CallerSavedSlots(SaveFPRegsMode fp_mode) {
  return CallerSavedRegisters().Count() +
         (fp_mode == SaveFPRegsMode::kSave ? kCallerSavedDoubles.Count() : 0);
}

void AdjustTailCallStack(MacroAssembler* masm, FrameAccessState* state,
                         int first_unused_slot, bool shrink) {
  int current = state->GetSPToFPSlotCount() +
                StandardFrameConstants::kFixedSlotCountAboveFp;
  int delta = first_unused_slot - current;
  if (delta > 0 || (shrink && delta < 0)) {
    masm->SubS64(sp, sp, intptr_t{delta} * kSystemPointerSize);
    state->IncreaseSPDelta(delta);
  }
}

}  // namespace

void CodeGenerator::FinishFrame(Frame* frame) {
  const auto* descriptor = linkage()->GetIncomingDescriptor();
  DoubleRegList fp_saves = descriptor->CalleeSavedFPRegisters();
  if (!fp_saves.is_empty()) {
    frame->AlignSavedCalleeRegisterSlots();
    frame->AllocateSavedCalleeRegisterSlots(fp_saves.Count());
  }
  RegList saves = SavedRegisters(descriptor);
  if (!saves.is_empty()) frame->AllocateSavedCalleeRegisterSlots(saves.Count());
}

void CodeGenerator::AssembleConstructFrame() {
  const auto* descriptor = linkage()->GetIncomingDescriptor();
  if (frame_access_state()->has_frame()) {
    if (descriptor->IsCFunctionCall()) {
      __ EnterCFunctionFrame();
    } else if (descriptor->IsJSFunctionCall()) {
      __ PushStandardFrame(kJSFunctionRegister);
    } else {
      StackFrame::Type type = info()->GetOutputStackFrameType();
      __ EnterFrame(type);
#if V8_ENABLE_WEBASSEMBLY
      if (type != StackFrame::WASM && (descriptor->IsAnyWasmFunctionCall() ||
                                       descriptor->IsWasmImportWrapper() ||
                                       descriptor->IsWasmCapiFunction())) {
        __ Push(kWasmImplicitArgRegister);
      }
      if (descriptor->IsWasmCapiFunction()) {
        __ AllocateStackSpace(kSystemPointerSize);
      }
#endif
    }
  }

  RegList saves = SavedRegisters(descriptor);
  DoubleRegList fp_saves = descriptor->CalleeSavedFPRegisters();
  int spill_slots = frame()->GetTotalFrameSlotCount() -
                    frame()->GetFixedSlotCount() - saves.Count() -
                    fp_saves.Count() - frame()->GetReturnSlotCount();
  if (info()->is_osr()) {
    __ Abort(AbortReason::kShouldNotDirectlyEnterOsrFunction);
    osr_pc_offset_ = __ pc_offset();
    spill_slots -= osr_helper()->UnoptimizedFrameSlots();
  }
  CHECK_GE(spill_slots, 0);
  __ AllocateStackSpace(spill_slots * kSystemPointerSize);
  for (DoubleRegister reg : fp_saves) {
    __ AllocateStackSpace(kDoubleSize);
    __ StoreF64(reg, MemOperand(sp));
  }
  for (Register reg : saves) __ Push(reg);
  __ AllocateStackSpace(frame()->GetReturnSlotCount() * kSystemPointerSize);

  for (int slot : frame()->tagged_slots()) {
    FrameOffset offset = frame_access_state()->GetFrameOffset(slot);
    DCHECK(offset.from_frame_pointer());
    __ StoreU64(g0, MemOperand(fp, offset.offset()));
  }
}

void CodeGenerator::AssembleDeconstructFrame() {
  __ LeaveFrame(StackFrame::MANUAL);
}

void CodeGenerator::AssembleReturn(InstructionOperand* additional_pop_count) {
  const auto* descriptor = linkage()->GetIncomingDescriptor();
  Sparc64OperandConverter g(this, nullptr);
  const int parameter_slots =
      static_cast<int>(descriptor->ParameterSlotCount());
  const bool immediate = additional_pop_count->IsImmediate();
  const int extra =
      immediate ? g.ToConstant(additional_pop_count).ToInt32() : 0;
  DCHECK_IMPLIES(parameter_slots != 0 && immediate, extra == 0);
  if (!immediate) __ Move(g5, g.ToRegister(additional_pop_count));

  __ Drop(frame()->GetReturnSlotCount());
  RegList saves = SavedRegisters(descriptor);
  for (auto it = saves.rbegin(); it != saves.rend(); ++it) __ Pop(*it);
  DoubleRegList fp_saves = descriptor->CalleeSavedFPRegisters();
  for (auto it = fp_saves.rbegin(); it != fp_saves.rend(); ++it) {
    __ LoadF64(*it, MemOperand(sp));
    __ Drop(kDoubleSize / kSystemPointerSize);
  }

  bool drop_jsargs = parameter_slots != 0 &&
                     frame_access_state()->has_frame() &&
                     descriptor->IsJSFunctionCall();
  if (drop_jsargs) {
    __ LoadU64(g5, MemOperand(fp, StandardFrameConstants::kArgCOffset));
  }
  if (frame_access_state()->has_frame()) AssembleDeconstructFrame();

  if (descriptor->IsCFunctionCall()) {
    CHECK(immediate && extra == 0);
    __ RetCFunction();
    return;
  }
  if (drop_jsargs) {
    Label enough;
    __ Move(g3, parameter_slots);
    __ cmp(g5, g3);
    __ b(greater_equal, ConditionCodes::kXcc, &enough);
    __ nop();
    __ mov(g5, g3);
    __ bind(&enough);
    __ Drop(g5);
  } else if (immediate) {
    __ Drop(parameter_slots + extra);
  } else if (parameter_slots == 0) {
    __ Drop(g5);
  } else {
    __ Drop(parameter_slots);
  }
  __ Ret();
}

void CodeGenerator::AssemblePrepareTailCall() {
  CHECK(!linkage()->GetIncomingDescriptor()->IsCFunctionCall());
  if (frame_access_state()->has_frame()) __ RestoreFrameStateForTailCall();
  frame_access_state()->SetFrameAccessToSP();
}

void CodeGenerator::AssembleTailCallBeforeGap(Instruction*, int first_unused) {
  AdjustTailCallStack(masm(), frame_access_state(), first_unused, false);
}

void CodeGenerator::AssembleTailCallAfterGap(Instruction*, int first_unused) {
  AdjustTailCallStack(masm(), frame_access_state(), first_unused, true);
}

void CodeGenerator::FinishCode() {}

void CodeGenerator::PrepareForDeoptimizationExits(
    ZoneDeque<DeoptimizationExit*>* exits) {
  __ EnsureSpaceFor(static_cast<int>(exits->size()) * 3 * kInstrSize);
}

void CodeGenerator::AssembleCodeStartRegisterCheck() {
  Label valid;
  int pc = __ pc_offset();
  __ rdpc(g1);
  __ SubS64(g1, g1, pc);
  __ cmp(g1, kJavaScriptCallCodeStartRegister);
  __ b(equal, ConditionCodes::kXcc, &valid);
  __ nop();
  __ Trap();
  __ bind(&valid);
}

void CodeGenerator::AssembleDispatchHandleRegisterCheck() {
  CHECK(!V8_JS_LINKAGE_INCLUDES_DISPATCH_HANDLE_BOOL);
}

void CodeGenerator::AssertNotDeoptimized() {
  Label valid;
  __ LoadU64(g1, MemOperand(kJavaScriptCallCodeStartRegister,
                            InstructionStream::kCodeOffset -
                                InstructionStream::kHeaderSize));
  __ LoadU32(g1, FieldMemOperand(g1, Code::kFlagsOffset));
  __ Move(g2, intptr_t{1} << Code::kMarkedForDeoptimizationBit);
  __ andcc(g0, g1, g2);
  __ b(equal, ConditionCodes::kIcc, &valid);
  __ nop();
  __ Trap();
  __ bind(&valid);
}

void CodeGenerator::AssembleArchBranch(Instruction* instr, BranchInfo* branch) {
  EmitSparc64Branch(masm(), instr->arch_opcode(), branch->condition,
                    branch->true_label);
  if (!branch->fallthru) __ Jump(branch->false_label);
}

#if V8_ENABLE_WEBASSEMBLY
void CodeGenerator::AssembleArchTrap(Instruction* instr,
                                     FlagsCondition condition) {
  class OutOfLineTrap final : public OutOfLineCode {
   public:
    OutOfLineTrap(CodeGenerator* gen, Instruction* instr)
        : OutOfLineCode(gen), instr_(instr), gen_(gen) {}
    void Generate() final {
      Sparc64OperandConverter i(gen_, instr_);
      gen_->AssembleSourcePosition(instr_);
      masm()->LoadLiteral(
          g1, static_cast<Address>(i.InputInt32(instr_->InputCount() - 1)),
          RelocInfo::WASM_STUB_CALL);
      masm()->Call(g1);
      auto* refs = gen_->zone()->New<ReferenceMap>(gen_->zone());
      gen_->RecordSafepoint(refs);
      masm()->Trap();
    }

   private:
    Instruction* instr_;
    CodeGenerator* gen_;
  };
  auto* ool = zone()->New<OutOfLineTrap>(this, instr);
  EmitSparc64Branch(masm(), instr->arch_opcode(), condition, ool->entry());
}

void CodeGenerator::AssembleArchConditionalTrap(Instruction*, FlagsCondition) {
  UNREACHABLE();
}
#endif

void CodeGenerator::AssembleArchDeoptBranch(Instruction* instr,
                                            BranchInfo* branch) {
  AssembleArchBranch(instr, branch);
}

void CodeGenerator::AssembleArchConditionalBranch(Instruction*, BranchInfo*) {
  // Chained conditional comparisons are not selected on SPARC64.
  UNREACHABLE();
}

void CodeGenerator::AssembleArchSelect(Instruction* instr,
                                       FlagsCondition condition) {
  Label take_true, done;
  InstructionOperand* output = instr->OutputAt(instr->OutputCount() - 1);
  EmitSparc64Branch(masm(), instr->arch_opcode(), condition, &take_true);
  AssembleMove(instr->InputAt(instr->InputCount() - 1), output);
  __ Jump(&done);
  __ bind(&take_true);
  AssembleMove(instr->InputAt(instr->InputCount() - 2), output);
  __ bind(&done);
}

void CodeGenerator::AssembleArchJumpRegardlessOfAssemblyOrder(
    RpoNumber target) {
  __ Jump(GetLabel(target));
}

void CodeGenerator::AssembleArchBoolean(Instruction* instr,
                                        FlagsCondition condition) {
  Sparc64OperandConverter i(this, instr);
  Register result = i.OutputRegister(instr->OutputCount() - 1);
  Label done;
  __ mov(result, 1);
  EmitSparc64Branch(masm(), instr->arch_opcode(), condition, &done);
  __ mov(result, 0);
  __ bind(&done);
}

void CodeGenerator::AssembleArchBinarySearchSwitch(Instruction* instr) {
  Sparc64OperandConverter i(this, instr);
  std::vector<std::pair<int32_t, Label*>> cases;
  for (size_t n = 2; n < instr->InputCount(); n += 2) {
    cases.push_back({i.InputInt32(n), i.InputLabel(n + 1)});
  }
  AssembleArchBinarySearchSwitchRange(i.InputRegister(0), i.InputRpo(1),
                                      cases.data(),
                                      cases.data() + cases.size());
}

void CodeGenerator::AssembleArchTableSwitch(Instruction* instr) {
  Sparc64OperandConverter i(this, instr);
  for (size_t n = 2; n < instr->InputCount(); ++n) {
    __ JumpIfEqual(i.InputRegister(0), static_cast<int32_t>(n - 2),
                   i.InputLabel(n));
  }
  __ Jump(i.InputLabel(1));
}

void CodeGenerator::AssembleJumpTable(base::Vector<Label*> targets) {
  __ DataAlign(kSystemPointerSize);
  for (Label* target : targets) {
    CHECK(target->is_bound());
    __ RecordRelocInfo(RelocInfo::INTERNAL_REFERENCE);
    __ dq(target->pos());
  }
}

CodeGenerator::CodeGenResult CodeGenerator::AssembleArchInstruction(
    Instruction* instr) {
  Sparc64OperandConverter i(this, instr);
  const ArchOpcode opcode = instr->arch_opcode();
  const bool has_flags = instr->flags_mode() != kFlags_none;
  const bool single = MiscField::decode(instr->opcode()) != 0;
  auto call_float = [&](ExternalReference function, int arguments) {
    CHECK_EQ(i.InputDoubleRegister(0), d0);
    if (arguments == 2) CHECK_EQ(i.InputDoubleRegister(1), d1);
    CHECK_EQ(i.OutputDoubleRegister(), d0);
    FrameScope scope(masm(), StackFrame::MANUAL);
    __ PrepareCallCFunction(0, g1);
    __ CallCFunction(function, 0);
  };
  auto atomic = [&](int bytes, bool is_signed, Sparc64AtomicOp op) {
    CHECK_EQ(AddressingModeField::decode(instr->opcode()), kMode_MRR);
    bool compare = op == Sparc64AtomicOp::kCompareExchange;
    bool little_endian = false;
#if V8_ENABLE_WEBASSEMBLY
    little_endian = info()->IsWasm() && !MiscField::decode(instr->opcode());
#endif
    EmitSparc64Atomic(masm(), i.OutputRegister(), i.InputRegister(0),
                      i.InputRegister(1), i.InputRegister(compare ? 3 : 2),
                      compare ? i.InputRegister(2) : no_reg, i.TempRegister(0),
                      i.TempRegister(1), bytes, is_signed, op, little_endian);
  };
  auto right = [&]() {
    if (instr->InputAt(1)->IsRegister()) return i.InputRegister(1);
    __ Move(g3, i.InputInt64(1));
    return g3;
  };
  auto set_result_flags = [&]() {
    if (has_flags) __ cmp(i.OutputRegister(), 0);
  };

#define BINARY_OP(op, instruction)                               \
  case op: {                                                     \
    Register rhs = right();                                      \
    __ instruction(i.OutputRegister(), i.InputRegister(0), rhs); \
    set_result_flags();                                          \
    break;                                                       \
  }
#define SHIFT_OP(op, instruction)                                              \
  case op:                                                                     \
    if (instr->InputAt(1)->IsRegister()) {                                     \
      __ instruction(i.OutputRegister(), i.InputRegister(0),                   \
                     i.InputRegister(1));                                      \
    } else {                                                                   \
      __ instruction(i.OutputRegister(), i.InputRegister(0), i.InputInt32(1)); \
    }                                                                          \
    break;
#define FLOAT_BINARY(op, instruction)                                        \
  case op:                                                                   \
    if (single) {                                                            \
      __ instruction##s(i.OutputFloatRegister(), i.InputFloatRegister(0),    \
                        i.InputFloatRegister(1));                            \
    } else {                                                                 \
      __ instruction##d(i.OutputDoubleRegister(), i.InputDoubleRegister(0),  \
                        i.InputDoubleRegister(1));                           \
    }                                                                        \
    sparc64::CanonicalizeNaN(                                                \
        masm(), single ? i.OutputFloatRegister() : i.OutputDoubleRegister(), \
        single);                                                             \
    break;
#define FLOAT_UNARY(op, single_instruction, double_instruction)                \
  case op:                                                                     \
    if (single) {                                                              \
      __ single_instruction(i.OutputFloatRegister(), i.InputFloatRegister(0)); \
    } else {                                                                   \
      __ double_instruction(i.OutputDoubleRegister(),                          \
                            i.InputDoubleRegister(0));                         \
    }                                                                          \
    break;
#define INTEGER_LOAD(op, method, bytes, sign)                 \
  case op: {                                                  \
    size_t next = 0;                                          \
    MemOperand mem = i.MemoryOperand(&next);                  \
    bool atomic = i.InputInt32(next);                         \
    if (atomic) __ membar(kSync);                             \
    if (single) {                                             \
      __ LoadUnaligned(i.OutputRegister(), mem, bytes, sign); \
    } else {                                                  \
      __ method(i.OutputRegister(), mem);                     \
    }                                                         \
    if (atomic) __ membar(kSync);                             \
    break;                                                    \
  }
#define INTEGER_STORE(op, method, bytes)      \
  case op: {                                  \
    size_t next = 0;                          \
    MemOperand mem = i.MemoryOperand(&next);  \
    Register value = i.InputRegister(next++); \
    bool atomic = i.InputInt32(next);         \
    if (atomic) __ membar(kSync);             \
    if (single) {                             \
      __ StoreUnaligned(value, mem, bytes);   \
    } else {                                  \
      __ method(value, mem);                  \
    }                                         \
    if (atomic) __ membar(kSync);             \
    break;                                    \
  }

  switch (opcode) {
    case kArchNop:
      break;
    case kArchComment:
      __ RecordComment(reinterpret_cast<const char*>(i.InputInt64(0)),
                       SourceLocation());
      break;
    case kArchDebugBreak:
      __ Trap();
      break;
    case kArchAbortCSADcheck: {
      CHECK_EQ(i.InputRegister(0), o1);
      FrameScope scope(masm(), StackFrame::NO_FRAME_TYPE);
      __ CallBuiltin(Builtin::kAbortCSADcheck);
      __ Trap();
      break;
    }
    case kArchJmp:
      AssembleArchJump(i.InputRpo(0));
      break;
    case kArchBinarySearchSwitch:
      AssembleArchBinarySearchSwitch(instr);
      break;
    case kArchTableSwitch:
      AssembleArchTableSwitch(instr);
      break;
    case kArchRet:
      AssembleReturn(instr->InputAt(0));
      break;
    case kArchPrepareTailCall:
      AssemblePrepareTailCall();
      break;
    case kArchSaveCallerRegisters: {
      fp_mode_ =
          static_cast<SaveFPRegsMode>(MiscField::decode(instr->opcode()));
      CHECK(fp_mode_ == SaveFPRegsMode::kIgnore ||
            fp_mode_ == SaveFPRegsMode::kSave);
      for (Register reg : CallerSavedRegisters()) __ Push(reg);
      if (fp_mode_ == SaveFPRegsMode::kSave) {
        for (DoubleRegister reg : kCallerSavedDoubles) {
          __ AllocateStackSpace(kDoubleSize);
          __ StoreF64(reg, MemOperand(sp));
        }
      }
      DCHECK_EQ(frame_access_state()->sp_delta(), 0);
      frame_access_state()->IncreaseSPDelta(CallerSavedSlots(fp_mode_));
      DCHECK(!caller_registers_saved_);
      caller_registers_saved_ = true;
      break;
    }
    case kArchRestoreCallerRegisters: {
      CHECK_EQ(fp_mode_,
               static_cast<SaveFPRegsMode>(MiscField::decode(instr->opcode())));
      if (fp_mode_ == SaveFPRegsMode::kSave) {
        for (DoubleRegister reg : base::Reversed(kCallerSavedDoubles)) {
          __ LoadF64(reg, MemOperand(sp));
          __ Drop(kDoubleSize / kSystemPointerSize);
        }
      }
      RegList saved = CallerSavedRegisters();
      for (Register reg : base::Reversed(saved)) __ Pop(reg);
      frame_access_state()->IncreaseSPDelta(-CallerSavedSlots(fp_mode_));
      DCHECK_EQ(frame_access_state()->sp_delta(), 0);
      DCHECK(caller_registers_saved_);
      caller_registers_saved_ = false;
      break;
    }
    case kArchPrepareCallCFunction: {
      int num_parameters = MiscField::decode(instr->opcode());
      __ PrepareCallCFunction(num_parameters, g1);
      // Gap-move temporaries must stay below the outgoing C arguments.
      frame_access_state()->IncreaseSPDelta(
          std::max(0, num_parameters - kRegisterPassedArguments));
      frame_access_state()->SetFrameAccessToFP();
      break;
    }
    case kArchCallCFunction: {
      uint32_t extensions = i.InputUint32(instr->InputCount() - 2);
      for (int slot = 0; slot < 6; ++slot) {
        unsigned extension = (extensions >> (slot * 3)) & 7;
        if (!extension) continue;
        int bits = 8 << ((extension - 1) / 2);
        Register reg = Register::from_code(o0.code() + slot);
        __ sllx(reg, reg, 64 - bits);
        if (extension & 1) {
          __ srax(reg, reg, 64 - bits);
        } else {
          __ srlx(reg, reg, 64 - bits);
        }
      }
      uint32_t counts = i.InputUint32(instr->InputCount() - 1);
      int gp_count = ParamField::decode(counts);
      CHECK_EQ(FPParamField::decode(counts), 0);
      int pc;
      if (instr->InputAt(0)->IsImmediate()) {
        pc = __ CallCFunction(i.InputExternalReference(0), gp_count);
      } else {
        pc = __ CallCFunction(i.InputRegister(0), gp_count);
      }
      RecordSafepoint(instr->reference_map(), pc);
      if (instr->HasCallDescriptorFlag(CallDescriptor::kHasExceptionHandler))
        handlers_.push_back({nullptr, pc});
      if (instr->HasCallDescriptorFlag(CallDescriptor::kNeedsFrameState))
        RecordDeoptInfo(instr, pc);
      frame_access_state()->SetFrameAccessToDefault();
      frame_access_state()->ClearSPDelta();
      if (caller_registers_saved_)
        frame_access_state()->IncreaseSPDelta(CallerSavedSlots(fp_mode_));
      break;
    }
#if V8_ENABLE_WEBASSEMBLY
    case kArchCallWasmFunction:
    case kArchCallWasmFunctionIndirect:
    case kArchTailCallWasm:
    case kArchTailCallWasmIndirect: {
      const bool tail =
          opcode == kArchTailCallWasm || opcode == kArchTailCallWasmIndirect;
      const bool indirect = opcode == kArchCallWasmFunctionIndirect ||
                            opcode == kArchTailCallWasmIndirect;
      if (instr->InputAt(0)->IsImmediate()) {
        Constant target = i.ToConstant(instr->InputAt(0));
        __ LoadLiteral(g1, static_cast<Address>(target.ToInt64()),
                       target.rmode());
      } else {
        __ Move(g1, i.InputRegister(0));
        if (indirect) {
          __ srl(g1, g1, 0);
          __ sllx(g1, g1, kSystemPointerSizeLog2);
          __ Move(g2, ExternalReference::wasm_code_pointer_table());
          __ add(g1, g2, g1);
          __ LoadU64(g1, MemOperand(g1));
        }
      }
      if (tail) {
        __ Jump(g1);
        frame_access_state()->SetFrameAccessToDefault();
      } else {
        __ Call(g1);
        RecordCallPosition(instr);
      }
      frame_access_state()->ClearSPDelta();
      break;
    }
#endif
    case kArchCallJSFunction: {
      if (instr->InputAt(0)->IsImmediate()) {
        __ Move(kJavaScriptCallTargetRegister,
                i.ToConstant(instr->InputAt(0)).ToHeapObject());
      } else {
        __ Move(kJavaScriptCallTargetRegister, i.InputRegister(0));
      }
      __ LoadU64(g2, __ AsMemOperand(IsolateFieldId::kJSDispatchTable));
      __ LoadU32(g1, FieldMemOperand(kJavaScriptCallTargetRegister,
                                     JSFunction::kDispatchHandleOffset));
      __ srlx(g1, g1, kJSDispatchHandleShift);
      __ sllx(g1, g1, kJSDispatchTableEntrySizeLog2);
      __ add(g2, g2, g1);
      __ LoadU64(kJavaScriptCallCodeStartRegister,
                 MemOperand(g2, JSDispatchEntry::kEntrypointOffset));
      __ Call(kJavaScriptCallCodeStartRegister);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchCallCodeObject:
    case kArchTailCallCodeObject: {
      Register target =
          instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister)
              ? kJavaScriptCallCodeStartRegister
              : g1;
      if (instr->InputAt(0)->IsImmediate()) {
        __ Move(target, i.InputCode(0));
      } else {
        DCHECK_IMPLIES(
            instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister),
            i.InputRegister(0) == kJavaScriptCallCodeStartRegister);
        __ Move(target, i.InputRegister(0));
      }
      __ LoadU64(target,
                 FieldMemOperand(target, Code::kInstructionStartOffset));
      if (opcode == kArchCallCodeObject) {
        __ Call(target);
        RecordCallPosition(instr);
      } else {
        __ Jump(target);
        frame_access_state()->SetFrameAccessToDefault();
      }
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchCallBuiltinPointer: {
      CHECK(!instr->InputAt(0)->IsImmediate());
      Register target =
          instr->HasCallDescriptorFlag(CallDescriptor::kFixedTargetRegister)
              ? kJavaScriptCallCodeStartRegister
              : g1;
      __ srax(g3, i.InputRegister(0), kSmiShiftSize + kSmiTagSize);
      __ sllx(g3, g3, kSystemPointerSizeLog2);
      __ AddS64(g1, kRootRegister, IsolateData::builtin_entry_table_offset());
      __ add(target, g1, g3);
      __ LoadU64(target, MemOperand(target));
      __ Call(target);
      RecordCallPosition(instr);
      frame_access_state()->ClearSPDelta();
      break;
    }
    case kArchTailCallAddress:
      __ Jump(i.InputRegister(0));
      frame_access_state()->ClearSPDelta();
      frame_access_state()->SetFrameAccessToDefault();
      break;
    case kArchDeoptimize: {
      DeoptimizationExit* exit =
          BuildTranslation(instr, -1, 0, 0, OutputFrameStateCombine::Ignore());
      __ Jump(exit->label());
      break;
    }
    case kArchFramePointer:
      __ Move(i.OutputRegister(), fp);
      break;
#if V8_ENABLE_WEBASSEMBLY
    case kArchStackPointer:
      __ LoadStackAddress(i.OutputRegister());
      break;
    case kArchSetStackPointer:
      __ Move(sp, i.InputRegister(0));
      break;
#endif
    case kArchParentFramePointer:
      if (frame_access_state()->has_frame()) {
        __ LoadU64(i.OutputRegister(), MemOperand(fp));
      } else {
        __ Move(i.OutputRegister(), fp);
      }
      break;
    case kArchRootPointer:
      __ Move(i.OutputRegister(), kRootRegister);
      break;
    case kArchStackSlot:
      __ LoadAddress(i.OutputRegister(), i.SlotToMemOperand(i.InputInt32(0)));
      break;
    case kArchStackPointerGreaterThan: {
      uint32_t offset = 0;
      ShouldApplyOffsetToStackCheck(instr, &offset);
      __ add(g1, sp, kSparcStackBias);
      __ SubS64(g1, g1, offset);
      __ cmp(g1, i.InputRegister(0));
      break;
    }
    case kArchStackCheckOffset:
      __ Move(i.OutputRegister(), Smi::FromInt(GetStackCheckOffset()));
      break;
    case kArchTruncateDoubleToI:
      __ TruncateDoubleToI(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kArchStoreWithWriteBarrier:
    case kArchAtomicStoreWithWriteBarrier: {
      RecordWriteMode mode = RecordWriteModeField::decode(instr->opcode());
      CHECK_NE(mode, RecordWriteMode::kValueIsIndirectPointer);
      Register object = i.InputRegister(0);
      Register value = i.InputRegister(2);
      Register slot = i.TempRegister(0);
      if (AddressingModeField::decode(instr->opcode()) == kMode_MRI) {
        __ AddS64(slot, object, i.InputInt32(1));
      } else {
        CHECK_EQ(AddressingModeField::decode(instr->opcode()), kMode_MRR);
        __ add(slot, object, i.InputRegister(1));
      }
      if (opcode == kArchAtomicStoreWithWriteBarrier) __ membar(kSync);
      __ StoreU64(value, MemOperand(slot));
      if (opcode == kArchAtomicStoreWithWriteBarrier) __ membar(kSync);
      __ RecordWrite(object, slot, value,
                     frame()->DidAllocateDoubleRegisters()
                         ? SaveFPRegsMode::kSave
                         : SaveFPRegsMode::kIgnore,
                     mode == RecordWriteMode::kValueIsEphemeronKey);
      break;
    }
    case kSparcSync:
      __ membar(kSync);
      break;
#define ATOMIC_CASE(name, bytes, sign, op)       \
  case k##name:                                  \
    atomic(bytes, sign, Sparc64AtomicOp::k##op); \
    break;
#define COMMON_ATOMICS(op)                      \
  ATOMIC_CASE(Atomic##op##Int8, 1, true, op)    \
  ATOMIC_CASE(Atomic##op##Uint8, 1, false, op)  \
  ATOMIC_CASE(Atomic##op##Int16, 2, true, op)   \
  ATOMIC_CASE(Atomic##op##Uint16, 2, false, op) \
  ATOMIC_CASE(Atomic##op##Word32, 4, false, op)
      COMMON_ATOMICS(Exchange)
      COMMON_ATOMICS(CompareExchange)
      COMMON_ATOMICS(Add)
      COMMON_ATOMICS(Sub)
      COMMON_ATOMICS(And)
      COMMON_ATOMICS(Or)
      COMMON_ATOMICS(Xor)
#define TARGET_EXCHANGE(op)                          \
  ATOMIC_CASE(SparcAtomic##op##Uint8, 1, false, op)  \
  ATOMIC_CASE(SparcAtomic##op##Uint16, 2, false, op) \
  ATOMIC_CASE(SparcAtomic##op##Word32, 4, false, op) \
  ATOMIC_CASE(SparcAtomic##op##Word64, 8, false, op)
      TARGET_EXCHANGE(Exchange)
      TARGET_EXCHANGE(CompareExchange)
#define TARGET_ATOMICS(op)                           \
  ATOMIC_CASE(SparcAtomic##op##Uint8, 1, false, op)  \
  ATOMIC_CASE(SparcAtomic##op##Uint16, 2, false, op) \
  ATOMIC_CASE(SparcAtomic##op##Uint32, 4, false, op) \
  ATOMIC_CASE(SparcAtomic##op##Uint64, 8, false, op) \
  ATOMIC_CASE(SparcAtomic##op##Int8, 1, true, op)    \
  ATOMIC_CASE(SparcAtomic##op##Int16, 2, true, op)   \
  ATOMIC_CASE(SparcAtomic##op##Int32, 4, true, op)   \
  ATOMIC_CASE(SparcAtomic##op##Int64, 8, true, op)
      TARGET_ATOMICS(Add)
      TARGET_ATOMICS(Sub)
      TARGET_ATOMICS(And)
      TARGET_ATOMICS(Or)
      TARGET_ATOMICS(Xor)
#undef TARGET_ATOMICS
#undef TARGET_EXCHANGE
#undef COMMON_ATOMICS
#undef ATOMIC_CASE
      BINARY_OP(kSparcAnd, and_)
      BINARY_OP(kSparcOr, or_)
      BINARY_OP(kSparcXor, xor_)
      BINARY_OP(kSparcMul64, mulx)
      SHIFT_OP(kSparcShiftLeft32, sll)
      SHIFT_OP(kSparcShiftLeft64, sllx)
      SHIFT_OP(kSparcShiftRight32, srl)
      SHIFT_OP(kSparcShiftRight64, srlx)
      SHIFT_OP(kSparcShiftRightAlg32, sra)
      SHIFT_OP(kSparcShiftRightAlg64, srax)
    case kSparcNot:
      __ xor_(i.OutputRegister(), i.InputRegister(0), -1);
      set_result_flags();
      break;
    case kSparcNeg:
      __ sub(i.OutputRegister(), g0, i.InputRegister(0));
      set_result_flags();
      break;
    case kSparcCntlz32:
    case kSparcCntlz64:
      EmitSparc64CountLeadingZeros(masm(), i.OutputRegister(),
                                   i.InputRegister(0), opcode == kSparcCntlz32);
      break;
    case kSparcAdd32:
    case kSparcAdd64:
    case kSparcAddWithOverflow32: {
      Register rhs = right();
      __ addcc(i.OutputRegister(), i.InputRegister(0), rhs);
      if (opcode != kSparcAdd64)
        __ sra(i.OutputRegister(), i.OutputRegister(), 0);
      break;
    }
    case kSparcSub:
    case kSparcSubWithOverflow32: {
      Register rhs = right();
      __ subcc(i.OutputRegister(), i.InputRegister(0), rhs);
      if (opcode == kSparcSubWithOverflow32) {
        __ sra(i.OutputRegister(), i.OutputRegister(), 0);
      }
      break;
    }
    case kSparcMul32:
      __ mulx(i.OutputRegister(), i.InputRegister(0), i.InputRegister(1));
      __ sra(i.OutputRegister(), i.OutputRegister(), 0);
      break;
    case kSparcMul32WithHigh32:
      EmitSparc64Mul32WithHigh32(masm(), i.OutputRegister(0),
                                 i.OutputRegister(1), i.InputRegister(0),
                                 i.InputRegister(1));
      break;
    case kSparcMulHigh32:
    case kSparcMulHighU32:
    case kSparcMulHighS64:
    case kSparcMulHighU64:
      EmitSparc64MultiplyHigh(
          masm(), i.OutputRegister(), i.InputRegister(0), i.InputRegister(1),
          opcode == kSparcMulHigh32 || opcode == kSparcMulHighU32,
          opcode == kSparcMulHighU32 || opcode == kSparcMulHighU64);
      break;
    case kSparcDiv32:
    case kSparcDivU32:
    case kSparcDiv64:
    case kSparcDivU64:
    case kSparcMod32:
    case kSparcModU32:
    case kSparcMod64:
    case kSparcModU64:
      EmitSparc64Divide(masm(), i.OutputRegister(), i.InputRegister(0),
                        i.InputRegister(1),
                        opcode == kSparcDiv32 || opcode == kSparcDivU32 ||
                            opcode == kSparcMod32 || opcode == kSparcModU32,
                        opcode == kSparcDivU32 || opcode == kSparcDivU64 ||
                            opcode == kSparcModU32 || opcode == kSparcModU64,
                        opcode == kSparcMod32 || opcode == kSparcModU32 ||
                            opcode == kSparcMod64 || opcode == kSparcModU64);
      break;
    case kSparcCmp32:
    case kSparcCmp64: {
      Register rhs = right();
      __ cmp(i.InputRegister(0), rhs);
      break;
    }
    case kSparcTst32:
    case kSparcTst64: {
      Register rhs = right();
      __ andcc(g0, i.InputRegister(0), rhs);
      break;
    }
    case kSparcExtendSignWord8:
    case kSparcExtendSignWord16: {
      const int shift = opcode == kSparcExtendSignWord8 ? 56 : 48;
      __ sllx(i.OutputRegister(), i.InputRegister(0), shift);
      __ srax(i.OutputRegister(), i.OutputRegister(), shift);
      break;
    }
    case kSparcExtendSignWord32:
    case kSparcInt64ToInt32:
      __ sra(i.OutputRegister(), i.InputRegister(0), 0);
      break;
    case kSparcUint32ToUint64:
      __ srl(i.OutputRegister(), i.InputRegister(0), 0);
      break;
    case kSparcFloat32ToInt32:
    case kSparcFloat32ToUint32: {
      const bool is_unsigned = opcode == kSparcFloat32ToUint32;
      EmitSparc64FloatToInteger(masm(), i.OutputRegister(), g4,
                                i.InputDoubleRegister(0), true, is_unsigned,
                                true);
      if (MiscField::decode(instr->opcode()) && !is_unsigned) {
        Label valid;
        __ cmp(g4, 0);
        __ b(not_equal, ConditionCodes::kIcc, &valid);
        __ nop();
        __ Move(i.OutputRegister(), int32_t{INT32_MIN});
        __ bind(&valid);
      }
      break;
    }
    case kSparcDoubleToInt32:
    case kSparcDoubleToUint32:
    case kSparcDoubleToInt64:
    case kSparcDoubleToUint64:
      EmitSparc64FloatToInteger(
          masm(), i.OutputRegister(0),
          instr->OutputCount() > 1 ? i.OutputRegister(1) : no_reg,
          i.InputDoubleRegister(0),
          opcode == kSparcDoubleToInt32 || opcode == kSparcDoubleToUint32,
          opcode == kSparcDoubleToUint32 || opcode == kSparcDoubleToUint64,
          single);
      break;
    case kSparcInt32ToFloat32:
    case kSparcUint32ToFloat32:
    case kSparcInt64ToFloat32:
    case kSparcUint64ToFloat32:
    case kSparcInt32ToDouble:
    case kSparcUint32ToDouble:
    case kSparcInt64ToDouble:
    case kSparcUint64ToDouble:
      EmitSparc64IntegerToFloat(
          masm(), i.OutputDoubleRegister(), i.InputRegister(0),
          opcode == kSparcInt32ToFloat32 || opcode == kSparcUint32ToFloat32 ||
              opcode == kSparcInt32ToDouble || opcode == kSparcUint32ToDouble,
          opcode == kSparcUint32ToFloat32 || opcode == kSparcUint64ToFloat32 ||
              opcode == kSparcUint32ToDouble || opcode == kSparcUint64ToDouble,
          opcode == kSparcInt32ToFloat32 || opcode == kSparcUint32ToFloat32 ||
              opcode == kSparcInt64ToFloat32 ||
              opcode == kSparcUint64ToFloat32);
      break;
      FLOAT_BINARY(kSparcAddDouble, fadd)
      FLOAT_BINARY(kSparcSubDouble, fsub)
      FLOAT_BINARY(kSparcMulDouble, fmul)
      FLOAT_BINARY(kSparcDivDouble, fdiv)
    case kSparcMinDouble:
    case kSparcMaxDouble:
      EmitSparc64FloatMinMax(masm(), i.OutputDoubleRegister(),
                             i.InputDoubleRegister(0), i.InputDoubleRegister(1),
                             single, opcode == kSparcMinDouble);
      break;
    case kSparcModDouble:
      call_float(ExternalReference::mod_two_doubles_operation(), 2);
      break;
#define IEEE754_CALL(op, name, argc)                                  \
  case kIeee754Float64##op:                                           \
    call_float(ExternalReference::ieee754_##name##_function(), argc); \
    break;
      IEEE754_CALL(Acos, acos, 1)
      IEEE754_CALL(Acosh, acosh, 1)
      IEEE754_CALL(Asin, asin, 1)
      IEEE754_CALL(Asinh, asinh, 1)
      IEEE754_CALL(Atan, atan, 1)
      IEEE754_CALL(Atanh, atanh, 1)
      IEEE754_CALL(Atan2, atan2, 2)
      IEEE754_CALL(Cbrt, cbrt, 1)
      IEEE754_CALL(Cos, cos, 1)
      IEEE754_CALL(Cosh, cosh, 1)
      IEEE754_CALL(Exp, exp, 1)
      IEEE754_CALL(Expm1, expm1, 1)
      IEEE754_CALL(Log, log, 1)
      IEEE754_CALL(Log1p, log1p, 1)
      IEEE754_CALL(Log10, log10, 1)
      IEEE754_CALL(Log2, log2, 1)
      IEEE754_CALL(Pow, pow, 2)
      IEEE754_CALL(Sin, sin, 1)
      IEEE754_CALL(Sinh, sinh, 1)
      IEEE754_CALL(Tan, tan, 1)
      IEEE754_CALL(Tanh, tanh, 1)
#undef IEEE754_CALL
      FLOAT_UNARY(kSparcNegDouble, fnegs, fnegd)
      FLOAT_UNARY(kSparcAbsDouble, fabss, fabsd)
    case kSparcSqrtDouble:
      if (single) {
        __ fsqrts(i.OutputFloatRegister(), i.InputFloatRegister(0));
      } else {
        __ fsqrtd(i.OutputDoubleRegister(), i.InputDoubleRegister(0));
      }
      sparc64::CanonicalizeNaN(
          masm(), single ? i.OutputFloatRegister() : i.OutputDoubleRegister(),
          single);
      break;
    case kSparcCmpDouble:
      if (single) {
        __ fcmps(i.InputFloatRegister(0), i.InputFloatRegister(1));
      } else {
        __ fcmpd(i.InputDoubleRegister(0), i.InputDoubleRegister(1));
      }
      break;
    case kSparcFloat64SilenceNaN:
      EmitSparc64SilenceNaN(masm(), i.OutputDoubleRegister(),
                            i.InputDoubleRegister(0));
      break;
    case kSparcDoubleToFloat32:
      __ fdtos(i.OutputFloatRegister(), i.InputDoubleRegister(0));
      break;
    case kSparcFloat32ToDouble:
      __ fstod(i.OutputDoubleRegister(), i.InputFloatRegister(0));
      break;
    case kSparcBitcastInt32ToFloat32:
      __ MovIntToFloat(i.OutputFloatRegister(), i.InputRegister(0));
      break;
    case kSparcBitcastFloat32ToInt32:
      __ MovFloatToInt(i.OutputRegister(), i.InputFloatRegister(0));
      break;
    case kSparcBitcastInt64ToDouble:
      __ MovInt64ToDouble(i.OutputDoubleRegister(), i.InputRegister(0));
      break;
    case kSparcBitcastDoubleToInt64:
      __ MovDoubleToInt64(i.OutputRegister(), i.InputDoubleRegister(0));
      break;
    case kSparcDoubleExtractLowWord32:
    case kSparcDoubleExtractHighWord32:
      EmitSparc64ExtractDoubleWord(masm(), i.OutputRegister(),
                                   i.InputDoubleRegister(0),
                                   opcode == kSparcDoubleExtractHighWord32);
      break;
    case kSparcDoubleFromWord32Pair:
      EmitSparc64DoubleFromWord32Pair(masm(), i.OutputDoubleRegister(),
                                      i.InputRegister(0), i.InputRegister(1));
      break;
    case kSparcRotRight32:
    case kSparcRotRight64:
      EmitSparc64RotateRight(masm(), i.OutputRegister(), i.InputRegister(0),
                             right(), opcode == kSparcRotRight32);
      set_result_flags();
      break;
    case kSparcByteRev32:
    case kSparcByteRev64:
      EmitSparc64ReverseBytes(masm(), i.OutputRegister(), i.InputRegister(0),
                              opcode == kSparcByteRev32 ? 4 : 8);
      break;
    case kSparcLoadByteRev32:
    case kSparcLoadByteRev64: {
      size_t next = 0;
      MemOperand mem = i.MemoryOperand(&next);
      bool is_atomic = i.InputInt32(next);
      int bytes = opcode == kSparcLoadByteRev32 ? 4 : 8;
      if (is_atomic) {
        __ membar(kSync);
        if (bytes == 4) {
          __ LoadU32(g5, mem);
        } else {
          __ LoadU64(g5, mem);
        }
      } else {
        __ LoadUnaligned(g5, mem, bytes);
      }
      EmitSparc64ReverseBytes(masm(), i.OutputRegister(), g5, bytes);
      if (is_atomic) __ membar(kSync);
      break;
    }
    case kSparcStoreByteRev32:
    case kSparcStoreByteRev64: {
      size_t next = 0;
      MemOperand mem = i.MemoryOperand(&next);
      Register value = i.InputRegister(next++);
      bool is_atomic = i.InputInt32(next);
      int bytes = opcode == kSparcStoreByteRev32 ? 4 : 8;
      EmitSparc64ReverseBytes(masm(), g5, value, bytes);
      if (is_atomic) {
        __ membar(kSync);
        if (bytes == 4) {
          __ StoreU32(g5, mem);
        } else {
          __ StoreU64(g5, mem);
        }
        __ membar(kSync);
      } else {
        __ StoreUnaligned(g5, mem, bytes);
      }
      break;
    }
      INTEGER_LOAD(kSparcLoadWordS8, LoadS8, 1, true)
      INTEGER_LOAD(kSparcLoadWordU8, LoadU8, 1, false)
      INTEGER_LOAD(kSparcLoadWordS16, LoadS16, 2, true)
      INTEGER_LOAD(kSparcLoadWordU16, LoadU16, 2, false)
      INTEGER_LOAD(kSparcLoadWordS32, LoadS32, 4, true)
      INTEGER_LOAD(kSparcLoadWordU32, LoadU32, 4, false)
      INTEGER_LOAD(kSparcLoadWord64, LoadU64, 8, false)
      INTEGER_STORE(kSparcStoreWord8, StoreU8, 1)
      INTEGER_STORE(kSparcStoreWord16, StoreU16, 2)
      INTEGER_STORE(kSparcStoreWord32, StoreU32, 4)
      INTEGER_STORE(kSparcStoreWord64, StoreU64, 8)
      INTEGER_STORE(kSparcStoreCompressTagged, StoreU64, 8)
    case kSparcLoadFloat32:
    case kSparcLoadDouble: {
      size_t next = 0;
      MemOperand mem = i.MemoryOperand(&next);
      CHECK_EQ(i.InputInt32(next), 0);
      if (single) {
        const int bytes = opcode == kSparcLoadFloat32 ? 4 : 8;
        __ LoadUnaligned(g5, mem, bytes);
        if (bytes == 4) {
          __ MovIntToFloat(i.OutputFloatRegister(), g5);
        } else {
          __ MovInt64ToDouble(i.OutputDoubleRegister(), g5);
        }
      } else if (opcode == kSparcLoadFloat32) {
        __ LoadF32(i.OutputFloatRegister(), mem);
      } else {
        __ LoadF64(i.OutputDoubleRegister(), mem);
      }
      break;
    }
    case kSparcStoreFloat32:
    case kSparcStoreDouble: {
      size_t next = 0;
      MemOperand mem = i.MemoryOperand(&next);
      DoubleRegister value = i.InputDoubleRegister(next++);
      CHECK_EQ(i.InputInt32(next), 0);
      if (single) {
        const int bytes = opcode == kSparcStoreFloat32 ? 4 : 8;
        if (bytes == 4) {
          __ MovFloatToInt(g5, value);
        } else {
          __ MovDoubleToInt64(g5, value);
        }
        __ StoreUnaligned(g5, mem, bytes);
      } else if (opcode == kSparcStoreFloat32) {
        __ StoreF32(value, mem);
      } else {
        __ StoreF64(value, mem);
      }
      break;
    }
    case kSparcPeek: {
      int reverse_slot = i.InputInt32(0);
      int offset =
          FrameSlotToFPOffset(frame()->GetTotalFrameSlotCount() - reverse_slot);
      auto representation =
          LocationOperand::cast(instr->OutputAt(0))->representation();
      if (representation == MachineRepresentation::kFloat32) {
        __ LoadF32(i.OutputFloatRegister(), MemOperand(fp, offset));
      } else if (representation == MachineRepresentation::kFloat64) {
        __ LoadF64(i.OutputDoubleRegister(), MemOperand(fp, offset));
      } else if (representation == MachineRepresentation::kSimd128) {
        sparc64::LoadSimd(masm(), i.OutputSimd128Register(),
                          MemOperand(fp, offset));
      } else if (representation == MachineRepresentation::kWord32) {
        __ LoadU32(i.OutputRegister(), MemOperand(fp, offset + 4));
      } else {
        CHECK_NE(representation, MachineRepresentation::kSimd128);
        __ LoadU64(i.OutputRegister(), MemOperand(fp, offset));
      }
      break;
    }
    case kSparcPush: {
      int slots = i.InputInt32(0) / kSystemPointerSize;
      __ AllocateStackSpace(slots * kSystemPointerSize);
      frame_access_state()->IncreaseSPDelta(slots);
      if (instr->InputAt(1)->IsSimd128Register()) {
        sparc64::StoreSimd(masm(), i.InputSimd128Register(1), MemOperand(sp));
      } else if (instr->InputAt(1)->IsFloatRegister()) {
        __ StoreF32(i.InputFloatRegister(1), MemOperand(sp));
      } else if (instr->InputAt(1)->IsDoubleRegister()) {
        __ StoreF64(i.InputDoubleRegister(1), MemOperand(sp));
      } else {
        __ StoreU64(i.InputRegister(1), MemOperand(sp));
      }
      break;
    }
    case kSparcStoreToStackSlot: {
      MemOperand mem(sp, i.InputInt32(1) * kSystemPointerSize);
      if (instr->InputAt(0)->IsSimd128Register()) {
        sparc64::StoreSimd(masm(), i.InputSimd128Register(0), mem);
      } else if (instr->InputAt(0)->IsFloatRegister()) {
        __ StoreF32(i.InputFloatRegister(0), mem);
      } else if (instr->InputAt(0)->IsDoubleRegister()) {
        __ StoreF64(i.InputDoubleRegister(0), mem);
      } else {
        Register src = i.InputRegister(0);
        if (instr->InputCount() == 3) {
          unsigned extension = i.InputUint32(2);
          if (extension) {
            int bits = 8 << ((extension - 1) / 2);
            __ sllx(g1, src, 64 - bits);
            if (extension & 1) {
              __ srax(g1, g1, 64 - bits);
            } else {
              __ srlx(g1, g1, 64 - bits);
            }
            src = g1;
          }
        }
        __ StoreU64(src, mem);
      }
      break;
    }
#define SIMD_BINARY_OP(opcode, emitter)                              \
  case k##opcode:                                                    \
    sparc64::SimdEmitter(masm()).emitter(i.OutputSimd128Register(),  \
                                         i.InputSimd128Register(0),  \
                                         i.InputSimd128Register(1)); \
    break;
      SIMD_BINARY_OP(SparcF64x2Add, emit_f64x2_add)
      SIMD_BINARY_OP(SparcF64x2Sub, emit_f64x2_sub)
      SIMD_BINARY_OP(SparcF64x2Mul, emit_f64x2_mul)
      SIMD_BINARY_OP(SparcF64x2Eq, emit_f64x2_eq)
      SIMD_BINARY_OP(SparcF64x2Ne, emit_f64x2_ne)
      SIMD_BINARY_OP(SparcF64x2Le, emit_f64x2_le)
      SIMD_BINARY_OP(SparcF64x2Lt, emit_f64x2_lt)
      SIMD_BINARY_OP(SparcF64x2Div, emit_f64x2_div)
      SIMD_BINARY_OP(SparcF64x2Min, emit_f64x2_min)
      SIMD_BINARY_OP(SparcF64x2Max, emit_f64x2_max)
      SIMD_BINARY_OP(SparcF64x2Pmin, emit_f64x2_pmin)
      SIMD_BINARY_OP(SparcF64x2Pmax, emit_f64x2_pmax)
      SIMD_BINARY_OP(SparcF32x4Add, emit_f32x4_add)
      SIMD_BINARY_OP(SparcF32x4Sub, emit_f32x4_sub)
      SIMD_BINARY_OP(SparcF32x4Mul, emit_f32x4_mul)
      SIMD_BINARY_OP(SparcF32x4Eq, emit_f32x4_eq)
      SIMD_BINARY_OP(SparcF32x4Ne, emit_f32x4_ne)
      SIMD_BINARY_OP(SparcF32x4Lt, emit_f32x4_lt)
      SIMD_BINARY_OP(SparcF32x4Le, emit_f32x4_le)
      SIMD_BINARY_OP(SparcF32x4Div, emit_f32x4_div)
      SIMD_BINARY_OP(SparcF32x4Min, emit_f32x4_min)
      SIMD_BINARY_OP(SparcF32x4Max, emit_f32x4_max)
      SIMD_BINARY_OP(SparcF32x4Pmin, emit_f32x4_pmin)
      SIMD_BINARY_OP(SparcF32x4Pmax, emit_f32x4_pmax)
      SIMD_BINARY_OP(SparcI64x2Add, emit_i64x2_add)
      SIMD_BINARY_OP(SparcI64x2Sub, emit_i64x2_sub)
      SIMD_BINARY_OP(SparcI64x2Mul, emit_i64x2_mul)
      SIMD_BINARY_OP(SparcI64x2Eq, emit_i64x2_eq)
      SIMD_BINARY_OP(SparcI64x2Ne, emit_i64x2_ne)
      SIMD_BINARY_OP(SparcI64x2GtS, emit_i64x2_gt_s)
      SIMD_BINARY_OP(SparcI64x2GeS, emit_i64x2_ge_s)
      SIMD_BINARY_OP(SparcI64x2ExtMulLowI32x4S, emit_i64x2_extmul_low_i32x4_s)
      SIMD_BINARY_OP(SparcI64x2ExtMulHighI32x4S, emit_i64x2_extmul_high_i32x4_s)
      SIMD_BINARY_OP(SparcI64x2ExtMulLowI32x4U, emit_i64x2_extmul_low_i32x4_u)
      SIMD_BINARY_OP(SparcI64x2ExtMulHighI32x4U, emit_i64x2_extmul_high_i32x4_u)
      SIMD_BINARY_OP(SparcI32x4Add, emit_i32x4_add)
      SIMD_BINARY_OP(SparcI32x4Sub, emit_i32x4_sub)
      SIMD_BINARY_OP(SparcI32x4Mul, emit_i32x4_mul)
      SIMD_BINARY_OP(SparcI32x4MinS, emit_i32x4_min_s)
      SIMD_BINARY_OP(SparcI32x4MinU, emit_i32x4_min_u)
      SIMD_BINARY_OP(SparcI32x4MaxS, emit_i32x4_max_s)
      SIMD_BINARY_OP(SparcI32x4MaxU, emit_i32x4_max_u)
      SIMD_BINARY_OP(SparcI32x4Eq, emit_i32x4_eq)
      SIMD_BINARY_OP(SparcI32x4Ne, emit_i32x4_ne)
      SIMD_BINARY_OP(SparcI32x4GtS, emit_i32x4_gt_s)
      SIMD_BINARY_OP(SparcI32x4GeS, emit_i32x4_ge_s)
      SIMD_BINARY_OP(SparcI32x4GtU, emit_i32x4_gt_u)
      SIMD_BINARY_OP(SparcI32x4GeU, emit_i32x4_ge_u)
      SIMD_BINARY_OP(SparcI32x4DotI16x8S, emit_i32x4_dot_i16x8_s)
      SIMD_BINARY_OP(SparcI32x4ExtMulLowI16x8S, emit_i32x4_extmul_low_i16x8_s)
      SIMD_BINARY_OP(SparcI32x4ExtMulHighI16x8S, emit_i32x4_extmul_high_i16x8_s)
      SIMD_BINARY_OP(SparcI32x4ExtMulLowI16x8U, emit_i32x4_extmul_low_i16x8_u)
      SIMD_BINARY_OP(SparcI32x4ExtMulHighI16x8U, emit_i32x4_extmul_high_i16x8_u)
      SIMD_BINARY_OP(SparcI16x8Add, emit_i16x8_add)
      SIMD_BINARY_OP(SparcI16x8Sub, emit_i16x8_sub)
      SIMD_BINARY_OP(SparcI16x8Mul, emit_i16x8_mul)
      SIMD_BINARY_OP(SparcI16x8MinS, emit_i16x8_min_s)
      SIMD_BINARY_OP(SparcI16x8MinU, emit_i16x8_min_u)
      SIMD_BINARY_OP(SparcI16x8MaxS, emit_i16x8_max_s)
      SIMD_BINARY_OP(SparcI16x8MaxU, emit_i16x8_max_u)
      SIMD_BINARY_OP(SparcI16x8Eq, emit_i16x8_eq)
      SIMD_BINARY_OP(SparcI16x8Ne, emit_i16x8_ne)
      SIMD_BINARY_OP(SparcI16x8GtS, emit_i16x8_gt_s)
      SIMD_BINARY_OP(SparcI16x8GeS, emit_i16x8_ge_s)
      SIMD_BINARY_OP(SparcI16x8GtU, emit_i16x8_gt_u)
      SIMD_BINARY_OP(SparcI16x8GeU, emit_i16x8_ge_u)
      SIMD_BINARY_OP(SparcI16x8SConvertI32x4, emit_i16x8_sconvert_i32x4)
      SIMD_BINARY_OP(SparcI16x8UConvertI32x4, emit_i16x8_uconvert_i32x4)
      SIMD_BINARY_OP(SparcI16x8AddSatS, emit_i16x8_add_sat_s)
      SIMD_BINARY_OP(SparcI16x8SubSatS, emit_i16x8_sub_sat_s)
      SIMD_BINARY_OP(SparcI16x8AddSatU, emit_i16x8_add_sat_u)
      SIMD_BINARY_OP(SparcI16x8SubSatU, emit_i16x8_sub_sat_u)
      SIMD_BINARY_OP(SparcI16x8RoundingAverageU, emit_i16x8_rounding_average_u)
      SIMD_BINARY_OP(SparcI16x8Q15MulRSatS, emit_i16x8_q15mulr_sat_s)
      SIMD_BINARY_OP(SparcI16x8ExtMulLowI8x16S, emit_i16x8_extmul_low_i8x16_s)
      SIMD_BINARY_OP(SparcI16x8ExtMulHighI8x16S, emit_i16x8_extmul_high_i8x16_s)
      SIMD_BINARY_OP(SparcI16x8ExtMulLowI8x16U, emit_i16x8_extmul_low_i8x16_u)
      SIMD_BINARY_OP(SparcI16x8ExtMulHighI8x16U, emit_i16x8_extmul_high_i8x16_u)
      SIMD_BINARY_OP(SparcI16x8DotI8x16S, emit_i16x8_dot_i8x16_i7x16_s)
      SIMD_BINARY_OP(SparcI8x16Add, emit_i8x16_add)
      SIMD_BINARY_OP(SparcI8x16Sub, emit_i8x16_sub)
      SIMD_BINARY_OP(SparcI8x16MinS, emit_i8x16_min_s)
      SIMD_BINARY_OP(SparcI8x16MinU, emit_i8x16_min_u)
      SIMD_BINARY_OP(SparcI8x16MaxS, emit_i8x16_max_s)
      SIMD_BINARY_OP(SparcI8x16MaxU, emit_i8x16_max_u)
      SIMD_BINARY_OP(SparcI8x16Eq, emit_i8x16_eq)
      SIMD_BINARY_OP(SparcI8x16Ne, emit_i8x16_ne)
      SIMD_BINARY_OP(SparcI8x16GtS, emit_i8x16_gt_s)
      SIMD_BINARY_OP(SparcI8x16GeS, emit_i8x16_ge_s)
      SIMD_BINARY_OP(SparcI8x16GtU, emit_i8x16_gt_u)
      SIMD_BINARY_OP(SparcI8x16GeU, emit_i8x16_ge_u)
      SIMD_BINARY_OP(SparcI8x16SConvertI16x8, emit_i8x16_sconvert_i16x8)
      SIMD_BINARY_OP(SparcI8x16UConvertI16x8, emit_i8x16_uconvert_i16x8)
      SIMD_BINARY_OP(SparcI8x16AddSatS, emit_i8x16_add_sat_s)
      SIMD_BINARY_OP(SparcI8x16SubSatS, emit_i8x16_sub_sat_s)
      SIMD_BINARY_OP(SparcI8x16AddSatU, emit_i8x16_add_sat_u)
      SIMD_BINARY_OP(SparcI8x16SubSatU, emit_i8x16_sub_sat_u)
      SIMD_BINARY_OP(SparcI8x16RoundingAverageU, emit_i8x16_rounding_average_u)
      SIMD_BINARY_OP(SparcI8x16Swizzle, emit_i8x16_swizzle)
      SIMD_BINARY_OP(SparcS128And, emit_s128_and)
      SIMD_BINARY_OP(SparcS128Or, emit_s128_or)
      SIMD_BINARY_OP(SparcS128Xor, emit_s128_xor)
      SIMD_BINARY_OP(SparcS128AndNot, emit_s128_and_not)
#undef SIMD_BINARY_OP
#define SIMD_UNARY_OP(opcode, emitter)                               \
  case k##opcode:                                                    \
    sparc64::SimdEmitter(masm()).emitter(i.OutputSimd128Register(),  \
                                         i.InputSimd128Register(0)); \
    break;
      SIMD_UNARY_OP(SparcF64x2Abs, emit_f64x2_abs)
      SIMD_UNARY_OP(SparcF64x2Neg, emit_f64x2_neg)
      SIMD_UNARY_OP(SparcF64x2Sqrt, emit_f64x2_sqrt)
      SIMD_UNARY_OP(SparcF64x2ConvertLowI32x4S, emit_f64x2_convert_low_i32x4_s)
      SIMD_UNARY_OP(SparcF64x2ConvertLowI32x4U, emit_f64x2_convert_low_i32x4_u)
      SIMD_UNARY_OP(SparcF64x2PromoteLowF32x4, emit_f64x2_promote_low_f32x4)
      SIMD_UNARY_OP(SparcF32x4Abs, emit_f32x4_abs)
      SIMD_UNARY_OP(SparcF32x4Neg, emit_f32x4_neg)
      SIMD_UNARY_OP(SparcF32x4Sqrt, emit_f32x4_sqrt)
      SIMD_UNARY_OP(SparcF32x4SConvertI32x4, emit_f32x4_sconvert_i32x4)
      SIMD_UNARY_OP(SparcF32x4UConvertI32x4, emit_f32x4_uconvert_i32x4)
      SIMD_UNARY_OP(SparcF32x4DemoteF64x2Zero, emit_f32x4_demote_f64x2_zero)
      SIMD_UNARY_OP(SparcI64x2Neg, emit_i64x2_neg)
      SIMD_UNARY_OP(SparcI64x2SConvertI32x4Low, emit_i64x2_sconvert_i32x4_low)
      SIMD_UNARY_OP(SparcI64x2SConvertI32x4High, emit_i64x2_sconvert_i32x4_high)
      SIMD_UNARY_OP(SparcI64x2UConvertI32x4Low, emit_i64x2_uconvert_i32x4_low)
      SIMD_UNARY_OP(SparcI64x2UConvertI32x4High, emit_i64x2_uconvert_i32x4_high)
      SIMD_UNARY_OP(SparcI64x2Abs, emit_i64x2_abs)
      SIMD_UNARY_OP(SparcI32x4Neg, emit_i32x4_neg)
      SIMD_UNARY_OP(SparcI32x4Abs, emit_i32x4_abs)
      SIMD_UNARY_OP(SparcI32x4SConvertF32x4, emit_i32x4_sconvert_f32x4)
      SIMD_UNARY_OP(SparcI32x4UConvertF32x4, emit_i32x4_uconvert_f32x4)
      SIMD_UNARY_OP(SparcI32x4SConvertI16x8Low, emit_i32x4_sconvert_i16x8_low)
      SIMD_UNARY_OP(SparcI32x4SConvertI16x8High, emit_i32x4_sconvert_i16x8_high)
      SIMD_UNARY_OP(SparcI32x4UConvertI16x8Low, emit_i32x4_uconvert_i16x8_low)
      SIMD_UNARY_OP(SparcI32x4UConvertI16x8High, emit_i32x4_uconvert_i16x8_high)
      SIMD_UNARY_OP(SparcI32x4ExtAddPairwiseI16x8S,
                    emit_i32x4_extadd_pairwise_i16x8_s)
      SIMD_UNARY_OP(SparcI32x4ExtAddPairwiseI16x8U,
                    emit_i32x4_extadd_pairwise_i16x8_u)
      SIMD_UNARY_OP(SparcI32x4TruncSatF64x2SZero,
                    emit_i32x4_trunc_sat_f64x2_s_zero)
      SIMD_UNARY_OP(SparcI32x4TruncSatF64x2UZero,
                    emit_i32x4_trunc_sat_f64x2_u_zero)
      SIMD_UNARY_OP(SparcI16x8Neg, emit_i16x8_neg)
      SIMD_UNARY_OP(SparcI16x8Abs, emit_i16x8_abs)
      SIMD_UNARY_OP(SparcI16x8SConvertI8x16Low, emit_i16x8_sconvert_i8x16_low)
      SIMD_UNARY_OP(SparcI16x8SConvertI8x16High, emit_i16x8_sconvert_i8x16_high)
      SIMD_UNARY_OP(SparcI16x8UConvertI8x16Low, emit_i16x8_uconvert_i8x16_low)
      SIMD_UNARY_OP(SparcI16x8UConvertI8x16High, emit_i16x8_uconvert_i8x16_high)
      SIMD_UNARY_OP(SparcI16x8ExtAddPairwiseI8x16S,
                    emit_i16x8_extadd_pairwise_i8x16_s)
      SIMD_UNARY_OP(SparcI16x8ExtAddPairwiseI8x16U,
                    emit_i16x8_extadd_pairwise_i8x16_u)
      SIMD_UNARY_OP(SparcI8x16Neg, emit_i8x16_neg)
      SIMD_UNARY_OP(SparcI8x16Abs, emit_i8x16_abs)
      SIMD_UNARY_OP(SparcI8x16Popcnt, emit_i8x16_popcnt)
      SIMD_UNARY_OP(SparcS128Not, emit_s128_not)
#undef SIMD_UNARY_OP
#define SIMD_TERNARY_OP(opcode, emitter)                       \
  case k##opcode:                                              \
    sparc64::SimdEmitter(masm()).emitter(                      \
        i.OutputSimd128Register(), i.InputSimd128Register(0),  \
        i.InputSimd128Register(1), i.InputSimd128Register(2)); \
    break;
      SIMD_TERNARY_OP(SparcF64x2Qfma, emit_f64x2_qfma)
      SIMD_TERNARY_OP(SparcF64x2Qfms, emit_f64x2_qfms)
      SIMD_TERNARY_OP(SparcF32x4Qfma, emit_f32x4_qfma)
      SIMD_TERNARY_OP(SparcF32x4Qfms, emit_f32x4_qfms)
      SIMD_TERNARY_OP(SparcI32x4DotI8x16AddS, emit_i32x4_dot_i8x16_i7x16_add_s)
    case kSparcS128Select:
      sparc64::SimdEmitter(masm()).emit_s128_select(
          i.OutputSimd128Register(), i.InputSimd128Register(1),
          i.InputSimd128Register(2), i.InputSimd128Register(0));
      break;
#undef SIMD_TERNARY_OP
#define SIMD_SHIFT_OP(opcode, emitter)                              \
  case k##opcode:                                                   \
    sparc64::SimdEmitter(masm()).emitter(i.OutputSimd128Register(), \
                                         i.InputSimd128Register(0), \
                                         i.InputRegister(1));       \
    break;
      SIMD_SHIFT_OP(SparcI64x2Shl, emit_i64x2_shl)
      SIMD_SHIFT_OP(SparcI64x2ShrS, emit_i64x2_shr_s)
      SIMD_SHIFT_OP(SparcI64x2ShrU, emit_i64x2_shr_u)
      SIMD_SHIFT_OP(SparcI32x4Shl, emit_i32x4_shl)
      SIMD_SHIFT_OP(SparcI32x4ShrS, emit_i32x4_shr_s)
      SIMD_SHIFT_OP(SparcI32x4ShrU, emit_i32x4_shr_u)
      SIMD_SHIFT_OP(SparcI16x8Shl, emit_i16x8_shl)
      SIMD_SHIFT_OP(SparcI16x8ShrS, emit_i16x8_shr_s)
      SIMD_SHIFT_OP(SparcI16x8ShrU, emit_i16x8_shr_u)
      SIMD_SHIFT_OP(SparcI8x16Shl, emit_i8x16_shl)
      SIMD_SHIFT_OP(SparcI8x16ShrS, emit_i8x16_shr_s)
      SIMD_SHIFT_OP(SparcI8x16ShrU, emit_i8x16_shr_u)
#undef SIMD_SHIFT_OP
#define SIMD_TEST_OP(opcode, emitter)                                \
  case k##opcode:                                                    \
    sparc64::SimdEmitter(masm()).emitter(i.OutputRegister(),         \
                                         i.InputSimd128Register(0)); \
    break;
      SIMD_TEST_OP(SparcI64x2BitMask, emit_i64x2_bitmask)
      SIMD_TEST_OP(SparcI32x4BitMask, emit_i32x4_bitmask)
      SIMD_TEST_OP(SparcI16x8BitMask, emit_i16x8_bitmask)
      SIMD_TEST_OP(SparcI8x16BitMask, emit_i8x16_bitmask)
      SIMD_TEST_OP(SparcI64x2AllTrue, emit_i64x2_alltrue)
      SIMD_TEST_OP(SparcI32x4AllTrue, emit_i32x4_alltrue)
      SIMD_TEST_OP(SparcI16x8AllTrue, emit_i16x8_alltrue)
      SIMD_TEST_OP(SparcI8x16AllTrue, emit_i8x16_alltrue)
      SIMD_TEST_OP(SparcV128AnyTrue, emit_v128_anytrue)
#undef SIMD_TEST_OP
    case kSparcISplat:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 8:
          sparc64::SimdEmitter(masm()).emit_i8x16_splat(
              i.OutputSimd128Register(), i.InputRegister(0));
          break;
        case 16:
          sparc64::SimdEmitter(masm()).emit_i16x8_splat(
              i.OutputSimd128Register(), i.InputRegister(0));
          break;
        case 32:
          sparc64::SimdEmitter(masm()).emit_i32x4_splat(
              i.OutputSimd128Register(), i.InputRegister(0));
          break;
        case 64:
          sparc64::SimdEmitter(masm()).emit_i64x2_splat(
              i.OutputSimd128Register(), i.InputRegister(0));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcFSplat:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 32:
          sparc64::SimdEmitter(masm()).emit_f32x4_splat(
              i.OutputSimd128Register(), i.InputFloatRegister(0));
          break;
        case 64:
          sparc64::SimdEmitter(masm()).emit_f64x2_splat(
              i.OutputSimd128Register(), i.InputDoubleRegister(0));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcIExtractLane:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 32:
          sparc64::SimdEmitter(masm()).emit_i32x4_extract_lane(
              i.OutputRegister(), i.InputSimd128Register(0), i.InputUint8(1));
          break;
        case 64:
          sparc64::SimdEmitter(masm()).emit_i64x2_extract_lane(
              i.OutputRegister(), i.InputSimd128Register(0), i.InputUint8(1));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcIExtractLaneS:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 8:
          sparc64::SimdEmitter(masm()).emit_i8x16_extract_lane_s(
              i.OutputRegister(), i.InputSimd128Register(0), i.InputUint8(1));
          break;
        case 16:
          sparc64::SimdEmitter(masm()).emit_i16x8_extract_lane_s(
              i.OutputRegister(), i.InputSimd128Register(0), i.InputUint8(1));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcIExtractLaneU:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 8:
          sparc64::SimdEmitter(masm()).emit_i8x16_extract_lane_u(
              i.OutputRegister(), i.InputSimd128Register(0), i.InputUint8(1));
          break;
        case 16:
          sparc64::SimdEmitter(masm()).emit_i16x8_extract_lane_u(
              i.OutputRegister(), i.InputSimd128Register(0), i.InputUint8(1));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcFExtractLane:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 32:
          sparc64::SimdEmitter(masm()).emit_f32x4_extract_lane(
              i.OutputFloatRegister(), i.InputSimd128Register(0),
              i.InputUint8(1));
          break;
        case 64:
          sparc64::SimdEmitter(masm()).emit_f64x2_extract_lane(
              i.OutputDoubleRegister(), i.InputSimd128Register(0),
              i.InputUint8(1));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcIReplaceLane:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 8:
          sparc64::SimdEmitter(masm()).emit_i8x16_replace_lane(
              i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputRegister(2), i.InputUint8(1));
          break;
        case 16:
          sparc64::SimdEmitter(masm()).emit_i16x8_replace_lane(
              i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputRegister(2), i.InputUint8(1));
          break;
        case 32:
          sparc64::SimdEmitter(masm()).emit_i32x4_replace_lane(
              i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputRegister(2), i.InputUint8(1));
          break;
        case 64:
          sparc64::SimdEmitter(masm()).emit_i64x2_replace_lane(
              i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputRegister(2), i.InputUint8(1));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcFReplaceLane:
      switch (LaneSizeField::decode(instr->opcode())) {
        case 32:
          sparc64::SimdEmitter(masm()).emit_f32x4_replace_lane(
              i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputFloatRegister(2), i.InputUint8(1));
          break;
        case 64:
          sparc64::SimdEmitter(masm()).emit_f64x2_replace_lane(
              i.OutputSimd128Register(), i.InputSimd128Register(0),
              i.InputDoubleRegister(2), i.InputUint8(1));
          break;
        default:
          UNREACHABLE();
      }
      break;
    case kSparcS128Const:
    case kSparcS128Zero:
    case kSparcS128AllOnes: {
      uint8_t bytes[16];
      for (int n = 0; n < 16; ++n) {
        bytes[n] = opcode == kSparcS128Const
                       ? i.InputUint32(n / 4) >> (8 * (n % 4))
                   : opcode == kSparcS128AllOnes ? 0xff
                                                 : 0;
      }
      sparc64::SimdEmitter(masm()).emit_s128_const(i.OutputSimd128Register(),
                                                   bytes);
      break;
    }
    case kSparcI8x16Shuffle: {
      uint8_t shuffle[16];
      for (int n = 0; n < 16; ++n) {
        shuffle[n] = i.InputUint32(2 + n / 4) >> (8 * (n % 4));
      }
      sparc64::SimdEmitter(masm()).emit_i8x16_shuffle(
          i.OutputSimd128Register(), i.InputSimd128Register(0),
          i.InputSimd128Register(1), shuffle, false);
      break;
    }
    case kSparcLoadReverseSimd128RR: {
      DoubleRegister src = i.InputSimd128Register(0);
      __ MovDoubleToInt64(g1, sparc64::SimdHalf(src, 0));
      __ MovDoubleToInt64(g2, sparc64::SimdHalf(src, 1));
      EmitSparc64ByteReverse(masm(), g1, g1, g3, g4, 8);
      EmitSparc64ByteReverse(masm(), g2, g2, g3, g4, 8);
      __ MovInt64ToDouble(sparc64::SimdHalf(i.OutputSimd128Register(), 1), g1);
      __ MovInt64ToDouble(sparc64::SimdHalf(i.OutputSimd128Register(), 0), g2);
      break;
    }
    case kSparcLoadSimd128: {
      size_t next = 0;
      MemOperand mem = i.MemoryOperand(&next);
      CHECK_EQ(i.InputInt32(next), 0);
      __ LoadAddress(g4, mem);
      sparc64::SimdEmitter(masm()).LoadVector(i.OutputSimd128Register(), g4,
                                              no_reg, 0, false, true, false,
                                              nullptr);
      break;
    }
    case kSparcStoreSimd128: {
      size_t next = 0;
      MemOperand mem = i.MemoryOperand(&next);
      DoubleRegister src = i.InputSimd128Register(next++);
      CHECK_EQ(i.InputInt32(next), 0);
      __ LoadAddress(g4, mem);
      sparc64::SimdEmitter(masm()).StoreVector(g4, no_reg, 0, src, false, true,
                                               nullptr);
      break;
    }
    case kSparcS128Load8Lane:
      sparc64::SimdEmitter(masm()).LoadLane(
          i.OutputSimd128Register(), i.InputSimd128Register(0),
          i.InputRegister(1), i.InputRegister(2), 0, 1, i.InputUint8(3),
          nullptr, true);
      break;
    case kSparcS128Store8Lane:
      sparc64::SimdEmitter(masm()).StoreLane(
          i.InputRegister(1), i.InputRegister(2), 0, i.InputSimd128Register(0),
          1, i.InputUint8(3), nullptr, true);
      break;
    case kSparcS128Load16Lane:
      sparc64::SimdEmitter(masm()).LoadLane(
          i.OutputSimd128Register(), i.InputSimd128Register(0),
          i.InputRegister(1), i.InputRegister(2), 0, 2, i.InputUint8(3),
          nullptr, true);
      break;
    case kSparcS128Store16Lane:
      sparc64::SimdEmitter(masm()).StoreLane(
          i.InputRegister(1), i.InputRegister(2), 0, i.InputSimd128Register(0),
          2, i.InputUint8(3), nullptr, true);
      break;
    case kSparcS128Load32Lane:
      sparc64::SimdEmitter(masm()).LoadLane(
          i.OutputSimd128Register(), i.InputSimd128Register(0),
          i.InputRegister(1), i.InputRegister(2), 0, 4, i.InputUint8(3),
          nullptr, true);
      break;
    case kSparcS128Store32Lane:
      sparc64::SimdEmitter(masm()).StoreLane(
          i.InputRegister(1), i.InputRegister(2), 0, i.InputSimd128Register(0),
          4, i.InputUint8(3), nullptr, true);
      break;
    case kSparcS128Load64Lane:
      sparc64::SimdEmitter(masm()).LoadLane(
          i.OutputSimd128Register(), i.InputSimd128Register(0),
          i.InputRegister(1), i.InputRegister(2), 0, 8, i.InputUint8(3),
          nullptr, true);
      break;
    case kSparcS128Store64Lane:
      sparc64::SimdEmitter(masm()).StoreLane(
          i.InputRegister(1), i.InputRegister(2), 0, i.InputSimd128Register(0),
          8, i.InputUint8(3), nullptr, true);
      break;
    case kSparcS128Load8Splat:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          1, sparc64::SimdEmitter::Transform::kSplat, false, nullptr, true);
      break;
    case kSparcS128Load16Splat:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          2, sparc64::SimdEmitter::Transform::kSplat, false, nullptr, true);
      break;
    case kSparcS128Load32Splat:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          4, sparc64::SimdEmitter::Transform::kSplat, false, nullptr, true);
      break;
    case kSparcS128Load64Splat:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          8, sparc64::SimdEmitter::Transform::kSplat, false, nullptr, true);
      break;
    case kSparcS128Load8x8S:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          1, sparc64::SimdEmitter::Transform::kExtend, true, nullptr, true);
      break;
    case kSparcS128Load8x8U:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          1, sparc64::SimdEmitter::Transform::kExtend, false, nullptr, true);
      break;
    case kSparcS128Load16x4S:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          2, sparc64::SimdEmitter::Transform::kExtend, true, nullptr, true);
      break;
    case kSparcS128Load16x4U:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          2, sparc64::SimdEmitter::Transform::kExtend, false, nullptr, true);
      break;
    case kSparcS128Load32x2S:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          4, sparc64::SimdEmitter::Transform::kExtend, true, nullptr, true);
      break;
    case kSparcS128Load32x2U:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          4, sparc64::SimdEmitter::Transform::kExtend, false, nullptr, true);
      break;
    case kSparcS128Load32Zero:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          4, sparc64::SimdEmitter::Transform::kZeroExtend, false, nullptr,
          true);
      break;
    case kSparcS128Load64Zero:
      sparc64::SimdEmitter(masm()).LoadTransform(
          i.OutputSimd128Register(), i.InputRegister(0), i.InputRegister(1), 0,
          8, sparc64::SimdEmitter::Transform::kZeroExtend, false, nullptr,
          true);
      break;
    default:
      FATAL("Unimplemented SPARC64 instruction %s (opcode %d)",
            Sparc64OpcodeName(opcode), opcode);
  }

#undef INTEGER_STORE
#undef INTEGER_LOAD
#undef FLOAT_UNARY
#undef FLOAT_BINARY
#undef SHIFT_OP
#undef BINARY_OP
  return kSuccess;
}

void CodeGenerator::AssembleMove(InstructionOperand* source,
                                 InstructionOperand* destination) {
  Sparc64OperandConverter g(this, nullptr);
  if (source->IsSimd128Register() || source->IsSimd128StackSlot()) {
    DCHECK(destination->IsSimd128Register() ||
           destination->IsSimd128StackSlot());
    if (source->IsSimd128Register()) {
      DoubleRegister src = g.ToSimd128Register(source);
      if (destination->IsSimd128Register()) {
        sparc64::MoveSimd(masm(), g.ToSimd128Register(destination), src);
      } else {
        sparc64::StoreSimd(masm(), src, g.ToMemOperand(destination));
      }
    } else if (destination->IsSimd128Register()) {
      sparc64::LoadSimd(masm(), g.ToSimd128Register(destination),
                        g.ToMemOperand(source));
    } else {
      sparc64::LoadSimd(masm(), d15, g.ToMemOperand(source));
      sparc64::StoreSimd(masm(), d15, g.ToMemOperand(destination));
    }
    return;
  }
  if (source->IsRegister()) {
    Register src = g.ToRegister(source);
    if (destination->IsRegister()) {
      __ Move(g.ToRegister(destination), src);
    } else {
      DCHECK(destination->IsStackSlot());
      __ StoreU64(src, g.ToMemOperand(destination));
    }
  } else if (source->IsStackSlot()) {
    if (destination->IsRegister()) {
      __ LoadU64(g.ToRegister(destination), g.ToMemOperand(source));
    } else {
      DCHECK(destination->IsStackSlot());
      __ LoadU64(g1, g.ToMemOperand(source));
      __ StoreU64(g1, g.ToMemOperand(destination));
    }
  } else if (source->IsConstant() || source->IsImmediate()) {
    Constant src = g.ToConstant(source);
    if (destination->IsRegister() || destination->IsStackSlot()) {
      Register dst = destination->IsRegister() ? g.ToRegister(destination) : g1;
      switch (src.type()) {
        case Constant::kInt32:
        case Constant::kInt64: {
          int64_t value =
              src.type() == Constant::kInt32 ? src.ToInt32() : src.ToInt64();
          if (RelocInfo::IsNoInfo(src.rmode())) {
            __ Move(dst, value);
          } else {
            __ LoadLiteral(dst, value, src.rmode());
          }
          break;
        }
        case Constant::kFloat32:
          __ MoveNumber(dst, src.ToFloat32());
          break;
        case Constant::kFloat64:
          __ MoveNumber(dst, src.ToFloat64().value());
          break;
        case Constant::kExternalReference:
          __ Move(dst, src.ToExternalReference());
          break;
        case Constant::kHeapObject: {
          auto object = src.ToHeapObject();
          RootIndex index;
          if (IsMaterializableFromRoot(object, &index)) {
            __ LoadRoot(dst, index);
          } else {
            __ Move(dst, object);
          }
          break;
        }
        case Constant::kCompressedHeapObject:
        case Constant::kRpoNumber:
          UNREACHABLE();
      }
      if (destination->IsStackSlot()) {
        __ StoreU64(dst, g.ToMemOperand(destination));
      }
    } else {
      DCHECK(destination->IsFPRegister() || destination->IsFPStackSlot());
      DoubleRegister dst = destination->IsFPRegister()
                               ? g.ToDoubleRegister(destination)
                               : kScratchDoubleReg;
      if (src.type() == Constant::kFloat32) {
        __ Move(g1, static_cast<intptr_t>(src.ToFloat32AsInt()));
        __ MovIntToFloat(dst, g1);
      } else {
        DCHECK_EQ(src.type(), Constant::kFloat64);
        __ Move(g1, static_cast<intptr_t>(src.ToFloat64().AsUint64()));
        __ MovInt64ToDouble(dst, g1);
      }
      if (destination->IsFloatStackSlot()) {
        __ StoreF32(dst, g.ToMemOperand(destination));
      } else if (destination->IsDoubleStackSlot()) {
        __ StoreF64(dst, g.ToMemOperand(destination));
      }
    }
  } else if (source->IsFPRegister()) {
    DoubleRegister src = g.ToDoubleRegister(source);
    if (source->IsFloatRegister()) {
      if (destination->IsFPRegister()) {
        __ fmovs(g.ToDoubleRegister(destination), src);
      } else {
        DCHECK(destination->IsFloatStackSlot());
        __ StoreF32(src, g.ToMemOperand(destination));
      }
    } else {
      DCHECK(source->IsDoubleRegister());
      if (destination->IsFPRegister()) {
        __ fmovd(g.ToDoubleRegister(destination), src);
      } else {
        DCHECK(destination->IsDoubleStackSlot());
        __ StoreF64(src, g.ToMemOperand(destination));
      }
    }
  } else {
    DCHECK(source->IsFloatStackSlot() || source->IsDoubleStackSlot());
    DoubleRegister dst = destination->IsFPRegister()
                             ? g.ToDoubleRegister(destination)
                             : kScratchDoubleReg;
    if (source->IsFloatStackSlot()) {
      __ LoadF32(dst, g.ToMemOperand(source));
      if (destination->IsFPStackSlot()) {
        __ StoreF32(dst, g.ToMemOperand(destination));
      }
    } else {
      __ LoadF64(dst, g.ToMemOperand(source));
      if (destination->IsFPStackSlot()) {
        __ StoreF64(dst, g.ToMemOperand(destination));
      }
    }
  }
}

AllocatedOperand CodeGenerator::Push(InstructionOperand* source) {
  auto rep = LocationOperand::cast(source)->representation();
  int slots = ElementSizeInPointers(rep);
  int slot_id = frame()->GetTotalFrameSlotCount() - 1 +
                frame_access_state()->sp_delta() + slots;
  AllocatedOperand stack_slot(LocationOperand::STACK_SLOT, rep, slot_id);
  __ AllocateStackSpace(slots * kSystemPointerSize);
  frame_access_state()->IncreaseSPDelta(slots);
  AssembleMove(source, &stack_slot);
  temp_slots_ += slots;
  return stack_slot;
}

void CodeGenerator::Pop(InstructionOperand* destination,
                        MachineRepresentation rep) {
  int slots = ElementSizeInPointers(rep);
  int slot_id =
      frame()->GetTotalFrameSlotCount() - 1 + frame_access_state()->sp_delta();
  AllocatedOperand stack_slot(LocationOperand::STACK_SLOT, rep, slot_id);
  AssembleMove(&stack_slot, destination);
  frame_access_state()->IncreaseSPDelta(-slots);
  __ Drop(slots);
  temp_slots_ -= slots;
}

void CodeGenerator::PopTempStackSlots() {
  if (temp_slots_ == 0) return;
  frame_access_state()->IncreaseSPDelta(-temp_slots_);
  __ Drop(temp_slots_);
  temp_slots_ = 0;
}

// Stack temporaries survive any scratch registers used by pending gap moves.
void CodeGenerator::MoveToTempLocation(InstructionOperand* source,
                                       MachineRepresentation rep) {
  Push(source);
}

void CodeGenerator::MoveTempLocationTo(InstructionOperand* destination,
                                       MachineRepresentation rep) {
  Pop(destination, rep);
}

void CodeGenerator::SetPendingMove(MoveOperands* move) {}

void CodeGenerator::AssembleSwap(InstructionOperand* source,
                                 InstructionOperand* destination) {
  auto rep = LocationOperand::cast(source)->representation();
  Push(source);
  AssembleMove(destination, source);
  Pop(destination, rep);
}

#undef __

}  // namespace v8::internal::compiler
