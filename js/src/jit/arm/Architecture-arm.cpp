/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm/Architecture-arm.h"

#if !defined(JS_SIMULATOR_ARM) && !defined(__APPLE__) && !defined(XP_WIN)
// Varan M1: elf.h is Unix-only (Linux auxv path); Windows uses the hardcoded
// CPU-feature profile in InitARMFlags() instead.
#include <elf.h>
#endif

#if !defined(XP_WIN)
// Varan M1: Unix-only headers for the Linux /proc feature-detection path
// (open/read); Windows uses the hardcoded profile in InitARMFlags().
#include <fcntl.h>
#include <unistd.h>
#endif

#include "jit/arm/Assembler-arm.h"
#include "jit/RegisterSets.h"

// Varan (H3, 2026-08-09): BUG-3 FAMILY SIZE LOCK for VFPRegister.
//
// VFPRegister packs {RegType kind:2, uint32_t code_:5, uint32_t _isInvalid:1,
// uint32_t _isMissing:1} and MUST stay one 32-bit word. Before the Varan fix the
// two flags were `bool`, which under the MS-ABI rule (a change of bitfield
// underlying type starts a NEW storage unit) made it 8 bytes -- the same shape
// that broke PoolHeader. PoolHeader's fix has been guarded by a sizeof assert
// since M1; VFPRegister's was not, and `sizeof(VFPRegister)` appeared in ZERO
// assertions tree-wide. That asymmetry is why PoolHeader's regression would have
// been loud and this one silent.
//
// MEASURED 2026-08-09 (not inferred): `-Xclang -fdump-record-layouts` on this TU
// with the real build flags reports `js::jit::VFPRegister sizeof=4 align=4`.
static_assert(sizeof(js::jit::VFPRegister) == 4,
              "Varan/Bug-3: VFPRegister must stay a single 32-bit word; a bool "
              "bitfield here starts a new MS-ABI storage unit and doubles it.");

#if !defined(__linux__) || defined(JS_SIMULATOR_ARM)
// The hwcap.h kernel header is not defined when building the simulator,
// so inline the header defines we need.
# define HWCAP_VFP        (1 << 6)
# define HWCAP_NEON       (1 << 12)
# define HWCAP_VFPv3      (1 << 13)
# define HWCAP_VFPv3D16   (1 << 14) /* also set for VFPv4-D16 */
# define HWCAP_VFPv4      (1 << 16)
# define HWCAP_IDIVA      (1 << 17)
# define HWCAP_IDIVT      (1 << 18)
# define HWCAP_VFPD32     (1 << 19) /* set if VFP has 32 regs (not 16) */
# define AT_HWCAP 16
#else
# include <asm/hwcap.h>
# if !defined(HWCAP_IDIVA)
#  define HWCAP_IDIVA     (1 << 17)
# endif
# if !defined(HWCAP_VFPD32)
#  define HWCAP_VFPD32    (1 << 19) /* set if VFP has 32 regs (not 16) */
# endif
#endif

namespace js {
namespace jit {

// Parse the Linux kernel cpuinfo features. This is also used to parse the
// override features which has some extensions: 'armv7', 'align' and 'hardfp'.
static uint32_t
ParseARMCpuFeatures(const char* features, bool override = false)
{
    uint32_t flags = 0;

    // For ease of running tests we want it to be the default to fixup faults.
    bool fixupAlignmentFault = true;

    for (;;) {
        char ch = *features;
        if (!ch) {
            // End of string.
            break;
        }
        if (ch == ' ' || ch == ',') {
            // Skip separator characters.
            features++;
            continue;
        }
        // Find the end of the token.
        const char* end = features + 1;
        for (; ; end++) {
            ch = *end;
            if (!ch || ch == ' ' || ch == ',')
                break;
        }
        size_t count = end - features;
        if (count == 3 && strncmp(features, "vfp", 3) == 0)
            flags |= HWCAP_VFP;
        else if (count == 4 && strncmp(features, "neon", 4) == 0)
            flags |= HWCAP_NEON;
        else if (count == 5 && strncmp(features, "vfpv3", 5) == 0)
            flags |= HWCAP_VFPv3;
        else if (count == 8 && strncmp(features, "vfpv3d16", 8) == 0)
            flags |= HWCAP_VFPv3D16;
        else if (count == 5 && strncmp(features, "vfpv4", 5) == 0)
            flags |= HWCAP_VFPv4;
        else if (count == 5 && strncmp(features, "idiva", 5) == 0)
            flags |= HWCAP_IDIVA;
        else if (count == 5 && strncmp(features, "idivt", 5) == 0)
            flags |= HWCAP_IDIVT;
        else if (count == 6 && strncmp(features, "vfpd32", 6) == 0)
            flags |= HWCAP_VFPD32;
        else if (count == 5 && strncmp(features, "armv7", 5) == 0)
            flags |= HWCAP_ARMv7;
        else if (count == 5 && strncmp(features, "align", 5) == 0)
            flags |= HWCAP_ALIGNMENT_FAULT | HWCAP_FIXUP_FAULT;
#if defined(JS_SIMULATOR_ARM)
        else if (count == 7 && strncmp(features, "nofixup", 7) == 0)
            fixupAlignmentFault = false;
        else if (count == 6 && strncmp(features, "hardfp", 6) == 0)
            flags |= HWCAP_USE_HARDFP_ABI;
#endif
        else if (override)
            fprintf(stderr, "Warning: unexpected ARM feature at: %s\n", features);
        features = end;
    }

    if (!fixupAlignmentFault)
        flags &= ~HWCAP_FIXUP_FAULT;

    return flags;
}

static uint32_t
CanonicalizeARMHwCapFlags(uint32_t flags)
{
    // Canonicalize the flags. These rules are also applied to the features
    // supplied for simulation.

    // The VFPv3 feature is expected when the VFPv3D16 is reported, but add it
    // just in case of a kernel difference in feature reporting.
    if (flags & HWCAP_VFPv3D16)
        flags |= HWCAP_VFPv3;

    // If VFPv3 or Neon is supported then this must be an ARMv7.
    if (flags & (HWCAP_VFPv3 | HWCAP_NEON))
        flags |= HWCAP_ARMv7;

    // Some old kernels report VFP and not VFPv3, but if ARMv7 then it must be
    // VFPv3.
    if (flags & HWCAP_VFP && flags & HWCAP_ARMv7)
        flags |= HWCAP_VFPv3;

    // Older kernels do not implement the HWCAP_VFPD32 flag.
    if ((flags & HWCAP_VFPv3) && !(flags & HWCAP_VFPv3D16))
        flags |= HWCAP_VFPD32;

    return flags;
}

volatile bool forceDoubleCacheFlush = false;

bool
ForceDoubleCacheFlush() {
    return forceDoubleCacheFlush;
}

// The override flags parsed from the ARMHWCAP environment variable or from the
// --arm-hwcap js shell argument.
volatile uint32_t armHwCapFlags = HWCAP_UNINITIALIZED;

bool
ParseARMHwCapFlags(const char* armHwCap)
{
    uint32_t flags = 0;

    if (!armHwCap)
        return false;

    if (strstr(armHwCap, "help")) {
        fflush(NULL);
        printf(
               "\n"
               "usage: ARMHWCAP=option,option,option,... where options can be:\n"
               "\n"
               "  vfp      \n"
               "  neon     \n"
               "  vfpv3    \n"
               "  vfpv3d16 \n"
               "  vfpv4    \n"
               "  idiva    \n"
               "  idivt    \n"
               "  vfpd32   \n"
               "  armv7    \n"
               "  align    - unaligned accesses will trap and be emulated\n"
#ifdef JS_SIMULATOR_ARM
               "  nofixup  - disable emulation of unaligned accesses\n"
               "  hardfp   \n"
#endif
               "\n"
               );
        exit(0);
        /*NOTREACHED*/
    }

    flags = ParseARMCpuFeatures(armHwCap, /* override = */ true);

#ifdef JS_CODEGEN_ARM_HARDFP
    flags |= HWCAP_USE_HARDFP_ABI;
#endif

    armHwCapFlags = CanonicalizeARMHwCapFlags(flags);
    JitSpew(JitSpew_Codegen, "ARM HWCAP: 0x%x\n", armHwCapFlags);
    return true;
}

void
InitARMFlags()
{
    uint32_t flags = 0;

    if (armHwCapFlags != HWCAP_UNINITIALIZED)
        return;

    const char* env = getenv("ARMHWCAP");
    if (ParseARMHwCapFlags(env))
        return;

#ifdef JS_SIMULATOR_ARM
    // HWCAP_FIXUP_FAULT is on by default even if HWCAP_ALIGNMENT_FAULT is
    // not on by default, because some memory access instructions always fault.
    // Notably, this is true for floating point accesses.
    // Match the device profile (Surface RT = Tegra 3 / Cortex-A9, see the _M_ARM block below):
    // NO hardware integer divide -- Cortex-A9 lacks sdiv/udiv, so `%` / `/` must use the software
    // path, NOT an inline udiv/sdiv (which the device would fault on and the Thumb-2 encoder does
    // not emit). Dropping HWCAP_IDIVA here makes the simulator faithful to the device. (HWCAP_VFPv4
    // vs the device's VFPv3 remains a separate faithfulness item -- Cortex-A9 has no VFMA.)
    flags = HWCAP_ARMv7 | HWCAP_VFP | HWCAP_VFPv3 | HWCAP_VFPv4 | HWCAP_NEON
          | HWCAP_FIXUP_FAULT;
#else

#if defined(__linux__)
    bool readAuxv = false;
    int fd = open("/proc/self/auxv", O_RDONLY);
    if (fd > 0) {
        struct { uint32_t a_type; uint32_t a_val; } aux;
        while (read(fd, &aux, sizeof(aux))) {
            if (aux.a_type == AT_HWCAP) {
                flags = aux.a_val;
                readAuxv = true;
                break;
            }
        }
        close(fd);
    }

    FILE* fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char buf[1024];
        memset(buf, 0, sizeof(buf));
        size_t len = fread(buf, sizeof(char), sizeof(buf) - 1, fp);
        fclose(fp);
        buf[len] = '\0';

        // Read the cpuinfo Features if the auxv is not available.
        if (!readAuxv) {
            char* featureList = strstr(buf, "Features");
            if (featureList) {
                if (char* featuresEnd = strstr(featureList, "\n"))
                    *featuresEnd = '\0';
                flags = ParseARMCpuFeatures(featureList + 8);
            }
            if (strstr(buf, "ARMv7"))
                flags |= HWCAP_ARMv7;
        }

        // The exynos7420 cpu (EU galaxy S6 (Note)) has a bug where sometimes
        // flushing doesn't invalidate the instruction cache. As a result we force
        // it by calling the cacheFlush twice on different start addresses.
        char* exynos7420 = strstr(buf, "Exynos7420");
        if (exynos7420)
            forceDoubleCacheFlush = true;
    }
#endif

#if defined(_M_ARM) && !defined(__linux__)
    // Varan M1: Windows has no /proc/self/auxv or /proc/cpuinfo. Hardcode the
    // smoke target's CPU profile -- Surface RT = NVIDIA Tegra 3 (Cortex-A9, ARMv7):
    // VFPv3 + NEON + 32 VFP registers, but DELIBERATELY NO hardware integer divide
    // (Cortex-A9 lacks sdiv/udiv; emitting idiv would fault ONLY on the device -- the
    // worst failure mode, so be conservative). Overridable at runtime via ARMHWCAP.
    flags = HWCAP_ARMv7 | HWCAP_VFP | HWCAP_VFPv3 | HWCAP_NEON | HWCAP_VFPD32;
#endif

    // If compiled to use specialized features then these features can be
    // assumed to be present otherwise the compiler would fail to run.

#ifdef JS_CODEGEN_ARM_HARDFP
    // Compiled to use the hardfp ABI.
    flags |= HWCAP_USE_HARDFP_ABI;
#endif

#if defined(__VFP_FP__) && !defined(__SOFTFP__)
    // Compiled to use VFP instructions so assume VFP support.
    flags |= HWCAP_VFP;
#endif

#if defined(__ARM_ARCH_7__) || defined (__ARM_ARCH_7A__)
    // Compiled to use ARMv7 instructions so assume the ARMv7 arch.
    flags |= HWCAP_ARMv7;
#endif

#if defined(__APPLE__)
    #if defined(__ARM_NEON__)
        flags |= HWCAP_NEON;
    #endif
    #if defined(__ARMVFPV3__)
        flags |= HWCAP_VFPv3 | HWCAP_VFPD32
    #endif
#endif

#endif // JS_SIMULATOR_ARM

    armHwCapFlags = CanonicalizeARMHwCapFlags(flags);

    JitSpew(JitSpew_Codegen, "ARM HWCAP: 0x%x\n", armHwCapFlags);
    return;
}

uint32_t
GetARMFlags()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags;
}

bool HasARMv7()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_ARMv7;
}

bool HasMOVWT()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_ARMv7;
}

bool HasLDSTREXBHD()
{
    // These are really available from ARMv6K and later, but why bother?
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_ARMv7;
}

bool HasDMBDSBISB()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_ARMv7;
}

bool HasVFPv3()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_VFPv3;
}

bool HasVFP()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_VFP;
}

bool Has32DP()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_VFPD32;
}

bool HasIDIV()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_IDIVA;
}

// This is defined in the header and inlined when not using the simulator.
#ifdef JS_SIMULATOR_ARM
bool UseHardFpABI()
{
    MOZ_ASSERT(armHwCapFlags != HWCAP_UNINITIALIZED);
    return armHwCapFlags & HWCAP_USE_HARDFP_ABI;
}
#endif

Registers::Code
Registers::FromName(const char* name)
{
    // Check for some register aliases first.
    if (strcmp(name, "ip") == 0)
        return ip;
    if (strcmp(name, "r13") == 0)
        return r13;
    if (strcmp(name, "lr") == 0)
        return lr;
    if (strcmp(name, "r15") == 0)
        return r15;

    for (size_t i = 0; i < Total; i++) {
        if (strcmp(GetName(i), name) == 0)
            return Code(i);
    }

    return Invalid;
}

FloatRegisters::Code
FloatRegisters::FromName(const char* name)
{
    for (size_t i = 0; i < TotalSingle; ++i) {
        if (strcmp(GetSingleName(Encoding(i)), name) == 0)
            return VFPRegister(i, VFPRegister::Single).code();
    }
    for (size_t i = 0; i < TotalDouble; ++i) {
        if (strcmp(GetDoubleName(Encoding(i)), name) == 0)
            return VFPRegister(i, VFPRegister::Double).code();
    }

    return Invalid;
}

FloatRegisterSet
VFPRegister::ReduceSetForPush(const FloatRegisterSet& s)
{
    LiveFloatRegisterSet mod;
    for (FloatRegisterIterator iter(s); iter.more(); ++iter) {
        if ((*iter).isSingle()) {
            // Add in just this float.
            mod.addUnchecked(*iter);
        } else if ((*iter).id() < 16) {
            // A double with an overlay, add in both floats.
            mod.addUnchecked((*iter).singleOverlay(0));
            mod.addUnchecked((*iter).singleOverlay(1));
        } else {
            // Add in the lone double in the range 16-31.
            mod.addUnchecked(*iter);
        }
    }
    return mod.set();
}

uint32_t
VFPRegister::GetPushSizeInBytes(const FloatRegisterSet& s)
{
    FloatRegisterSet ss = s.reduceSetForPush();
    uint64_t bits = ss.bits();
    uint32_t ret = mozilla::CountPopulation32(bits&0xffffffff) * sizeof(float);
    ret +=  mozilla::CountPopulation32(bits >> 32) * sizeof(double);
    return ret;
}
uint32_t
VFPRegister::getRegisterDumpOffsetInBytes()
{
    if (isSingle())
        return id() * sizeof(float);
    if (isDouble())
        return id() * sizeof(double);
    MOZ_CRASH("not Single or Double");
}

uint32_t
FloatRegisters::ActualTotalPhys()
{
    if (Has32DP())
        return 32;
    return 16;
}


} // namespace jit
} // namespace js

