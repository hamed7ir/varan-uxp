/* ARM32/UWP build spike — force-included before the Windows SDK headers.
 *
 * clang-cl 18's <armintr.h> defines only the _ARM_BARRIER_* enum; it does NOT
 * define the CP15 coprocessor-register operand macros that the Windows SDK's
 * um/winnt.h passes to _MoveFromCoprocessor()/_MoveToCoprocessor() on ARM
 * (CP15_TPIDRURW etc.). MSVC's own armintr.h provides them. Supply them here so
 * winnt.h compiles under clang-cl. Values are the ARM CP15 register encodings
 * {coproc, opcode1, CRn, CRm, opcode2}, matching MSVC's armintr.h.
 *
 * Guarded to ARM targets and to the undefined case so it is inert elsewhere and
 * cannot clash with a future real armintr.h.
 */
/* Varan M2: _ARM_ is now supplied by AC_DEFINE(_ARM_) in old-configure.in's
 * arm*) case (both copies), delivered via the -FI'd js-confdefs.h / mozilla-config.h
 * on every target compile -- the same path that already carries _X86_/_AMD64_ -- so
 * winnt.h's ARM path is selected without a shim workaround. No longer defined here.
 * (If a configure-time test compile ever pulls in winnt.h before confdefs exists and
 * hits "#error No Target Architecture", restore the idempotent guard below:
 *   #if defined(_M_ARM) && !defined(_ARM_)
 *   #define _ARM_ 1
 *   #endif  ) */

#if defined(_M_ARM) || defined(__arm__)
#ifndef CP15_TPIDRURW
#define CP15_TPIDRURW  15, 0, 13, 0, 2   /* User Read/Write Thread ID (TEB ptr) */
#define CP15_TPIDRURO  15, 0, 13, 0, 3   /* User Read-Only Thread ID */
#define CP15_TPIDRPRW  15, 0, 13, 0, 4   /* Privileged-only Read/Write Thread ID */
#define CP15_PMCCNTR   15, 0,  9, 13, 0  /* Performance Monitor Cycle Counter */
#define CP15_PMSELR    15, 0,  9, 12, 5  /* Performance Monitor Event Counter Selection */
#define CP15_PMXEVCNTR 15, 0,  9, 13, 2  /* Performance Monitor Selected Event Counter */
#define CP15_BPIALL    15, 0,  7,  5, 6  /* Branch Predictor Invalidate All */
#endif
#endif
