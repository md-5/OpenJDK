/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/codeBlob.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zBarrierSet.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zBarrierSetRuntime.hpp"
#include "memory/resourceArea.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/z/c1/zBarrierSetC1.hpp"
#endif // COMPILER1

#include "gc/z/zThreadLocalData.hpp"

ZBarrierSetAssembler::ZBarrierSetAssembler() :
    _load_barrier_slow_stub(),
    _load_barrier_weak_slow_stub() {}

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#undef __
#define __ masm->

void ZBarrierSetAssembler::load_at(MacroAssembler* masm,
                                   DecoratorSet decorators,
                                   BasicType type,
                                   Register dst,
                                   Address src,
                                   Register tmp1,
                                   Register tmp_thread) {
  if (!ZBarrierSet::barrier_needed(decorators, type)) {
    // Barrier not needed
    BarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp_thread);
    return;
  }

  // rscratch1 can be passed as src or dst, so don't use it.
  RegSet savedRegs = RegSet::of(rscratch2, rheapbase);

  Label done;
  assert_different_registers(rheapbase, rscratch2, dst);
  assert_different_registers(rheapbase, rscratch2, src.base());

  __ push(savedRegs, sp);

  // Load bad mask into scratch register.
  __ ldr(rheapbase, address_bad_mask_from_thread(rthread));
  __ lea(rscratch2, src);
  __ ldr(dst, src);

  // Test reference against bad mask. If mask bad, then we need to fix it up.
  __ tst(dst, rheapbase);
  __ br(Assembler::EQ, done);

  __ enter();

  __ push(RegSet::range(r0,r28) - RegSet::of(dst), sp);

  if (c_rarg0 != dst) {
    __ mov(c_rarg0, dst);
  }
  __ mov(c_rarg1, rscratch2);

  int step = 4 * wordSize;
  __ mov(rscratch1, -step);
  __ sub(sp, sp, step);

  for (int i = 28; i >= 4; i -= 4) {
    __ st1(as_FloatRegister(i), as_FloatRegister(i+1), as_FloatRegister(i+2),
        as_FloatRegister(i+3), __ T1D, Address(__ post(sp, rscratch1)));
  }

  __ call_VM_leaf(ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators), 2);

  for (int i = 0; i <= 28; i += 4) {
    __ ld1(as_FloatRegister(i), as_FloatRegister(i+1), as_FloatRegister(i+2),
        as_FloatRegister(i+3), __ T1D, Address(__ post(sp, step)));
  }

  // Make sure dst has the return value.
  if (dst != r0) {
    __ mov(dst, r0);
  }

  __ pop(RegSet::range(r0,r28) - RegSet::of(dst), sp);
  __ leave();

  __ bind(done);

  // Restore tmps
  __ pop(savedRegs, sp);
}

#ifdef ASSERT

void ZBarrierSetAssembler::store_at(MacroAssembler* masm,
                                        DecoratorSet decorators,
                                        BasicType type,
                                        Address dst,
                                        Register val,
                                        Register tmp1,
                                        Register tmp2) {
  // Verify value
  if (type == T_OBJECT || type == T_ARRAY) {
    // Note that src could be noreg, which means we
    // are storing null and can skip verification.
    if (val != noreg) {
      Label done;

      // tmp1 and tmp2 are often set to noreg.
      RegSet savedRegs = RegSet::of(rscratch1);
      __ push(savedRegs, sp);

      __ ldr(rscratch1, address_bad_mask_from_thread(rthread));
      __ tst(val, rscratch1);
      __ br(Assembler::EQ, done);
      __ stop("Verify oop store failed");
      __ should_not_reach_here();
      __ bind(done);
      __ pop(savedRegs, sp);
    }
  }

  // Store value
  BarrierSetAssembler::store_at(masm, decorators, type, dst, val, tmp1, tmp2);
}

#endif // ASSERT

void ZBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm,
                                              DecoratorSet decorators,
                                              bool is_oop,
                                              Register src,
                                              Register dst,
                                              Register count,
                                              RegSet saved_regs) {
  if (!is_oop) {
    // Barrier not needed
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::arraycopy_prologue {");

  assert_different_registers(src, count, rscratch1);

  __ pusha();

  if (count == c_rarg0) {
    if (src == c_rarg1) {
      // exactly backwards!!
      __ mov(rscratch1, c_rarg0);
      __ mov(c_rarg0, c_rarg1);
      __ mov(c_rarg1, rscratch1);
    } else {
      __ mov(c_rarg1, count);
      __ mov(c_rarg0, src);
    }
  } else {
    __ mov(c_rarg0, src);
    __ mov(c_rarg1, count);
  }

  __ call_VM_leaf(ZBarrierSetRuntime::load_barrier_on_oop_array_addr(), 2);

  __ popa();
  BLOCK_COMMENT("} ZBarrierSetAssembler::arraycopy_prologue");
}

void ZBarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm,
                                                         Register jni_env,
                                                         Register robj,
                                                         Register tmp,
                                                         Label& slowpath) {
  BLOCK_COMMENT("ZBarrierSetAssembler::try_resolve_jobject_in_native {");

  assert_different_registers(jni_env, robj, tmp);

  // Resolve jobject
  BarrierSetAssembler::try_resolve_jobject_in_native(masm, jni_env, robj, tmp, slowpath);

  // The Address offset is too large to direct load - -784. Our range is +127, -128.
  __ mov(tmp, (long int)(in_bytes(ZThreadLocalData::address_bad_mask_offset()) -
      in_bytes(JavaThread::jni_environment_offset())));
  // Load address bad mask
  __ add(tmp, jni_env, tmp);
  __ ldr(tmp, Address(tmp));

  // Check address bad mask
  __ tst(robj, tmp);
  __ br(Assembler::NE, slowpath);

  BLOCK_COMMENT("} ZBarrierSetAssembler::try_resolve_jobject_in_native");
}

#ifdef COMPILER1

#undef __
#define __ ce->masm()->

void ZBarrierSetAssembler::generate_c1_load_barrier_test(LIR_Assembler* ce,
                                                         LIR_Opr ref) const {
  assert_different_registers(rheapbase, rthread, ref->as_register());

  __ ldr(rheapbase, address_bad_mask_from_thread(rthread));
  __ tst(ref->as_register(), rheapbase);
}

void ZBarrierSetAssembler::generate_c1_load_barrier_stub(LIR_Assembler* ce,
                                                         ZLoadBarrierStubC1* stub) const {
  // Stub entry
  __ bind(*stub->entry());

  Register ref = stub->ref()->as_register();
  Register ref_addr = noreg;
  Register tmp = noreg;

  if (stub->tmp()->is_valid()) {
    // Load address into tmp register
    ce->leal(stub->ref_addr(), stub->tmp());
    ref_addr = tmp = stub->tmp()->as_pointer_register();
  } else {
    // Address already in register
    ref_addr = stub->ref_addr()->as_address_ptr()->base()->as_pointer_register();
  }

  assert_different_registers(ref, ref_addr, noreg);

  // Save r0 unless it is the result or tmp register
  // Set up SP to accomodate parameters and maybe r0..
  if (ref != r0 && tmp != r0) {
    __ sub(sp, sp, 32);
    __ str(r0, Address(sp, 16));
  } else {
    __ sub(sp, sp, 16);
  }

  // Setup arguments and call runtime stub
  ce->store_parameter(ref_addr, 1);
  ce->store_parameter(ref, 0);

  __ far_call(stub->runtime_stub());

  // Verify result
  __ verify_oop(r0, "Bad oop");

  // Move result into place
  if (ref != r0) {
    __ mov(ref, r0);
  }

  // Restore r0 unless it is the result or tmp register
  if (ref != r0 && tmp != r0) {
    __ ldr(r0, Address(sp, 16));
    __ add(sp, sp, 32);
  } else {
    __ add(sp, sp, 16);
  }

  // Stub exit
  __ b(*stub->continuation());
}

#undef __
#define __ sasm->

void ZBarrierSetAssembler::generate_c1_load_barrier_runtime_stub(StubAssembler* sasm,
                                                                 DecoratorSet decorators) const {
  __ prologue("zgc_load_barrier stub", false);

  // We don't use push/pop_clobbered_registers() - we need to pull out the result from r0.
  for (int i = 0; i < 32; i +=2) {
    __ stpd(as_FloatRegister(i), as_FloatRegister(i+1), Address(__ pre(sp,-16)));
  }

  RegSet saveRegs = RegSet::range(r0,r28) - RegSet::of(r0);
  __ push(saveRegs, sp);

  // Setup arguments
  __ load_parameter(0, c_rarg0);
  __ load_parameter(1, c_rarg1);

  __ call_VM_leaf(ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators), 2);

  __ pop(saveRegs, sp);

  for (int i = 30; i >0; i -=2) {
      __ ldpd(as_FloatRegister(i), as_FloatRegister(i+1), Address(__ post(sp, 16)));
    }

  __ epilogue();
}
#endif // COMPILER1

#undef __
#define __ cgen->assembler()->

// Generates a register specific stub for calling
// ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded() or
// ZBarrierSetRuntime::load_barrier_on_weak_oop_field_preloaded().
//
// The raddr register serves as both input and output for this stub. When the stub is
// called the raddr register contains the object field address (oop*) where the bad oop
// was loaded from, which caused the slow path to be taken. On return from the stub the
// raddr register contains the good/healed oop returned from
// ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded() or
// ZBarrierSetRuntime::load_barrier_on_weak_oop_field_preloaded().
static address generate_load_barrier_stub(StubCodeGenerator* cgen, Register raddr, DecoratorSet decorators) {
  // Don't generate stub for invalid registers
  if (raddr == zr || raddr == r29 || raddr == r30) {
    return NULL;
  }

  // Create stub name
  char name[64];
  const bool weak = (decorators & ON_WEAK_OOP_REF) != 0;
  os::snprintf(name, sizeof(name), "zgc_load_barrier%s_stub_%s", weak ? "_weak" : "", raddr->name());

  __ align(CodeEntryAlignment);
  StubCodeMark mark(cgen, "StubRoutines", os::strdup(name, mtCode));
  address start = __ pc();

  // Save live registers
  RegSet savedRegs = RegSet::range(r0,r18) - RegSet::of(raddr);

  __ enter();
  __ push(savedRegs, sp);

  // Setup arguments
  if (raddr != c_rarg1) {
    __ mov(c_rarg1, raddr);
  }

  __ ldr(c_rarg0, Address(raddr));

  // Call barrier function
  __ call_VM_leaf(ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators), c_rarg0, c_rarg1);

  // Move result returned in r0 to raddr, if needed
  if (raddr != r0) {
    __ mov(raddr, r0);
  }

  __ pop(savedRegs, sp);
  __ leave();
  __ ret(lr);

  return start;
}

#undef __

static void barrier_stubs_init_inner(const char* label, const DecoratorSet decorators, address* stub) {
  const int nregs = 28;              // Exclude FP, XZR, SP from calculation.
  const int code_size = nregs * 254; // Rough estimate of code size

  ResourceMark rm;

  CodeBuffer buf(BufferBlob::create(label, code_size));
  StubCodeGenerator cgen(&buf);

  for (int i = 0; i < nregs; i++) {
    const Register reg = as_Register(i);
    stub[i] = generate_load_barrier_stub(&cgen, reg, decorators);
  }
}

void ZBarrierSetAssembler::barrier_stubs_init() {
  barrier_stubs_init_inner("zgc_load_barrier_stubs", ON_STRONG_OOP_REF, _load_barrier_slow_stub);
  barrier_stubs_init_inner("zgc_load_barrier_weak_stubs", ON_WEAK_OOP_REF, _load_barrier_weak_slow_stub);
}

address ZBarrierSetAssembler::load_barrier_slow_stub(Register reg) {
  return _load_barrier_slow_stub[reg->encoding()];
}

address ZBarrierSetAssembler::load_barrier_weak_slow_stub(Register reg) {
  return _load_barrier_weak_slow_stub[reg->encoding()];
}
