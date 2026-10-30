// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

.text
.align 4
.global sparc64_reference_begin
.global sparc64_reference_end
sparc64_reference_begin:
  sllx %o1, 63, %o0
  srlx %o1, 32, %o0
  srax %o1, 0, %o0
  ldx [%o1 - 4096], %o0
  lduw [%o1 + 4095], %o0
  ldsw [%o1 + %g1], %o0
  ldub [%o1 + 7], %o0
  lduh [%o1 + 6], %o0
  ldsb [%o1 + 0], %o0
  ldsh [%o1 + 2], %o0
  stx %o0, [%o1 + 16]
  stw %o0, [%o1 + %g1]
  stb %o0, [%o1 + 1]
  sth %o0, [%o1 + 2]
  membar #Sync
  membar #LoadLoad | #LoadStore | #StoreStore
  casx [%o0], %o1, %o2
  faddd %f2, %f4, %f0
  fsubd %f34, %f62, %f32
  fmuld %f32, %f30, %f62
  fdivd %f62, %f32, %f30
  ldd [%o0 + 0], %f32
  std %f62, [%o0 + 8]
  mulx %o1, %o2, %o0
  sdivx %o1, -1, %o0
  udivx %o1, %o2, %o0
  addcc %o1, %o2, %o0
  subcc %o1, 1, %o0
  and %o1, %o2, %o0
  or %o1, 4095, %o0
  xor %o1, %o2, %o0
  sethi 0x3fffff, %o0
  be,pt %xcc, .+16
   nop
  bne,a,pt %icc, .-4
   nop
  call .+16
   nop
  call .-4
   nop
  rd %pc, %o0
   nop
  ba .+16
   nop
  flushw
  sll %o1, 31, %o0
  srl %o1, 0, %o0
  sra %o1, 16, %o0
  sll %o1, %o2, %o0
  srl %o1, %o2, %o0
  sra %o1, %o2, %o0
  sllx %o1, %o2, %o0
  srlx %o1, %o2, %o0
  srax %o1, %o2, %o0
  fmovd %f0, %f32
  fnegd %f32, %f62
  fabsd %f62, %f0
  fsqrtd %f32, %f62
  fcmpd %fcc0, %f0, %f2
  fcmpd %fcc3, %f32, %f62
  fbu,pt %fcc0, .+16
   nop
  fbl,a,pt %fcc3, .-4
   nop
  ta 1
  ld [%o0 + 4], %f30
  st %f28, [%o0 - 4]
  fmovs %f28, %f30
  andcc %o0, %o1, %g0
  fadds %f28, %f30, %f26
  fsubs %f30, %f26, %f28
  fmuls %f26, %f28, %f30
  fdivs %f28, %f30, %f26
  fnegs %f26, %f28
  fabss %f28, %f30
  fsqrts %f30, %f26
  fcmps %fcc3, %f28, %f30
  fxtod %f32, %f62
  fxtod %f62, %f0
  fxtos %f62, %f30
  fxtos %f32, %f0
  fdtos %f62, %f30
  fstod %f30, %f62
  fdtos %f32, %f0
  fstod %f0, %f32
sparc64_reference_end:
.section .note.GNU-stack,"",@progbits
