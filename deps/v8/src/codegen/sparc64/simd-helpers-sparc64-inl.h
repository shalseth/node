// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_CODEGEN_SPARC64_SIMD_HELPERS_SPARC64_INL_H_
#define V8_CODEGEN_SPARC64_SIMD_HELPERS_SPARC64_INL_H_

#include "src/codegen/macro-assembler.h"
#include "src/compiler/backend/sparc64/code-generator-sparc64-inl.h"

namespace v8::internal::sparc64 {

inline void MemoryAddress(MacroAssembler* assm, Register base, Register offset,
                          uintptr_t immediate, bool offset64,
                          unsigned shift = 0) {
  if (offset == no_reg)
    assm->MacroAssembler::Move(g2, base);
  else {
    if (offset64)
      assm->MacroAssembler::Move(g1, offset);
    else
      assm->srl(g1, offset, 0);
    if (shift) assm->sllx(g1, g1, shift);
    assm->add(g2, base, g1);
  }
  assm->MacroAssembler::Move(g3, immediate);
  assm->add(g2, g2, g3);
}

inline void Branch(MacroAssembler* assm, Condition cond, ConditionCodes cc,
                   Label* target) {
  Label skip;
  assm->b(static_cast<Condition>(cond ^ 8), cc, &skip);
  assm->nop();
  assm->Jump(target);
  assm->bind(&skip);
}
inline void CanonicalizeNaN(MacroAssembler* assm, DoubleRegister dst,
                            bool single) {
  Label done;
  if (single)
    assm->fcmps(dst, dst);
  else
    assm->fcmpd(dst, dst);
  assm->fb(kFOrdered, 0, &done);
  assm->nop();
  assm->MacroAssembler::Move(g1, single
                                     ? intptr_t{0x7fc00000}
                                     : intptr_t{INT64_C(0x7ff8000000000000)});
  if (single)
    assm->MovIntToFloat(dst, g1);
  else
    assm->MovInt64ToDouble(dst, g1);
  assm->bind(&done);
}
inline void SaturatingFloatToInteger(MacroAssembler* assm, Register dst,
                                     DoubleRegister src, bool word32,
                                     bool is_unsigned, bool single) {
  compiler::EmitSparc64FloatToInteger(assm, dst, g4, src, word32, is_unsigned,
                                      single);
  Label done, zero, negative;
  assm->cmp(g4, 0);
  Branch(assm, not_equal, ConditionCodes::kIcc, &done);
  if (single)
    assm->fcmps(src, src);
  else
    assm->fcmpd(src, src);
  assm->fb(kFUnordered, 0, &zero);
  assm->nop();
  if (single) {
    assm->MovFloatToInt(g1, src);
    assm->sra(g1, g1, 0);
  } else
    assm->MovDoubleToInt64(g1, src);
  assm->cmp(g1, 0);
  Branch(assm, less, ConditionCodes::kXcc, &negative);
  if (is_unsigned)
    assm->MacroAssembler::Move(dst,
                               word32 ? static_cast<intptr_t>(UINT32_MAX) : -1);
  else
    assm->MacroAssembler::Move(
        dst, word32 ? static_cast<intptr_t>(INT32_MAX) : INT64_MAX);
  assm->Jump(&done);
  assm->bind(&negative);
  if (is_unsigned)
    assm->Jump(&zero);
  else {
    assm->MacroAssembler::Move(
        dst, word32 ? static_cast<intptr_t>(INT32_MIN) : INT64_MIN);
    assm->Jump(&done);
  }
  assm->bind(&zero);
  assm->MacroAssembler::Move(dst, 0);
  assm->bind(&done);
}

// Native big-endian vectors alias scalar dN in their high half.
// d(N+16) holds the low half and is never scalar-allocated.
inline DoubleRegister SimdHalf(DoubleRegister reg, int half) {
  DCHECK_LT(reg.code(), 16);
  return DoubleRegister::from_code(reg.code() + 16 * (1 - half));
}
inline MemOperand SimdOffset(MemOperand mem, int offset) {
  return MemOperand(mem.base(),
                    static_cast<int>(mem.offset()) + offset -
                        (mem.base() == sp ? kSparcStackSlotBias : 0));
}
inline void LoadSimd(MacroAssembler* assm, DoubleRegister dst, MemOperand mem) {
  assm->LoadF64(SimdHalf(dst, 1), mem);
  assm->LoadF64(SimdHalf(dst, 0), SimdOffset(mem, 8));
}
inline void StoreSimd(MacroAssembler* assm, DoubleRegister src,
                      MemOperand mem) {
  assm->StoreF64(SimdHalf(src, 1), mem);
  assm->StoreF64(SimdHalf(src, 0), SimdOffset(mem, 8));
}
inline void MoveSimd(MacroAssembler* assm, DoubleRegister dst,
                     DoubleRegister src) {
  for (int half = 0; half < 2; ++half)
    assm->fmovd(SimdHalf(dst, half), SimdHalf(src, half));
}
inline void TruncateLane(MacroAssembler* assm, Register reg, int bits,
                         bool sign = false) {
  if (bits == 64) return;
  assm->sllx(reg, reg, 64 - bits);
  if (sign)
    assm->srax(reg, reg, 64 - bits);
  else
    assm->srlx(reg, reg, 64 - bits);
}
inline void ReadLane(MacroAssembler* assm, Register dst, DoubleRegister src,
                     int bits, int lane, bool sign = false) {
  assm->MovDoubleToInt64(dst, SimdHalf(src, lane * bits / 64));
  if (int shift = lane * bits % 64) assm->srlx(dst, dst, shift);
  TruncateLane(assm, dst, bits, sign);
}
// The input is in g1, and must be read before dst can alias its scalar
// register.
inline void ReplaceLane(MacroAssembler* assm, DoubleRegister dst,
                        DoubleRegister src, int bits, int lane) {
  MoveSimd(assm, dst, src);
  DoubleRegister half = SimdHalf(dst, lane * bits / 64);
  if (bits == 64) {
    assm->MovInt64ToDouble(half, g1);
    return;
  }
  int shift = lane * bits % 64;
  TruncateLane(assm, g1, bits);
  if (shift) assm->sllx(g1, g1, shift);
  uint64_t mask = ((uint64_t{1} << bits) - 1) << shift;
  assm->MacroAssembler::Move(g3, static_cast<intptr_t>(~mask));
  assm->MovDoubleToInt64(g2, half);
  assm->and_(g2, g2, g3);
  assm->or_(g1, g1, g2);
  assm->MovInt64ToDouble(half, g1);
}
inline void Splat(MacroAssembler* assm, DoubleRegister dst, int bits) {
  TruncateLane(assm, g1, bits);
  for (int shift = bits; shift < 64; shift *= 2) {
    assm->sllx(g2, g1, shift);
    assm->or_(g1, g1, g2);
  }
  for (int half = 0; half < 2; ++half)
    assm->MovInt64ToDouble(SimdHalf(dst, half), g1);
}

enum class SimdBinaryOp {
  kAdd,
  kSub,
  kMul,
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kMin,
  kMax
};
inline void SimdBinary(MacroAssembler* assm, DoubleRegister dst,
                       DoubleRegister lhs, DoubleRegister rhs, int bits,
                       bool sign, SimdBinaryOp op) {
  for (int half = 0; half < 2; ++half) {
    assm->MovDoubleToInt64(g1, SimdHalf(lhs, half));
    assm->MovDoubleToInt64(g2, SimdHalf(rhs, half));
    assm->mov(g3, 0);
    for (int shift = 0; shift < 64; shift += bits) {
      assm->srlx(g4, g1, shift);
      assm->srlx(g5, g2, shift);
      TruncateLane(assm, g4, bits, sign);
      TruncateLane(assm, g5, bits, sign);
      if (op == SimdBinaryOp::kAdd)
        assm->add(g4, g4, g5);
      else if (op == SimdBinaryOp::kSub)
        assm->sub(g4, g4, g5);
      else if (op == SimdBinaryOp::kMul)
        assm->mulx(g4, g4, g5);
      else {
        Condition cond = equal;
        switch (op) {
          case SimdBinaryOp::kEq:
            cond = equal;
            break;
          case SimdBinaryOp::kNe:
            cond = not_equal;
            break;
          case SimdBinaryOp::kLt:
          case SimdBinaryOp::kMin:
            cond = sign ? less : less_unsigned;
            break;
          case SimdBinaryOp::kLe:
            cond = sign ? less_equal : less_equal_unsigned;
            break;
          case SimdBinaryOp::kGt:
          case SimdBinaryOp::kMax:
            cond = sign ? greater : greater_unsigned;
            break;
          case SimdBinaryOp::kGe:
            cond = sign ? greater_equal : greater_equal_unsigned;
            break;
          default:
            UNREACHABLE();
        }
        Label yes, done;
        assm->cmp(g4, g5);
        assm->b(cond, ConditionCodes::kXcc, &yes);
        assm->nop();
        if (op == SimdBinaryOp::kMin || op == SimdBinaryOp::kMax) {
          assm->mov(g4, g5);
          assm->ba(&done);
          assm->nop();
          assm->bind(&yes);
        } else {
          assm->mov(g4, 0);
          assm->ba(&done);
          assm->nop();
          assm->bind(&yes);
          assm->mov(g4, -1);
        }
        assm->bind(&done);
      }
      TruncateLane(assm, g4, bits);
      if (shift) assm->sllx(g4, g4, shift);
      assm->or_(g3, g3, g4);
    }
    assm->MovInt64ToDouble(SimdHalf(dst, half), g3);
  }
}
inline int SimdLaneOffset(int bits, int lane) {
  return 16 - (lane + 1) * bits / 8;
}
inline void ReadStackLane(MacroAssembler* assm, Register dst, int base,
                          int bits, int lane, bool sign) {
  MemOperand mem(sp, base + SimdLaneOffset(bits, lane));
  switch (bits) {
    case 8:
      assm->LoadU8(dst, mem);
      break;
    case 16:
      assm->LoadU16(dst, mem);
      break;
    case 32:
      assm->LoadU32(dst, mem);
      break;
    case 64:
      assm->LoadU64(dst, mem);
      break;
    default:
      UNREACHABLE();
  }
  TruncateLane(assm, dst, bits, sign);
}
inline void WriteStackLane(MacroAssembler* assm, Register src, int base,
                           int bits, int lane) {
  MemOperand mem(sp, base + SimdLaneOffset(bits, lane));
  switch (bits) {
    case 8:
      assm->StoreU8(src, mem);
      break;
    case 16:
      assm->StoreU16(src, mem);
      break;
    case 32:
      assm->StoreU32(src, mem);
      break;
    case 64:
      assm->StoreU64(src, mem);
      break;
    default:
      UNREACHABLE();
  }
}
inline void ClampSimdLane(MacroAssembler* assm, int bits, bool sign) {
  int64_t minimum = sign ? -(int64_t{1} << (bits - 1)) : 0;
  int64_t maximum = (int64_t{1} << (bits - (sign ? 1 : 0))) - 1;
  Label lower_ok, done;
  assm->MacroAssembler::Move(g2, minimum);
  assm->cmp(g1, g2);
  assm->b(greater_equal, ConditionCodes::kXcc, &lower_ok);
  assm->nop();
  assm->mov(g1, g2);
  assm->ba(&done);
  assm->nop();
  assm->bind(&lower_ok);
  assm->MacroAssembler::Move(g2, maximum);
  assm->cmp(g1, g2);
  assm->b(less_equal, ConditionCodes::kXcc, &done);
  assm->nop();
  assm->mov(g1, g2);
  assm->bind(&done);
}
enum class SimdTransformOp {
  kWiden,
  kExtMul,
  kPairAdd,
  kNarrow,
  kAddSat,
  kSubSat,
  kAverage,
  kQ15,
  kDot
};
inline void TransformSimd(MacroAssembler* assm, DoubleRegister dst,
                          DoubleRegister lhs, DoubleRegister rhs,
                          int input_bits, int output_bits, int first_lane,
                          bool sign, SimdTransformOp op) {
  assm->AllocateStackSpace(48);
  StoreSimd(assm, lhs, MemOperand(sp));
  StoreSimd(assm, rhs, MemOperand(sp, 16));
  for (int lane = 0; lane < 128 / output_bits; ++lane) {
    int input_lane = first_lane + lane;
    if (op == SimdTransformOp::kNarrow) {
      int input_lanes = 128 / input_bits;
      ReadStackLane(assm, g1, lane < input_lanes ? 0 : 16, input_bits,
                    lane % input_lanes, true);
      ClampSimdLane(assm, output_bits, sign);
    } else if (op == SimdTransformOp::kPairAdd) {
      ReadStackLane(assm, g1, 0, input_bits, lane * 2, sign);
      ReadStackLane(assm, g2, 0, input_bits, lane * 2 + 1, sign);
      assm->add(g1, g1, g2);
    } else if (op == SimdTransformOp::kDot) {
      ReadStackLane(assm, g1, 0, input_bits, lane * 2, true);
      ReadStackLane(assm, g2, 16, input_bits, lane * 2, true);
      assm->mulx(g3, g1, g2);
      ReadStackLane(assm, g1, 0, input_bits, lane * 2 + 1, true);
      ReadStackLane(assm, g2, 16, input_bits, lane * 2 + 1, true);
      assm->mulx(g1, g1, g2);
      assm->add(g1, g1, g3);
    } else {
      ReadStackLane(assm, g1, 0, input_bits, input_lane, sign);
      if (op != SimdTransformOp::kWiden) {
        ReadStackLane(assm, g2, 16, input_bits, input_lane, sign);
        if (op == SimdTransformOp::kAddSat || op == SimdTransformOp::kAverage)
          assm->add(g1, g1, g2);
        else if (op == SimdTransformOp::kSubSat)
          assm->sub(g1, g1, g2);
        else
          assm->mulx(g1, g1, g2);
        if (op == SimdTransformOp::kAverage) {
          assm->add(g1, g1, 1);
          assm->srlx(g1, g1, 1);
        } else if (op == SimdTransformOp::kQ15) {
          assm->MacroAssembler::Move(g2, 0x4000);
          assm->add(g1, g1, g2);
          assm->srax(g1, g1, 15);
          ClampSimdLane(assm, 16, true);
        } else if (op == SimdTransformOp::kAddSat ||
                   op == SimdTransformOp::kSubSat) {
          ClampSimdLane(assm, output_bits, sign);
        }
      }
    }
    WriteStackLane(assm, g1, 32, output_bits, lane);
  }
  LoadSimd(assm, dst, MemOperand(sp, 32));
  assm->Drop(6);
}
inline void SimdSwizzle(MacroAssembler* assm, DoubleRegister dst,
                        DoubleRegister lhs, DoubleRegister rhs) {
  assm->AllocateStackSpace(32);
  StoreSimd(assm, lhs, MemOperand(sp));
  StoreSimd(assm, rhs, MemOperand(sp, 16));
  for (int half = 0; half < 2; ++half) {
    assm->mov(g3, 0);
    for (int byte = 0; byte < 8; ++byte) {
      ReadStackLane(assm, g1, 16, 8, half * 8 + byte, false);
      Label zero, done;
      assm->cmp(g1, 16);
      assm->b(greater_equal_unsigned, ConditionCodes::kXcc, &zero);
      assm->nop();
      assm->xor_(g1, g1, 15);
      assm->LoadStackAddress(g2);
      assm->add(g2, g2, g1);
      assm->LoadU8(g1, MemOperand(g2));
      assm->ba(&done);
      assm->nop();
      assm->bind(&zero);
      assm->mov(g1, 0);
      assm->bind(&done);
      if (byte) assm->sllx(g1, g1, byte * 8);
      assm->or_(g3, g3, g1);
    }
    assm->MovInt64ToDouble(SimdHalf(dst, half), g3);
  }
  assm->Drop(4);
}
inline void SimdDotAdd(MacroAssembler* assm, DoubleRegister dst,
                       DoubleRegister lhs, DoubleRegister rhs,
                       DoubleRegister accumulator) {
  assm->AllocateStackSpace(64);
  StoreSimd(assm, lhs, MemOperand(sp));
  StoreSimd(assm, rhs, MemOperand(sp, 16));
  StoreSimd(assm, accumulator, MemOperand(sp, 32));
  for (int lane = 0; lane < 4; ++lane) {
    assm->mov(g4, 0);
    for (int n = 0; n < 4; ++n) {
      ReadStackLane(assm, g1, 0, 8, lane * 4 + n, true);
      ReadStackLane(assm, g2, 16, 8, lane * 4 + n, true);
      assm->mulx(g1, g1, g2);
      assm->add(g4, g4, g1);
    }
    ReadStackLane(assm, g1, 32, 32, lane, false);
    assm->add(g1, g1, g4);
    WriteStackLane(assm, g1, 48, 32, lane);
  }
  LoadSimd(assm, dst, MemOperand(sp, 48));
  assm->Drop(8);
}

}  // namespace v8::internal::sparc64
#endif  // V8_CODEGEN_SPARC64_SIMD_HELPERS_SPARC64_INL_H_
