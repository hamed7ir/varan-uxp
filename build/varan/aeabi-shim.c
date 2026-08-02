/* Varan M2 -- ARM EABI integer divmod helpers (wrap MSVC ARM CRT).
 *
 * The Cortex-A9 (Tegra 3) has no hardware integer divide. Where SpiderMonkey
 * explicitly names the combined EABI helpers __aeabi_idivmod / __aeabi_uidivmod
 * (its A32 JIT soft-divide -- dropped under JS_CODEGEN_NONE -- and wasm), clang
 * emits calls to those names, which the MSVC ARM CRT does NOT ship. (Ordinary
 * C++ `a/b` / `a%b` on the -windows-msvc target lowers to __rt_sdiv/__rt_udiv
 * directly and needs no shim.) This shim provides the __aeabi_* names by
 * delegating to the CRT's own hardware-tuned helpers -- replacing the M1
 * software long-division (VARAN-M1-REPORT.md debt table).
 *
 * __rt_sdiv / __rt_udiv use the ARM RTABI operand order: DIVISOR first (r0),
 * DIVIDEND second (r1); they return __value_in_regs { quotient in r0,
 * remainder in r1 } (verified by disassembly of libcmt.lib divide.obj -- NOT
 * quotient-only as the M1 comment wrongly assumed). Declaring them as returning
 * a 64-bit value makes the compiler read that r0:r1 pair as
 * { low32 = r0 = quotient, high32 = r1 = remainder }.
 *
 * __aeabi_*divmod take the OPPOSITE order (numerator, denominator) and produce
 * the SAME { r0 = quotient, r1 = remainder } packing, so each wrapper is a
 * register swap + tail call: pass (denominator, numerator) to __rt_* and
 * forward its 64-bit result unchanged. Verified: compiles clean under clang-cl
 * --target=thumbv7-unknown-windows-msvc and disassembles to
 * `mov r2,r0; mov r0,r1; mov r1,r2; b.w __rt_*div` (the num<->den swap + tail
 * call, r0/r1 result passed straight through). Thumb-2, per the standing rule.
 *
 * NOTE: divide-by-zero traps in the CRT helpers (__brkdiv0), unlike the M1
 * 0xFFFFFFFF sentinel. Unreachable from SpiderMonkey codegen (divICommon
 * zero-guards before the call). On-device numeric divmod correctness is owed
 * (deferred: fold into a future device trip).
 */

/* MSVC ARM CRT run-time division helpers (no public header declares them). */
long long          __rt_sdiv(int divisor, int dividend);
unsigned long long __rt_udiv(unsigned divisor, unsigned dividend);

unsigned long long __aeabi_uidivmod(unsigned num, unsigned den)
{
    return __rt_udiv(den, num);   /* swap: divisor = den, dividend = num */
}

long long __aeabi_idivmod(int num, int den)
{
    return __rt_sdiv(den, num);   /* swap: divisor = den, dividend = num */
}

/* Quotient-only variants (return quotient in r0). Currently UNREFERENCED by the
 * link (only the *divmod pair is undefined; ordinary division uses __rt_* on the
 * MSVC target). Provided for completeness -- the MSVC ARM CRT defines no __aeabi_*
 * symbols, so no duplicate-symbol risk. */
unsigned __aeabi_uidiv(unsigned num, unsigned den)
{
    return (unsigned)__rt_udiv(den, num);
}

int __aeabi_idiv(int num, int den)
{
    return (int)__rt_sdiv(den, num);
}
