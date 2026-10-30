// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>

#include "src/base/memory.h"
#include "src/base/strings.h"
#include "src/codegen/sparc64/base-assembler-sparc64.h"
#include "src/diagnostics/disasm.h"

namespace disasm {

namespace {

int64_t SignedField(uint32_t value, int bits) {
  uint32_t mask = (uint32_t{1} << bits) - 1;
  value &= mask;
  return (value & (uint32_t{1} << (bits - 1))) != 0
             ? int64_t{value} - (int64_t{1} << bits)
             : value;
}

int DoubleEncodingToPhysical(int code) {
  return (code & 30) | ((code & 1) << 5);
}

}  // namespace

const char* NameConverter::NameOfCPURegister(int reg) const {
  return v8::internal::RegisterName(v8::internal::Register::from_code(reg));
}

const char* NameConverter::NameOfByteCPURegister(int) const { UNREACHABLE(); }

const char* NameConverter::NameOfXMMRegister(int) const { UNREACHABLE(); }

const char* NameConverter::NameOfAddress(uint8_t* addr) const {
  v8::base::SNPrintF(tmp_buffer_, "%p", static_cast<void*>(addr));
  return tmp_buffer_.begin();
}

const char* NameConverter::NameOfConstant(uint8_t* addr) const {
  return NameOfAddress(addr);
}

const char* NameConverter::NameInCode(uint8_t*) const { return ""; }

int Disassembler::InstructionDecode(v8::base::Vector<char> buffer,
                                    uint8_t* instruction) {
  uint32_t word = v8::base::ReadUnalignedValue<uint32_t>(
      reinterpret_cast<v8::base::Address>(instruction));
  const int op = word >> 30;
  const int op3 = (word >> 19) & 63;
  const int rd = (word >> 25) & 31;
  const int rs1 = (word >> 14) & 31;
  const int rs2 = word & 31;
  const bool immediate = (word & (1u << 13)) != 0;
  auto print = [&](const char* format, auto... args) {
    if constexpr (sizeof...(args) == 0) {
      v8::base::SNPrintF(buffer, "%s", format);
    } else {
      v8::base::SNPrintF(buffer, format, args...);
    }
  };
  auto reg = [&](int code) { return converter_.NameOfCPURegister(code); };
  auto target = [&](int64_t offset) {
    return converter_.NameOfAddress(reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(instruction) + offset));
  };
  v8::base::EmbeddedVector<char, 32> rhs;
  if (immediate) {
    v8::base::SNPrintF(rhs, "%d", static_cast<int>(SignedField(word, 13)));
  } else {
    v8::base::SNPrintF(rhs, "%%%s", reg(rs2));
  }

  if (word == 0x01000000u) {
    print("nop");
  } else if (word == 0x81580000u) {
    print("flushw");
  } else if (word == 0x81c3e008u) {
    print("retl");
  } else if (word == 0x81c7e008u) {
    print("ret");
  } else if (word == 0x81e80000u) {
    print("restore");
  } else if ((word & ~uint32_t{127}) == 0x91d02000u) {
    print("ta %u", word & 127);
  } else if (op == 1) {
    print("call %s", target(SignedField(word, 30) * 4));
  } else if (op == 0) {
    const int op2 = (word >> 22) & 7;
    const char* integer_names[] = {"bn",   "be",  "ble",  "bl",  "bleu", "bcs",
                                   "bneg", "bvs", "ba",   "bne", "bg",   "bge",
                                   "bgu",  "bcc", "bpos", "bvc"};
    const char* float_names[] = {
        "fbn", "fbne", "fblg", "fbul", "fbl",   "fbug", "fbg",   "fbu",
        "fba", "fbe",  "fbue", "fbge", "fbuge", "fble", "fbule", "fbo"};
    const int condition = (word >> 25) & 15;
    const char* annul = (word & (1u << 29)) ? ",a" : "";
    if (op2 == 4) {
      print("sethi 0x%x, %%%s", word & 0x3fffff, reg(rd));
    } else if (op2 == 2) {
      print("%s%s %s", integer_names[condition], annul,
            target(SignedField(word, 22) * 4));
    } else if (op2 == 1 || op2 == 5) {
      const int cc = (word >> 20) & 3;
      const char* prediction = (word & (1u << 19)) ? "pt" : "pn";
      if (op2 == 5) {
        print("%s%s,%s %%fcc%d, %s", float_names[condition], annul, prediction,
              cc, target(SignedField(word, 19) * 4));
      } else {
        print("%s%s,%s %%%s, %s", integer_names[condition], annul, prediction,
              cc == 2 ? "xcc" : "icc", target(SignedField(word, 19) * 4));
      }
    } else {
      print(".word 0x%08x", word);
    }
  } else if (op == 2 && op3 >= 0x25 && op3 <= 0x27) {
    const char* names[] = {"sll", "srl", "sra"};
    const bool wide = (word & (1u << 12)) != 0;
    if (immediate) {
      v8::base::SNPrintF(rhs, "%u", word & (wide ? 63 : 31));
    }
    print("%s%s %%%s, %s, %%%s", names[op3 - 0x25], wide ? "x" : "", reg(rs1),
          rhs.begin(), reg(rd));
  } else if (op == 2 && op3 == 0x28 && rs1 == 5 && !immediate) {
    print("rd %%pc, %%%s", reg(rd));
  } else if (op == 2 && op3 == 0x28 && rs1 == 15 && immediate) {
    print("membar 0x%x", word & 127);
  } else if (op == 2 && (op3 == 0x34 || op3 == 0x35)) {
    const int opf = (word >> 5) & 0x1ff;
    const bool wide = (opf & 1) == 0;
    auto fp = [&](int code) {
      return wide ? DoubleEncodingToPhysical(code) : code;
    };
    const char* name = nullptr;
    bool unary = false;
    switch (opf) {
      case 0x01:
      case 0x02:
        name = "fmov";
        unary = true;
        break;
      case 0x05:
      case 0x06:
        name = "fneg";
        unary = true;
        break;
      case 0x09:
      case 0x0a:
        name = "fabs";
        unary = true;
        break;
      case 0x29:
      case 0x2a:
        name = "fsqrt";
        unary = true;
        break;
      case 0x41:
      case 0x42:
        name = "fadd";
        break;
      case 0x45:
      case 0x46:
        name = "fsub";
        break;
      case 0x49:
      case 0x4a:
        name = "fmul";
        break;
      case 0x4d:
      case 0x4e:
        name = "fdiv";
        break;
    }
    if (op3 == 0x34 && (opf == 0xc6 || opf == 0xc9)) {
      print("%s %%f%d, %%f%d", opf == 0xc6 ? "fdtos" : "fstod",
            opf == 0xc6 ? DoubleEncodingToPhysical(rs2) : rs2,
            opf == 0xc9 ? DoubleEncodingToPhysical(rd) : rd);
    } else if (op3 == 0x34 && (opf == 0x84 || opf == 0x88)) {
      print("fxto%s %%f%d, %%f%d", opf == 0x88 ? "d" : "s",
            DoubleEncodingToPhysical(rs2),
            opf == 0x88 ? DoubleEncodingToPhysical(rd) : rd);
    } else if (op3 == 0x35 && (opf == 0x51 || opf == 0x52)) {
      print("fcmp%s %%fcc%d, %%f%d, %%f%d", wide ? "d" : "s", rd & 3, fp(rs1),
            fp(rs2));
    } else if (op3 == 0x34 && name != nullptr) {
      if (unary) {
        print("%s%s %%f%d, %%f%d", name, wide ? "d" : "s", fp(rs2), fp(rd));
      } else {
        print("%s%s %%f%d, %%f%d, %%f%d", name, wide ? "d" : "s", fp(rs1),
              fp(rs2), fp(rd));
      }
    } else {
      print(".word 0x%08x", word);
    }
  } else if (op == 2) {
    const char* name = nullptr;
    switch (op3) {
#define ALU_NAME(name_, value) \
  case value:                  \
    name = #name_;             \
    break;
      SPARC64_ALU_INSTRUCTIONS(ALU_NAME)
#undef ALU_NAME
    }
    if (op3 == 1) name = "and";
    if (op3 == 2) name = "or";
    if (op3 == 3) name = "xor";
    if (name == nullptr) {
      print(".word 0x%08x", word);
    } else if (op3 == 0x38) {
      print("jmpl %%%s + %s, %%%s", reg(rs1), rhs.begin(), reg(rd));
    } else {
      print("%s %%%s, %s, %%%s", name, reg(rs1), rhs.begin(), reg(rd));
    }
  } else if (op == 3 && op3 == 0x3e && ((word >> 5) & 0x1ff) == 0x80) {
    print("casx [%%%s], %%%s, %%%s", reg(rs1), reg(rs2), reg(rd));
  } else if (op == 3) {
    const char* name = nullptr;
    bool fp_reg = false;
    bool wide = false;
    switch (op3) {
#define MEMORY_NAME(name_, value) \
  case value:                     \
    name = #name_;                \
    break;
      SPARC64_MEMORY_INSTRUCTIONS(MEMORY_NAME)
#undef MEMORY_NAME
      case 0x20:
        name = "ld";
        fp_reg = true;
        break;
      case 0x23:
        name = "ldd";
        fp_reg = true;
        wide = true;
        break;
      case 0x24:
        name = "st";
        fp_reg = true;
        break;
      case 0x27:
        name = "std";
        fp_reg = true;
        wide = true;
        break;
    }
    v8::base::EmbeddedVector<char, 16> value;
    if (fp_reg) {
      v8::base::SNPrintF(value, "%%f%d",
                         wide ? DoubleEncodingToPhysical(rd) : rd);
    } else {
      v8::base::SNPrintF(value, "%%%s", reg(rd));
    }
    if (name == nullptr) {
      print(".word 0x%08x", word);
    } else if ((op3 & 4) != 0) {
      print("%s %s, [%%%s + %s]", name, value.begin(), reg(rs1), rhs.begin());
    } else {
      print("%s [%%%s + %s], %s", name, reg(rs1), rhs.begin(), value.begin());
    }
  } else {
    print(".word 0x%08x", word);
  }
  return v8::internal::kInstrSize;
}

int Disassembler::ConstantPoolSizeAt(uint8_t*) { return -1; }

void Disassembler::Disassemble(FILE* file, uint8_t* begin, uint8_t* end,
                               UnimplementedOpcodeAction action) {
  NameConverter converter;
  Disassembler disassembler(converter, action);
  for (uint8_t* pc = begin; pc < end; pc += v8::internal::kInstrSize) {
    v8::base::EmbeddedVector<char, 256> buffer;
    disassembler.InstructionDecode(buffer, pc);
    std::fprintf(file, "%p    %08x    %s\n", static_cast<void*>(pc),
                 v8::base::ReadUnalignedValue<uint32_t>(
                     reinterpret_cast<v8::base::Address>(pc)),
                 buffer.begin());
  }
}

}  // namespace disasm
