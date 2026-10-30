// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_BACKEND_SPARC64_CODE_GENERATOR_SPARC64_INL_H_
#define V8_COMPILER_BACKEND_SPARC64_CODE_GENERATOR_SPARC64_INL_H_

#include "src/codegen/macro-assembler.h"
#include "src/compiler/backend/instruction-codes.h"

namespace v8::internal::compiler {

inline void EmitSparc64RotateRight(MacroAssembler* masm, Register dst,
                                   Register src, Register amount, bool word32) {
  if (word32) {
    masm->srl(g1, src, amount);
    masm->sub(g2, g0, amount);
    masm->sll(g2, src, g2);
    masm->or_(dst, g1, g2);
    masm->sra(dst, dst, 0);
  } else {
    masm->srlx(g1, src, amount);
    masm->sub(g2, g0, amount);
    masm->sllx(g2, src, g2);
    masm->or_(dst, g1, g2);
  }
}

inline void EmitSparc64ReverseBytes(MacroAssembler* masm, Register dst,
                                    Register src, int bytes) {
  DCHECK(bytes == 4 || bytes == 8);
  masm->Move(g1, src);
  masm->mov(g2, 0);
  for (int n = 0; n < bytes; ++n) {
    masm->and_(g3, g1, 255);
    masm->sllx(g2, g2, 8);
    masm->or_(g2, g2, g3);
    if (n + 1 < bytes) masm->srlx(g1, g1, 8);
  }
  if (bytes == 4) {
    masm->sra(dst, g2, 0);
  } else {
    masm->Move(dst, g2);
  }
}

inline void EmitSparc64FloatMinMax(MacroAssembler* masm, DoubleRegister dst,
                                   DoubleRegister lhs, DoubleRegister rhs,
                                   bool single, bool minimum) {
  Label left, right, nan, done;
  if (single) {
    masm->fcmps(lhs, rhs);
  } else {
    masm->fcmpd(lhs, rhs);
  }
  masm->fb(kFUnordered, 0, &nan);
  masm->nop();
  masm->fb(kFLess, 0, minimum ? &left : &right);
  masm->nop();
  masm->fb(kFGreater, 0, minimum ? &right : &left);
  masm->nop();
  if (single) {
    masm->MovFloatToInt(g1, lhs);
    masm->MovFloatToInt(g2, rhs);
  } else {
    masm->MovDoubleToInt64(g1, lhs);
    masm->MovDoubleToInt64(g2, rhs);
  }
  if (minimum) {
    masm->or_(g1, g1, g2);
  } else {
    masm->and_(g1, g1, g2);
  }
  if (single) {
    masm->MovIntToFloat(dst, g1);
  } else {
    masm->MovInt64ToDouble(dst, g1);
  }
  masm->Jump(&done);
  masm->bind(&left);
  if (single)
    masm->fmovs(dst, lhs);
  else
    masm->fmovd(dst, lhs);
  masm->Jump(&done);
  masm->bind(&right);
  if (single)
    masm->fmovs(dst, rhs);
  else
    masm->fmovd(dst, rhs);
  masm->Jump(&done);
  masm->bind(&nan);
  masm->Move(g1, single ? intptr_t{0x7fc00000}
                        : intptr_t{INT64_C(0x7ff8000000000000)});
  if (single)
    masm->MovIntToFloat(dst, g1);
  else
    masm->MovInt64ToDouble(dst, g1);
  masm->bind(&done);
}

inline void EmitSparc64ByteReverse(MacroAssembler* masm, Register dst,
                                   Register src, Register scratch,
                                   Register mask, int bytes) {
  DCHECK(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8);
  DCHECK(!AreAliased(dst, scratch, mask));
  masm->Move(dst, src);
  if (bytes == 1) return;
  masm->Move(mask, bytes == 8   ? INT64_C(0x00ff00ff00ff00ff)
                   : bytes == 4 ? INT64_C(0x00ff00ff)
                                : 0xff);
  masm->and_(scratch, dst, mask);
  masm->sllx(scratch, scratch, 8);
  masm->srlx(dst, dst, 8);
  masm->and_(dst, dst, mask);
  masm->or_(dst, dst, scratch);
  if (bytes == 2) return;
  masm->Move(mask, bytes == 8 ? INT64_C(0x0000ffff0000ffff) : 0xffff);
  masm->and_(scratch, dst, mask);
  masm->sllx(scratch, scratch, 16);
  masm->srlx(dst, dst, 16);
  masm->and_(dst, dst, mask);
  masm->or_(dst, dst, scratch);
  if (bytes == 4) return;
  masm->sllx(scratch, dst, 32);
  masm->srlx(dst, dst, 32);
  masm->or_(dst, dst, scratch);
}

enum class Sparc64AtomicOp {
  kExchange,
  kCompareExchange,
  kAdd,
  kSub,
  kAnd,
  kOr,
  kXor
};

inline void EmitSparc64Atomic(MacroAssembler* masm, Register dst, Register base,
                              Register index, Register value, Register expected,
                              Register mask, Register temp, int bytes,
                              bool is_signed, Sparc64AtomicOp op,
                              bool little_endian = false,
                              uint32_t* protected_load_pc = nullptr) {
  const int bits = bytes * 8;
  // CASX preserves the other lanes in the aligned, big-endian word.
  masm->add(g1, base, index);
  masm->and_(g4, g1, 7);
  masm->mov(g2, 8 - bytes);
  masm->sub(g2, g2, g4);
  masm->sllx(g2, g2, 3);
  masm->and_(g1, g1, -8);
  masm->Move(
      mask, bytes == 8 ? -1 : static_cast<intptr_t>((uint64_t{1} << bits) - 1));
  masm->sllx(mask, mask, g2);
  masm->membar(kSync);
  Label loop, done;
  masm->bind(&loop);
  if (protected_load_pc) *protected_load_pc = masm->pc_offset();
  masm->LoadU64(g3, MemOperand(g1));
  masm->srlx(g5, g3, g2);
  if (bytes != 8) {
    masm->sllx(g5, g5, 64 - bits);
    masm->srlx(g5, g5, 64 - bits);
  }
  if (little_endian) EmitSparc64ByteReverse(masm, g5, g5, temp, g4, bytes);
  if (op == Sparc64AtomicOp::kCompareExchange) {
    if (bytes == 8) {
      masm->Move(temp, expected);
    } else {
      masm->sllx(temp, expected, 64 - bits);
      masm->srlx(temp, temp, 64 - bits);
    }
    masm->cmp(g5, temp);
    masm->b(not_equal, ConditionCodes::kXcc, &done);
    masm->nop();
  }
  switch (op) {
    case Sparc64AtomicOp::kExchange:
    case Sparc64AtomicOp::kCompareExchange:
      masm->Move(temp, value);
      break;
    case Sparc64AtomicOp::kAdd:
      masm->add(temp, g5, value);
      break;
    case Sparc64AtomicOp::kSub:
      masm->sub(temp, g5, value);
      break;
    case Sparc64AtomicOp::kAnd:
      masm->and_(temp, g5, value);
      break;
    case Sparc64AtomicOp::kOr:
      masm->or_(temp, g5, value);
      break;
    case Sparc64AtomicOp::kXor:
      masm->xor_(temp, g5, value);
      break;
  }
  if (little_endian) {
    EmitSparc64ByteReverse(masm, temp, temp, g4, mask, bytes);
    masm->Move(mask, bytes == 8
                         ? -1
                         : static_cast<intptr_t>((uint64_t{1} << bits) - 1));
    masm->sllx(mask, mask, g2);
  }
  masm->sllx(temp, temp, g2);
  masm->and_(temp, temp, mask);
  masm->xor_(g4, mask, -1);
  masm->and_(g4, g3, g4);
  masm->or_(g4, g4, temp);
  masm->casx(g4, g1, g3);
  masm->cmp(g4, g3);
  masm->b(not_equal, ConditionCodes::kXcc, &loop);
  masm->nop();
  masm->bind(&done);
  masm->membar(kSync);
  if (is_signed && bytes != 8) {
    masm->sllx(g5, g5, 64 - bits);
    masm->srax(g5, g5, 64 - bits);
  }
  masm->Move(dst, g5);
}

inline void EmitSparc64Divide(MacroAssembler* masm, Register dst, Register lhs,
                              Register rhs, bool word32, bool is_unsigned,
                              bool remainder) {
  if (word32) {
    if (is_unsigned) {
      masm->srl(g1, lhs, 0);
      masm->srl(g2, rhs, 0);
    } else {
      masm->sra(g1, lhs, 0);
      masm->sra(g2, rhs, 0);
    }
  } else {
    masm->Move(g1, lhs);
    masm->Move(g2, rhs);
  }
  Label zero, normal, result, done;
  masm->cmp(g2, 0);
  masm->b(equal, ConditionCodes::kXcc, &zero);
  masm->nop();
  if (!word32 && !is_unsigned) {
    masm->cmp(g2, -1);
    masm->b(not_equal, ConditionCodes::kXcc, &normal);
    masm->nop();
    masm->mov(g3, 1);
    masm->sllx(g3, g3, 63);
    masm->cmp(g1, g3);
    masm->b(not_equal, ConditionCodes::kXcc, &normal);
    masm->nop();
    if (remainder) masm->mov(g3, 0);
    masm->Jump(&result);
  }
  masm->bind(&normal);
  if (is_unsigned) {
    masm->udivx(g3, g1, g2);
  } else {
    masm->sdivx(g3, g1, g2);
  }
  if (remainder) {
    masm->mulx(g3, g3, g2);
    masm->sub(g3, g1, g3);
  }
  masm->bind(&result);
  if (word32) {
    if (is_unsigned) {
      masm->srl(g3, g3, 0);
    } else {
      masm->sra(g3, g3, 0);
    }
  }
  masm->Jump(&done);
  masm->bind(&zero);
  masm->mov(g3, 0);
  masm->bind(&done);
  masm->Move(dst, g3);
}

inline void EmitSparc64MultiplyHigh(MacroAssembler* masm, Register dst,
                                    Register lhs, Register rhs, bool word32,
                                    bool is_unsigned) {
  if (word32) {
    if (is_unsigned) {
      masm->srl(g1, lhs, 0);
      masm->srl(g2, rhs, 0);
    } else {
      masm->sra(g1, lhs, 0);
      masm->sra(g2, rhs, 0);
    }
    masm->mulx(g1, g1, g2);
    if (is_unsigned) {
      masm->srlx(dst, g1, 32);
    } else {
      masm->srax(dst, g1, 32);
    }
    return;
  }
  masm->srl(g1, lhs, 0);
  masm->srlx(g2, lhs, 32);
  masm->srl(g3, rhs, 0);
  masm->srlx(g4, rhs, 32);
  masm->mulx(g5, g1, g3);
  masm->srlx(g5, g5, 32);
  masm->mulx(g3, g2, g3);
  masm->add(g3, g3, g5);
  masm->srl(g5, g3, 0);
  masm->srlx(g3, g3, 32);
  masm->mulx(g1, g1, g4);
  masm->add(g1, g1, g5);
  masm->srlx(g1, g1, 32);
  masm->mulx(g2, g2, g4);
  masm->add(g2, g2, g3);
  masm->add(g2, g2, g1);
  if (!is_unsigned) {
    masm->srax(g1, lhs, 63);
    masm->and_(g1, g1, rhs);
    masm->sub(g2, g2, g1);
    masm->srax(g1, rhs, 63);
    masm->and_(g1, g1, lhs);
    masm->sub(g2, g2, g1);
  }
  masm->Move(dst, g2);
}

inline void EmitSparc64SilenceNaN(MacroAssembler* masm, DoubleRegister dst,
                                  DoubleRegister src) {
  Label done;
  masm->MovDoubleToInt64(g1, src);
  masm->srlx(g2, g1, 52);
  masm->and_(g2, g2, 0x7ff);
  masm->cmp(g2, 0x7ff);
  masm->b(not_equal, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->sllx(g2, g1, 12);
  masm->cmp(g2, 0);
  masm->b(equal, ConditionCodes::kXcc, &done);
  masm->nop();
  masm->mov(g2, 1);
  masm->sllx(g2, g2, 51);
  masm->or_(g1, g1, g2);
  masm->bind(&done);
  masm->MovInt64ToDouble(dst, g1);
}

inline void EmitSparc64CountLeadingZeros(MacroAssembler* masm, Register dst,
                                         Register src, bool word32) {
  Label zero, done;
  if (word32) {
    masm->sllx(g1, src, 32);
  } else {
    masm->Move(g1, src);
  }
  masm->cmp(g1, 0);
  masm->b(equal, ConditionCodes::kXcc, &zero);
  masm->nop();
  masm->mov(g3, 0);
  for (int shift = word32 ? 16 : 32; shift != 0; shift /= 2) {
    Label next;
    masm->srlx(g2, g1, 64 - shift);
    masm->cmp(g2, 0);
    masm->b(not_equal, ConditionCodes::kXcc, &next);
    masm->nop();
    masm->add(g3, g3, shift);
    masm->sllx(g1, g1, shift);
    masm->bind(&next);
  }
  masm->Move(dst, g3);
  masm->Jump(&done);
  masm->bind(&zero);
  masm->mov(dst, word32 ? 32 : 64);
  masm->bind(&done);
}

inline void EmitSparc64ExtractDoubleWord(MacroAssembler* masm, Register dst,
                                         DoubleRegister src, bool high) {
  masm->MovDoubleToInt64(dst, src);
  if (high) {
    masm->srlx(dst, dst, 32);
  } else {
    masm->srl(dst, dst, 0);
  }
}

inline void EmitSparc64DoubleFromWord32Pair(MacroAssembler* masm,
                                            DoubleRegister dst, Register high,
                                            Register low) {
  masm->sllx(g1, high, 32);
  masm->srl(g2, low, 0);
  masm->or_(g1, g1, g2);
  masm->MovInt64ToDouble(dst, g1);
}

inline void EmitSparc64FloatToInteger(MacroAssembler* masm, Register dst,
                                      Register success, DoubleRegister src,
                                      bool word32, bool is_unsigned,
                                      bool single) {
  const int mantissa_bits = single ? 23 : 52;
  if (single) {
    masm->MovFloatToInt(g1, src);
    masm->srlx(g2, g1, 23);
    masm->and_(g2, g2, 0xff);
    masm->sub(g2, g2, 127);
    masm->sllx(g1, g1, 32);
  } else {
    masm->MovDoubleToInt64(g1, src);
    masm->srlx(g2, g1, 52);
    masm->and_(g2, g2, 0x7ff);
    masm->sub(g2, g2, 1023);
  }
  Label zero, invalid, right, sign, negative, range, done;
  masm->mov(g5, 1);
  masm->cmp(g2, 0);
  masm->b(less, ConditionCodes::kXcc, &zero);
  masm->nop();
  masm->cmp(g2, 64);
  masm->b(greater_equal, ConditionCodes::kXcc, &invalid);
  masm->nop();
  masm->sllx(g3, g1, single ? 9 : 12);
  masm->srlx(g3, g3, single ? 41 : 12);
  masm->mov(g4, 1);
  masm->sllx(g4, g4, mantissa_bits);
  masm->or_(g3, g3, g4);
  masm->cmp(g2, mantissa_bits);
  masm->b(less, ConditionCodes::kXcc, &right);
  masm->nop();
  masm->sub(g4, g2, mantissa_bits);
  masm->sllx(g3, g3, g4);
  masm->Jump(&sign);
  masm->bind(&right);
  masm->mov(g4, mantissa_bits);
  masm->sub(g4, g4, g2);
  masm->srlx(g3, g3, g4);
  masm->bind(&sign);
  masm->cmp(g1, 0);
  masm->b(less, ConditionCodes::kXcc, &negative);
  masm->nop();
  if (!is_unsigned) {
    masm->cmp(g3, 0);
    masm->b(less, ConditionCodes::kXcc, &invalid);
    masm->nop();
  }
  masm->Jump(&range);
  masm->bind(&negative);
  if (is_unsigned) {
    masm->Jump(&invalid);
  } else {
    masm->mov(g4, 1);
    masm->sllx(g4, g4, 63);
    masm->cmp(g3, g4);
    masm->b(greater_unsigned, ConditionCodes::kXcc, &invalid);
    masm->nop();
    masm->sub(g3, g0, g3);
  }
  masm->bind(&range);
  if (word32) {
    if (is_unsigned) {
      masm->srl(g4, g3, 0);
    } else {
      masm->sra(g4, g3, 0);
    }
    masm->cmp(g3, g4);
    masm->b(not_equal, ConditionCodes::kXcc, &invalid);
    masm->nop();
  }
  masm->Jump(&done);
  masm->bind(&invalid);
  masm->mov(g5, 0);
  masm->bind(&zero);
  masm->mov(g3, 0);
  masm->bind(&done);
  masm->Move(dst, g3);
  if (success != no_reg) masm->Move(success, g5);
}

inline void EmitSparc64IntegerToFloat(MacroAssembler* masm, DoubleRegister dst,
                                      Register src, bool word32,
                                      bool is_unsigned, bool single) {
  auto convert = [&](Register value) {
    masm->MovInt64ToDouble(dst, value);
    if (single) {
      masm->fxtos(dst, dst);
    } else {
      masm->fxtod(dst, dst);
    }
  };
  if (word32) {
    if (is_unsigned) {
      masm->srl(g1, src, 0);
    } else {
      masm->sra(g1, src, 0);
    }
    convert(g1);
  } else if (is_unsigned) {
    Label nonnegative, done;
    masm->cmp(src, 0);
    masm->b(greater_equal, ConditionCodes::kXcc, &nonnegative);
    masm->nop();
    // Preserve the discarded bit so halfway values round correctly.
    masm->srlx(g1, src, 1);
    masm->and_(g2, src, 1);
    masm->or_(g1, g1, g2);
    convert(g1);
    if (single) {
      masm->fadds(dst, dst, dst);
    } else {
      masm->faddd(dst, dst, dst);
    }
    masm->Jump(&done);
    masm->bind(&nonnegative);
    convert(src);
    masm->bind(&done);
  } else {
    convert(src);
  }
}

inline void EmitSparc64Mul32WithHigh32(MacroAssembler* masm, Register low,
                                       Register high, Register lhs,
                                       Register rhs) {
  masm->sra(g1, lhs, 0);
  masm->sra(g2, rhs, 0);
  masm->mulx(g1, g1, g2);
  masm->srax(high, g1, 32);
  masm->sra(low, g1, 0);
}

inline Condition Sparc64BranchCondition(FlagsCondition condition) {
  switch (condition) {
    case kEqual:
      return equal;
    case kNotEqual:
      return not_equal;
    case kSignedLessThan:
      return less;
    case kSignedGreaterThanOrEqual:
      return greater_equal;
    case kSignedLessThanOrEqual:
      return less_equal;
    case kSignedGreaterThan:
      return greater;
    case kUnsignedLessThan:
      return less_unsigned;
    case kUnsignedGreaterThanOrEqual:
      return greater_equal_unsigned;
    case kUnsignedLessThanOrEqual:
      return less_equal_unsigned;
    case kUnsignedGreaterThan:
      return greater_unsigned;
    case kOverflow:
      return overflow;
    case kNotOverflow:
      return no_overflow;
    case kNegative:
      return negative;
    case kPositiveOrZero:
      return positive;
    default:
      UNREACHABLE();
  }
}

inline FloatCondition Sparc64FloatBranchCondition(FlagsCondition condition) {
  switch (condition) {
    case kEqual:
    case kUnorderedEqual:
      return kFEqual;
    case kNotEqual:
    case kUnorderedNotEqual:
      return kFNotEqual;
    case kUnsignedLessThan:
    case kFloatLessThan:
      return kFLess;
    case kUnsignedGreaterThanOrEqual:
    case kFloatGreaterThanOrEqualOrUnordered:
      return kFUnorderedOrGreaterOrEqual;
    case kUnsignedLessThanOrEqual:
    case kFloatLessThanOrEqual:
      return kFLessOrEqual;
    case kUnsignedGreaterThan:
    case kFloatGreaterThanOrUnordered:
      return kFUnorderedOrGreater;
    case kFloatLessThanOrUnordered:
      return kFUnorderedOrLess;
    case kFloatGreaterThanOrEqual:
      return kFGreaterOrEqual;
    case kFloatLessThanOrEqualOrUnordered:
      return kFUnorderedOrLessOrEqual;
    case kFloatGreaterThan:
      return kFGreater;
    case kIsNaN:
      return kFUnordered;
    case kIsNotNaN:
      return kFOrdered;
    default:
      UNREACHABLE();
  }
}

inline void EmitSparc64Branch(MacroAssembler* masm, ArchOpcode opcode,
                              FlagsCondition condition, Label* target) {
  Label skip;
  if (opcode == kSparcCmpDouble) {
    masm->fb(
        static_cast<FloatCondition>(Sparc64FloatBranchCondition(condition) ^ 8),
        0, &skip);
  } else {
    ConditionCodes cc;
    switch (opcode) {
      case kSparcCmp32:
      case kSparcTst32:
      case kSparcAdd32:
      case kSparcAddWithOverflow32:
      case kSparcSubWithOverflow32:
        cc = ConditionCodes::kIcc;
        break;
      default:
        cc = ConditionCodes::kXcc;
        break;
    }
    masm->b(static_cast<Condition>(Sparc64BranchCondition(condition) ^ 8), cc,
            &skip);
  }
  masm->nop();
  masm->Jump(target);
  masm->bind(&skip);
}

}  // namespace v8::internal::compiler

#endif  // V8_COMPILER_BACKEND_SPARC64_CODE_GENERATOR_SPARC64_INL_H_
