// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <optional>

#include "src/base/iterator.h"
#include "src/compiler/backend/instruction-selector-impl.h"
#include "src/compiler/turboshaft/opmasks.h"
#include "src/execution/sparc64/frame-constants-sparc64.h"

namespace v8 {
namespace internal {
namespace compiler {

using namespace turboshaft;  // NOLINT(build/namespaces)

enum ImmediateMode {
  kSimm13,
  kUint12Imm,
  kSimm13Memory,
  kShift32Imm,
  kShift64Imm,
  kNoImmediate
};

// Adds SPARC-specific methods for generating operands.
class Sparc64OperandGenerator final : public OperandGenerator {
 public:
  explicit Sparc64OperandGenerator(InstructionSelector* selector)
      : OperandGenerator(selector) {}

  InstructionOperand UseOperand(OpIndex node, ImmediateMode mode) {
    if (CanBeImmediate(node, mode)) {
      return UseImmediate(node);
    }
    return UseRegister(node);
  }

  bool CanBeImmediate(OpIndex node, ImmediateMode mode) {
    int64_t value;
    if (!selector()->MatchSignedIntegralConstant(node, &value)) return false;
    return CanBeImmediate(value, mode);
  }

  bool CanBeImmediate(int64_t value, ImmediateMode mode) {
    switch (mode) {
      case kSimm13:
        return is_int13(value);
      case kUint12Imm:
        return is_uint12(value);
      case kSimm13Memory:
        return is_int13(value);
      case kShift32Imm:
        return 0 <= value && value < 32;
      case kShift64Imm:
        return 0 <= value && value < 64;
      case kNoImmediate:
        return false;
    }
    return false;
  }
};

namespace {

void VisitRR(InstructionSelector* selector, InstructionCode opcode,
             OpIndex node) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  DCHECK_EQ(op.input_count, 1);
  selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void VisitRRR(InstructionSelector* selector, InstructionCode opcode,
              OpIndex node) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  DCHECK_EQ(op.input_count, 2);
  selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
                 g.UseRegister(op.input(1)));
}

void VisitRRO(InstructionSelector* selector, InstructionCode opcode,
              OpIndex node, ImmediateMode operand_mode) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  selector->Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
                 g.UseOperand(op.input(1), operand_mode));
}

void VisitTryTruncateDouble(InstructionSelector* selector,
                            InstructionCode opcode, OpIndex node) {
  Sparc64OperandGenerator g(selector);
  const TryChangeOp& op = selector->Cast<TryChangeOp>(node);
  opcode |= MiscField::encode(op.from == FloatRepresentation::Float32());
  InstructionOperand inputs[] = {g.UseRegister(op.input())};
  InstructionOperand outputs[2];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  OptionalOpIndex success_output = selector->FindProjection(node, 1);
  if (success_output.valid()) {
    outputs[output_count++] = g.DefineAsRegister(success_output.value());
  }

  selector->Emit(opcode, output_count, outputs, 1, inputs);
}

// Shared routine for multiple binary operations.
void VisitBinop(InstructionSelector* selector, OpIndex node,
                InstructionCode opcode, ImmediateMode operand_mode,
                FlagsContinuation* cont) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  InstructionOperand inputs[4];
  size_t input_count = 0;
  InstructionOperand outputs[2];
  size_t output_count = 0;

  inputs[input_count++] = g.UseRegister(op.input(0));
  inputs[input_count++] = g.UseOperand(op.input(1), operand_mode);

  if (cont->IsDeoptimize()) {
    // If we can deoptimize as a result of the binop, we need to make sure that
    // the deopt inputs are not overwritten by the binop result. One way
    // to achieve that is to declare the output register as same-as-first.
    outputs[output_count++] = g.DefineSameAsFirst(node);
  } else {
    outputs[output_count++] = g.DefineAsRegister(node);
  }

  DCHECK_NE(0u, input_count);
  DCHECK_NE(0u, output_count);
  DCHECK_GE(arraysize(inputs), input_count);
  DCHECK_GE(arraysize(outputs), output_count);

  selector->EmitWithContinuation(opcode, output_count, outputs, input_count,
                                 inputs, cont);
}

// Shared routine for multiple binary operations.
void VisitBinop(InstructionSelector* selector, OpIndex node,
                InstructionCode opcode, ImmediateMode operand_mode) {
  FlagsContinuation cont;
  VisitBinop(selector, node, opcode, operand_mode, &cont);
}

}  // namespace

void InstructionSelector::VisitStackSlot(OpIndex node) {
  const StackSlotOp& stack_slot = Cast<StackSlotOp>(node);
  int slot = frame_->AllocateSpillSlot(stack_slot.size, stack_slot.alignment,
                                       stack_slot.is_tagged);
  OperandGenerator g(this);

  Emit(kArchStackSlot, g.DefineAsRegister(node),
       sequence()->AddImmediate(Constant(slot)), 0, nullptr);
}

void InstructionSelector::VisitAbortCSADcheck(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const AbortCSADcheckOp& op = Cast<AbortCSADcheckOp>(node);
  Emit(kArchAbortCSADcheck, g.NoOutput(), g.UseFixed(op.input(0), o1));
}

ArchOpcode SelectLoadOpcode(MemoryRepresentation loaded_rep,
                            RegisterRepresentation result_rep,
                            ImmediateMode* mode) {
  // NOTE: The meaning of `loaded_rep` = `MemoryRepresentation::AnyTagged()` is
  // we are loading a compressed tagged field, while `result_rep` =
  // `RegisterRepresentation::Tagged()` refers to an uncompressed tagged value.
  *mode = kSimm13;
  switch (loaded_rep) {
    case MemoryRepresentation::Int8():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kSparcLoadWordS8;
    case MemoryRepresentation::Uint8():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kSparcLoadWordU8;
    case MemoryRepresentation::Int16():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kSparcLoadWordS16;
    case MemoryRepresentation::Uint16():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kSparcLoadWordU16;
    case MemoryRepresentation::Int32():
    case MemoryRepresentation::Uint32():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word32());
      return kSparcLoadWordU32;
    case MemoryRepresentation::Int64():
    case MemoryRepresentation::Uint64():
      DCHECK_EQ(result_rep, RegisterRepresentation::Word64());
      *mode = kSimm13Memory;
      return kSparcLoadWord64;
    case MemoryRepresentation::Float16():
      UNIMPLEMENTED();
    case MemoryRepresentation::Float32():
      DCHECK_EQ(result_rep, RegisterRepresentation::Float32());
      return kSparcLoadFloat32;
    case MemoryRepresentation::Float64():
      DCHECK_EQ(result_rep, RegisterRepresentation::Float64());
      return kSparcLoadDouble;
#ifdef V8_COMPRESS_POINTERS
    case MemoryRepresentation::AnyTagged():
    case MemoryRepresentation::TaggedPointer():
      if (result_rep == RegisterRepresentation::Compressed()) {
        *mode = kSimm13Memory;
        return kSparcLoadWordS32;
      }
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      return kSparcLoadDecompressTagged;
    case MemoryRepresentation::TaggedSigned():
      if (result_rep == RegisterRepresentation::Compressed()) {
        *mode = kSimm13Memory;
        return kSparcLoadWordS32;
      }
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      return kSparcLoadDecompressTaggedSigned;
#else
      USE(result_rep);
    case MemoryRepresentation::AnyTagged():
    case MemoryRepresentation::TaggedPointer():
    case MemoryRepresentation::TaggedSigned():
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      *mode = kSimm13Memory;
      return kSparcLoadWord64;
#endif
    case MemoryRepresentation::AnyUncompressedTagged():
    case MemoryRepresentation::UncompressedTaggedPointer():
    case MemoryRepresentation::UncompressedTaggedSigned():
      DCHECK_EQ(result_rep, RegisterRepresentation::Tagged());
      *mode = kSimm13Memory;
      return kSparcLoadWord64;
    case MemoryRepresentation::Simd128():
      DCHECK_EQ(result_rep, RegisterRepresentation::Simd128());
      // Vectors do not support MRI mode, only MRR is available.
      *mode = kNoImmediate;
      return kSparcLoadSimd128;
    case MemoryRepresentation::ProtectedPointer():
    case MemoryRepresentation::IndirectPointer():
    case MemoryRepresentation::SandboxedPointer():
    case MemoryRepresentation::Simd256():
      UNREACHABLE();
  }
}

ArchOpcode SelectLoadOpcode(LoadRepresentation load_rep, ImmediateMode* mode) {
  *mode = kSimm13;
  switch (load_rep.representation()) {
    case MachineRepresentation::kFloat32:
      return kSparcLoadFloat32;
    case MachineRepresentation::kFloat64:
      return kSparcLoadDouble;
    case MachineRepresentation::kBit:  // Fall through.
    case MachineRepresentation::kWord8:
      return load_rep.IsSigned() ? kSparcLoadWordS8 : kSparcLoadWordU8;
    case MachineRepresentation::kWord16:
      return load_rep.IsSigned() ? kSparcLoadWordS16 : kSparcLoadWordU16;
    case MachineRepresentation::kWord32:
      return kSparcLoadWordU32;
    case MachineRepresentation::kCompressedPointer:  // Fall through.
    case MachineRepresentation::kCompressed:
#ifdef V8_COMPRESS_POINTERS
      *mode = kSimm13Memory;
      return kSparcLoadWordS32;
#else
      UNREACHABLE();
#endif
#ifdef V8_COMPRESS_POINTERS
    case MachineRepresentation::kTaggedSigned:
      return kSparcLoadDecompressTaggedSigned;
    case MachineRepresentation::kTaggedPointer:
      return kSparcLoadDecompressTagged;
    case MachineRepresentation::kTagged:
      return kSparcLoadDecompressTagged;
#else
    case MachineRepresentation::kTaggedSigned:   // Fall through.
    case MachineRepresentation::kTaggedPointer:  // Fall through.
    case MachineRepresentation::kTagged:         // Fall through.
#endif
    case MachineRepresentation::kWord64:
      *mode = kSimm13Memory;
      return kSparcLoadWord64;
    case MachineRepresentation::kSimd128:
      // Vectors do not support MRI mode, only MRR is available.
      *mode = kNoImmediate;
      return kSparcLoadSimd128;
    case MachineRepresentation::kFloat16:
      UNIMPLEMENTED();
    case MachineRepresentation::kProtectedPointer:  // Fall through.
    case MachineRepresentation::kIndirectPointer:   // Fall through.
    case MachineRepresentation::kSandboxedPointer:  // Fall through.
    case MachineRepresentation::kSimd256:           // Fall through.
    case MachineRepresentation::kMapWord:           // Fall through.
    case MachineRepresentation::kFloat16RawBits:    // Fall through.
    case MachineRepresentation::kNone:
      UNREACHABLE();
  }
}

static void VisitLoadCommon(InstructionSelector* selector, OpIndex node,
                            ImmediateMode mode, InstructionCode opcode) {
  Sparc64OperandGenerator g(selector);
  auto load = selector->load_view(node);
  OpIndex base = load.base();
  OpIndex offset = load.index();

  bool is_atomic = load.is_atomic();

  if (selector->Is<LoadRootRegisterOp>(base)) {
    selector->Emit(opcode |= AddressingModeField::encode(kMode_Root),
                   g.DefineAsRegister(node), g.UseRegister(offset),
                   g.UseImmediate(is_atomic));
  } else if (g.CanBeImmediate(offset, mode)) {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                   g.DefineAsRegister(node), g.UseRegister(base),
                   g.UseImmediate(offset), g.UseImmediate(is_atomic));
  } else if (g.CanBeImmediate(base, mode)) {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                   g.DefineAsRegister(node), g.UseRegister(offset),
                   g.UseImmediate(base), g.UseImmediate(is_atomic));
  } else {
    selector->Emit(opcode | AddressingModeField::encode(kMode_MRR),
                   g.DefineAsRegister(node), g.UseRegister(base),
                   g.UseRegister(offset), g.UseImmediate(is_atomic));
  }
}

void InstructionSelector::VisitLoad(OpIndex node) {
  LoadView load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode =
      SelectLoadOpcode(load.ts_loaded_rep(), load.ts_result_rep(), &mode);
  VisitLoadCommon(this, node, mode, opcode);
}

void InstructionSelector::VisitProtectedLoad(OpIndex node) {
  // TODO(eholk)
  UNIMPLEMENTED();
}

void VisitStoreCommon(InstructionSelector* selector, OpIndex node,
                      StoreRepresentation store_rep,
                      std::optional<AtomicMemoryOrder> atomic_order,
                      bool unaligned = false) {
  Sparc64OperandGenerator g(selector);
  auto store = selector->store_view(node);
  OpIndex base = store.base();
  OpIndex offset = store.index().value();
  OpIndex value = store.value();
  bool is_atomic = store.is_atomic();
  DCHECK_IMPLIES(unaligned, !is_atomic);

  MachineRepresentation rep = store_rep.representation();
  WriteBarrierKind write_barrier_kind = kNoWriteBarrier;

  if (!is_atomic) {
    write_barrier_kind = store_rep.write_barrier_kind();
  }

  if (v8_flags.enable_unconditional_write_barriers &&
      CanBeTaggedOrCompressedPointer(rep)) {
    write_barrier_kind = kFullWriteBarrier;
  }

  DCHECK_IMPLIES(write_barrier_kind == kSkippedWriteBarrier,
                 v8_flags.verify_write_barriers);

  if (write_barrier_kind != kNoWriteBarrier &&
      !v8_flags.disable_write_barriers) {
    DCHECK(CanBeTaggedOrCompressedPointer(rep));
    // Uncompressed stores should not happen if we need a write barrier.
    CHECK((store.ts_stored_rep() !=
           MemoryRepresentation::AnyUncompressedTagged()) &&
          (store.ts_stored_rep() !=
           MemoryRepresentation::UncompressedTaggedPointer()) &&
          (store.ts_stored_rep() !=
           MemoryRepresentation::UncompressedTaggedPointer()));
    AddressingMode addressing_mode;
    InstructionOperand inputs[3];
    size_t input_count = 0;
    inputs[input_count++] = g.UseUniqueRegister(base);
    if (g.CanBeImmediate(offset, kSimm13)) {
      inputs[input_count++] = g.UseImmediate(offset);
      addressing_mode = kMode_MRI;
    } else {
      inputs[input_count++] = g.UseUniqueRegister(offset);
      addressing_mode = kMode_MRR;
    }
    inputs[input_count++] = g.UseUniqueRegister(value);
    InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
    size_t const temp_count = arraysize(temps);
    InstructionCode code;

    if (write_barrier_kind == kSkippedWriteBarrier) {
      code = kArchStoreSkippedWriteBarrier;
    } else {
      RecordWriteMode record_write_mode =
          WriteBarrierKindToRecordWriteMode(write_barrier_kind);
      code = kArchStoreWithWriteBarrier;
      code |= RecordWriteModeField::encode(record_write_mode);
    }

    code |= AddressingModeField::encode(addressing_mode);
    CHECK_EQ(is_atomic, false);
    selector->Emit(code, 0, nullptr, input_count, inputs, temp_count, temps);
  } else {
    InstructionCode opcode;
    ImmediateMode mode;
    mode = kSimm13;
    switch (store.ts_stored_rep()) {
      case MemoryRepresentation::Int8():
      case MemoryRepresentation::Uint8():
        opcode = kSparcStoreWord8;
        break;
      case MemoryRepresentation::Int16():
      case MemoryRepresentation::Uint16():
        opcode = kSparcStoreWord16;
        break;
      case MemoryRepresentation::Int32():
      case MemoryRepresentation::Uint32(): {
        opcode = kSparcStoreWord32;
        const Operation& reverse_op = selector->Get(value);
        if (reverse_op.Is<Opmask::kWord32ReverseBytes>()) {
          opcode = kSparcStoreByteRev32;
          value = reverse_op.input(0);
          mode = kNoImmediate;
        }
        break;
      }
      case MemoryRepresentation::Int64():
      case MemoryRepresentation::Uint64(): {
        mode = kSimm13Memory;
        opcode = kSparcStoreWord64;
        const Operation& reverse_op = selector->Get(value);
        if (reverse_op.Is<Opmask::kWord64ReverseBytes>()) {
          opcode = kSparcStoreByteRev64;
          value = reverse_op.input(0);
          mode = kNoImmediate;
        }
        break;
      }
      case MemoryRepresentation::Float16():
        UNIMPLEMENTED();
      case MemoryRepresentation::Float32():
        opcode = kSparcStoreFloat32;
        break;
      case MemoryRepresentation::Float64():
        opcode = kSparcStoreDouble;
        break;
      case MemoryRepresentation::AnyTagged():
      case MemoryRepresentation::TaggedPointer():
      case MemoryRepresentation::TaggedSigned():
        mode = kSimm13;
        opcode = kSparcStoreCompressTagged;
        break;
      case MemoryRepresentation::AnyUncompressedTagged():
      case MemoryRepresentation::UncompressedTaggedPointer():
      case MemoryRepresentation::UncompressedTaggedSigned():
        mode = kSimm13Memory;
        opcode = kSparcStoreWord64;
        break;
      case MemoryRepresentation::ProtectedPointer():
        // We never store directly to protected pointers from generated code.
        UNREACHABLE();
      case MemoryRepresentation::Simd128():
        opcode = kSparcStoreSimd128;
        // Vectors do not support MRI mode, only MRR is available.
        mode = kNoImmediate;
        break;
      case MemoryRepresentation::IndirectPointer():
      case MemoryRepresentation::SandboxedPointer():
      case MemoryRepresentation::Simd256():
        UNREACHABLE();
    }

    if (unaligned) opcode |= MiscField::encode(kSparcUnalignedMemoryAccess);
    if (selector->Is<LoadRootRegisterOp>(base)) {
      selector->Emit(opcode | AddressingModeField::encode(kMode_Root),
                     g.NoOutput(), g.UseRegister(offset), g.UseRegister(value),
                     g.UseImmediate(is_atomic));
    } else if (g.CanBeImmediate(offset, mode)) {
      selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                     g.NoOutput(), g.UseRegister(base), g.UseImmediate(offset),
                     g.UseRegister(value), g.UseImmediate(is_atomic));
    } else if (g.CanBeImmediate(base, mode)) {
      selector->Emit(opcode | AddressingModeField::encode(kMode_MRI),
                     g.NoOutput(), g.UseRegister(offset), g.UseImmediate(base),
                     g.UseRegister(value), g.UseImmediate(is_atomic));
    } else {
      selector->Emit(opcode | AddressingModeField::encode(kMode_MRR),
                     g.NoOutput(), g.UseRegister(base), g.UseRegister(offset),
                     g.UseRegister(value), g.UseImmediate(is_atomic));
    }
  }
}

void InstructionSelector::VisitStorePair(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitStore(OpIndex node) {
  VisitStoreCommon(this, node, store_view(node).stored_rep(), std::nullopt);
}

void InstructionSelector::VisitProtectedStore(OpIndex node) {
  // TODO(eholk)
  UNIMPLEMENTED();
}

void InstructionSelector::VisitUnalignedLoad(OpIndex node) {
  auto load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode =
      SelectLoadOpcode(load.ts_loaded_rep(), load.ts_result_rep(), &mode);
  VisitLoadCommon(this, node, mode,
                  opcode | MiscField::encode(kSparcUnalignedMemoryAccess));
}

void InstructionSelector::VisitUnalignedStore(OpIndex node) {
  VisitStoreCommon(this, node, store_view(node).stored_rep(), std::nullopt,
                   true);
}

void InstructionSelector::VisitWord32And(OpIndex node) {
  VisitBinop(this, node, kSparcAnd, kUint12Imm);
}

void InstructionSelector::VisitWord64And(OpIndex node) {
  VisitBinop(this, node, kSparcAnd, kUint12Imm);
}

void InstructionSelector::VisitWord32Or(OpIndex node) {
  VisitBinop(this, node, kSparcOr, kUint12Imm);
}

void InstructionSelector::VisitWord64Or(OpIndex node) {
  VisitBinop(this, node, kSparcOr, kUint12Imm);
}

void InstructionSelector::VisitWord32Xor(OpIndex node) {
  VisitBinop(this, node, kSparcXor, kUint12Imm);
}

void InstructionSelector::VisitStackPointerGreaterThan(
    OpIndex node, FlagsContinuation* cont) {
  StackCheckKind kind;
  OpIndex value;
  const auto& op = this->turboshaft_graph()
                       ->Get(node)
                       .template Cast<StackPointerGreaterThanOp>();
  kind = op.kind;
  value = op.stack_limit();
  InstructionCode opcode =
      kArchStackPointerGreaterThan |
      StackCheckField::encode(static_cast<StackCheckKind>(kind));

  Sparc64OperandGenerator g(this);

  // No outputs.
  InstructionOperand* const outputs = nullptr;
  const int output_count = 0;

  // Applying an offset to this stack check requires a temp register. Offsets
  // are only applied to the first stack check. If applying an offset, we must
  // ensure the input and temp registers do not alias, thus kUniqueRegister.
  InstructionOperand temps[] = {g.TempRegister()};
  const int temp_count = (kind == StackCheckKind::kJSFunctionEntry) ? 1 : 0;
  const auto register_mode = (kind == StackCheckKind::kJSFunctionEntry)
                                 ? OperandGenerator::kUniqueRegister
                                 : OperandGenerator::kRegister;

  InstructionOperand inputs[] = {g.UseRegisterWithMode(value, register_mode)};
  static constexpr int input_count = arraysize(inputs);

  EmitWithContinuation(opcode, output_count, outputs, input_count, inputs,
                       temp_count, temps, cont);
}

void InstructionSelector::VisitWord64Xor(OpIndex node) {
  VisitBinop(this, node, kSparcXor, kUint12Imm);
}

void InstructionSelector::VisitWord32Shl(OpIndex node) {
  VisitRRO(this, kSparcShiftLeft32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Shl(OpIndex node) {
  VisitRRO(this, kSparcShiftLeft64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Shr(OpIndex node) {
  VisitRRO(this, kSparcShiftRight32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Shr(OpIndex node) {
  VisitRRO(this, kSparcShiftRight64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Sar(OpIndex node) {
  VisitRRO(this, kSparcShiftRightAlg32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Sar(OpIndex node) {
  VisitRRO(this, kSparcShiftRightAlg64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Rol(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord64Rol(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord32Ror(OpIndex node) {
  VisitRRO(this, kSparcRotRight32, node, kShift32Imm);
}

void InstructionSelector::VisitWord64Ror(OpIndex node) {
  VisitRRO(this, kSparcRotRight64, node, kShift64Imm);
}

void InstructionSelector::VisitWord32Clz(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcCntlz32, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord64Clz(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcCntlz64, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord32Popcnt(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcPopcnt32, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord64Popcnt(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcPopcnt64, g.DefineAsRegister(node), g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitWord32Ctz(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord64Ctz(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitWord32ReverseBits(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitWord64ReverseBits(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitWord64ReverseBytes(OpIndex node) {
  Sparc64OperandGenerator g(this);
  InstructionOperand temp[] = {g.TempRegister()};
  OpIndex input = this->Get(node).input(0);
  const Operation& input_op = this->Get(input);
  if (CanCover(node, input) && input_op.Is<LoadOp>()) {
    auto load = load_view(input);
    LoadRepresentation load_rep = load.loaded_rep();
    if (load_rep.representation() == MachineRepresentation::kWord64) {
      OpIndex base = load.base();
      OpIndex offset = load.index();
      bool is_atomic = load.is_atomic();
      Emit(kSparcLoadByteRev64 | AddressingModeField::encode(kMode_MRR),
           g.DefineAsRegister(node), g.UseRegister(base), g.UseRegister(offset),
           g.UseImmediate(is_atomic));
      return;
    }
  }
  const Operation& op = this->Get(node);
  Emit(kSparcByteRev64, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)), 1, temp);
}

void InstructionSelector::VisitWord32ReverseBytes(OpIndex node) {
  Sparc64OperandGenerator g(this);
  OpIndex input = this->Get(node).input(0);
  const Operation& input_op = this->Get(input);
  if (CanCover(node, input) && input_op.Is<LoadOp>()) {
    auto load = load_view(input);
    LoadRepresentation load_rep = load.loaded_rep();
    if (load_rep.representation() == MachineRepresentation::kWord32) {
      OpIndex base = load.base();
      OpIndex offset = load.index();
      bool is_atomic = load.is_atomic();
      Emit(kSparcLoadByteRev32 | AddressingModeField::encode(kMode_MRR),
           g.DefineAsRegister(node), g.UseRegister(base), g.UseRegister(offset),
           g.UseImmediate(is_atomic));
      return;
    }
  }
  const Operation& op = this->Get(node);
  Emit(kSparcByteRev32, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)));
}

void InstructionSelector::VisitSimd128ReverseBytes(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcLoadReverseSimd128RR, g.DefineAsRegister(node),
       g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitInt32Add(OpIndex node) {
  VisitBinop(this, node, kSparcAdd32, kSimm13);
}

void InstructionSelector::VisitInt64Add(OpIndex node) {
  VisitBinop(this, node, kSparcAdd64, kSimm13);
}

void InstructionSelector::VisitInt32Sub(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const WordBinopOp& sub = this->Get(node).template Cast<WordBinopOp>();
  if (this->MatchIntegralZero(sub.left())) {
    Emit(kSparcNeg, g.DefineAsRegister(node), g.UseRegister(sub.right()));
  } else {
    VisitBinop(this, node, kSparcSub, kSimm13);
  }
}

void InstructionSelector::VisitInt64Sub(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const WordBinopOp& sub = this->Get(node).template Cast<WordBinopOp>();
  if (this->MatchIntegralZero(sub.left())) {
    Emit(kSparcNeg, g.DefineAsRegister(node), g.UseRegister(sub.right()));
  } else {
    VisitBinop(this, node, kSparcSub, kSimm13);
  }
}

namespace {

void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  InstructionOperand left, InstructionOperand right,
                  FlagsContinuation* cont);
void EmitInt32MulWithOverflow(InstructionSelector* selector, OpIndex node,
                              FlagsContinuation* cont) {
  Sparc64OperandGenerator g(selector);
  const OverflowCheckedBinopOp& op =
      selector->Get(node).Cast<OverflowCheckedBinopOp>();
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  InstructionOperand result_operand = g.DefineAsRegister(node);
  InstructionOperand high32_operand = g.TempRegister();
  InstructionOperand temp_operand = g.TempRegister();
  {
    InstructionOperand outputs[] = {result_operand, high32_operand};
    InstructionOperand inputs[] = {g.UseRegister(lhs), g.UseRegister(rhs)};
    selector->Emit(kSparcMul32WithHigh32, 2, outputs, 2, inputs);
  }
  {
    InstructionOperand shift_31 = g.UseImmediate(31);
    InstructionOperand outputs[] = {temp_operand};
    InstructionOperand inputs[] = {result_operand, shift_31};
    selector->Emit(kSparcShiftRightAlg32, 1, outputs, 2, inputs);
  }

  VisitCompare(selector, kSparcCmp32, high32_operand, temp_operand, cont);
}

void EmitInt64MulWithOverflow(InstructionSelector* selector, OpIndex node,
                              FlagsContinuation* cont) {
  Sparc64OperandGenerator g(selector);
  const OverflowCheckedBinopOp& op =
      selector->Cast<OverflowCheckedBinopOp>(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  InstructionOperand result = g.DefineAsRegister(node);
  InstructionOperand left = g.UseRegister(lhs);
  InstructionOperand high = g.TempRegister();
  InstructionOperand result_sign = g.TempRegister();
  InstructionOperand right = g.UseRegister(rhs);
  selector->Emit(kSparcMul64, result, left, right);
  selector->Emit(kSparcMulHighS64, high, left, right);
  selector->Emit(kSparcShiftRightAlg64, result_sign, result,
                 g.TempImmediate(63));
  // Test whether {high} is a sign-extension of {result}.
  selector->EmitWithContinuation(kSparcCmp64, high, result_sign, cont);
}

}  // namespace

void InstructionSelector::VisitInt32Mul(OpIndex node) {
  VisitRRR(this, kSparcMul32, node);
}

void InstructionSelector::VisitInt64Mul(OpIndex node) {
  VisitRRR(this, kSparcMul64, node);
}

void InstructionSelector::VisitInt32MulHigh(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kSparcMulHigh32, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitUint32MulHigh(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kSparcMulHighU32, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitInt64MulHigh(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kSparcMulHighS64, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitUint64MulHigh(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const WordBinopOp& op = Cast<WordBinopOp>(node);
  Emit(kSparcMulHighU64, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)));
}

void InstructionSelector::VisitInt32Div(OpIndex node) {
  VisitRRR(this, kSparcDiv32, node);
}

void InstructionSelector::VisitInt64Div(OpIndex node) {
  VisitRRR(this, kSparcDiv64, node);
}

void InstructionSelector::VisitUint32Div(OpIndex node) {
  VisitRRR(this, kSparcDivU32, node);
}

void InstructionSelector::VisitUint64Div(OpIndex node) {
  VisitRRR(this, kSparcDivU64, node);
}

void InstructionSelector::VisitInt32Mod(OpIndex node) {
  VisitRRR(this, kSparcMod32, node);
}

void InstructionSelector::VisitInt64Mod(OpIndex node) {
  VisitRRR(this, kSparcMod64, node);
}

void InstructionSelector::VisitUint32Mod(OpIndex node) {
  VisitRRR(this, kSparcModU32, node);
}

void InstructionSelector::VisitUint64Mod(OpIndex node) {
  VisitRRR(this, kSparcModU64, node);
}

void InstructionSelector::VisitChangeFloat32ToFloat64(OpIndex node) {
  VisitRR(this, kSparcFloat32ToDouble, node);
}

void InstructionSelector::VisitRoundInt32ToFloat32(OpIndex node) {
  VisitRR(this, kSparcInt32ToFloat32, node);
}

void InstructionSelector::VisitRoundUint32ToFloat32(OpIndex node) {
  VisitRR(this, kSparcUint32ToFloat32, node);
}

void InstructionSelector::VisitChangeInt32ToFloat64(OpIndex node) {
  VisitRR(this, kSparcInt32ToDouble, node);
}

void InstructionSelector::VisitChangeUint32ToFloat64(OpIndex node) {
  VisitRR(this, kSparcUint32ToDouble, node);
}

void InstructionSelector::VisitChangeFloat64ToInt32(OpIndex node) {
  VisitRR(this, kSparcDoubleToInt32, node);
}

void InstructionSelector::VisitChangeFloat64ToUint32(OpIndex node) {
  VisitRR(this, kSparcDoubleToUint32, node);
}

void InstructionSelector::VisitTruncateFloat64ToUint32(OpIndex node) {
  VisitRR(this, kSparcDoubleToUint32, node);
}

void InstructionSelector::VisitSignExtendWord8ToInt32(OpIndex node) {
  VisitRR(this, kSparcExtendSignWord8, node);
}

void InstructionSelector::VisitSignExtendWord16ToInt32(OpIndex node) {
  VisitRR(this, kSparcExtendSignWord16, node);
}

void InstructionSelector::VisitTryTruncateFloat32ToInt64(OpIndex node) {
  VisitTryTruncateDouble(this, kSparcDoubleToInt64, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToInt64(OpIndex node) {
  VisitTryTruncateDouble(this, kSparcDoubleToInt64, node);
}

void InstructionSelector::VisitTruncateFloat64ToInt64(OpIndex node) {
  VisitRR(this, kSparcDoubleToInt64, node);
}

void InstructionSelector::VisitTryTruncateFloat32ToUint64(OpIndex node) {
  VisitTryTruncateDouble(this, kSparcDoubleToUint64, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToUint64(OpIndex node) {
  VisitTryTruncateDouble(this, kSparcDoubleToUint64, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToInt32(OpIndex node) {
  VisitTryTruncateDouble(this, kSparcDoubleToInt32, node);
}

void InstructionSelector::VisitTryTruncateFloat64ToUint32(OpIndex node) {
  VisitTryTruncateDouble(this, kSparcDoubleToUint32, node);
}

void InstructionSelector::VisitBitcastWord32ToWord64(OpIndex node) {
  DCHECK(SmiValuesAre31Bits());
  DCHECK(COMPRESS_POINTERS_BOOL);
  EmitIdentity(node);
}

void InstructionSelector::VisitChangeInt32ToInt64(OpIndex node) {
  VisitRR(this, kSparcExtendSignWord32, node);
}

void InstructionSelector::VisitSignExtendWord8ToInt64(OpIndex node) {
  VisitRR(this, kSparcExtendSignWord8, node);
}

void InstructionSelector::VisitSignExtendWord16ToInt64(OpIndex node) {
  VisitRR(this, kSparcExtendSignWord16, node);
}

void InstructionSelector::VisitSignExtendWord32ToInt64(OpIndex node) {
  VisitRR(this, kSparcExtendSignWord32, node);
}

bool InstructionSelector::ZeroExtendsWord32ToWord64NoPhis(OpIndex node) {
  return false;
}

void InstructionSelector::VisitChangeUint32ToUint64(OpIndex node) {
  VisitRR(this, kSparcUint32ToUint64, node);
}

void InstructionSelector::VisitTruncateFloat64ToFloat16RawBits(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitChangeFloat16RawBitsToFloat64(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitChangeFloat64ToUint64(OpIndex node) {
  VisitRR(this, kSparcDoubleToUint64, node);
}

void InstructionSelector::VisitChangeFloat64ToInt64(OpIndex node) {
  VisitRR(this, kSparcDoubleToInt64, node);
}

void InstructionSelector::VisitTruncateFloat64ToFloat32(OpIndex node) {
  VisitRR(this, kSparcDoubleToFloat32, node);
}

void InstructionSelector::VisitTruncateFloat64ToWord32(OpIndex node) {
  VisitRR(this, kArchTruncateDoubleToI, node);
}

void InstructionSelector::VisitRoundFloat64ToInt32(OpIndex node) {
  VisitRR(this, kSparcDoubleToInt32, node);
}

void InstructionSelector::VisitTruncateFloat32ToInt32(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const ChangeOp& op = Cast<ChangeOp>(node);
  InstructionCode opcode = kSparcFloat32ToInt32;
  if (op.Is<Opmask::kTruncateFloat32ToInt32OverflowToMin>()) {
    opcode |= MiscField::encode(true);
  }
  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input()));
}

void InstructionSelector::VisitTruncateFloat32ToUint32(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const ChangeOp& op = Cast<ChangeOp>(node);
  InstructionCode opcode = kSparcFloat32ToUint32;
  if (op.Is<Opmask::kTruncateFloat32ToUint32OverflowToMin>()) {
    opcode |= MiscField::encode(true);
  }

  Emit(opcode, g.DefineAsRegister(node), g.UseRegister(op.input()));
}

void InstructionSelector::VisitTruncateInt64ToInt32(OpIndex node) {
  VisitRR(this, kSparcInt64ToInt32, node);
}

void InstructionSelector::VisitRoundInt64ToFloat32(OpIndex node) {
  VisitRR(this, kSparcInt64ToFloat32, node);
}

void InstructionSelector::VisitRoundInt64ToFloat64(OpIndex node) {
  VisitRR(this, kSparcInt64ToDouble, node);
}

void InstructionSelector::VisitChangeInt64ToFloat64(OpIndex node) {
  VisitRR(this, kSparcInt64ToDouble, node);
}

void InstructionSelector::VisitRoundUint64ToFloat32(OpIndex node) {
  VisitRR(this, kSparcUint64ToFloat32, node);
}

void InstructionSelector::VisitRoundUint64ToFloat64(OpIndex node) {
  VisitRR(this, kSparcUint64ToDouble, node);
}

void InstructionSelector::VisitBitcastFloat32ToInt32(OpIndex node) {
  VisitRR(this, kSparcBitcastFloat32ToInt32, node);
}

void InstructionSelector::VisitBitcastFloat64ToInt64(OpIndex node) {
  VisitRR(this, kSparcBitcastDoubleToInt64, node);
}

void InstructionSelector::VisitBitcastInt32ToFloat32(OpIndex node) {
  VisitRR(this, kSparcBitcastInt32ToFloat32, node);
}

void InstructionSelector::VisitBitcastInt64ToFloat64(OpIndex node) {
  VisitRR(this, kSparcBitcastInt64ToDouble, node);
}

void InstructionSelector::VisitFloat32Add(OpIndex node) {
  VisitRRR(this, kSparcAddDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Add(OpIndex node) {
  VisitRRR(this, kSparcAddDouble, node);
}

void InstructionSelector::VisitFloat32Sub(OpIndex node) {
  VisitRRR(this, kSparcSubDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Sub(OpIndex node) {
  VisitRRR(this, kSparcSubDouble, node);
}

void InstructionSelector::VisitFloat32Mul(OpIndex node) {
  VisitRRR(this, kSparcMulDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Mul(OpIndex node) {
  VisitRRR(this, kSparcMulDouble, node);
}

void InstructionSelector::VisitFloat32Div(OpIndex node) {
  VisitRRR(this, kSparcDivDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Div(OpIndex node) {
  VisitRRR(this, kSparcDivDouble, node);
}

void InstructionSelector::VisitFloat64Mod(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const FloatBinopOp& op = Cast<FloatBinopOp>(node);
  Emit(kSparcModDouble, g.DefineAsFixed(node, d0), g.UseFixed(op.input(0), d0),
       g.UseFixed(op.input(1), d1))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat32Max(OpIndex node) {
  VisitRRR(this, kSparcMaxDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Max(OpIndex node) {
  VisitRRR(this, kSparcMaxDouble, node);
}

void InstructionSelector::VisitFloat64SilenceNaN(OpIndex node) {
  VisitRR(this, kSparcFloat64SilenceNaN, node);
}

void InstructionSelector::VisitFloat32Min(OpIndex node) {
  VisitRRR(this, kSparcMinDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Min(OpIndex node) {
  VisitRRR(this, kSparcMinDouble, node);
}

void InstructionSelector::VisitFloat32Abs(OpIndex node) {
  VisitRR(this, kSparcAbsDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Abs(OpIndex node) {
  VisitRR(this, kSparcAbsDouble, node);
}

void InstructionSelector::VisitFloat32Sqrt(OpIndex node) {
  VisitRR(this, kSparcSqrtDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Ieee754Unop(OpIndex node,
                                                  InstructionCode opcode) {
  Sparc64OperandGenerator g(this);
  const FloatUnaryOp& op = Cast<FloatUnaryOp>(node);
  Emit(opcode, g.DefineAsFixed(node, d0), g.UseFixed(op.input(), d0))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat64Ieee754Binop(OpIndex node,
                                                   InstructionCode opcode) {
  Sparc64OperandGenerator g(this);
  const FloatBinopOp& op = Cast<FloatBinopOp>(node);
  Emit(opcode, g.DefineAsFixed(node, d0), g.UseFixed(op.input(0), d0),
       g.UseFixed(op.input(1), d1))
      ->MarkAsCall();
}

void InstructionSelector::VisitFloat64Sqrt(OpIndex node) {
  VisitRR(this, kSparcSqrtDouble, node);
}

void InstructionSelector::VisitFloat32RoundDown(OpIndex node) {
  VisitRR(this, kSparcFloorDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64RoundDown(OpIndex node) {
  VisitRR(this, kSparcFloorDouble, node);
}

void InstructionSelector::VisitFloat32RoundUp(OpIndex node) {
  VisitRR(this, kSparcCeilDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64RoundUp(OpIndex node) {
  VisitRR(this, kSparcCeilDouble, node);
}

void InstructionSelector::VisitFloat32RoundTruncate(OpIndex node) {
  VisitRR(this, kSparcTruncateDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64RoundTruncate(OpIndex node) {
  VisitRR(this, kSparcTruncateDouble, node);
}

void InstructionSelector::VisitFloat64RoundTiesAway(OpIndex node) {
  VisitRR(this, kSparcRoundDouble, node);
}

void InstructionSelector::VisitFloat32Neg(OpIndex node) {
  VisitRR(this, kSparcNegDouble | MiscField::encode(1), node);
}

void InstructionSelector::VisitFloat64Neg(OpIndex node) {
  VisitRR(this, kSparcNegDouble, node);
}

void InstructionSelector::VisitInt32AddWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kSparcAddWithOverflow32, kSimm13, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kSparcAddWithOverflow32, kSimm13, &cont);
}

void InstructionSelector::VisitInt32SubWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kSparcSubWithOverflow32, kSimm13, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kSparcSubWithOverflow32, kSimm13, &cont);
}

void InstructionSelector::VisitInt64AddWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kSparcAdd64, kSimm13, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kSparcAdd64, kSimm13, &cont);
}

void InstructionSelector::VisitInt64SubWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kOverflow, ovf.value());
    return VisitBinop(this, node, kSparcSub, kSimm13, &cont);
  }
  FlagsContinuation cont;
  VisitBinop(this, node, kSparcSub, kSimm13, &cont);
}

void InstructionSelector::VisitInt64MulWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kNotEqual, ovf.value());
    return EmitInt64MulWithOverflow(this, node, &cont);
  }
  FlagsContinuation cont;
  EmitInt64MulWithOverflow(this, node, &cont);
}

static bool CompareLogical(FlagsContinuation* cont) {
  switch (cont->condition()) {
    case kUnsignedLessThan:
    case kUnsignedGreaterThanOrEqual:
    case kUnsignedLessThanOrEqual:
    case kUnsignedGreaterThan:
      return true;
    default:
      return false;
  }
  UNREACHABLE();
}

namespace {

// Shared routine for multiple compare operations.
void VisitCompare(InstructionSelector* selector, InstructionCode opcode,
                  InstructionOperand left, InstructionOperand right,
                  FlagsContinuation* cont) {
  selector->EmitWithContinuation(opcode, left, right, cont);
}

// Shared routine for multiple word compare operations.
void VisitWordCompare(InstructionSelector* selector, OpIndex node,
                      InstructionCode opcode, FlagsContinuation* cont,
                      bool commutative, ImmediateMode immediate_mode) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);

  // Match immediates on left or right side of comparison.
  if (g.CanBeImmediate(rhs, immediate_mode)) {
    VisitCompare(selector, opcode, g.UseRegister(lhs), g.UseImmediate(rhs),
                 cont);
  } else if (g.CanBeImmediate(lhs, immediate_mode)) {
    if (!commutative) cont->Commute();
    VisitCompare(selector, opcode, g.UseRegister(rhs), g.UseImmediate(lhs),
                 cont);
  } else {
    VisitCompare(selector, opcode, g.UseRegister(lhs), g.UseRegister(rhs),
                 cont);
  }
}

void VisitWord32Compare(InstructionSelector* selector, OpIndex node,
                        FlagsContinuation* cont) {
  ImmediateMode mode = (CompareLogical(cont) ? kUint12Imm : kSimm13);
  VisitWordCompare(selector, node, kSparcCmp32, cont, false, mode);
}

void VisitWord64Compare(InstructionSelector* selector, OpIndex node,
                        FlagsContinuation* cont) {
  ImmediateMode mode = (CompareLogical(cont) ? kUint12Imm : kSimm13);
  VisitWordCompare(selector, node, kSparcCmp64, cont, false, mode);
}

// Shared routine for multiple float32 compare operations.
void VisitFloat32Compare(InstructionSelector* selector, OpIndex node,
                         FlagsContinuation* cont) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  VisitCompare(selector, kSparcCmpDouble | MiscField::encode(1),
               g.UseRegister(lhs), g.UseRegister(rhs), cont);
}

// Shared routine for multiple float64 compare operations.
void VisitFloat64Compare(InstructionSelector* selector, OpIndex node,
                         FlagsContinuation* cont) {
  Sparc64OperandGenerator g(selector);
  const Operation& op = selector->Get(node);
  OpIndex lhs = op.input(0);
  OpIndex rhs = op.input(1);
  VisitCompare(selector, kSparcCmpDouble, g.UseRegister(lhs),
               g.UseRegister(rhs), cont);
}

}  // namespace

void InstructionSelector::VisitWordCompareZero(OpIndex user, OpIndex value,
                                               FlagsContinuation* cont) {
  // Try to combine with comparisons against 0 by simply inverting the branch.
  ConsumeEqualZero(&user, &value, cont);

  if (CanCover(user, value)) {
    const Operation& value_op = Get(value);
    if (const ComparisonOp* comparison = value_op.TryCast<ComparisonOp>()) {
      switch (comparison->rep.MapTaggedToWord().value()) {
        case RegisterRepresentation::Word32():
          cont->OverwriteAndNegateIfEqual(
              GetComparisonFlagCondition(*comparison));
          return VisitWord32Compare(this, value, cont);
        case RegisterRepresentation::Word64():
          cont->OverwriteAndNegateIfEqual(
              GetComparisonFlagCondition(*comparison));
          return VisitWord64Compare(this, value, cont);
        case RegisterRepresentation::Float32():
          switch (comparison->kind) {
            case ComparisonOp::Kind::kEqual:
              cont->OverwriteAndNegateIfEqual(kEqual);
              return VisitFloat32Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThan:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
              return VisitFloat32Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThanOrEqual:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
              return VisitFloat32Compare(this, value, cont);
            default:
              UNREACHABLE();
          }
        case RegisterRepresentation::Float64():
          switch (comparison->kind) {
            case ComparisonOp::Kind::kEqual:
              cont->OverwriteAndNegateIfEqual(kEqual);
              return VisitFloat64Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThan:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThan);
              return VisitFloat64Compare(this, value, cont);
            case ComparisonOp::Kind::kSignedLessThanOrEqual:
              cont->OverwriteAndNegateIfEqual(kUnsignedLessThanOrEqual);
              return VisitFloat64Compare(this, value, cont);
            default:
              UNREACHABLE();
          }
        default:
          break;
      }
    } else if (const ProjectionOp* projection =
                   value_op.TryCast<ProjectionOp>()) {
      // Check if this is the overflow output projection of an
      // <Operation>WithOverflow node.
      if (projection->index == 1u) {
        // We cannot combine the <Operation>WithOverflow with this branch
        // unless the 0th projection (the use of the actual value of the
        // <Operation> is either nullptr, which means there's no use of the
        // actual value, or was already defined, which means it is scheduled
        // *AFTER* this branch).
        OpIndex node = projection->input();
        if (const OverflowCheckedBinopOp* binop =
                TryCast<OverflowCheckedBinopOp>(node);
            binop && CanDoBranchIfOverflowFusion(node)) {
          const bool is64 = binop->rep == WordRepresentation::Word64();
          switch (binop->kind) {
            case OverflowCheckedBinopOp::Kind::kSignedAdd:
              cont->OverwriteAndNegateIfEqual(kOverflow);
              return VisitBinop(this, node,
                                is64 ? kSparcAdd64 : kSparcAddWithOverflow32,
                                kSimm13, cont);
            case OverflowCheckedBinopOp::Kind::kSignedSub:
              cont->OverwriteAndNegateIfEqual(kOverflow);
              return VisitBinop(this, node,
                                is64 ? kSparcSub : kSparcSubWithOverflow32,
                                kSimm13, cont);
            case OverflowCheckedBinopOp::Kind::kSignedMul:
              if (is64) {
                cont->OverwriteAndNegateIfEqual(kNotEqual);
                return EmitInt64MulWithOverflow(this, node, cont);
              } else {
                cont->OverwriteAndNegateIfEqual(kNotEqual);
                return EmitInt32MulWithOverflow(this, node, cont);
              }
          }
        }
      }
    } else if (value_op.Is<Opmask::kWord32Sub>()) {
      return VisitWord32Compare(this, value, cont);
    } else if (value_op.Is<Opmask::kWord32BitwiseAnd>()) {
      return VisitWordCompare(this, value, kSparcTst32, cont, true, kUint12Imm);
    } else if (value_op.Is<Opmask::kWord64Sub>()) {
      return VisitWord64Compare(this, value, cont);
    } else if (value_op.Is<Opmask::kWord64BitwiseAnd>()) {
      return VisitWordCompare(this, value, kSparcTst64, cont, true, kUint12Imm);
    } else if (value_op.Is<StackPointerGreaterThanOp>()) {
      cont->OverwriteAndNegateIfEqual(kStackPointerGreaterThanCondition);
      return VisitStackPointerGreaterThan(value, cont);
    }
  }

  // Branch could not be combined with a compare, emit compare against 0.
  Sparc64OperandGenerator g(this);
  VisitCompare(this, kSparcCmp32, g.UseRegister(value), g.TempImmediate(0),
               cont);
}

void InstructionSelector::VisitSwitch(OpIndex node, const SwitchInfo& sw) {
  Sparc64OperandGenerator g(this);
  const SwitchOp& op = Cast<SwitchOp>(node);
  InstructionOperand value_operand = g.UseRegister(op.input());

  // Emit either ArchTableSwitch or ArchBinarySearchSwitch.
  if (enable_switch_jump_table_) {
    static const size_t kMaxTableSwitchValueRange = 2 << 16;
    size_t table_space_cost = 4 + sw.value_range();
    size_t table_time_cost = 3;
    size_t lookup_space_cost = 3 + 2 * sw.case_count();
    size_t lookup_time_cost = sw.case_count();
    if (sw.case_count() > 0 &&
        table_space_cost + 3 * table_time_cost <=
            lookup_space_cost + 3 * lookup_time_cost &&
        sw.min_value() > std::numeric_limits<int32_t>::min() &&
        sw.value_range() <= kMaxTableSwitchValueRange) {
      InstructionOperand index_operand = value_operand;
      if (sw.min_value()) {
        index_operand = g.TempRegister();
        Emit(kSparcSub, index_operand, value_operand,
             g.TempImmediate(sw.min_value()));
      }
      // Zero extend, because we use it as 64-bit index into the jump table.
      InstructionOperand index_operand_zero_ext = g.TempRegister();
      Emit(kSparcUint32ToUint64, index_operand_zero_ext, index_operand);
      index_operand = index_operand_zero_ext;
      // Generate a table lookup.
      return EmitTableSwitch(sw, index_operand);
    }
  }

  // Generate a tree of conditional jumps.
  return EmitBinarySearchSwitch(sw, value_operand);
}

void InstructionSelector::VisitWord32Equal(OpIndex const node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitInt32LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitInt32LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitUint32LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitUint32LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWord32Compare(this, node, &cont);
}

void InstructionSelector::VisitWord64Equal(OpIndex const node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitInt64LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kSignedLessThan, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitInt64LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kSignedLessThanOrEqual, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitUint64LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitUint64LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitWord64Compare(this, node, &cont);
}

void InstructionSelector::VisitInt32MulWithOverflow(OpIndex node) {
  OptionalOpIndex ovf = FindProjection(node, 1);
  if (ovf.valid()) {
    FlagsContinuation cont = FlagsContinuation::ForSet(kNotEqual, ovf.value());
    return EmitInt32MulWithOverflow(this, node, &cont);
  }
  FlagsContinuation cont;
  EmitInt32MulWithOverflow(this, node, &cont);
}

void InstructionSelector::VisitFloat32Equal(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat32LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat32LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitFloat32Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64Equal(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64LessThan(OpIndex node) {
  FlagsContinuation cont = FlagsContinuation::ForSet(kUnsignedLessThan, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::VisitFloat64LessThanOrEqual(OpIndex node) {
  FlagsContinuation cont =
      FlagsContinuation::ForSet(kUnsignedLessThanOrEqual, node);
  VisitFloat64Compare(this, node, &cont);
}

void InstructionSelector::EmitMoveParamToFPR(OpIndex node, int index) {}

void InstructionSelector::EmitMoveFPRToParam(InstructionOperand* op,
                                             LinkageLocation location) {}

void InstructionSelector::EmitPrepareArguments(
    ZoneVector<PushParameter>* arguments, const CallDescriptor* call_descriptor,
    OpIndex node) {
  Sparc64OperandGenerator g(this);

  // Prepare for C function call.
  if (call_descriptor->IsCFunctionCall()) {
    Emit(kArchPrepareCallCFunction | MiscField::encode(static_cast<int>(
                                         call_descriptor->ParameterCount())),
         0, nullptr, 0, nullptr);

    // Poke any stack arguments.
    int slot = 0;
    for (PushParameter input : (*arguments)) {
      if (!input.node.valid()) continue;
      MachineType type = input.location.GetType();
      unsigned extension = 0;
      switch (type.representation()) {
        case MachineRepresentation::kWord8:
          extension = 1;
          break;
        case MachineRepresentation::kWord16:
          extension = 3;
          break;
        case MachineRepresentation::kWord32:
          extension = 5;
          break;
        default:
          break;
      }
      if (!type.IsSigned() && !type.IsUnsigned())
        extension = 0;
      else if (extension && type.IsUnsigned())
        ++extension;
      Emit(kSparcStoreToStackSlot, g.NoOutput(), g.UseRegister(input.node),
           g.TempImmediate(slot), g.TempImmediate(extension));
      ++slot;
    }
  } else {
    // Push any stack arguments.
    int stack_decrement = 0;
    for (PushParameter input : base::Reversed(*arguments)) {
      stack_decrement += kSystemPointerSize;
      // Skip any alignment holes in pushed nodes.
      if (!input.node.valid()) continue;
      InstructionOperand decrement = g.UseImmediate(stack_decrement);
      stack_decrement = 0;
      Emit(kSparcPush, g.NoOutput(), decrement, g.UseRegister(input.node));
    }
  }
}

bool InstructionSelector::IsTailCallAddressImmediate() { return false; }

void InstructionSelector::VisitFloat64ExtractLowWord32(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcDoubleExtractLowWord32, g.DefineAsRegister(node),
       g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitFloat64ExtractHighWord32(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcDoubleExtractHighWord32, g.DefineAsRegister(node),
       g.UseRegister(op.input(0)));
}

void InstructionSelector::VisitBitcastWord32PairToFloat64(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const auto& bitcast = this->Cast<BitcastWord32PairToFloat64Op>(node);
  OpIndex hi = bitcast.high_word32();
  OpIndex lo = bitcast.low_word32();

  InstructionOperand temps[] = {g.TempRegister()};
  Emit(kSparcDoubleFromWord32Pair, g.DefineAsRegister(node), g.UseRegister(hi),
       g.UseRegister(lo), arraysize(temps), temps);
}

void InstructionSelector::VisitFloat64InsertLowWord32(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitFloat64InsertHighWord32(OpIndex node) {
  UNIMPLEMENTED();
}

void InstructionSelector::VisitMemoryBarrier(OpIndex node) {
  Sparc64OperandGenerator g(this);
  Emit(kSparcSync, g.NoOutput());
}

void InstructionSelector::VisitWord32AtomicLoad(OpIndex node) {
  auto load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode = SelectLoadOpcode(load.loaded_rep(), &mode);
  VisitLoadCommon(this, node, mode, opcode);
}

void InstructionSelector::VisitWord64AtomicLoad(OpIndex node) {
  auto load = load_view(node);
  ImmediateMode mode;
  InstructionCode opcode = SelectLoadOpcode(load.loaded_rep(), &mode);
  VisitLoadCommon(this, node, mode, opcode);
}

void InstructionSelector::VisitWord32AtomicStore(OpIndex node) {
  auto store = store_view(node);
  AtomicStoreParameters store_params(store.stored_rep().representation(),
                                     store.stored_rep().write_barrier_kind(),
                                     store.memory_order().value(),
                                     store.access_kind());
  VisitStoreCommon(this, node, store_params.store_representation(),
                   store_params.order());
}

void InstructionSelector::VisitWord64AtomicStore(OpIndex node) {
  auto store = store_view(node);
  AtomicStoreParameters store_params(store.stored_rep().representation(),
                                     store.stored_rep().write_barrier_kind(),
                                     store.memory_order().value(),
                                     store.access_kind());
  VisitStoreCommon(this, node, store_params.store_representation(),
                   store_params.order());
}

void VisitAtomicExchange(InstructionSelector* selector, OpIndex node,
                         ArchOpcode opcode) {
  Sparc64OperandGenerator g(selector);
  const AtomicRMWOp& atomic_op = selector->Cast<AtomicRMWOp>(node);
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex value = atomic_op.value();

  AddressingMode addressing_mode = kMode_MRR;
  InstructionOperand inputs[3];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionCode code =
      opcode | AddressingModeField::encode(addressing_mode) |
      MiscField::encode(atomic_op.base_rep == RegisterRepresentation::Tagged());
  InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
  selector->Emit(code, 1, outputs, input_count, inputs, arraysize(temps),
                 temps);
}

void InstructionSelector::VisitWord32AtomicExchange(OpIndex node) {
  ArchOpcode opcode;
  const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
  if (atomic_op.memory_rep == MemoryRepresentation::Int8()) {
    opcode = kAtomicExchangeInt8;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
    opcode = kSparcAtomicExchangeUint8;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int16()) {
    opcode = kAtomicExchangeInt16;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
    opcode = kSparcAtomicExchangeUint16;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int32() ||
             atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
    opcode = kSparcAtomicExchangeWord32;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode);
}

void InstructionSelector::VisitWord64AtomicExchange(OpIndex node) {
  ArchOpcode opcode;
  const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
  if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
    opcode = kSparcAtomicExchangeUint8;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
    opcode = kSparcAtomicExchangeUint16;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
    opcode = kSparcAtomicExchangeWord32;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint64()) {
    opcode = kSparcAtomicExchangeWord64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicExchange(this, node, opcode);
}

void VisitAtomicCompareExchange(InstructionSelector* selector, OpIndex node,
                                ArchOpcode opcode) {
  Sparc64OperandGenerator g(selector);
  const AtomicRMWOp& atomic_op = selector->Cast<AtomicRMWOp>(node);
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex old_value = atomic_op.expected().value();
  OpIndex new_value = atomic_op.value();

  AddressingMode addressing_mode = kMode_MRR;
  InstructionCode code =
      opcode | AddressingModeField::encode(addressing_mode) |
      MiscField::encode(atomic_op.base_rep == RegisterRepresentation::Tagged());

  InstructionOperand inputs[4];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(old_value);
  inputs[input_count++] = g.UseUniqueRegister(new_value);

  InstructionOperand outputs[1];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
  selector->Emit(code, output_count, outputs, input_count, inputs,
                 arraysize(temps), temps);
}

void InstructionSelector::VisitTaggedAtomicExchange(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const AtomicRMWOp& atomic_op = Cast<AtomicRMWOp>(node);
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex value = atomic_op.value();

  AddressingMode addressing_mode = kMode_MRR;
  InstructionOperand inputs[3];
  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);
  InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
  size_t const temp_count = arraysize(temps);
  InstructionOperand outputs[1];
  outputs[0] = g.UseUniqueRegister(node);
  InstructionCode code = kAtomicExchangeWithWriteBarrier |
                         AddressingModeField::encode(addressing_mode);
  Emit(code, 1, outputs, input_count, inputs, temp_count, temps);
}

void InstructionSelector::VisitWord32AtomicCompareExchange(OpIndex node) {
  ArchOpcode opcode;
  const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
  if (atomic_op.memory_rep == MemoryRepresentation::Int8()) {
    opcode = kAtomicCompareExchangeInt8;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
    opcode = kSparcAtomicCompareExchangeUint8;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int16()) {
    opcode = kAtomicCompareExchangeInt16;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
    opcode = kSparcAtomicCompareExchangeUint16;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int32() ||
             atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
    opcode = kSparcAtomicCompareExchangeWord32;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode);
}

void InstructionSelector::VisitWord64AtomicCompareExchange(OpIndex node) {
  ArchOpcode opcode;
  const AtomicRMWOp& atomic_op = this->Get(node).template Cast<AtomicRMWOp>();
  if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
    opcode = kSparcAtomicCompareExchangeUint8;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
    opcode = kSparcAtomicCompareExchangeUint16;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
    opcode = kSparcAtomicCompareExchangeWord32;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint64()) {
    opcode = kSparcAtomicCompareExchangeWord64;
  } else {
    UNREACHABLE();
  }
  VisitAtomicCompareExchange(this, node, opcode);
}

void VisitAtomicBinaryOperation(InstructionSelector* selector, OpIndex node,
                                ArchOpcode int8_op, ArchOpcode uint8_op,
                                ArchOpcode int16_op, ArchOpcode uint16_op,
                                ArchOpcode int32_op, ArchOpcode uint32_op,
                                ArchOpcode int64_op, ArchOpcode uint64_op) {
  Sparc64OperandGenerator g(selector);
  ArchOpcode opcode;
  const AtomicRMWOp& atomic_op =
      selector->Get(node).template Cast<AtomicRMWOp>();
  OpIndex base = atomic_op.base();
  OpIndex index = atomic_op.index();
  OpIndex value = atomic_op.value();
  if (atomic_op.memory_rep == MemoryRepresentation::Int8()) {
    opcode = int8_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint8()) {
    opcode = uint8_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int16()) {
    opcode = int16_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint16()) {
    opcode = uint16_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int32()) {
    opcode = int32_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint32()) {
    opcode = uint32_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Int64()) {
    opcode = int64_op;
  } else if (atomic_op.memory_rep == MemoryRepresentation::Uint64()) {
    opcode = uint64_op;
  } else {
    UNREACHABLE();
  }

  AddressingMode addressing_mode = kMode_MRR;
  InstructionCode code =
      opcode | AddressingModeField::encode(addressing_mode) |
      MiscField::encode(atomic_op.base_rep == RegisterRepresentation::Tagged());
  InstructionOperand inputs[3];

  size_t input_count = 0;
  inputs[input_count++] = g.UseUniqueRegister(base);
  inputs[input_count++] = g.UseUniqueRegister(index);
  inputs[input_count++] = g.UseUniqueRegister(value);

  InstructionOperand outputs[1];
  size_t output_count = 0;
  outputs[output_count++] = g.DefineAsRegister(node);

  InstructionOperand temps[] = {g.TempRegister(), g.TempRegister()};
  selector->Emit(code, output_count, outputs, input_count, inputs,
                 arraysize(temps), temps);
}

void InstructionSelector::VisitTaggedAtomicCompareExchange(OpIndex node) {
  VisitAtomicCompareExchange(this, node,
                             kAtomicCompareExchangeWithWriteBarrier);
}

void InstructionSelector::VisitWord32AtomicBinaryOperation(
    OpIndex node, ArchOpcode int8_op, ArchOpcode uint8_op, ArchOpcode int16_op,
    ArchOpcode uint16_op, ArchOpcode word32_op) {
  // Unused
  UNREACHABLE();
}

void InstructionSelector::VisitWord64AtomicBinaryOperation(
    OpIndex node, ArchOpcode uint8_op, ArchOpcode uint16_op,
    ArchOpcode uint32_op, ArchOpcode uint64_op) {
  // Unused
  UNREACHABLE();
}

#define VISIT_ATOMIC_BINOP(op)                                       \
  void InstructionSelector::VisitWord32Atomic##op(OpIndex node) {    \
    VisitAtomicBinaryOperation(                                      \
        this, node, kSparcAtomic##op##Int8, kSparcAtomic##op##Uint8, \
        kSparcAtomic##op##Int16, kSparcAtomic##op##Uint16,           \
        kSparcAtomic##op##Int32, kSparcAtomic##op##Uint32,           \
        kSparcAtomic##op##Int64, kSparcAtomic##op##Uint64);          \
  }                                                                  \
  void InstructionSelector::VisitWord64Atomic##op(OpIndex node) {    \
    VisitAtomicBinaryOperation(                                      \
        this, node, kSparcAtomic##op##Int8, kSparcAtomic##op##Uint8, \
        kSparcAtomic##op##Int16, kSparcAtomic##op##Uint16,           \
        kSparcAtomic##op##Int32, kSparcAtomic##op##Uint32,           \
        kSparcAtomic##op##Int64, kSparcAtomic##op##Uint64);          \
  }
VISIT_ATOMIC_BINOP(Add)
VISIT_ATOMIC_BINOP(Sub)
VISIT_ATOMIC_BINOP(And)
VISIT_ATOMIC_BINOP(Or)
VISIT_ATOMIC_BINOP(Xor)
#undef VISIT_ATOMIC_BINOP

void InstructionSelector::VisitInt32AbsWithOverflow(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitInt64AbsWithOverflow(OpIndex node) {
  UNREACHABLE();
}

#if V8_ENABLE_WEBASSEMBLY
#define SIMD_TYPES(V) \
  V(F64x2)            \
  V(F32x4)            \
  V(I64x2)            \
  V(I32x4)            \
  V(I16x8)            \
  V(I8x16)

#define SIMD_BINOP_LIST(V) \
  V(F64x2Add)              \
  V(F64x2Sub)              \
  V(F64x2Mul)              \
  V(F64x2Eq)               \
  V(F64x2Ne)               \
  V(F64x2Le)               \
  V(F64x2Lt)               \
  V(F64x2Div)              \
  V(F64x2Min)              \
  V(F64x2Max)              \
  V(F64x2Pmin)             \
  V(F64x2Pmax)             \
  V(F32x4Add)              \
  V(F32x4Sub)              \
  V(F32x4Mul)              \
  V(F32x4Eq)               \
  V(F32x4Ne)               \
  V(F32x4Lt)               \
  V(F32x4Le)               \
  V(F32x4Div)              \
  V(F32x4Min)              \
  V(F32x4Max)              \
  V(F32x4Pmin)             \
  V(F32x4Pmax)             \
  V(I64x2Add)              \
  V(I64x2Sub)              \
  V(I64x2Mul)              \
  V(I64x2Eq)               \
  V(I64x2Ne)               \
  V(I64x2ExtMulLowI32x4S)  \
  V(I64x2ExtMulHighI32x4S) \
  V(I64x2ExtMulLowI32x4U)  \
  V(I64x2ExtMulHighI32x4U) \
  V(I64x2GtS)              \
  V(I64x2GeS)              \
  V(I64x2Shl)              \
  V(I64x2ShrS)             \
  V(I64x2ShrU)             \
  V(I32x4Add)              \
  V(I32x4Sub)              \
  V(I32x4Mul)              \
  V(I32x4MinS)             \
  V(I32x4MinU)             \
  V(I32x4MaxS)             \
  V(I32x4MaxU)             \
  V(I32x4Eq)               \
  V(I32x4Ne)               \
  V(I32x4GtS)              \
  V(I32x4GeS)              \
  V(I32x4GtU)              \
  V(I32x4GeU)              \
  V(I32x4DotI16x8S)        \
  V(I32x4ExtMulLowI16x8S)  \
  V(I32x4ExtMulHighI16x8S) \
  V(I32x4ExtMulLowI16x8U)  \
  V(I32x4ExtMulHighI16x8U) \
  V(I32x4Shl)              \
  V(I32x4ShrS)             \
  V(I32x4ShrU)             \
  V(I16x8Add)              \
  V(I16x8Sub)              \
  V(I16x8Mul)              \
  V(I16x8MinS)             \
  V(I16x8MinU)             \
  V(I16x8MaxS)             \
  V(I16x8MaxU)             \
  V(I16x8Eq)               \
  V(I16x8Ne)               \
  V(I16x8GtS)              \
  V(I16x8GeS)              \
  V(I16x8GtU)              \
  V(I16x8GeU)              \
  V(I16x8SConvertI32x4)    \
  V(I16x8UConvertI32x4)    \
  V(I16x8AddSatS)          \
  V(I16x8SubSatS)          \
  V(I16x8AddSatU)          \
  V(I16x8SubSatU)          \
  V(I16x8RoundingAverageU) \
  V(I16x8Q15MulRSatS)      \
  V(I16x8ExtMulLowI8x16S)  \
  V(I16x8ExtMulHighI8x16S) \
  V(I16x8ExtMulLowI8x16U)  \
  V(I16x8ExtMulHighI8x16U) \
  V(I16x8Shl)              \
  V(I16x8ShrS)             \
  V(I16x8ShrU)             \
  V(I8x16Add)              \
  V(I8x16Sub)              \
  V(I8x16MinS)             \
  V(I8x16MinU)             \
  V(I8x16MaxS)             \
  V(I8x16MaxU)             \
  V(I8x16Eq)               \
  V(I8x16Ne)               \
  V(I8x16GtS)              \
  V(I8x16GeS)              \
  V(I8x16GtU)              \
  V(I8x16GeU)              \
  V(I8x16SConvertI16x8)    \
  V(I8x16UConvertI16x8)    \
  V(I8x16AddSatS)          \
  V(I8x16SubSatS)          \
  V(I8x16AddSatU)          \
  V(I8x16SubSatU)          \
  V(I8x16RoundingAverageU) \
  V(I8x16Swizzle)          \
  V(I8x16Shl)              \
  V(I8x16ShrS)             \
  V(I8x16ShrU)             \
  V(S128And)               \
  V(S128Or)                \
  V(S128Xor)               \
  V(S128AndNot)

#define SIMD_UNOP_LIST(V)      \
  V(F64x2Abs)                  \
  V(F64x2Neg)                  \
  V(F64x2Sqrt)                 \
  V(F64x2Ceil)                 \
  V(F64x2Floor)                \
  V(F64x2Trunc)                \
  V(F64x2ConvertLowI32x4S)     \
  V(F64x2ConvertLowI32x4U)     \
  V(F64x2PromoteLowF32x4)      \
  V(F32x4Abs)                  \
  V(F32x4Neg)                  \
  V(F32x4Sqrt)                 \
  V(F32x4SConvertI32x4)        \
  V(F32x4UConvertI32x4)        \
  V(F32x4Ceil)                 \
  V(F32x4Floor)                \
  V(F32x4Trunc)                \
  V(F32x4DemoteF64x2Zero)      \
  V(I64x2Abs)                  \
  V(I64x2Neg)                  \
  V(I64x2SConvertI32x4Low)     \
  V(I64x2SConvertI32x4High)    \
  V(I64x2UConvertI32x4Low)     \
  V(I64x2UConvertI32x4High)    \
  V(I64x2AllTrue)              \
  V(I64x2BitMask)              \
  V(I32x4Neg)                  \
  V(I32x4Abs)                  \
  V(I32x4SConvertF32x4)        \
  V(I32x4UConvertF32x4)        \
  V(I32x4SConvertI16x8Low)     \
  V(I32x4SConvertI16x8High)    \
  V(I32x4UConvertI16x8Low)     \
  V(I32x4UConvertI16x8High)    \
  V(I32x4ExtAddPairwiseI16x8S) \
  V(I32x4ExtAddPairwiseI16x8U) \
  V(I32x4TruncSatF64x2SZero)   \
  V(I32x4TruncSatF64x2UZero)   \
  V(I32x4AllTrue)              \
  V(I32x4BitMask)              \
  V(I16x8Neg)                  \
  V(I16x8Abs)                  \
  V(I16x8AllTrue)              \
  V(I16x8BitMask)              \
  V(I8x16Neg)                  \
  V(I8x16Abs)                  \
  V(I8x16Popcnt)               \
  V(I8x16AllTrue)              \
  V(I8x16BitMask)              \
  V(I16x8SConvertI8x16Low)     \
  V(I16x8SConvertI8x16High)    \
  V(I16x8UConvertI8x16Low)     \
  V(I16x8UConvertI8x16High)    \
  V(I16x8ExtAddPairwiseI8x16S) \
  V(I16x8ExtAddPairwiseI8x16U) \
  V(S128Not)                   \
  V(V128AnyTrue)

#define SIMD_VISIT_SPLAT(Type, T, LaneSize)                     \
  void InstructionSelector::Visit##Type##Splat(OpIndex node) {  \
    Sparc64OperandGenerator g(this);                            \
    const Operation& op = this->Get(node);                      \
    Emit(kSparc##T##Splat | LaneSizeField::encode(LaneSize),    \
         g.DefineAsRegister(node), g.UseRegister(op.input(0))); \
  }
SIMD_VISIT_SPLAT(F64x2, F, 64)
SIMD_VISIT_SPLAT(F32x4, F, 32)
SIMD_VISIT_SPLAT(I64x2, I, 64)
SIMD_VISIT_SPLAT(I32x4, I, 32)
SIMD_VISIT_SPLAT(I16x8, I, 16)
SIMD_VISIT_SPLAT(I8x16, I, 8)
#undef SIMD_VISIT_SPLAT

#define SIMD_VISIT_EXTRACT_LANE(Type, T, Sign, LaneSize)                   \
  void InstructionSelector::Visit##Type##ExtractLane##Sign(OpIndex node) { \
    Sparc64OperandGenerator g(this);                                       \
    int32_t lane;                                                          \
    const Operation& op = this->Get(node);                                 \
    lane = op.template Cast<Simd128ExtractLaneOp>().lane;                  \
    Emit(kSparc##T##ExtractLane##Sign | LaneSizeField::encode(LaneSize),   \
         g.DefineAsRegister(node), g.UseRegister(op.input(0)),             \
         g.UseImmediate(lane));                                            \
  }
SIMD_VISIT_EXTRACT_LANE(F64x2, F, , 64)
SIMD_VISIT_EXTRACT_LANE(F32x4, F, , 32)
SIMD_VISIT_EXTRACT_LANE(I64x2, I, , 64)
SIMD_VISIT_EXTRACT_LANE(I32x4, I, , 32)
SIMD_VISIT_EXTRACT_LANE(I16x8, I, U, 16)
SIMD_VISIT_EXTRACT_LANE(I16x8, I, S, 16)
SIMD_VISIT_EXTRACT_LANE(I8x16, I, U, 8)
SIMD_VISIT_EXTRACT_LANE(I8x16, I, S, 8)
#undef SIMD_VISIT_EXTRACT_LANE

#define SIMD_VISIT_REPLACE_LANE(Type, T, LaneSize)                   \
  void InstructionSelector::Visit##Type##ReplaceLane(OpIndex node) { \
    Sparc64OperandGenerator g(this);                                 \
    int32_t lane;                                                    \
    const Operation& op = this->Get(node);                           \
    lane = op.template Cast<Simd128ReplaceLaneOp>().lane;            \
    Emit(kSparc##T##ReplaceLane | LaneSizeField::encode(LaneSize),   \
         g.DefineSameAsFirst(node), g.UseRegister(op.input(0)),      \
         g.UseImmediate(lane), g.UseRegister(op.input(1)));          \
  }
SIMD_VISIT_REPLACE_LANE(F64x2, F, 64)
SIMD_VISIT_REPLACE_LANE(F32x4, F, 32)
SIMD_VISIT_REPLACE_LANE(I64x2, I, 64)
SIMD_VISIT_REPLACE_LANE(I32x4, I, 32)
SIMD_VISIT_REPLACE_LANE(I16x8, I, 16)
SIMD_VISIT_REPLACE_LANE(I8x16, I, 8)
#undef SIMD_VISIT_REPLACE_LANE

#define SIMD_VISIT_BINOP(Opcode)                                               \
  void InstructionSelector::Visit##Opcode(OpIndex node) {                      \
    Sparc64OperandGenerator g(this);                                           \
    const Operation& op = this->Get(node);                                     \
    InstructionOperand temps[] = {g.TempRegister()};                           \
    Emit(kSparc##Opcode, g.DefineAsRegister(node), g.UseRegister(op.input(0)), \
         g.UseRegister(op.input(1)), arraysize(temps), temps);                 \
  }
SIMD_BINOP_LIST(SIMD_VISIT_BINOP)
#undef SIMD_VISIT_BINOP
#undef SIMD_BINOP_LIST

#define SIMD_VISIT_UNOP(Opcode)                           \
  void InstructionSelector::Visit##Opcode(OpIndex node) { \
    Sparc64OperandGenerator g(this);                      \
    const Operation& op = this->Get(node);                \
    Emit(kSparc##Opcode, g.DefineAsRegister(node),        \
         g.UseRegister(op.input(0)));                     \
  }
SIMD_UNOP_LIST(SIMD_VISIT_UNOP)
#undef SIMD_VISIT_UNOP
#undef SIMD_UNOP_LIST

#define SIMD_VISIT_QFMOP(Opcode)                                 \
  void InstructionSelector::Visit##Opcode(OpIndex node) {        \
    Sparc64OperandGenerator g(this);                             \
    const Operation& op = this->Get(node);                       \
    Emit(kSparc##Opcode, g.DefineSameAsFirst(node),              \
         g.UseRegister(op.input(0)), g.UseRegister(op.input(1)), \
         g.UseRegister(op.input(2)));                            \
  }
SIMD_VISIT_QFMOP(F64x2Qfma)
SIMD_VISIT_QFMOP(F64x2Qfms)
SIMD_VISIT_QFMOP(F32x4Qfma)
SIMD_VISIT_QFMOP(F32x4Qfms)
#undef SIMD_VISIT_QFMOP

#define SIMD_RELAXED_OP_LIST(V)                           \
  V(F64x2RelaxedMin, F64x2Pmin)                           \
  V(F64x2RelaxedMax, F64x2Pmax)                           \
  V(F32x4RelaxedMin, F32x4Pmin)                           \
  V(F32x4RelaxedMax, F32x4Pmax)                           \
  V(I32x4RelaxedTruncF32x4S, I32x4SConvertF32x4)          \
  V(I32x4RelaxedTruncF32x4U, I32x4UConvertF32x4)          \
  V(I32x4RelaxedTruncF64x2SZero, I32x4TruncSatF64x2SZero) \
  V(I32x4RelaxedTruncF64x2UZero, I32x4TruncSatF64x2UZero) \
  V(I16x8RelaxedQ15MulRS, I16x8Q15MulRSatS)               \
  V(I8x16RelaxedLaneSelect, S128Select)                   \
  V(I16x8RelaxedLaneSelect, S128Select)                   \
  V(I32x4RelaxedLaneSelect, S128Select)                   \
  V(I64x2RelaxedLaneSelect, S128Select)

#define SIMD_VISIT_RELAXED_OP(name, op) \
  void InstructionSelector::Visit##name(OpIndex node) { Visit##op(node); }
SIMD_RELAXED_OP_LIST(SIMD_VISIT_RELAXED_OP)
#undef SIMD_VISIT_RELAXED_OP
#undef SIMD_RELAXED_OP_LIST

#define F16_OP_LIST(V)    \
  V(F16x8Splat)           \
  V(F16x8ExtractLane)     \
  V(F16x8ReplaceLane)     \
  V(F16x8Abs)             \
  V(F16x8Neg)             \
  V(F16x8Sqrt)            \
  V(F16x8Floor)           \
  V(F16x8Ceil)            \
  V(F16x8Trunc)           \
  V(F16x8NearestInt)      \
  V(F16x8Add)             \
  V(F16x8Sub)             \
  V(F16x8Mul)             \
  V(F16x8Div)             \
  V(F16x8Min)             \
  V(F16x8Max)             \
  V(F16x8Pmin)            \
  V(F16x8Pmax)            \
  V(F16x8Eq)              \
  V(F16x8Ne)              \
  V(F16x8Lt)              \
  V(F16x8Le)              \
  V(F16x8SConvertI16x8)   \
  V(F16x8UConvertI16x8)   \
  V(I16x8SConvertF16x8)   \
  V(I16x8UConvertF16x8)   \
  V(F32x4PromoteLowF16x8) \
  V(F16x8DemoteF32x4Zero) \
  V(F16x8DemoteF64x2Zero) \
  V(F16x8Qfma)            \
  V(F16x8Qfms)

#define VISIT_F16_OP(name) \
  void InstructionSelector::Visit##name(OpIndex node) { UNIMPLEMENTED(); }
F16_OP_LIST(VISIT_F16_OP)
#undef VISIT_F16_OP
#undef F16_OP_LIST
#undef SIMD_TYPES

#if V8_ENABLE_WEBASSEMBLY
void InstructionSelector::VisitI8x16Shuffle(OpIndex node) {
  uint8_t shuffle[kSimd128Size];
  bool is_swizzle;
  // TODO(nicohartmann@): Properly use view here once Turboshaft support is
  // implemented.
  auto view = this->simd_shuffle_view(node);
  CanonicalizeShuffle(view, shuffle, &is_swizzle);
  Sparc64OperandGenerator g(this);
  OpIndex input0 = view.input(0);
  OpIndex input1 = view.input(1);
  Emit(kSparcI8x16Shuffle, g.DefineAsRegister(node), g.UseRegister(input0),
       g.UseRegister(input1),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle + 4)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle + 8)),
       g.UseImmediate(wasm::SimdShuffle::Pack4Lanes(shuffle + 12)));
}

void InstructionSelector::VisitSetStackPointer(OpIndex node) {
  OperandGenerator g(this);
  const SetStackPointerOp& op = Cast<SetStackPointerOp>(node);
  // TODO(miladfarca): Optimize by using UseAny.
  auto input = g.UseRegister(op.input(0));
  Emit(kArchSetStackPointer, 0, nullptr, 1, &input);
}

#else
void InstructionSelector::VisitI8x16Shuffle(OpIndex node) { UNREACHABLE(); }
#endif  // V8_ENABLE_WEBASSEMBLY

void InstructionSelector::VisitS128Zero(OpIndex node) {
  Sparc64OperandGenerator g(this);
  Emit(kSparcS128Zero, g.DefineAsRegister(node));
}

void InstructionSelector::VisitS128Select(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Simd128TernaryOp& op = Cast<Simd128TernaryOp>(node);
  Emit(kSparcS128Select, g.DefineAsRegister(node), g.UseRegister(op.input(0)),
       g.UseRegister(op.input(1)), g.UseRegister(op.input(2)));
}

// This is a replica of SimdShuffle::Pack4Lanes. However, above function will
// not be available on builds with webassembly disabled, hence we need to have
// it declared locally as it is used on other visitors such as S128Const.
static int32_t Pack4Lanes(const uint8_t* shuffle) {
  int32_t result = 0;
  for (int i = 3; i >= 0; --i) {
    result <<= 8;
    result |= shuffle[i];
  }
  return result;
}

void InstructionSelector::VisitS128Const(OpIndex node) {
  Sparc64OperandGenerator g(this);
  uint32_t val[kSimd128Size / sizeof(uint32_t)];
  const Simd128ConstantOp& constant =
      this->Get(node).template Cast<Simd128ConstantOp>();
  memcpy(val, constant.value, kSimd128Size);
  // If all bytes are zeros, avoid emitting code for generic constants.
  bool all_zeros = !(val[0] || val[1] || val[2] || val[3]);
  bool all_ones = val[0] == UINT32_MAX && val[1] == UINT32_MAX &&
                  val[2] == UINT32_MAX && val[3] == UINT32_MAX;
  InstructionOperand dst = g.DefineAsRegister(node);
  if (all_zeros) {
    Emit(kSparcS128Zero, dst);
  } else if (all_ones) {
    Emit(kSparcS128AllOnes, dst);
  } else {
    // We have to use Pack4Lanes to reverse the bytes (lanes) on BE,
    // Which in this case is ineffective on LE.
    Emit(kSparcS128Const, g.DefineAsRegister(node),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]))),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]) + 4)),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]) + 8)),
         g.UseImmediate(Pack4Lanes(reinterpret_cast<uint8_t*>(&val[0]) + 12)));
  }
}

void InstructionSelector::VisitI16x8DotI8x16I7x16S(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Operation& op = this->Get(node);
  Emit(kSparcI16x8DotI8x16S, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)), g.UseUniqueRegister(op.input(1)));
}

void InstructionSelector::VisitI32x4DotI8x16I7x16AddS(OpIndex node) {
  Sparc64OperandGenerator g(this);
  const Simd128TernaryOp& op = Cast<Simd128TernaryOp>(node);
  Emit(kSparcI32x4DotI8x16AddS, g.DefineAsRegister(node),
       g.UseUniqueRegister(op.input(0)), g.UseUniqueRegister(op.input(1)),
       g.UseUniqueRegister(op.input(2)));
}

#endif  // V8_ENABLE_WEBASSEMBLY

void InstructionSelector::EmitPrepareResults(
    ZoneVector<PushParameter>* results, const CallDescriptor* call_descriptor,
    OpIndex node) {
  Sparc64OperandGenerator g(this);

  for (PushParameter output : *results) {
    if (!output.location.IsCallerFrameSlot()) continue;
    // Skip any alignment holes in nodes.
    if (output.node.valid()) {
      DCHECK(!call_descriptor->IsCFunctionCall());
      if (output.location.GetType() == MachineType::Float32()) {
        MarkAsFloat32(output.node);
      } else if (output.location.GetType() == MachineType::Float64()) {
        MarkAsFloat64(output.node);
      } else if (output.location.GetType() == MachineType::Simd128()) {
        MarkAsSimd128(output.node);
      }
      int offset = call_descriptor->GetOffsetToReturns();
      int reverse_slot = -output.location.GetLocation() - offset;
      Emit(kSparcPeek, g.DefineAsRegister(output.node),
           g.UseImmediate(reverse_slot));
    }
  }
}

#if V8_ENABLE_WEBASSEMBLY
void InstructionSelector::VisitLoadLane(OpIndex node) {
  Sparc64OperandGenerator g(this);
  InstructionCode opcode = kArchNop;
  const Simd128LaneMemoryOp& load =
      this->Get(node).template Cast<Simd128LaneMemoryOp>();
  switch (load.lane_kind) {
    case Simd128LaneMemoryOp::LaneKind::k8:
      opcode = kSparcS128Load8Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k16:
      opcode = kSparcS128Load16Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k32:
      opcode = kSparcS128Load32Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k64:
      opcode = kSparcS128Load64Lane;
      break;
  }
  Emit(opcode | AddressingModeField::encode(kMode_MRR),
       g.DefineSameAsFirst(node), g.UseRegister(load.value()),
       g.UseRegister(load.base()), g.UseRegister(load.index()),
       g.UseImmediate(load.lane));
}

void InstructionSelector::VisitLoadTransform(OpIndex node) {
  Sparc64OperandGenerator g(this);
  ArchOpcode opcode;
  const Simd128LoadTransformOp& op =
      this->Get(node).template Cast<Simd128LoadTransformOp>();
  OpIndex base = op.base();
  OpIndex index = op.index();

  switch (op.transform_kind) {
    case Simd128LoadTransformOp::TransformKind::k8Splat:
      opcode = kSparcS128Load8Splat;
      break;
    case Simd128LoadTransformOp::TransformKind::k16Splat:
      opcode = kSparcS128Load16Splat;
      break;
    case Simd128LoadTransformOp::TransformKind::k32Splat:
      opcode = kSparcS128Load32Splat;
      break;
    case Simd128LoadTransformOp::TransformKind::k64Splat:
      opcode = kSparcS128Load64Splat;
      break;
    case Simd128LoadTransformOp::TransformKind::k8x8S:
      opcode = kSparcS128Load8x8S;
      break;
    case Simd128LoadTransformOp::TransformKind::k8x8U:
      opcode = kSparcS128Load8x8U;
      break;
    case Simd128LoadTransformOp::TransformKind::k16x4S:
      opcode = kSparcS128Load16x4S;
      break;
    case Simd128LoadTransformOp::TransformKind::k16x4U:
      opcode = kSparcS128Load16x4U;
      break;
    case Simd128LoadTransformOp::TransformKind::k32x2S:
      opcode = kSparcS128Load32x2S;
      break;
    case Simd128LoadTransformOp::TransformKind::k32x2U:
      opcode = kSparcS128Load32x2U;
      break;
    case Simd128LoadTransformOp::TransformKind::k32Zero:
      opcode = kSparcS128Load32Zero;
      break;
    case Simd128LoadTransformOp::TransformKind::k64Zero:
      opcode = kSparcS128Load64Zero;
      break;
    default:
      UNIMPLEMENTED();
  }
  Emit(opcode | AddressingModeField::encode(kMode_MRR),
       g.DefineAsRegister(node), g.UseRegister(base), g.UseRegister(index));
}

void InstructionSelector::VisitStoreLane(OpIndex node) {
  Sparc64OperandGenerator g(this);
  InstructionCode opcode = kArchNop;
  InstructionOperand inputs[4];
  const Simd128LaneMemoryOp& store =
      this->Get(node).template Cast<Simd128LaneMemoryOp>();
  switch (store.lane_kind) {
    case Simd128LaneMemoryOp::LaneKind::k8:
      opcode = kSparcS128Store8Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k16:
      opcode = kSparcS128Store16Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k32:
      opcode = kSparcS128Store32Lane;
      break;
    case Simd128LaneMemoryOp::LaneKind::k64:
      opcode = kSparcS128Store64Lane;
      break;
  }
  inputs[0] = g.UseRegister(store.value());
  inputs[1] = g.UseRegister(store.base());
  inputs[2] = g.UseRegister(store.index());
  inputs[3] = g.UseImmediate(store.lane);
  Emit(opcode | AddressingModeField::encode(kMode_MRR), 0, nullptr, 4, inputs);
}

#endif  // V8_ENABLE_WEBASSEMBLY

void InstructionSelector::AddOutputToSelectContinuation(OperandGenerator* g,
                                                        int first_input_index,
                                                        OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitFloat32RoundTiesEven(OpIndex node) {
  UNREACHABLE();
}

void InstructionSelector::VisitFloat64RoundTiesEven(OpIndex node) {
  UNREACHABLE();
}

#if V8_ENABLE_WEBASSEMBLY
void InstructionSelector::VisitF64x2NearestInt(OpIndex node) { UNREACHABLE(); }

void InstructionSelector::VisitF32x4NearestInt(OpIndex node) { UNREACHABLE(); }

#endif  // V8_ENABLE_WEBASSEMBLY

MachineOperatorBuilder::Flags
InstructionSelector::SupportedMachineOperatorFlags() {
  return MachineOperatorBuilder::kWord32ShiftIsSafe;
}

MachineOperatorBuilder::AlignmentRequirements
InstructionSelector::AlignmentRequirements() {
  return MachineOperatorBuilder::AlignmentRequirements::
      NoUnalignedAccessSupport();
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8
