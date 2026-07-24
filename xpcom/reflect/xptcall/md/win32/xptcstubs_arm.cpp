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
 * !!! KNOWN LIMITATION (float/double stub params) !!!
 * SharedStub saves only the integer argument registers r1-r3, NOT the VFP registers
 * d0-d7. Under hardfp, float/double method parameters of a *stub* (i.e. a call FROM
 * native code INTO a script-implemented XPCOM object) arrive in VFP and would be read
 * from the wrong (integer) slot. Integer / pointer / 64-bit params ARE handled. This
 * matches the behaviour md/unix/xptcstubs_arm.cpp ships on Linux-ARM. This MUST be
 * validated on-device with an int/double/int64 dispatch test before it is trusted; if a
 * script interface with a by-value float/double param must work, SharedStub has to also
 * `vstmdb sp!, {d0-d7}` and PrepareAndDispatch route those params from the saved VFP area.
 */

#include "xptcprivate.h"
#include "xptiprivate.h"

#if !defined(_M_ARM) && !defined(__arm__)
#error "This file is for Windows ARM32 (thumbv7) only."
#endif

/* AAPCS: 64-bit values are 8-byte aligned on the stack. */
#define DOUBLEWORD_ALIGN(p) ((uint32_t *)((((uint32_t)(p)) + 7) & 0xfffffff8))

extern "C" nsresult
PrepareAndDispatch(nsXPTCStubBase* self, uint32_t methodIndex, uint32_t* args)
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
    for(i = 0; i < paramCount; i++, ap++)
    {
        const nsXPTParamInfo& param = info->GetParam(i);
        const nsXPTType& type = param.GetType();
        nsXPTCMiniVariant* dp = &dispatchParams[i];

        if(param.IsOut() || !type.IsArithmetic())
        {
            dp->val.p = (void*) *ap;
            continue;
        }
        // else
        switch(type)
        {
        case nsXPTType::T_I8     : dp->val.i8  = *((int8_t*)  ap);       break;
        case nsXPTType::T_I16    : dp->val.i16 = *((int16_t*) ap);       break;
        case nsXPTType::T_I32    : dp->val.i32 = *((int32_t*) ap);       break;
        case nsXPTType::T_I64    : ap = DOUBLEWORD_ALIGN(ap);
                                   dp->val.i64 = *((int64_t*) ap); ap++; break;
        case nsXPTType::T_U8     : dp->val.u8  = *((uint8_t*) ap);       break;
        case nsXPTType::T_U16    : dp->val.u16 = *((uint16_t*)ap);       break;
        case nsXPTType::T_U32    : dp->val.u32 = *((uint32_t*)ap);       break;
        case nsXPTType::T_U64    : ap = DOUBLEWORD_ALIGN(ap);
                                   dp->val.u64 = *((uint64_t*)ap); ap++; break;
        case nsXPTType::T_FLOAT  : dp->val.f   = *((float*)   ap);       break;
        case nsXPTType::T_DOUBLE : ap = DOUBLEWORD_ALIGN(ap);
                                   dp->val.d   = *((double*)  ap); ap++; break;
        case nsXPTType::T_BOOL   : dp->val.b   = *((bool*)    ap);       break;
        case nsXPTType::T_CHAR   : dp->val.c   = *((char*)    ap);       break;
        case nsXPTType::T_WCHAR  : dp->val.wc  = *((wchar_t*) ap);       break;
        default:
            NS_ERROR("bad type");
            break;
        }
    }

    result = self->mOuter->CallMethod((uint16_t)methodIndex, info, dispatchParams);

    if(dispatchParams != paramBuffer)
        delete [] dispatchParams;

    return result;
}

/*
 * SharedStub (FROM md/unix/xptcstubs_arm.cpp, Thumb-2). On entry:
 *   r0 = self ; r1,r2,r3 = first 3 integer method args ; ip = stub (method) number ;
 *   further args on the caller stack immediately above r1..r3 once we push them.
 * We push {r1,r2,r3} so that [sp] is a contiguous args array (r1,r2,r3, then the
 * caller's stack args), take its address into r2, save lr, move the method number
 * (ip) into r1, and call PrepareAndDispatch(r0=self, r1=methodIndex, r2=args). On
 * return we reload pc from the saved lr and pop the 16 bytes (lr + r1,r2,r3).
 */
__asm__ ("\n"
         ".syntax unified\n"
         ".thumb\n"
         ".text\n"
         ".align 2\n"
         ".thumb_func\n"
         "SharedStub:\n"
         "stmfd  sp!, {r1, r2, r3}\n"
         "mov    r2, sp\n"
         "str    lr, [sp, #-4]!\n"
         "mov    r1, ip\n"
         "bl     PrepareAndDispatch\n"
         "ldr    pc, [sp], #16\n");

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
