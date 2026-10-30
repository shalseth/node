// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_BASE_ASSEMBLER_SPARC64_H_
#define V8_CODEGEN_SPARC64_BASE_ASSEMBLER_SPARC64_H_

#include "src/codegen/sparc64/constants-sparc64.h"
#include "src/codegen/sparc64/register-sparc64.h"

namespace v8::internal {

#define SPARC64_ALU_INSTRUCTIONS(V) \
  V(add, 0x00)                      \
  V(and_, 0x01)                     \
  V(andcc, 0x11)                    \
  V(or_, 0x02)                      \
  V(xor_, 0x03)                     \
  V(sub, 0x04)                      \
  V(mulx, 0x09)                     \
  V(udivx, 0x0d)                    \
  V(addcc, 0x10)                    \
  V(subcc, 0x14)                    \
  V(sdivx, 0x2d)                    \
  V(jmpl, 0x38)                     \
  V(save, 0x3c)                     \
  V(restore, 0x3d)

#define SPARC64_MEMORY_INSTRUCTIONS(V) \
  V(lduw, 0x00)                        \
  V(ldub, 0x01)                        \
  V(lduh, 0x02)                        \
  V(stw, 0x04)                         \
  V(stb, 0x05)                         \
  V(sth, 0x06)                         \
  V(ldsw, 0x08)                        \
  V(ldsb, 0x09)                        \
  V(ldsh, 0x0a)                        \
  V(ldx, 0x0b)                         \
  V(stx, 0x0e)

class AssemblerSparc64Base {
 public:
#define DECLARE_INSTRUCTION(name, op3)                \
  void name(Register rd, Register rs1, Register rs2); \
  void name(Register rd, Register rs1, int32_t imm13);
  SPARC64_ALU_INSTRUCTIONS(DECLARE_INSTRUCTION)
  SPARC64_MEMORY_INSTRUCTIONS(DECLARE_INSTRUCTION)
#undef DECLARE_INSTRUCTION

  void sethi(Register rd, uint32_t imm22);
  void sllx(Register rd, Register rs1, int shift);
  void sllx(Register rd, Register rs1, Register shift);
  void srlx(Register rd, Register rs1, int shift);
  void srlx(Register rd, Register rs1, Register shift);
  void srax(Register rd, Register rs1, int shift);
  void srax(Register rd, Register rs1, Register shift);
  void sll(Register rd, Register rs1, int shift);
  void sll(Register rd, Register rs1, Register shift);
  void srl(Register rd, Register rs1, int shift);
  void srl(Register rd, Register rs1, Register shift);
  void sra(Register rd, Register rs1, int shift);
  void sra(Register rd, Register rs1, Register shift);
  void b(Condition cond, ConditionCodes cc, int32_t byte_offset,
         bool annul = false);
  void call(int64_t byte_offset);
  void ba(int32_t byte_offset, bool annul = false);
  void fb(FloatCondition condition, int fcc, int32_t byte_offset,
          bool annul = false);
  void rdpc(Register rd);
  void flushw() { emit(0x81580000); }
  void ta(int trap) {
    DCHECK_GE(trap, 0);
    DCHECK_LT(trap, 128);
    emit(0x91d02000u | static_cast<uint32_t>(trap));
  }
  void membar(int mask);
  void casx(Register rd, Register rs1, Register rs2);
  void faddd(DoubleRegister rd, DoubleRegister rs1, DoubleRegister rs2);
  void fadds(FloatRegister rd, FloatRegister rs1, FloatRegister rs2);
  void fsubs(FloatRegister rd, FloatRegister rs1, FloatRegister rs2);
  void fmuls(FloatRegister rd, FloatRegister rs1, FloatRegister rs2);
  void fdivs(FloatRegister rd, FloatRegister rs1, FloatRegister rs2);
  void fnegs(FloatRegister rd, FloatRegister rs);
  void fabss(FloatRegister rd, FloatRegister rs);
  void fsqrts(FloatRegister rd, FloatRegister rs);
  void fcmps(FloatRegister rs1, FloatRegister rs2, int fcc = 0);
  void fsubd(DoubleRegister rd, DoubleRegister rs1, DoubleRegister rs2);
  void fmuld(DoubleRegister rd, DoubleRegister rs1, DoubleRegister rs2);
  void fdivd(DoubleRegister rd, DoubleRegister rs1, DoubleRegister rs2);
  void fmovd(DoubleRegister rd, DoubleRegister rs);
  void fmovs(FloatRegister rd, FloatRegister rs);
  void fnegd(DoubleRegister rd, DoubleRegister rs);
  void fabsd(DoubleRegister rd, DoubleRegister rs);
  void fsqrtd(DoubleRegister rd, DoubleRegister rs);
  void fxtod(DoubleRegister rd, DoubleRegister rs);
  void fxtos(FloatRegister rd, DoubleRegister rs);
  void fdtos(FloatRegister rd, DoubleRegister rs);
  void fstod(DoubleRegister rd, FloatRegister rs);
  void fcmpd(DoubleRegister rs1, DoubleRegister rs2, int fcc = 0);
  void ldd(DoubleRegister rd, Register rs1, int32_t imm13);
  void std(DoubleRegister rd, Register rs1, int32_t imm13);
  void ldf(FloatRegister rd, Register rs1, int32_t imm13);
  void stf(FloatRegister rd, Register rs1, int32_t imm13);

  void nop() { sethi(g0, 0); }
  void mov(Register rd, Register rs) { or_(rd, g0, rs); }
  void mov(Register rd, int32_t imm13) { or_(rd, g0, imm13); }
  void cmp(Register rs1, Register rs2) { subcc(g0, rs1, rs2); }
  void cmp(Register rs1, int32_t imm13) { subcc(g0, rs1, imm13); }
  // Control transfers leave the delay slot to the caller.
  void retl() { jmpl(g0, o7, kSparcReturnAddressOffset); }
  void ret() { jmpl(g0, i7, kSparcReturnAddressOffset); }
  void restore() { restore(g0, g0, g0); }

 protected:
  virtual void emit(Instr instr) = 0;

 private:
  void GenFormat3(int op, int op3, int rd, Register rs1, Register rs2);
  void GenFormat3(int op, int op3, int rd, Register rs1, int32_t imm13);
  void GenShift(int op3, Register rd, Register rs1, int shift, bool wide);
  void GenShift(int op3, Register rd, Register rs1, Register shift, bool wide);
  void GenFPop(int opf, DoubleRegister rd, DoubleRegister rs1,
               DoubleRegister rs2);
  void GenFPopSingle(int opf, FloatRegister rd, FloatRegister rs1,
                     FloatRegister rs2);
};

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_BASE_ASSEMBLER_SPARC64_H_
