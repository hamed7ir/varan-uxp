/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Varan -- Varan JIT device diagnostics (shared TU).
 *
 * The JIT-PC resolver and the CPSR-T-bit fault reporter, moved out of the js.exe shell
 * (shell/js.cpp) into jit/ so BOTH the standalone shell AND the browser (xul.dll) use ONE
 * copy -- there is no second copy to drift. The resolver is architecture-independent (the
 * x86 simulator host runs it to self-test); only the Windows-ARM fault FILTER is guarded.
 */

#ifndef jit_VaranFaultReporter_h
#define jit_VaranFaultReporter_h

#include "jsapi.h"

class JSScript;

namespace js {
namespace jit {

// Resolve a code address to a human-readable "BASELINE file:line bytecode offset N (JSOP_xxx)"
// line. Cross-platform: reads the JitcodeGlobalTable, allocates nothing, takes no lock, guards
// every dereference, and MASKS bit0 (never sets it -- JitCode ranges are even). Returns true iff
// |pcRaw| landed inside a registered entry. |scriptOut|/|bcOffOut| are optional ground-truth
// outputs for the self-test. Safe to call from a crashed process.
bool VaranResolveJitPc(JSRuntime* rt, void* pcRaw, char* buf, size_t bufLen,
                       JSScript** scriptOut, uint32_t* bcOffOut);

// Install the last-chance fault reporter (Windows-ARM only; no-op elsewhere). Captures |cx|'s
// runtime so the argument-less filter can reach the table. Idempotent. Called once from
// JitRuntime::initialize, i.e. exactly when the JIT comes up -- so it is absent under
// JS_CODEGEN_NONE for free.
void VaranInstallFaultReporter(JSContext* cx);

} // namespace jit
} // namespace js

#endif /* jit_VaranFaultReporter_h */
