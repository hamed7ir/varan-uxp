/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Platform specific code to invoke XPCOM methods on native objects
 *
 * Varan: Windows ARM32 (Thumb-2, clang-cl thumbv7, hardfp/AAPCS-VFP).
 *
 * PROVENANCE (explicit per single-artifact discipline):
 *   - ARG MARSHALLING + inline asm: taken essentially VERBATIM from
 *     md/unix/xptcinvoke_arm.cpp, the __ARM_PCS_VFP (hardfp) path. That IS the
 *     correct AAPCS-VFP model for Windows-ARM: first args in r0-r3 (r0='this'),
 *     floats/doubles in VFP s0-s15/d0-d7, 64-bit ints in even/odd GPR pairs with
 *     the "hole" skipped, 8-byte stack alignment at the call, remainder spilled to
 *     stack. The x86 md/win32/xptcinvoke.cpp is NOT used here -- it is the wrong ABI
 *     (all args on the stack) and only ever served as the MSVC name-mangling model
 *     for the stubs side.
 *   - No MSVC name-mangling needed on this side: NS_InvokeByIndex is a plain
 *     EXPORT_XPCOM_API C-linkage symbol.
 *
 * Differences from the unix original: dropped the Linux/__arm__ #error guard and the
 * softfp (#ifndef __ARM_PCS_VFP) half; trimmed the VFP clobber list to d0-d7 (the
 * Tegra-3/Cortex-A9 is VFPv3-d16 -- d16-d31 do not exist, and clang-cl rejects them).
 */

#include "xptcprivate.h"

/* "Procedure Call Standard for the ARM Architecture" document, sections
 * "5.5 Parameter Passing" and "6.1.2 Procedure Calling", cover all of this.
 * http://infocenter.arm.com/help/topic/com.arm.doc.ihi0042d/IHI0042D_aapcs.pdf
 */

#if !defined(_M_ARM) && !defined(__arm__)
#error "This file is for Windows ARM32 (thumbv7) only."
#endif

/*
 * Allocation of integer function arguments initially to registers r1-r3 and then
 * to stack. The 'this' argument (r0) is handled separately in the asm below.
 * Doubleword arguments go to even:odd register pairs or get 8-byte aligned on the
 * stack; any "holes" from realignment remain unused.
 */
static inline void copy_word(uint32_t* &ireg_args,
                             uint32_t* &stack_args,
                             uint32_t* end,
                             uint32_t  data)
{
  if (ireg_args < end) {
    *ireg_args = data;
    ireg_args++;
  } else {
    *stack_args = data;
    stack_args++;
  }
}

static inline void copy_dword(uint32_t* &ireg_args,
                              uint32_t* &stack_args,
                              uint32_t* end,
                              uint64_t  data)
{
  if (ireg_args + 1 < end) {
    if ((uint32_t)ireg_args & 4) {
      ireg_args++;
    }
    *(uint64_t *)ireg_args = data;
    ireg_args += 2;
  } else {
    ireg_args = end;
    if ((uint32_t)stack_args & 4) {
      stack_args++;
    }
    *(uint64_t *)stack_args = data;
    stack_args += 2;
  }
}

/*
 * Allocation of floating point arguments to VFP registers (s0-s15, d0-d7), with
 * back-filling support (e.g. the 3rd arg of f(float s0, double d1, float s1) goes
 * to s1, back-filling the hole). See the AAPCS document for the full rules.
 */
static inline bool copy_vfp_single(float* &vfp_s_args, double* &vfp_d_args,
                                   float* end, float data)
{
  if (vfp_s_args >= end)
    return false;

  *vfp_s_args = data;
  vfp_s_args++;
  if (vfp_s_args < (float *)vfp_d_args) {
    // back-filling: the next free single now overlaps the next free double
    vfp_s_args = (float *)vfp_d_args;
  } else if (vfp_s_args > (float *)vfp_d_args) {
    vfp_d_args++;
  }
  return true;
}

static inline bool copy_vfp_double(float* &vfp_s_args, double* &vfp_d_args,
                                   float* end, double data)
{
  if (vfp_d_args >= (double *)end) {
    // Back-filling stops once any VFP CPRC has been allocated to the stack.
    vfp_s_args = end;
    return false;
  }

  if (vfp_s_args == (float *)vfp_d_args) {
    vfp_s_args += 2;
  }
  *vfp_d_args = data;
  vfp_d_args++;
  return true;
}

static void
invoke_copy_to_stack(uint32_t* stk, uint32_t *end,
                     uint32_t paramCount, nsXPTCVariant* s)
{
  uint32_t *ireg_args  = end - 3;
  float    *vfp_s_args = (float *)end;
  double   *vfp_d_args = (double *)end;
  float    *vfp_end    = vfp_s_args + 16;

  for (uint32_t i = 0; i < paramCount; i++, s++) {
    if (s->IsPtrData()) {
      copy_word(ireg_args, stk, end, (uint32_t)s->ptr);
      continue;
    }
    // Integral types smaller than a word are sign/zero-extended to 4 bytes.
    switch (s->type)
    {
      case nsXPTType::T_FLOAT:
        if (!copy_vfp_single(vfp_s_args, vfp_d_args, vfp_end, s->val.f)) {
          copy_word(end, stk, end, reinterpret_cast<uint32_t&>(s->val.f));
        }
        break;
      case nsXPTType::T_DOUBLE:
        if (!copy_vfp_double(vfp_s_args, vfp_d_args, vfp_end, s->val.d)) {
          copy_dword(end, stk, end, reinterpret_cast<uint64_t&>(s->val.d));
        }
        break;
      case nsXPTType::T_I8:  copy_word(ireg_args, stk, end, s->val.i8);   break;
      case nsXPTType::T_I16: copy_word(ireg_args, stk, end, s->val.i16);  break;
      case nsXPTType::T_I32: copy_word(ireg_args, stk, end, s->val.i32);  break;
      case nsXPTType::T_I64: copy_dword(ireg_args, stk, end, s->val.i64); break;
      case nsXPTType::T_U8:  copy_word(ireg_args, stk, end, s->val.u8);   break;
      case nsXPTType::T_U16: copy_word(ireg_args, stk, end, s->val.u16);  break;
      case nsXPTType::T_U32: copy_word(ireg_args, stk, end, s->val.u32);  break;
      case nsXPTType::T_U64: copy_dword(ireg_args, stk, end, s->val.u64); break;
      case nsXPTType::T_BOOL: copy_word(ireg_args, stk, end, s->val.b);   break;
      case nsXPTType::T_CHAR: copy_word(ireg_args, stk, end, s->val.c);   break;
      case nsXPTType::T_WCHAR: copy_word(ireg_args, stk, end, s->val.wc); break;
      default:
        // all the others are plain pointer types
        copy_word(ireg_args, stk, end, reinterpret_cast<uint32_t>(s->val.p));
        break;
    }
  }
}

typedef uint32_t (*vtable_func)(nsISupports *, uint32_t, uint32_t, uint32_t);

EXPORT_XPCOM_API(nsresult)
NS_InvokeByIndex(nsISupports* that, uint32_t methodIndex,
                   uint32_t paramCount, nsXPTCVariant* params)
{
  // MSVC-ABI vtable: for a single-inheritance object (all XPCOM interfaces are),
  // the vtable pointer is at offset 0 and vtable[methodIndex] is the method --
  // identical to the Itanium layout the unix original assumed.
  vtable_func *vtable = *reinterpret_cast<vtable_func **>(that);
  vtable_func func = vtable[methodIndex];
  nsresult result;
  asm (
    "mov    r3, sp\n"
    "mov    %[stack_space_size], %[param_count_plus_2], lsl #3\n"
    "tst    r3, #4\n" /* check stack alignment */

    "add    %[stack_space_size], #(4 * 16)\n" /* space for VFP registers */
    "mov    r3, %[params]\n"

    "it     ne\n"
    "addne  %[stack_space_size], %[stack_space_size], #4\n"
    "sub    r0, sp, %[stack_space_size]\n" /* allocate space on stack */

    "sub    r2, %[param_count_plus_2], #2\n"
    "mov    sp, r0\n"

    "add    r1, r0, %[param_count_plus_2], lsl #3\n"
    "blx    %[invoke_copy_to_stack]\n"

    "add    ip, sp, %[param_count_plus_2], lsl #3\n"
    "mov    r0, %[that]\n"
    "ldmdb  ip, {r1, r2, r3}\n"
    "vldm   ip, {d0, d1, d2, d3, d4, d5, d6, d7}\n"
    "blx    %[func]\n"

    "add    sp, sp, %[stack_space_size]\n" /* cleanup stack */
    "mov    %[stack_space_size], r0\n" /* it's actually 'result' variable */
    : [stack_space_size]     "=&r" (result)
    : [func]                 "r"   (func),
      [that]                 "r"   (that),
      [params]               "r"   (params),
      [param_count_plus_2]   "r"   (paramCount + 2),
      [invoke_copy_to_stack] "r"   (invoke_copy_to_stack)
    : "cc", "memory",
      "r0", "r1", "r2", "r3", "ip", "lr",
      "d0",  "d1",  "d2",  "d3",  "d4",  "d5",  "d6",  "d7"
  );
  return result;
}
