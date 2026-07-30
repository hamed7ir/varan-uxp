/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implement shared vtbl methods.
 *
 * Varan: Windows ARM32 (Thumb-2, clang-cl thumbv7, hardfp/AAPCS-VFP).
 *
 * PROVENANCE (explicit per single-artifact discipline -- this file MERGES two models):
 *   - PrepareAndDispatch + SharedStub arg model + the Thumb-2 asm: FROM
 *     md/unix/xptcstubs_arm.cpp. AAPCS is CALLER-cleanup, so PrepareAndDispatch takes
 *     (self, methodIndex, args) with NO stackBytesToPop (that is the x86 __stdcall
 *     callee-cleanup param in md/win32/xptcstubs.cpp -- deliberately dropped).
 *   - MSVC C++ NAME MANGLING for the StubN entry symbols: FROM the __clang__ branch of
 *     md/win32/xptcstubs.cpp. clang-cl uses MSVC mangling, not the GNU mangling of the
 *     unix file. The x86 form is `?Stub<n>@nsXPTCStubBase@@UAG?AW4nsresult@@XZ`; on ARM
 *     the calling-convention letter G (__stdcall) becomes A (there is one calling
 *     convention on ARM) -> `...@@UAA?AW4nsresult@@XZ`.
 *   - The x86 asm bodies (push %ebp ... / mov $n,%ecx) are NOT copied -- that is the
 *     wrong ABI. Only the mangling scaffolding is reused.
 *
 * THUMB-2 (R4): every emitted symbol is marked `.thumb_func` so the linker sets bit0=1;
 * the vtable is called via `blx`, which needs bit0=1 to enter Thumb state. Verify by
 * disassembly (llvm-objdump --triple=thumbv7...) that these are Thumb-2 encodings, never
 * 4-byte A32 words, BEFORE first link.
 *
 * FLOAT/DOUBLE STUB PARAMS -- FIXED 2026-07-30 (was a KNOWN LIMITATION, held since M4.1b).
 *
 * HOW IT SURFACED: a chrome-JS probe implementing nsIReflowObserver, whose method is
 * `reflow(in DOMHighResTimeStamp start, in DOMHighResTimeStamp end)` -- i.e. a
 * script-implemented XPCOM interface with by-value doubles, exactly the case the old
 * comment said did not work. On device it returned 2.67e+145 ms for every reflow: the
 * high half of each double was garbage from the integer register file while the low half
 * varied with the real value.
 *
 * THE DEFECT WAS WIDER THAN THE OLD COMMENT ADMITTED. Under hardfp a float/double
 * argument is a VFP CPRC: it consumes an s/d register and **ZERO integer slots**. The old
 * loop both (a) read the value from the integer stream, and (b) ADVANCED `ap` past it. So
 * it was not only the float that came out wrong -- every integer or pointer parameter
 * AFTER a float/double was shifted and corrupted too. `foo(double, nsISupports*)` handed
 * the callee a garbage pointer.
 *
 * THE FIX, in two halves:
 *   1. SharedStub now `vpush {d0-d7}` and passes the saved area as a 4th argument.
 *      d0-d7 are argument/scratch registers in AAPCS (d8-d15 are the callee-saved ones),
 *      so they need saving on entry and no restoring on exit.
 *   2. PrepareAndDispatch walks a SEPARATE VFP cursor over that area, with the same
 *      back-filling rules as the allocator, and no longer advances `ap` for a
 *      VFP-passed parameter. `ap` advancement is now explicit per case rather than
 *      implicit in the loop header, because "one word unless stated" is precisely the
 *      assumption that was wrong.
 *
 * The read_vfp_single/read_vfp_double helpers below are the EXACT INVERSE of
 * copy_vfp_single/copy_vfp_double in xptcinvoke_arm.cpp -- same control flow, same
 * back-filling, reading where those write. That symmetry is the correctness argument:
 * the invoke side is device-proven, and an allocator and a de-allocator that disagree
 * would show up as a shifted argument on the very first call.
 */

#include "xptcprivate.h"
#include "xptiprivate.h"

#if !defined(_M_ARM) && !defined(__arm__)
#error "This file is for Windows ARM32 (thumbv7) only."
#endif

/* AAPCS: 64-bit values are 8-byte aligned on the stack. */
#define DOUBLEWORD_ALIGN(p) ((uint32_t *)((((uint32_t)(p)) + 7) & 0xfffffff8))

/*
 * Reading floating-point arguments back out of the saved VFP area (s0-s15 / d0-d7), with
 * the same back-filling the caller used to put them there -- e.g. the 3rd argument of
 * f(float, double, float) was placed in s1, back-filling the hole left when the double
 * took d1. These are the exact inverse of copy_vfp_single/copy_vfp_double in
 * xptcinvoke_arm.cpp; keep them in step if either side ever changes.
 */
static inline bool read_vfp_single(float* &vfp_s, double* &vfp_d, float* end, float& out)
{
  if (vfp_s >= end)
    return false;                       // VFP exhausted -> this one came on the stack

  out = *vfp_s;
  vfp_s++;
  if (vfp_s < (float *)vfp_d) {
    vfp_s = (float *)vfp_d;
  } else if (vfp_s > (float *)vfp_d) {
    vfp_d++;
  }
  return true;
}

static inline bool read_vfp_double(float* &vfp_s, double* &vfp_d, float* end, double& out)
{
  if (vfp_d >= (double *)end) {
    // Back-filling stops once any VFP CPRC has been allocated to the stack.
    vfp_s = end;
    return false;
  }

  if (vfp_s == (float *)vfp_d) {
    vfp_s += 2;
  }
  out = *vfp_d;
  vfp_d++;
  return true;
}

extern "C" nsresult
PrepareAndDispatch(nsXPTCStubBase* self, uint32_t methodIndex, uint32_t* args,
                   uint32_t* vfpArgs)
{
#define PARAM_BUFFER_COUNT     16

    nsXPTCMiniVariant paramBuffer[PARAM_BUFFER_COUNT];
    nsXPTCMiniVariant* dispatchParams = nullptr;
    const nsXPTMethodInfo* info;
    uint8_t paramCount;
    uint8_t i;
    nsresult result = NS_ERROR_FAILURE;

    NS_ASSERTION(self,"no self");

    self->mEntry->GetMethodInfo(uint16_t(methodIndex), &info);
    paramCount = info->GetParamCount();

    // setup variant array pointer
    if(paramCount > PARAM_BUFFER_COUNT)
        dispatchParams = new nsXPTCMiniVariant[paramCount];
    else
        dispatchParams = paramBuffer;
    NS_ASSERTION(dispatchParams,"no place for params");

    uint32_t* ap = args;

    // The VFP argument area saved by SharedStub: 8 doubles == 16 floats, in register
    // order, so s0/s1 alias the low/high halves of d0 and a plain float* walk is correct
    // on little-endian. Both cursors start at the base and advance independently of `ap`,
    // exactly as the allocator in xptcinvoke_arm.cpp does.
    float*  vfp_s   = (float *)vfpArgs;
    double* vfp_d   = (double *)vfpArgs;
    float*  vfp_end = (float *)vfpArgs + 16;

    // NB `ap` is advanced EXPLICITLY in every arm below -- it is deliberately NOT
    // incremented by the loop header any more. A VFP-passed float or double consumes no
    // integer slot at all, and the old implicit "one word per parameter" was what shifted
    // every subsequent integer argument.
    for(i = 0; i < paramCount; i++)
    {
        const nsXPTParamInfo& param = info->GetParam(i);
        const nsXPTType& type = param.GetType();
        nsXPTCMiniVariant* dp = &dispatchParams[i];

        if(param.IsOut() || !type.IsArithmetic())
        {
            dp->val.p = (void*) *ap; ap++;
            continue;
        }
        // else
        switch(type)
        {
        case nsXPTType::T_I8     : dp->val.i8  = *((int8_t*)  ap); ap++;   break;
        case nsXPTType::T_I16    : dp->val.i16 = *((int16_t*) ap); ap++;   break;
        case nsXPTType::T_I32    : dp->val.i32 = *((int32_t*) ap); ap++;   break;
        case nsXPTType::T_I64    : ap = DOUBLEWORD_ALIGN(ap);
                                   dp->val.i64 = *((int64_t*) ap); ap += 2; break;
        case nsXPTType::T_U8     : dp->val.u8  = *((uint8_t*) ap); ap++;   break;
        case nsXPTType::T_U16    : dp->val.u16 = *((uint16_t*)ap); ap++;   break;
        case nsXPTType::T_U32    : dp->val.u32 = *((uint32_t*)ap); ap++;   break;
        case nsXPTType::T_U64    : ap = DOUBLEWORD_ALIGN(ap);
                                   dp->val.u64 = *((uint64_t*)ap); ap += 2; break;
        case nsXPTType::T_FLOAT  :
            // VFP first; only once s0-s15 are exhausted does it arrive on the stack.
            if(!read_vfp_single(vfp_s, vfp_d, vfp_end, dp->val.f)) {
                dp->val.f = *((float*) ap); ap++;
            }
            break;
        case nsXPTType::T_DOUBLE :
            if(!read_vfp_double(vfp_s, vfp_d, vfp_end, dp->val.d)) {
                ap = DOUBLEWORD_ALIGN(ap);
                dp->val.d = *((double*) ap); ap += 2;
            }
            break;
        case nsXPTType::T_BOOL   : dp->val.b   = *((bool*)    ap); ap++;   break;
        case nsXPTType::T_CHAR   : dp->val.c   = *((char*)    ap); ap++;   break;
        case nsXPTType::T_WCHAR  : dp->val.wc  = *((wchar_t*) ap); ap++;   break;
        default:
            NS_ERROR("bad type");
            ap++;
            break;
        }
    }

    result = self->mOuter->CallMethod((uint16_t)methodIndex, info, dispatchParams);

    if(dispatchParams != paramBuffer)
        delete [] dispatchParams;

    return result;
}

/*
 * SharedStub (FROM md/unix/xptcstubs_arm.cpp, Thumb-2; VFP save added by Varan). On entry:
 *   r0 = self ; r1,r2,r3 = first 3 integer method args ; d0-d7 = the float/double args ;
 *   ip = stub (method) number ; further args on the caller stack.
 *
 * Calls PrepareAndDispatch(r0 = self, r1 = methodIndex, r2 = args, r3 = vfpArgs).
 *
 * ORDER IS LOAD-BEARING, and this is the one thing to get right if it is ever touched:
 * `stmfd {r1,r2,r3}` MUST come first, while sp still points at the caller's stack args,
 * so that [sp] .. [sp+8] followed immediately by the caller's stack args form ONE
 * contiguous integer-argument array. Saving the VFP block first would push the integer
 * registers 64 bytes away from the stack args and silently break every method with more
 * than three integer parameters.
 *
 * d0-d7 are argument/scratch registers under AAPCS (d8-d15 are the callee-saved ones), so
 * they are saved on entry and deliberately NOT restored on exit.
 *
 * Stack alignment at the call: 12 (r1-r3) + 64 (d0-d7) + 4 (lr) = 80, so sp is still
 * 8-byte aligned at the `bl`, as AAPCS requires. The old 12 + 4 = 16 had the same
 * property; keep any future change a multiple of 8 in total.
 */
__asm__ ("\n"
         ".syntax unified\n"
         ".thumb\n"
         ".text\n"
         ".align 2\n"
         ".thumb_func\n"
         "SharedStub:\n"
         "stmfd  sp!, {r1, r2, r3}\n"   /* first: keeps the int args contiguous */
         "mov    r2, sp\n"              /* r2 = args                            */
         "vpush  {d0-d7}\n"             /* 64 B of VFP args (== vstmdb sp!)     */
         "mov    r3, sp\n"              /* r3 = vfpArgs                         */
         "str    lr, [sp, #-4]!\n"
         "mov    r1, ip\n"              /* r1 = methodIndex                     */
         "bl     PrepareAndDispatch\n"
         "ldr    lr, [sp], #4\n"        /* restore lr                           */
         "add    sp, sp, #76\n"         /* 64 (d0-d7) + 12 (r1,r2,r3)           */
         "bx     lr\n");

/*
 * Create the sets of stubs that call SharedStub. Each is a virtual method
 * nsXPTCStubBase::Stub<n>() -- emitted with its MSVC-mangled name (clang-cl mangling,
 * UAA on ARM) so it slots into the C++ vtable. We only touch ip (defined corruptible)
 * and tail-branch to SharedStub.
 */
#define STUB_ENTRY(n) \
__asm__(".syntax unified\n\t" \
        ".thumb\n\t" \
        ".text\n\t" \
        ".align 2\n\t" \
        ".globl \"?Stub" #n "@nsXPTCStubBase@@UAA?AW4nsresult@@XZ\"\n\t" \
        ".def   \"?Stub" #n "@nsXPTCStubBase@@UAA?AW4nsresult@@XZ\";\n\t" \
        ".scl   2\n\t" \
        ".type  46\n\t" \
        ".endef\n\t" \
        ".thumb_func\n\t" \
        "\"?Stub" #n "@nsXPTCStubBase@@UAA?AW4nsresult@@XZ\":\n\t" \
        "mov    ip, #" #n "\n\t" \
        "b      SharedStub\n\t");

#define SENTINEL_ENTRY(n) \
nsresult nsXPTCStubBase::Sentinel##n() \
{ \
    NS_ERROR("nsXPTCStubBase::Sentinel called"); \
    return NS_ERROR_NOT_IMPLEMENTED; \
}

#include "xptcstubsdef.inc"
