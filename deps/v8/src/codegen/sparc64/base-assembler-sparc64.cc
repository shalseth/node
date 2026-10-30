// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/sparc64/base-assembler-sparc64.h"

namespace v8::internal {

void AssemblerSparc64Base::GenFormat3(int op, int op3, int rd, Register rs1,
                                      Register rs2) {
  emit((static_cast<uint32_t>(op) << 30) | (static_cast<uint32_t>(rd) << 25) |
       (static_cast<uint32_t>(op3) << 19) |
       (static_cast<uint32_t>(rs1.code()) << 14) | rs2.code());
}

void AssemblerSparc64Base::GenFormat3(int op, int op3, int rd, Register rs1,
                                      int32_t imm13) {
  DCHECK_GE(imm13, -4096);
  DCHECK_LE(imm13, 4095);
  emit((static_cast<uint32_t>(op) << 30) | (static_cast<uint32_t>(rd) << 25) |
       (static_cast<uint32_t>(op3) << 19) |
       (static_cast<uint32_t>(rs1.code()) << 14) | (1u << 13) |
       (static_cast<uint32_t>(imm13) & 0x1fff));
}

#define DEFINE_INSTRUCTION(name, op3)                                         \
  void AssemblerSparc64Base::name(Register rd, Register rs1, Register rs2) {  \
    GenFormat3(2, op3, rd.code(), rs1, rs2);                                  \
  }                                                                           \
  void AssemblerSparc64Base::name(Register rd, Register rs1, int32_t imm13) { \
    GenFormat3(2, op3, rd.code(), rs1, imm13);                                \
  }
SPARC64_ALU_INSTRUCTIONS(DEFINE_INSTRUCTION)
#undef DEFINE_INSTRUCTION

#define DEFINE_INSTRUCTION(name, op3)                                         \
  void AssemblerSparc64Base::name(Register rd, Register rs1, Register rs2) {  \
    GenFormat3(3, op3, rd.code(), rs1, rs2);                                  \
  }                                                                           \
  void AssemblerSparc64Base::name(Register rd, Register rs1, int32_t imm13) { \
    GenFormat3(3, op3, rd.code(), rs1, imm13);                                \
  }
SPARC64_MEMORY_INSTRUCTIONS(DEFINE_INSTRUCTION)
#undef DEFINE_INSTRUCTION

void AssemblerSparc64Base::sethi(Register rd, uint32_t imm22) {
  DCHECK_LT(imm22, 1u << 22);
  emit((static_cast<uint32_t>(rd.code()) << 25) | (4u << 22) | imm22);
}

void AssemblerSparc64Base::GenShift(int op3, Register rd, Register rs1,
                                    int shift, bool wide) {
  DCHECK_GE(shift, 0);
  DCHECK_LT(shift, wide ? 64 : 32);
  emit((2u << 30) | (static_cast<uint32_t>(rd.code()) << 25) |
       (static_cast<uint32_t>(op3) << 19) |
       (static_cast<uint32_t>(rs1.code()) << 14) | (1u << 13) |
       (static_cast<uint32_t>(wide) << 12) | static_cast<uint32_t>(shift));
}

void AssemblerSparc64Base::GenShift(int op3, Register rd, Register rs1,
                                    Register shift, bool wide) {
  emit((2u << 30) | (static_cast<uint32_t>(rd.code()) << 25) |
       (static_cast<uint32_t>(op3) << 19) |
       (static_cast<uint32_t>(rs1.code()) << 14) |
       (static_cast<uint32_t>(wide) << 12) | shift.code());
}

#define DEFINE_SHIFT(name, op3, wide)                                          \
  void AssemblerSparc64Base::name(Register rd, Register rs1, int shift) {      \
    GenShift(op3, rd, rs1, shift, wide);                                       \
  }                                                                            \
  void AssemblerSparc64Base::name(Register rd, Register rs1, Register shift) { \
    GenShift(op3, rd, rs1, shift, wide);                                       \
  }
DEFINE_SHIFT(sll, 0x25, false)
DEFINE_SHIFT(srl, 0x26, false)
DEFINE_SHIFT(sra, 0x27, false)
DEFINE_SHIFT(sllx, 0x25, true)
DEFINE_SHIFT(srlx, 0x26, true)
DEFINE_SHIFT(srax, 0x27, true)
#undef DEFINE_SHIFT

void AssemblerSparc64Base::b(Condition cond, ConditionCodes cc,
                             int32_t byte_offset, bool annul) {
  DCHECK_EQ(byte_offset & 3, 0);
  int32_t disp19 = byte_offset / kInstrSize;
  DCHECK_GE(disp19, -(1 << 18));
  DCHECK_LT(disp19, 1 << 18);
  emit((static_cast<uint32_t>(annul) << 29) |
       (static_cast<uint32_t>(cond) << 25) | (1u << 22) |
       (static_cast<uint32_t>(cc) << 20) | (1u << 19) |
       (static_cast<uint32_t>(disp19) & 0x7ffff));
}

void AssemblerSparc64Base::call(int64_t byte_offset) {
  DCHECK_EQ(byte_offset & 3, 0);
  int64_t disp30 = byte_offset / kInstrSize;
  DCHECK_GE(disp30, -(int64_t{1} << 29));
  DCHECK_LT(disp30, int64_t{1} << 29);
  emit((1u << 30) | (static_cast<uint32_t>(disp30) & 0x3fffffff));
}

void AssemblerSparc64Base::fb(FloatCondition condition, int fcc,
                              int32_t byte_offset, bool annul) {
  DCHECK_GE(fcc, 0);
  DCHECK_LT(fcc, 4);
  DCHECK_EQ(byte_offset & 3, 0);
  const int32_t disp19 = byte_offset / kInstrSize;
  DCHECK_GE(disp19, -(1 << 18));
  DCHECK_LT(disp19, 1 << 18);
  emit((static_cast<uint32_t>(annul) << 29) |
       (static_cast<uint32_t>(condition) << 25) | (5u << 22) |
       (static_cast<uint32_t>(fcc) << 20) | (1u << 19) |
       (static_cast<uint32_t>(disp19) & 0x7ffff));
}

void AssemblerSparc64Base::ba(int32_t byte_offset, bool annul) {
  DCHECK_EQ(byte_offset & 3, 0);
  int32_t disp22 = byte_offset / kInstrSize;
  DCHECK_GE(disp22, -(1 << 21));
  DCHECK_LT(disp22, 1 << 21);
  emit((static_cast<uint32_t>(annul) << 29) | (8u << 25) | (2u << 22) |
       (static_cast<uint32_t>(disp22) & 0x3fffff));
}

void AssemblerSparc64Base::rdpc(Register rd) {
  emit((2u << 30) | (static_cast<uint32_t>(rd.code()) << 25) | (0x28u << 19) |
       (5u << 14));
}

void AssemblerSparc64Base::membar(int mask) {
  DCHECK_GE(mask, 0);
  DCHECK_LT(mask, 128);
  GenFormat3(2, 0x28, 0, o7, mask);
}

void AssemblerSparc64Base::casx(Register rd, Register rs1, Register rs2) {
  // ASI_PRIMARY is explicit; the immediate bit would select %asi instead.
  emit((3u << 30) | (static_cast<uint32_t>(rd.code()) << 25) | (0x3eu << 19) |
       (static_cast<uint32_t>(rs1.code()) << 14) | (0x80u << 5) | rs2.code());
}

void AssemblerSparc64Base::GenFPop(int opf, DoubleRegister rd,
                                   DoubleRegister rs1, DoubleRegister rs2) {
  emit((2u << 30) | (static_cast<uint32_t>(rd.encoding()) << 25) |
       (0x34u << 19) | (static_cast<uint32_t>(rs1.encoding()) << 14) |
       (static_cast<uint32_t>(opf) << 5) | rs2.encoding());
}

void AssemblerSparc64Base::faddd(DoubleRegister rd, DoubleRegister rs1,
                                 DoubleRegister rs2) {
  GenFPop(0x42, rd, rs1, rs2);
}

void AssemblerSparc64Base::GenFPopSingle(int opf, FloatRegister rd,
                                         FloatRegister rs1, FloatRegister rs2) {
  emit((2u << 30) | (static_cast<uint32_t>(rd.single_encoding()) << 25) |
       (0x34u << 19) | (static_cast<uint32_t>(rs1.single_encoding()) << 14) |
       (static_cast<uint32_t>(opf) << 5) | rs2.single_encoding());
}

void AssemblerSparc64Base::fadds(FloatRegister rd, FloatRegister rs1,
                                 FloatRegister rs2) {
  GenFPopSingle(0x41, rd, rs1, rs2);
}

void AssemblerSparc64Base::fsubs(FloatRegister rd, FloatRegister rs1,
                                 FloatRegister rs2) {
  GenFPopSingle(0x45, rd, rs1, rs2);
}

void AssemblerSparc64Base::fmuls(FloatRegister rd, FloatRegister rs1,
                                 FloatRegister rs2) {
  GenFPopSingle(0x49, rd, rs1, rs2);
}

void AssemblerSparc64Base::fdivs(FloatRegister rd, FloatRegister rs1,
                                 FloatRegister rs2) {
  GenFPopSingle(0x4d, rd, rs1, rs2);
}

void AssemblerSparc64Base::fnegs(FloatRegister rd, FloatRegister rs) {
  GenFPopSingle(0x05, rd, d0, rs);
}

void AssemblerSparc64Base::fabss(FloatRegister rd, FloatRegister rs) {
  GenFPopSingle(0x09, rd, d0, rs);
}

void AssemblerSparc64Base::fsqrts(FloatRegister rd, FloatRegister rs) {
  GenFPopSingle(0x29, rd, d0, rs);
}

void AssemblerSparc64Base::fcmps(FloatRegister rs1, FloatRegister rs2,
                                 int fcc) {
  DCHECK_GE(fcc, 0);
  DCHECK_LT(fcc, 4);
  emit((2u << 30) | (static_cast<uint32_t>(fcc) << 25) | (0x35u << 19) |
       (static_cast<uint32_t>(rs1.single_encoding()) << 14) | (0x51u << 5) |
       rs2.single_encoding());
}

void AssemblerSparc64Base::fsubd(DoubleRegister rd, DoubleRegister rs1,
                                 DoubleRegister rs2) {
  GenFPop(0x46, rd, rs1, rs2);
}

void AssemblerSparc64Base::fmuld(DoubleRegister rd, DoubleRegister rs1,
                                 DoubleRegister rs2) {
  GenFPop(0x4a, rd, rs1, rs2);
}

void AssemblerSparc64Base::fdivd(DoubleRegister rd, DoubleRegister rs1,
                                 DoubleRegister rs2) {
  GenFPop(0x4e, rd, rs1, rs2);
}

void AssemblerSparc64Base::fmovd(DoubleRegister rd, DoubleRegister rs) {
  GenFPop(0x02, rd, d0, rs);
}

void AssemblerSparc64Base::fmovs(FloatRegister rd, FloatRegister rs) {
  emit((2u << 30) | (static_cast<uint32_t>(rd.single_encoding()) << 25) |
       (0x34u << 19) | (0x01u << 5) | rs.single_encoding());
}

void AssemblerSparc64Base::fnegd(DoubleRegister rd, DoubleRegister rs) {
  GenFPop(0x06, rd, d0, rs);
}

void AssemblerSparc64Base::fabsd(DoubleRegister rd, DoubleRegister rs) {
  GenFPop(0x0a, rd, d0, rs);
}

void AssemblerSparc64Base::fsqrtd(DoubleRegister rd, DoubleRegister rs) {
  GenFPop(0x2a, rd, d0, rs);
}

void AssemblerSparc64Base::fxtod(DoubleRegister rd, DoubleRegister rs) {
  GenFPop(0x88, rd, d0, rs);
}

void AssemblerSparc64Base::fxtos(FloatRegister rd, DoubleRegister rs) {
  DCHECK_LT(rd.code(), 16);
  GenFPop(0x84, rd, d0, rs);
}

void AssemblerSparc64Base::fdtos(FloatRegister rd, DoubleRegister rs) {
  DCHECK_LT(rd.code(), 16);
  GenFPop(0xc6, rd, d0, rs);
}

void AssemblerSparc64Base::fstod(DoubleRegister rd, FloatRegister rs) {
  DCHECK_LT(rs.code(), 16);
  GenFPop(0xc9, rd, d0, rs);
}

void AssemblerSparc64Base::fcmpd(DoubleRegister rs1, DoubleRegister rs2,
                                 int fcc) {
  DCHECK_GE(fcc, 0);
  DCHECK_LT(fcc, 4);
  emit((2u << 30) | (static_cast<uint32_t>(fcc) << 25) | (0x35u << 19) |
       (static_cast<uint32_t>(rs1.encoding()) << 14) | (0x52u << 5) |
       rs2.encoding());
}

void AssemblerSparc64Base::ldd(DoubleRegister rd, Register rs1, int32_t imm13) {
  GenFormat3(3, 0x23, rd.encoding(), rs1, imm13);
}

void AssemblerSparc64Base::std(DoubleRegister rd, Register rs1, int32_t imm13) {
  GenFormat3(3, 0x27, rd.encoding(), rs1, imm13);
}

void AssemblerSparc64Base::ldf(FloatRegister rd, Register rs1, int32_t imm13) {
  GenFormat3(3, 0x20, rd.single_encoding(), rs1, imm13);
}

void AssemblerSparc64Base::stf(FloatRegister rd, Register rs1, int32_t imm13) {
  GenFormat3(3, 0x24, rd.single_encoding(), rs1, imm13);
}

}  // namespace v8::internal
