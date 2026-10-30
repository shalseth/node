// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_CODEGEN_SPARC64_SIMD_SPARC64_INL_H_
#define V8_CODEGEN_SPARC64_SIMD_SPARC64_INL_H_
#include "src/codegen/sparc64/simd-helpers-sparc64-inl.h"
namespace v8::internal::sparc64 {
class SimdEmitter {
 public:
  enum class Transform { kExtend, kSplat, kZeroExtend };

  void LoadVector(DoubleRegister dst, Register src_addr, Register offset_reg,
                  uintptr_t offset_imm, bool is_load_mem, bool i64_offset,
                  bool needs_shift, uint32_t* protected_load_pc) {
    sparc64::MemoryAddress(masm_, src_addr, offset_reg, offset_imm, i64_offset,
                           needs_shift ? 4 : 0);
    if (protected_load_pc) *protected_load_pc = masm_->pc_offset();
    for (int half = 0; half < 2; ++half) {
      masm_->mov(g1, 0);
      for (int n = 0; n < 8; ++n) {
        masm_->ldub(g3, g2, is_load_mem ? half * 8 + n : 15 - (half * 8 + n));
        if (n) masm_->sllx(g3, g3, n * 8);
        masm_->or_(g1, g1, g3);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void StoreVector(Register dst_addr, Register offset_reg, uintptr_t offset_imm,
                   DoubleRegister src, bool is_store_mem, bool i64_offset,
                   uint32_t* protected_store_pc) {
    sparc64::MemoryAddress(masm_, dst_addr, offset_reg, offset_imm, i64_offset);
    if (protected_store_pc) *protected_store_pc = masm_->pc_offset();
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      for (int n = 0; n < 8; ++n) {
        masm_->srlx(g3, g1, n * 8);
        masm_->stb(g3, g2, is_store_mem ? half * 8 + n : 15 - (half * 8 + n));
      }
    }
  }
  void LoadTransform(DoubleRegister dst, Register src_addr, Register offset_reg,
                     uintptr_t offset_imm, int bytes, Transform transform,
                     bool sign, uint32_t* protected_load_pc, bool i64_offset) {
    sparc64::MemoryAddress(masm_, src_addr, offset_reg, offset_imm, i64_offset);
    if (protected_load_pc) *protected_load_pc = masm_->pc_offset();
    const int bits = bytes * 8;
    if (transform != Transform::kExtend) {
      masm_->mov(g1, 0);
      for (unsigned n = 0; n < bytes; ++n) {
        masm_->ldub(g3, g2, n);
        if (n) masm_->sllx(g3, g3, n * 8);
        masm_->or_(g1, g1, g3);
      }
      if (transform == Transform::kSplat) {
        sparc64::Splat(masm_, dst, bits);
      } else {
        masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, 0), g1);
        masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, 1), g0);
      }
      return;
    }
    for (int half = 0; half < 2; ++half) {
      masm_->mov(g4, 0);
      for (int lane = 0; lane < 64 / (bits * 2); ++lane) {
        masm_->mov(g1, 0);
        for (unsigned n = 0; n < bytes; ++n) {
          masm_->ldub(g3, g2, half * 4 + lane * bytes + n);
          if (n) masm_->sllx(g3, g3, n * 8);
          masm_->or_(g1, g1, g3);
        }
        sparc64::TruncateLane(masm_, g1, bits, sign);
        sparc64::TruncateLane(masm_, g1, bits * 2);
        if (lane) masm_->sllx(g1, g1, lane * bits * 2);
        masm_->or_(g4, g4, g1);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g4);
    }
  }
  void LoadLane(DoubleRegister dst, DoubleRegister src, Register addr,
                Register offset_reg, uintptr_t offset_imm, int bytes,
                uint8_t lane, uint32_t* protected_load_pc, bool i64_offset) {
    sparc64::MemoryAddress(masm_, addr, offset_reg, offset_imm, i64_offset);
    if (protected_load_pc) *protected_load_pc = masm_->pc_offset();
    masm_->mov(g1, 0);
    for (unsigned n = 0; n < bytes; ++n) {
      masm_->ldub(g3, g2, n);
      if (n) masm_->sllx(g3, g3, n * 8);
      masm_->or_(g1, g1, g3);
    }
    sparc64::ReplaceLane(masm_, dst, src, bytes * 8, lane);
  }
  void StoreLane(Register dst, Register offset, uintptr_t offset_imm,
                 DoubleRegister src, int bytes, uint8_t lane,
                 uint32_t* protected_store_pc, bool i64_offset) {
    sparc64::MemoryAddress(masm_, dst, offset, offset_imm, i64_offset);
    sparc64::ReadLane(masm_, g1, src, bytes * 8, lane);
    if (protected_store_pc) *protected_store_pc = masm_->pc_offset();
    for (unsigned n = 0; n < bytes; ++n) {
      masm_->srlx(g3, g1, n * 8);
      masm_->stb(g3, g2, n);
    }
  }

  explicit SimdEmitter(MacroAssembler* masm) : masm_(masm) {}
  void emit_i8x16_shuffle(DoubleRegister dst, DoubleRegister lhs,
                          DoubleRegister rhs, const uint8_t shuffle[16],
                          bool is_swizzle) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, is_swizzle ? lhs : rhs, MemOperand(sp, 16));
    for (int half = 0; half < 2; ++half) {
      masm_->mov(g1, 0);
      for (int byte = 0; byte < 8; ++byte) {
        int lane = shuffle[half * 8 + byte];
        masm_->LoadU8(g2, MemOperand(sp, lane / 16 * 16 + 15 - lane % 16));
        if (byte) masm_->sllx(g2, g2, byte * 8);
        masm_->or_(g1, g1, g2);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
    masm_->Drop(4);
  }
  void emit_i8x16_swizzle(DoubleRegister dst, DoubleRegister lhs,
                          DoubleRegister rhs) {
    sparc64::SimdSwizzle(masm_, dst, lhs, rhs);
  }
  void emit_i8x16_relaxed_swizzle(DoubleRegister dst, DoubleRegister lhs,
                                  DoubleRegister rhs) {
    sparc64::SimdSwizzle(masm_, dst, lhs, rhs);
  }
  void emit_i32x4_relaxed_trunc_f32x4_s(DoubleRegister dst,
                                        DoubleRegister src) {
    emit_i32x4_sconvert_f32x4(dst, src);
  }
  void emit_i32x4_relaxed_trunc_f32x4_u(DoubleRegister dst,
                                        DoubleRegister src) {
    emit_i32x4_uconvert_f32x4(dst, src);
  }
  void emit_i32x4_relaxed_trunc_f64x2_s_zero(DoubleRegister dst,
                                             DoubleRegister src) {
    emit_i32x4_trunc_sat_f64x2_s_zero(dst, src);
  }
  void emit_i32x4_relaxed_trunc_f64x2_u_zero(DoubleRegister dst,
                                             DoubleRegister src) {
    emit_i32x4_trunc_sat_f64x2_u_zero(dst, src);
  }
  void emit_s128_relaxed_laneselect(DoubleRegister dst, DoubleRegister src1,
                                    DoubleRegister src2, DoubleRegister mask,
                                    int lane_width) {
    USE(lane_width);
    emit_s128_select(dst, src1, src2, mask);
  }
  void emit_i8x16_popcnt(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->Move(g2, INT64_C(0x5555555555555555));
      masm_->srlx(g3, g1, 1);
      masm_->and_(g3, g3, g2);
      masm_->sub(g1, g1, g3);
      masm_->Move(g2, INT64_C(0x3333333333333333));
      masm_->and_(g3, g1, g2);
      masm_->srlx(g1, g1, 2);
      masm_->and_(g1, g1, g2);
      masm_->add(g1, g1, g3);
      masm_->srlx(g3, g1, 4);
      masm_->add(g1, g1, g3);
      masm_->Move(g2, INT64_C(0x0f0f0f0f0f0f0f0f));
      masm_->and_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_i8x16_splat(DoubleRegister dst, Register src) {
    masm_->Move(g1, src);
    sparc64::Splat(masm_, dst, 8);
  }
  void emit_i16x8_splat(DoubleRegister dst, Register src) {
    masm_->Move(g1, src);
    sparc64::Splat(masm_, dst, 16);
  }
  void emit_i32x4_splat(DoubleRegister dst, Register src) {
    masm_->Move(g1, src);
    sparc64::Splat(masm_, dst, 32);
  }
  void emit_i64x2_splat(DoubleRegister dst, Register src) {
    masm_->Move(g1, src);
    sparc64::Splat(masm_, dst, 64);
  }
  void emit_f32x4_splat(DoubleRegister dst, DoubleRegister src) {
    masm_->MovFloatToInt(g1, src);
    sparc64::Splat(masm_, dst, 32);
  }
  void emit_f64x2_splat(DoubleRegister dst, DoubleRegister src) {
    masm_->MovDoubleToInt64(g1, src);
    sparc64::Splat(masm_, dst, 64);
  }
  void emit_i8x16_eq(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kEq);
  }
  void emit_i8x16_ne(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kNe);
  }
  void emit_i8x16_gt_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, true,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i8x16_gt_u(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i8x16_ge_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, true,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_i8x16_ge_u(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_i16x8_eq(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kEq);
  }
  void emit_i16x8_ne(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kNe);
  }
  void emit_i16x8_gt_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, true,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i16x8_gt_u(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i16x8_ge_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, true,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_i16x8_ge_u(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_i32x4_eq(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kEq);
  }
  void emit_i32x4_ne(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kNe);
  }
  void emit_i32x4_gt_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, true,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i32x4_gt_u(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i32x4_ge_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, true,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_i32x4_ge_u(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_i64x2_eq(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, false,
                        sparc64::SimdBinaryOp::kEq);
  }
  void emit_i64x2_ne(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, false,
                        sparc64::SimdBinaryOp::kNe);
  }
  void emit_i64x2_gt_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, true,
                        sparc64::SimdBinaryOp::kGt);
  }
  void emit_i64x2_ge_s(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, true,
                        sparc64::SimdBinaryOp::kGe);
  }
  void emit_f32x4_eq(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fcmps(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFEqual, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU32(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_ne(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fcmps(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFNotEqual, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU32(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_lt(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fcmps(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFLess, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU32(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_le(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fcmps(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFLessOrEqual, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU32(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f64x2_eq(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fcmpd(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFEqual, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU64(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f64x2_ne(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fcmpd(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFNotEqual, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU64(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f64x2_lt(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fcmpd(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFLess, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU64(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f64x2_le(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fcmpd(kScratchDoubleReg, dst);
      Label yes, done;
      masm_->fb(kFLessOrEqual, 0, &yes);
      masm_->nop();
      masm_->mov(g1, 0);
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&yes);
      masm_->mov(g1, -1);
      masm_->bind(&done);
      masm_->StoreU64(g1, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_s128_const(DoubleRegister dst, const uint8_t imms[16]) {
    for (int half = 0; half < 2; ++half) {
      uint64_t bits = 0;
      for (int n = 0; n < 8; ++n)
        bits |= uint64_t{imms[half * 8 + n]} << (n * 8);
      masm_->Move(g1, static_cast<intptr_t>(bits));
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_s128_not(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->xor_(g1, g1, -1);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_s128_and(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->MovDoubleToInt64(g2, sparc64::SimdHalf(rhs, half));
      masm_->and_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_s128_or(DoubleRegister dst, DoubleRegister lhs,
                    DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->MovDoubleToInt64(g2, sparc64::SimdHalf(rhs, half));
      masm_->or_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_s128_xor(DoubleRegister dst, DoubleRegister lhs,
                     DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->MovDoubleToInt64(g2, sparc64::SimdHalf(rhs, half));
      masm_->xor_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_s128_select(DoubleRegister dst, DoubleRegister src1,
                        DoubleRegister src2, DoubleRegister mask) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src1, half));
      masm_->MovDoubleToInt64(g2, sparc64::SimdHalf(src2, half));
      masm_->MovDoubleToInt64(g3, sparc64::SimdHalf(mask, half));
      masm_->xor_(g1, g1, g2);
      masm_->and_(g1, g1, g3);
      masm_->xor_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_i8x16_neg(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, true);
        masm_->sub(g4, g0, g4);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_v128_anytrue(Register dst, DoubleRegister src) {
    masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, 0));
    masm_->MovDoubleToInt64(g2, sparc64::SimdHalf(src, 1));
    masm_->or_(g1, g1, g2);
    masm_->cmp(g1, 0);
    Label done;
    masm_->mov(dst, 1);
    masm_->b(not_equal, ConditionCodes::kXcc, &done);
    masm_->nop();
    masm_->mov(dst, 0);
    masm_->bind(&done);
  }
  void emit_i8x16_alltrue(Register dst, DoubleRegister src) {
    Label zero, done;
    for (int lane = 0; lane < 16; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 8, lane);
      masm_->cmp(g1, 0);
      masm_->b(equal, ConditionCodes::kXcc, &zero);
      masm_->nop();
    }
    masm_->mov(dst, 1);
    masm_->Jump(&done);
    masm_->bind(&zero);
    masm_->mov(dst, 0);
    masm_->bind(&done);
  }
  void emit_i8x16_bitmask(Register dst, DoubleRegister src) {
    masm_->mov(g4, 0);
    for (int lane = 0; lane < 16; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 8, lane);
      masm_->srlx(g1, g1, 7);
      if (lane) masm_->sllx(g1, g1, lane);
      masm_->or_(g4, g4, g1);
    }
    masm_->Move(dst, g4);
  }
  void emit_i8x16_shl(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 7);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_shli(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 7);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_shr_s(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 7);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_shri_s(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 7);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_shr_u(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 7);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_shri_u(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 7);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_add(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kAdd);
  }
  void emit_i8x16_add_sat_s(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 8, 8, 0, true,
                           sparc64::SimdTransformOp::kAddSat);
  }
  void emit_i8x16_add_sat_u(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 8, 8, 0, false,
                           sparc64::SimdTransformOp::kAddSat);
  }
  void emit_i8x16_sub(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kSub);
  }
  void emit_i8x16_sub_sat_s(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 8, 8, 0, true,
                           sparc64::SimdTransformOp::kSubSat);
  }
  void emit_i8x16_sub_sat_u(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 8, 8, 0, false,
                           sparc64::SimdTransformOp::kSubSat);
  }
  void emit_i8x16_min_s(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, true,
                        sparc64::SimdBinaryOp::kMin);
  }
  void emit_i8x16_min_u(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kMin);
  }
  void emit_i8x16_max_s(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, true,
                        sparc64::SimdBinaryOp::kMax);
  }
  void emit_i8x16_max_u(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 8, false,
                        sparc64::SimdBinaryOp::kMax);
  }
  void emit_i16x8_neg(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, true);
        masm_->sub(g4, g0, g4);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_alltrue(Register dst, DoubleRegister src) {
    Label zero, done;
    for (int lane = 0; lane < 8; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 16, lane);
      masm_->cmp(g1, 0);
      masm_->b(equal, ConditionCodes::kXcc, &zero);
      masm_->nop();
    }
    masm_->mov(dst, 1);
    masm_->Jump(&done);
    masm_->bind(&zero);
    masm_->mov(dst, 0);
    masm_->bind(&done);
  }
  void emit_i16x8_bitmask(Register dst, DoubleRegister src) {
    masm_->mov(g4, 0);
    for (int lane = 0; lane < 8; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 16, lane);
      masm_->srlx(g1, g1, 15);
      if (lane) masm_->sllx(g1, g1, lane);
      masm_->or_(g4, g4, g1);
    }
    masm_->Move(dst, g4);
  }
  void emit_i16x8_shl(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 15);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_shli(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 15);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_shr_s(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 15);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_shri_s(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 15);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_shr_u(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 15);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_shri_u(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 15);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_add(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kAdd);
  }
  void emit_i16x8_add_sat_s(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 16, 0, true,
                           sparc64::SimdTransformOp::kAddSat);
  }
  void emit_i16x8_add_sat_u(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 16, 0, false,
                           sparc64::SimdTransformOp::kAddSat);
  }
  void emit_i16x8_sub(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kSub);
  }
  void emit_i16x8_sub_sat_s(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 16, 0, true,
                           sparc64::SimdTransformOp::kSubSat);
  }
  void emit_i16x8_sub_sat_u(DoubleRegister dst, DoubleRegister lhs,
                            DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 16, 0, false,
                           sparc64::SimdTransformOp::kSubSat);
  }
  void emit_i16x8_mul(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kMul);
  }
  void emit_i16x8_min_s(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, true,
                        sparc64::SimdBinaryOp::kMin);
  }
  void emit_i16x8_min_u(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kMin);
  }
  void emit_i16x8_max_s(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, true,
                        sparc64::SimdBinaryOp::kMax);
  }
  void emit_i16x8_max_u(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 16, false,
                        sparc64::SimdBinaryOp::kMax);
  }
  void emit_i16x8_extadd_pairwise_i8x16_s(DoubleRegister dst,
                                          DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 8, 16, 0, true,
                           sparc64::SimdTransformOp::kPairAdd);
  }
  void emit_i16x8_extadd_pairwise_i8x16_u(DoubleRegister dst,
                                          DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 8, 16, 0, false,
                           sparc64::SimdTransformOp::kPairAdd);
  }
  void emit_i16x8_extmul_low_i8x16_s(DoubleRegister dst, DoubleRegister src1,
                                     DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 8, 16, 0, true,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i16x8_extmul_low_i8x16_u(DoubleRegister dst, DoubleRegister src1,
                                     DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 8, 16, 0, false,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i16x8_extmul_high_i8x16_s(DoubleRegister dst, DoubleRegister src1,
                                      DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 8, 16, 8, true,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i16x8_extmul_high_i8x16_u(DoubleRegister dst, DoubleRegister src1,
                                      DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 8, 16, 8, false,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i16x8_q15mulr_sat_s(DoubleRegister dst, DoubleRegister src1,
                                DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 16, 16, 0, true,
                           sparc64::SimdTransformOp::kQ15);
  }
  void emit_i16x8_relaxed_q15mulr_s(DoubleRegister dst, DoubleRegister src1,
                                    DoubleRegister src2) {
    emit_i16x8_q15mulr_sat_s(dst, src1, src2);
  }
  void emit_i16x8_dot_i8x16_i7x16_s(DoubleRegister dst, DoubleRegister src1,
                                    DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 8, 16, 0, true,
                           sparc64::SimdTransformOp::kDot);
  }
  void emit_i32x4_dot_i8x16_i7x16_add_s(DoubleRegister dst, DoubleRegister src1,
                                        DoubleRegister src2,
                                        DoubleRegister acc) {
    sparc64::SimdDotAdd(masm_, dst, src1, src2, acc);
  }
  void emit_i32x4_neg(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, true);
        masm_->sub(g4, g0, g4);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_alltrue(Register dst, DoubleRegister src) {
    Label zero, done;
    for (int lane = 0; lane < 4; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 32, lane);
      masm_->cmp(g1, 0);
      masm_->b(equal, ConditionCodes::kXcc, &zero);
      masm_->nop();
    }
    masm_->mov(dst, 1);
    masm_->Jump(&done);
    masm_->bind(&zero);
    masm_->mov(dst, 0);
    masm_->bind(&done);
  }
  void emit_i32x4_bitmask(Register dst, DoubleRegister src) {
    masm_->mov(g4, 0);
    for (int lane = 0; lane < 4; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 32, lane);
      masm_->srlx(g1, g1, 31);
      if (lane) masm_->sllx(g1, g1, lane);
      masm_->or_(g4, g4, g1);
    }
    masm_->Move(dst, g4);
  }
  void emit_i32x4_shl(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 31);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_shli(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 31);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_shr_s(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 31);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_shri_s(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 31);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_shr_u(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 31);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_shri_u(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 31);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_add(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kAdd);
  }
  void emit_i32x4_sub(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kSub);
  }
  void emit_i32x4_mul(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kMul);
  }
  void emit_i32x4_min_s(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, true,
                        sparc64::SimdBinaryOp::kMin);
  }
  void emit_i32x4_min_u(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kMin);
  }
  void emit_i32x4_max_s(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, true,
                        sparc64::SimdBinaryOp::kMax);
  }
  void emit_i32x4_max_u(DoubleRegister dst, DoubleRegister lhs,
                        DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 32, false,
                        sparc64::SimdBinaryOp::kMax);
  }
  void emit_i32x4_dot_i16x8_s(DoubleRegister dst, DoubleRegister lhs,
                              DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 32, 0, true,
                           sparc64::SimdTransformOp::kDot);
  }
  void emit_i32x4_extadd_pairwise_i16x8_s(DoubleRegister dst,
                                          DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 16, 32, 0, true,
                           sparc64::SimdTransformOp::kPairAdd);
  }
  void emit_i32x4_extadd_pairwise_i16x8_u(DoubleRegister dst,
                                          DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 16, 32, 0, false,
                           sparc64::SimdTransformOp::kPairAdd);
  }
  void emit_i32x4_extmul_low_i16x8_s(DoubleRegister dst, DoubleRegister src1,
                                     DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 16, 32, 0, true,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i32x4_extmul_low_i16x8_u(DoubleRegister dst, DoubleRegister src1,
                                     DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 16, 32, 0, false,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i32x4_extmul_high_i16x8_s(DoubleRegister dst, DoubleRegister src1,
                                      DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 16, 32, 4, true,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i32x4_extmul_high_i16x8_u(DoubleRegister dst, DoubleRegister src1,
                                      DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 16, 32, 4, false,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i64x2_neg(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, true);
        masm_->sub(g4, g0, g4);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_alltrue(Register dst, DoubleRegister src) {
    Label zero, done;
    for (int lane = 0; lane < 2; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 64, lane);
      masm_->cmp(g1, 0);
      masm_->b(equal, ConditionCodes::kXcc, &zero);
      masm_->nop();
    }
    masm_->mov(dst, 1);
    masm_->Jump(&done);
    masm_->bind(&zero);
    masm_->mov(dst, 0);
    masm_->bind(&done);
  }
  void emit_i64x2_shl(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 63);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_shli(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 63);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, false);
        masm_->sllx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_shr_s(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 63);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_shri_s(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 63);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, true);
        masm_->srax(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_shr_u(DoubleRegister dst, DoubleRegister lhs, Register rhs) {
    masm_->and_(g2, rhs, 63);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_shri_u(DoubleRegister dst, DoubleRegister lhs, int32_t rhs) {
    masm_->Move(g2, rhs & 63);
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, false);
        masm_->srlx(g4, g4, g2);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_add(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, false,
                        sparc64::SimdBinaryOp::kAdd);
  }
  void emit_i64x2_sub(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, false,
                        sparc64::SimdBinaryOp::kSub);
  }
  void emit_i64x2_mul(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    sparc64::SimdBinary(masm_, dst, lhs, rhs, 64, false,
                        sparc64::SimdBinaryOp::kMul);
  }
  void emit_i64x2_extmul_low_i32x4_s(DoubleRegister dst, DoubleRegister src1,
                                     DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 32, 64, 0, true,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i64x2_extmul_low_i32x4_u(DoubleRegister dst, DoubleRegister src1,
                                     DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 32, 64, 0, false,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i64x2_extmul_high_i32x4_s(DoubleRegister dst, DoubleRegister src1,
                                      DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 32, 64, 2, true,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i64x2_extmul_high_i32x4_u(DoubleRegister dst, DoubleRegister src1,
                                      DoubleRegister src2) {
    sparc64::TransformSimd(masm_, dst, src1, src2, 32, 64, 2, false,
                           sparc64::SimdTransformOp::kExtMul);
  }
  void emit_i64x2_bitmask(Register dst, DoubleRegister src) {
    masm_->mov(g4, 0);
    for (int lane = 0; lane < 2; ++lane) {
      sparc64::ReadLane(masm_, g1, src, 64, lane);
      masm_->srlx(g1, g1, 63);
      if (lane) masm_->sllx(g1, g1, lane);
      masm_->or_(g4, g4, g1);
    }
    masm_->Move(dst, g4);
  }
  void emit_i64x2_sconvert_i32x4_low(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 32, 64, 0, true,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i64x2_sconvert_i32x4_high(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 32, 64, 2, true,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i64x2_uconvert_i32x4_low(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 32, 64, 0, false,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i64x2_uconvert_i32x4_high(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 32, 64, 2, false,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_f32x4_abs(DoubleRegister dst, DoubleRegister src) {
    masm_->Move(g2, static_cast<intptr_t>(UINT64_C(0x7fffffff7fffffff)));
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->and_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_f32x4_neg(DoubleRegister dst, DoubleRegister src) {
    masm_->Move(g2, static_cast<intptr_t>(UINT64_C(0x8000000080000000)));
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->xor_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_f32x4_sqrt(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->fsqrts(dst, kScratchDoubleReg);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 16 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_f32x4_add(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fadds(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_sub(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fsubs(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_mul(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fmuls(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_div(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fdivs(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_min(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      emit_f32_min(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_max(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      emit_f32_max(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_pmin(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fcmps(kScratchDoubleReg, dst);
      Label choose_left, done;
      masm_->fb(kFUnorderedOrLessOrEqual, 0, &choose_left);
      masm_->nop();
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&choose_left);
      masm_->fmovs(dst, kScratchDoubleReg);
      masm_->bind(&done);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_pmax(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fcmps(kScratchDoubleReg, dst);
      Label choose_left, done;
      masm_->fb(kFUnorderedOrGreaterOrEqual, 0, &choose_left);
      masm_->nop();
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&choose_left);
      masm_->fmovs(dst, kScratchDoubleReg);
      masm_->bind(&done);
      masm_->StoreF32(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f32x4_relaxed_min(DoubleRegister dst, DoubleRegister lhs,
                              DoubleRegister rhs) {
    emit_f32x4_pmin(dst, lhs, rhs);
  }
  void emit_f32x4_relaxed_max(DoubleRegister dst, DoubleRegister lhs,
                              DoubleRegister rhs) {
    emit_f32x4_pmax(dst, lhs, rhs);
  }
  void emit_f64x2_abs(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half)
      masm_->fabsd(sparc64::SimdHalf(dst, half), sparc64::SimdHalf(src, half));
  }
  void emit_f64x2_neg(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half)
      masm_->fnegd(sparc64::SimdHalf(dst, half), sparc64::SimdHalf(src, half));
  }
  void emit_f64x2_sqrt(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      DoubleRegister result = sparc64::SimdHalf(dst, half);
      masm_->fsqrtd(result, sparc64::SimdHalf(src, half));
      sparc64::CanonicalizeNaN(masm_, result, false);
    }
  }
  void emit_f64x2_add(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      DoubleRegister result = sparc64::SimdHalf(dst, half);
      masm_->faddd(result, sparc64::SimdHalf(lhs, half),
                   sparc64::SimdHalf(rhs, half));
      sparc64::CanonicalizeNaN(masm_, result, false);
    }
  }
  void emit_f64x2_sub(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      DoubleRegister result = sparc64::SimdHalf(dst, half);
      masm_->fsubd(result, sparc64::SimdHalf(lhs, half),
                   sparc64::SimdHalf(rhs, half));
      sparc64::CanonicalizeNaN(masm_, result, false);
    }
  }
  void emit_f64x2_mul(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      DoubleRegister result = sparc64::SimdHalf(dst, half);
      masm_->fmuld(result, sparc64::SimdHalf(lhs, half),
                   sparc64::SimdHalf(rhs, half));
      sparc64::CanonicalizeNaN(masm_, result, false);
    }
  }
  void emit_f64x2_div(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      DoubleRegister result = sparc64::SimdHalf(dst, half);
      masm_->fdivd(result, sparc64::SimdHalf(lhs, half),
                   sparc64::SimdHalf(rhs, half));
      sparc64::CanonicalizeNaN(masm_, result, false);
    }
  }
  void emit_f64x2_min(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half)
      emit_f64_min(sparc64::SimdHalf(dst, half), sparc64::SimdHalf(lhs, half),
                   sparc64::SimdHalf(rhs, half));
  }
  void emit_f64x2_max(DoubleRegister dst, DoubleRegister lhs,
                      DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half)
      emit_f64_max(sparc64::SimdHalf(dst, half), sparc64::SimdHalf(lhs, half),
                   sparc64::SimdHalf(rhs, half));
  }
  void emit_f64x2_pmin(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fcmpd(kScratchDoubleReg, dst);
      Label choose_left, done;
      masm_->fb(kFUnorderedOrLessOrEqual, 0, &choose_left);
      masm_->nop();
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&choose_left);
      masm_->fmovd(dst, kScratchDoubleReg);
      masm_->bind(&done);
      masm_->StoreF64(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f64x2_pmax(DoubleRegister dst, DoubleRegister lhs,
                       DoubleRegister rhs) {
    masm_->AllocateStackSpace(48);
    sparc64::StoreSimd(masm_, lhs, MemOperand(sp));
    sparc64::StoreSimd(masm_, rhs, MemOperand(sp, 16));
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fcmpd(kScratchDoubleReg, dst);
      Label choose_left, done;
      masm_->fb(kFUnorderedOrGreaterOrEqual, 0, &choose_left);
      masm_->nop();
      masm_->ba(&done);
      masm_->nop();
      masm_->bind(&choose_left);
      masm_->fmovd(dst, kScratchDoubleReg);
      masm_->bind(&done);
      masm_->StoreF64(dst, MemOperand(sp, 32 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 32));
    masm_->Drop(6);
  }
  void emit_f64x2_relaxed_min(DoubleRegister dst, DoubleRegister lhs,
                              DoubleRegister rhs) {
    emit_f64x2_pmin(dst, lhs, rhs);
  }
  void emit_f64x2_relaxed_max(DoubleRegister dst, DoubleRegister lhs,
                              DoubleRegister rhs) {
    emit_f64x2_pmax(dst, lhs, rhs);
  }
  void emit_f64x2_convert_low_i32x4_s(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 2; ++lane) {
      sparc64::ReadStackLane(masm_, g1, 0, 32, lane, false);
      compiler::EmitSparc64IntegerToFloat(masm_, dst, g1, true, false, false);
      sparc64::CanonicalizeNaN(masm_, dst, false);
      masm_->StoreF64(dst,
                      MemOperand(sp, 16 + sparc64::SimdLaneOffset(64, lane)));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_f64x2_convert_low_i32x4_u(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 2; ++lane) {
      sparc64::ReadStackLane(masm_, g1, 0, 32, lane, false);
      compiler::EmitSparc64IntegerToFloat(masm_, dst, g1, true, true, false);
      sparc64::CanonicalizeNaN(masm_, dst, false);
      masm_->StoreF64(dst,
                      MemOperand(sp, 16 + sparc64::SimdLaneOffset(64, lane)));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_i32x4_trunc_sat_f64x2_s_zero(DoubleRegister dst,
                                         DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 2; ++lane) {
      masm_->LoadF64(dst, MemOperand(sp, sparc64::SimdLaneOffset(64, lane)));
      sparc64::SaturatingFloatToInteger(masm_, g1, dst, true, false, false);
      sparc64::WriteStackLane(masm_, g1, 16, 32, lane);
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_i32x4_trunc_sat_f64x2_u_zero(DoubleRegister dst,
                                         DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 2; ++lane) {
      masm_->LoadF64(dst, MemOperand(sp, sparc64::SimdLaneOffset(64, lane)));
      sparc64::SaturatingFloatToInteger(masm_, g1, dst, true, true, false);
      sparc64::WriteStackLane(masm_, g1, 16, 32, lane);
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_f32x4_demote_f64x2_zero(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 2; ++lane) {
      masm_->LoadF64(dst, MemOperand(sp, sparc64::SimdLaneOffset(64, lane)));
      masm_->fdtos(dst, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst,
                      MemOperand(sp, 16 + sparc64::SimdLaneOffset(32, lane)));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_f64x2_promote_low_f32x4(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 2; ++lane) {
      masm_->LoadF32(dst, MemOperand(sp, sparc64::SimdLaneOffset(32, lane)));
      masm_->fstod(dst, dst);
      sparc64::CanonicalizeNaN(masm_, dst, false);
      masm_->StoreF64(dst,
                      MemOperand(sp, 16 + sparc64::SimdLaneOffset(64, lane)));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_i32x4_sconvert_f32x4(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 4; ++lane) {
      masm_->LoadF32(dst, MemOperand(sp, sparc64::SimdLaneOffset(32, lane)));
      sparc64::SaturatingFloatToInteger(masm_, g1, dst, true, false, true);
      sparc64::WriteStackLane(masm_, g1, 16, 32, lane);
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_i32x4_uconvert_f32x4(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 4; ++lane) {
      masm_->LoadF32(dst, MemOperand(sp, sparc64::SimdLaneOffset(32, lane)));
      sparc64::SaturatingFloatToInteger(masm_, g1, dst, true, true, true);
      sparc64::WriteStackLane(masm_, g1, 16, 32, lane);
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_f32x4_sconvert_i32x4(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 4; ++lane) {
      sparc64::ReadStackLane(masm_, g1, 0, 32, lane, false);
      compiler::EmitSparc64IntegerToFloat(masm_, dst, g1, true, false, true);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst,
                      MemOperand(sp, 16 + sparc64::SimdLaneOffset(32, lane)));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_f32x4_uconvert_i32x4(DoubleRegister dst, DoubleRegister src) {
    masm_->AllocateStackSpace(32);
    sparc64::StoreSimd(masm_, src, MemOperand(sp));
    masm_->StoreU64(g0, MemOperand(sp, 16));
    masm_->StoreU64(g0, MemOperand(sp, 24));
    for (int lane = 0; lane < 4; ++lane) {
      sparc64::ReadStackLane(masm_, g1, 0, 32, lane, false);
      compiler::EmitSparc64IntegerToFloat(masm_, dst, g1, true, true, true);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst,
                      MemOperand(sp, 16 + sparc64::SimdLaneOffset(32, lane)));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 16));
    masm_->Drop(4);
  }
  void emit_i8x16_sconvert_i16x8(DoubleRegister dst, DoubleRegister lhs,
                                 DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 8, 0, true,
                           sparc64::SimdTransformOp::kNarrow);
  }
  void emit_i8x16_uconvert_i16x8(DoubleRegister dst, DoubleRegister lhs,
                                 DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 8, 0, false,
                           sparc64::SimdTransformOp::kNarrow);
  }
  void emit_i16x8_sconvert_i32x4(DoubleRegister dst, DoubleRegister lhs,
                                 DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 32, 16, 0, true,
                           sparc64::SimdTransformOp::kNarrow);
  }
  void emit_i16x8_uconvert_i32x4(DoubleRegister dst, DoubleRegister lhs,
                                 DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 32, 16, 0, false,
                           sparc64::SimdTransformOp::kNarrow);
  }
  void emit_i16x8_sconvert_i8x16_low(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 8, 16, 0, true,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i16x8_sconvert_i8x16_high(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 8, 16, 8, true,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i16x8_uconvert_i8x16_low(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 8, 16, 0, false,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i16x8_uconvert_i8x16_high(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 8, 16, 8, false,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i32x4_sconvert_i16x8_low(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 16, 32, 0, true,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i32x4_sconvert_i16x8_high(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 16, 32, 4, true,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i32x4_uconvert_i16x8_low(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 16, 32, 0, false,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_i32x4_uconvert_i16x8_high(DoubleRegister dst, DoubleRegister src) {
    sparc64::TransformSimd(masm_, dst, src, src, 16, 32, 4, false,
                           sparc64::SimdTransformOp::kWiden);
  }
  void emit_s128_and_not(DoubleRegister dst, DoubleRegister lhs,
                         DoubleRegister rhs) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(lhs, half));
      masm_->MovDoubleToInt64(g2, sparc64::SimdHalf(rhs, half));
      masm_->xor_(g2, g2, -1);
      masm_->and_(g1, g1, g2);
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g1);
    }
  }
  void emit_i8x16_rounding_average_u(DoubleRegister dst, DoubleRegister lhs,
                                     DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 8, 8, 0, false,
                           sparc64::SimdTransformOp::kAverage);
  }
  void emit_i16x8_rounding_average_u(DoubleRegister dst, DoubleRegister lhs,
                                     DoubleRegister rhs) {
    sparc64::TransformSimd(masm_, dst, lhs, rhs, 16, 16, 0, false,
                           sparc64::SimdTransformOp::kAverage);
  }
  void emit_i8x16_abs(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 8) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 8, true);
        Label positive;
        masm_->cmp(g4, 0);
        masm_->b(greater_equal, ConditionCodes::kXcc, &positive);
        masm_->nop();
        masm_->sub(g4, g0, g4);
        masm_->bind(&positive);
        sparc64::TruncateLane(masm_, g4, 8);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i16x8_abs(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 16) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 16, true);
        Label positive;
        masm_->cmp(g4, 0);
        masm_->b(greater_equal, ConditionCodes::kXcc, &positive);
        masm_->nop();
        masm_->sub(g4, g0, g4);
        masm_->bind(&positive);
        sparc64::TruncateLane(masm_, g4, 16);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i32x4_abs(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 32) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 32, true);
        Label positive;
        masm_->cmp(g4, 0);
        masm_->b(greater_equal, ConditionCodes::kXcc, &positive);
        masm_->nop();
        masm_->sub(g4, g0, g4);
        masm_->bind(&positive);
        sparc64::TruncateLane(masm_, g4, 32);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i64x2_abs(DoubleRegister dst, DoubleRegister src) {
    for (int half = 0; half < 2; ++half) {
      masm_->MovDoubleToInt64(g1, sparc64::SimdHalf(src, half));
      masm_->mov(g3, 0);
      for (int shift = 0; shift < 64; shift += 64) {
        masm_->srlx(g4, g1, shift);
        sparc64::TruncateLane(masm_, g4, 64, true);
        Label positive;
        masm_->cmp(g4, 0);
        masm_->b(greater_equal, ConditionCodes::kXcc, &positive);
        masm_->nop();
        masm_->sub(g4, g0, g4);
        masm_->bind(&positive);
        sparc64::TruncateLane(masm_, g4, 64);
        if (shift) masm_->sllx(g4, g4, shift);
        masm_->or_(g3, g3, g4);
      }
      masm_->MovInt64ToDouble(sparc64::SimdHalf(dst, half), g3);
    }
  }
  void emit_i8x16_extract_lane_s(Register dst, DoubleRegister lhs,
                                 uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, dst, lhs, 8, imm_lane_idx, true);
    masm_->srl(dst, dst, 0);
  }
  void emit_i8x16_extract_lane_u(Register dst, DoubleRegister lhs,
                                 uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, dst, lhs, 8, imm_lane_idx, false);
    masm_->srl(dst, dst, 0);
  }
  void emit_i16x8_extract_lane_s(Register dst, DoubleRegister lhs,
                                 uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, dst, lhs, 16, imm_lane_idx, true);
    masm_->srl(dst, dst, 0);
  }
  void emit_i16x8_extract_lane_u(Register dst, DoubleRegister lhs,
                                 uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, dst, lhs, 16, imm_lane_idx, false);
    masm_->srl(dst, dst, 0);
  }
  void emit_i32x4_extract_lane(Register dst, DoubleRegister lhs,
                               uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, dst, lhs, 32, imm_lane_idx, false);
    masm_->srl(dst, dst, 0);
  }
  void emit_i64x2_extract_lane(Register dst, DoubleRegister lhs,
                               uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, dst, lhs, 64, imm_lane_idx, false);
  }
  void emit_f32x4_extract_lane(DoubleRegister dst, DoubleRegister lhs,
                               uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, g1, lhs, 32, imm_lane_idx);
    masm_->MovIntToFloat(dst, g1);
  }
  void emit_f64x2_extract_lane(DoubleRegister dst, DoubleRegister lhs,
                               uint8_t imm_lane_idx) {
    sparc64::ReadLane(masm_, g1, lhs, 64, imm_lane_idx);
    masm_->MovInt64ToDouble(dst, g1);
  }
  void emit_i8x16_replace_lane(DoubleRegister dst, DoubleRegister src1,
                               Register src2, uint8_t imm_lane_idx) {
    masm_->Move(g1, src2);
    sparc64::ReplaceLane(masm_, dst, src1, 8, imm_lane_idx);
  }
  void emit_i16x8_replace_lane(DoubleRegister dst, DoubleRegister src1,
                               Register src2, uint8_t imm_lane_idx) {
    masm_->Move(g1, src2);
    sparc64::ReplaceLane(masm_, dst, src1, 16, imm_lane_idx);
  }
  void emit_i32x4_replace_lane(DoubleRegister dst, DoubleRegister src1,
                               Register src2, uint8_t imm_lane_idx) {
    masm_->Move(g1, src2);
    sparc64::ReplaceLane(masm_, dst, src1, 32, imm_lane_idx);
  }
  void emit_i64x2_replace_lane(DoubleRegister dst, DoubleRegister src1,
                               Register src2, uint8_t imm_lane_idx) {
    masm_->Move(g1, src2);
    sparc64::ReplaceLane(masm_, dst, src1, 64, imm_lane_idx);
  }
  void emit_f32x4_replace_lane(DoubleRegister dst, DoubleRegister src1,
                               DoubleRegister src2, uint8_t imm_lane_idx) {
    masm_->MovFloatToInt(g1, src2);
    sparc64::ReplaceLane(masm_, dst, src1, 32, imm_lane_idx);
  }
  void emit_f64x2_replace_lane(DoubleRegister dst, DoubleRegister src1,
                               DoubleRegister src2, uint8_t imm_lane_idx) {
    masm_->MovDoubleToInt64(g1, src2);
    sparc64::ReplaceLane(masm_, dst, src1, 64, imm_lane_idx);
  }
  void emit_f32x4_qfma(DoubleRegister dst, DoubleRegister src1,
                       DoubleRegister src2, DoubleRegister src3) {
    masm_->AllocateStackSpace(64);
    sparc64::StoreSimd(masm_, src1, MemOperand(sp));
    sparc64::StoreSimd(masm_, src2, MemOperand(sp, 16));
    sparc64::StoreSimd(masm_, src3, MemOperand(sp, 32));
    // Relaxed SIMD permits the unfused multiply/add result.
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fmuls(kScratchDoubleReg, kScratchDoubleReg, dst);
      masm_->LoadF32(dst, MemOperand(sp, 32 + offset));
      masm_->fadds(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 48 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 48));
    masm_->Drop(8);
  }
  void emit_f32x4_qfms(DoubleRegister dst, DoubleRegister src1,
                       DoubleRegister src2, DoubleRegister src3) {
    masm_->AllocateStackSpace(64);
    sparc64::StoreSimd(masm_, src1, MemOperand(sp));
    sparc64::StoreSimd(masm_, src2, MemOperand(sp, 16));
    sparc64::StoreSimd(masm_, src3, MemOperand(sp, 32));
    // Relaxed SIMD permits the unfused multiply/add result.
    for (int lane = 0; lane < 4; ++lane) {
      int offset = sparc64::SimdLaneOffset(32, lane);
      masm_->LoadF32(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF32(dst, MemOperand(sp, 16 + offset));
      masm_->fmuls(kScratchDoubleReg, kScratchDoubleReg, dst);
      masm_->LoadF32(dst, MemOperand(sp, 32 + offset));
      masm_->fsubs(dst, dst, kScratchDoubleReg);
      sparc64::CanonicalizeNaN(masm_, dst, true);
      masm_->StoreF32(dst, MemOperand(sp, 48 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 48));
    masm_->Drop(8);
  }
  void emit_f64x2_qfma(DoubleRegister dst, DoubleRegister src1,
                       DoubleRegister src2, DoubleRegister src3) {
    masm_->AllocateStackSpace(64);
    sparc64::StoreSimd(masm_, src1, MemOperand(sp));
    sparc64::StoreSimd(masm_, src2, MemOperand(sp, 16));
    sparc64::StoreSimd(masm_, src3, MemOperand(sp, 32));
    // Relaxed SIMD permits the unfused multiply/add result.
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fmuld(kScratchDoubleReg, kScratchDoubleReg, dst);
      masm_->LoadF64(dst, MemOperand(sp, 32 + offset));
      masm_->faddd(dst, kScratchDoubleReg, dst);
      sparc64::CanonicalizeNaN(masm_, dst, false);
      masm_->StoreF64(dst, MemOperand(sp, 48 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 48));
    masm_->Drop(8);
  }
  void emit_f64x2_qfms(DoubleRegister dst, DoubleRegister src1,
                       DoubleRegister src2, DoubleRegister src3) {
    masm_->AllocateStackSpace(64);
    sparc64::StoreSimd(masm_, src1, MemOperand(sp));
    sparc64::StoreSimd(masm_, src2, MemOperand(sp, 16));
    sparc64::StoreSimd(masm_, src3, MemOperand(sp, 32));
    // Relaxed SIMD permits the unfused multiply/add result.
    for (int lane = 0; lane < 2; ++lane) {
      int offset = sparc64::SimdLaneOffset(64, lane);
      masm_->LoadF64(kScratchDoubleReg, MemOperand(sp, offset));
      masm_->LoadF64(dst, MemOperand(sp, 16 + offset));
      masm_->fmuld(kScratchDoubleReg, kScratchDoubleReg, dst);
      masm_->LoadF64(dst, MemOperand(sp, 32 + offset));
      masm_->fsubd(dst, dst, kScratchDoubleReg);
      sparc64::CanonicalizeNaN(masm_, dst, false);
      masm_->StoreF64(dst, MemOperand(sp, 48 + offset));
    }
    sparc64::LoadSimd(masm_, dst, MemOperand(sp, 48));
    masm_->Drop(8);
  }

 private:
  void emit_f32_min(DoubleRegister dst, DoubleRegister lhs,
                    DoubleRegister rhs) {
    compiler::EmitSparc64FloatMinMax(masm_, dst, lhs, rhs, true, true);
  }
  void emit_f32_max(DoubleRegister dst, DoubleRegister lhs,
                    DoubleRegister rhs) {
    compiler::EmitSparc64FloatMinMax(masm_, dst, lhs, rhs, true, false);
  }
  void emit_f64_min(DoubleRegister dst, DoubleRegister lhs,
                    DoubleRegister rhs) {
    compiler::EmitSparc64FloatMinMax(masm_, dst, lhs, rhs, false, true);
  }
  void emit_f64_max(DoubleRegister dst, DoubleRegister lhs,
                    DoubleRegister rhs) {
    compiler::EmitSparc64FloatMinMax(masm_, dst, lhs, rhs, false, false);
  }
  MacroAssembler* masm_;
};
}  // namespace v8::internal::sparc64
#endif  // V8_CODEGEN_SPARC64_SIMD_SPARC64_INL_H_
