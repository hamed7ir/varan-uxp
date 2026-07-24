/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Varan -- Varan JIT device diagnostics: the JIT-PC resolver + the
 * CPSR-T-bit last-chance fault reporter. See VaranFaultReporter.h for the rationale.
 *
 * ONE copy for the shell and the browser. The resolver is compiled wherever the JIT is
 * (ENABLE_ION); the fault FILTER body is Windows-ARM only.
 */

#include "jit/VaranFaultReporter.h"

#include "jsapi.h"
#include "jsscript.h"
#include "jsopcode.h"

#include "jit/BaselineJIT.h"
#include "jit/JitcodeMap.h"

#include "vm/Runtime.h"

#include "jsscriptinlines.h"

using namespace js;
using namespace js::jit;

// ---------------------------------------------------------------------------
// The resolver (cross-platform).
//
// PROBLEM. When a fault lands in generated code, Windows Error Reporting cannot attribute the PC
// to any module (JIT pages belong to no module and carry no .pdata): "Fault Module StackHash_...".
// A device trip that yields only "it crashed at an address that means nothing" is a wasted trip.
//
// WHY NOT RtlAddGrowableFunctionTable. On ARM that table feeds EXCEPTION DISPATCH, not just
// backtraces, so unwind data that is subtly wrong corrupts the handling of the very fault we are
// observing -- strictly worse than the empty trace it replaces. We have no verified ARM32 unwind
// encoder for our frames, so we do not get to gamble on one.
//
// WHAT WE DO. Baseline registers every compiled script in the JitcodeGlobalTable UNCONDITIONALLY
// in the opt build (BaselineCompiler::compile, under no #ifdef -- the profiler can be turned on
// with baseline code on stack). So the map is already in memory with nothing enabled.
//
// FAULT-PATH SAFETY: no allocation (skiplist walk + CompactBufferReader decode), no lock, every
// dereference staged-guarded, output into a caller stack buffer. ★ MASKS bit0, never sets -- a
// captured Thumb PC may be odd; JitCode ranges are even; comparing an odd PC against an even range
// is the mask-vs-set confusion that has already produced device-lethal bugs in this port.
// ---------------------------------------------------------------------------
bool
js::jit::VaranResolveJitPc(JSRuntime* rt, void* pcRaw, char* buf, size_t bufLen,
                           JSScript** scriptOut, uint32_t* bcOffOut)
{
    if (scriptOut)
        *scriptOut = nullptr;
    if (bcOffOut)
        *bcOffOut = UINT32_MAX;
    if (!buf || bufLen == 0)
        return false;
    buf[0] = '\0';

    uint8_t* pc = (uint8_t*)((uintptr_t)pcRaw & ~(uintptr_t)1);   // mask, never set

    if (!rt) {
        snprintf(buf, bufLen, "%p  (no JSRuntime captured)", pc);
        return false;
    }
    if (!rt->hasJitRuntime()) {
        snprintf(buf, bufLen, "%p  (no JitRuntime -- the JIT never initialised)", pc);
        return false;
    }
    JitRuntime* jrt = rt->jitRuntime();
    if (!jrt->hasJitcodeGlobalTable()) {
        snprintf(buf, bufLen, "%p  (no JitcodeGlobalTable)", pc);
        return false;
    }
    JitcodeGlobalTable* table = jrt->getJitcodeGlobalTable();
    if (table->empty()) {
        snprintf(buf, bufLen, "%p  (JitcodeGlobalTable is EMPTY -- nothing JIT-compiled yet)", pc);
        return false;
    }

    const JitcodeGlobalEntry* e = table->lookup((void*)pc);
    if (!e) {
        snprintf(buf, bufLen,
                 "%p  NOT IN TABLE (trampoline, IC stub, regexp or wasm code -- read the Lr line)",
                 pc);
        return false;
    }

    uint8_t* base = (uint8_t*)e->nativeStartAddr();
    uint8_t* end  = (uint8_t*)e->nativeEndAddr();
    unsigned off  = (unsigned)(pc - base);
    unsigned size = (unsigned)(end - base);

    if (!e->isBaseline()) {
        snprintf(buf, bufLen, "%p  kind=%d (not Baseline)  [+0x%x of 0x%x @ %p]",
                 pc, (int)e->kind(), off, size, base);
        return true;
    }

    const JitcodeGlobalEntry::BaselineEntry& b = e->baselineEntry();
    JSScript* script = b.script();
    if (!script) {
        snprintf(buf, bufLen, "BASELINE <no script>  [+0x%x of 0x%x @ %p]", off, size, base);
        return true;
    }
    if (scriptOut)
        *scriptOut = script;

    const char* filename = script->filename() ? script->filename() : "<no filename>";
    unsigned lineno = script->lineno();

    if (script->hasBaselineScript()) {
        BaselineScript* bs = script->baselineScript();
        // numPCMappingIndexEntries()==0 would make approximatePcForNativeAddress read entry(0)
        // out of bounds -- a second fault inside the fault handler.
        if (bs && bs->numPCMappingIndexEntries() > 0 &&
            pc >= bs->method()->raw() &&
            pc < bs->method()->raw() + bs->method()->instructionsSize())
        {
            jsbytecode* bpc = bs->approximatePcForNativeAddress(script, pc);
            if (bpc && script->containsPC(bpc)) {
                uint32_t bcOff = (uint32_t)script->pcToOffset(bpc);
                if (bcOffOut)
                    *bcOffOut = bcOff;
                unsigned op = (unsigned)(uint8_t)*bpc;
                const char* opname = (op < JSOP_LIMIT) ? js::CodeName[op] : "?";
                snprintf(buf, bufLen,
                         "BASELINE  %s:%u  bytecode offset %u  (%s)  [native +0x%x of 0x%x @ %p]",
                         filename, lineno, (unsigned)bcOff, opname, off, size, base);
                return true;
            }
        }
    }

    snprintf(buf, bufLen, "BASELINE  %s:%u  <no pc mapping>  [native +0x%x of 0x%x @ %p]",
             filename, lineno, off, size, base);
    return true;
}

// ---------------------------------------------------------------------------
// The last-chance fault reporter (Windows-ARM only).
//
// The browser is a GUI (windows-subsystem) process, so stderr is usually invisible -- the report
// must reach a FILE. This variant writes BOTH: a log file under %TEMP% (pre-opened at install so
// the handler does no CreateFile) and stderr (for console shells like js.exe). It is LAST-CHANCE
// (SetUnhandledExceptionFilter), never a first-chance VEH -- a first-chance handler masked every
// AV in M4.1. It ACTS only on a JIT fault (PC in unattributable memory, or an illegal
// instruction) and otherwise CHAINS to the previous filter, so ordinary browser crashes are
// handled normally.
//
// ★ THE DECISIVE DATUM IS THE CPSR T-BIT. 0xC000001D means either an undefined instruction or a
// branch to an EVEN target that switched the core to ARM state; CPSR bit5 (T) separates them:
// T==1 -> corrupt/undefined Thumb word or stale I-cache; T==0 -> interworking (even target).
// ---------------------------------------------------------------------------
#if defined(XP_WIN) && defined(_M_ARM)

#include <windows.h>

static JSRuntime* gVaranRt = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER gVaranPrevFilter = nullptr;
static HANDLE gVaranLog = INVALID_HANDLE_VALUE;
static bool gVaranInHandler = false;
static char gVaranLogPath[MAX_PATH] = { 0 };   // resolved path, for the install header

static void
VaranEmit(const char* s)
{
    size_t n = 0; while (s[n]) n++;
    if (gVaranLog != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(gVaranLog, s, (DWORD)n, &wrote, nullptr);
    }
    fputs(s, stderr);
}

static LONG WINAPI
VaranFaultFilter(LPEXCEPTION_POINTERS info)
{
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    const CONTEXT* cx = info->ContextRecord;
    char line[640];

    // Is this OURS? A JIT fault has its PC in memory that belongs to no module, or it is an
    // illegal instruction (which only our generated code should ever produce here).
    bool isJit = (er->ExceptionCode == 0xC000001DUL);
    MEMORY_BASIC_INFORMATION mbi;
    bool haveRegion = (VirtualQuery(er->ExceptionAddress, &mbi, sizeof(mbi)) == sizeof(mbi));
    char mod[MAX_PATH]; DWORD modn = 0;
    if (haveRegion) {
        modn = GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, sizeof(mod));
        if (modn == 0)
            isJit = true;   // no module => dynamically allocated => JIT code
    }

    if (!isJit) {
        // Not a JIT fault -- let the previous handler (e.g. FPE) or the OS deal with it.
        return gVaranPrevFilter ? gVaranPrevFilter(info) : EXCEPTION_CONTINUE_SEARCH;
    }
    if (gVaranInHandler)
        return EXCEPTION_EXECUTE_HANDLER;   // never recurse
    gVaranInHandler = true;

    VaranEmit("\n===== VARAN JIT FAULT (browser) =====\n");
    snprintf(line, sizeof(line), "code   : 0x%08lx%s\n", (unsigned long)er->ExceptionCode,
             er->ExceptionCode == 0xC000001DUL ? "  (ILLEGAL INSTRUCTION)" :
             er->ExceptionCode == 0xC0000005UL ? "  (ACCESS VIOLATION)" : "");
    VaranEmit(line);
    snprintf(line, sizeof(line), "Pc     : 0x%08lx    Lr : 0x%08lx    Sp : 0x%08lx\n",
             (unsigned long)cx->Pc, (unsigned long)cx->Lr, (unsigned long)cx->Sp);
    VaranEmit(line);

    const bool thumb = (cx->Cpsr & (1u << 5)) != 0;
    snprintf(line, sizeof(line), "Cpsr   : 0x%08lx    T-bit: %d => %s\n",
             (unsigned long)cx->Cpsr, thumb ? 1 : 0,
             thumb ? "THUMB: undefined/corrupt Thumb word, or stale I-cache"
                   : "*** ARM STATE: INTERWORKING BUG -- branched to an EVEN target ***");
    VaranEmit(line);

    if (haveRegion) {
        snprintf(line, sizeof(line), "region : base=%p protect=0x%lx state=0x%lx  %s\n",
                 mbi.AllocationBase, (unsigned long)mbi.Protect, (unsigned long)mbi.State,
                 modn > 0 ? mod : "<NO MODULE -- dynamically allocated, i.e. JIT CODE>");
        VaranEmit(line);
        bool readable = (mbi.State == MEM_COMMIT) &&
                        !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (readable) {
            const uint16_t* hw = (const uint16_t*)(cx->Pc & ~(uintptr_t)1);
            char* p = line; p += snprintf(p, sizeof(line), "bytes  :");
            for (int i = -4; i < 8; i++)
                p += snprintf(p, sizeof(line) - (p - line), " %s%04x", i == 0 ? ">" : "", hw[i]);
            snprintf(p, sizeof(line) - (p - line), "\n");
            VaranEmit(line);
        }
    }

    // Attribution -- only now do we touch the JS heap (see the resolver's safety notes).
    char buf[512];
    VaranResolveJitPc(gVaranRt, (void*)cx->Pc, buf, sizeof(buf), nullptr, nullptr);
    snprintf(line, sizeof(line), "Pc  -> %s\n", buf); VaranEmit(line);
    VaranResolveJitPc(gVaranRt, (void*)cx->Lr, buf, sizeof(buf), nullptr, nullptr);
    snprintf(line, sizeof(line), "Lr  -> %s\n", buf); VaranEmit(line);
    VaranEmit("=====================================\n");

    if (gVaranLog != INVALID_HANDLE_VALUE)
        FlushFileBuffers(gVaranLog);
    fflush(stderr);

    gVaranInHandler = false;
    // Terminate cleanly: no WER dialog to click through on an unattended device run.
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// Open the log at a FIXED ABSOLUTE PATH, appending.
//
// ★ WHY NOT %TEMP% ANY MORE. The 2026-07-24 Ion trip came back with an EMPTY fault
// log and the emptiness had two readings we could not separate offline: either the
// filter never fired, or the file was written somewhere we never looked. The run was
// elevated, and an elevated process's %TEMP% is a DIFFERENT directory from the one
// you inspect afterwards -- so GetTempPathW made the log's location depend on how the
// browser happened to be launched. A fixed path removes that variable entirely.
//
// ★ APPEND, NOT CREATE_ALWAYS. A bisect session restarts the browser several times.
// Truncating on every start would leave only the last run's evidence.
//
// Fallback chain: C:\varan\  ->  the executable's own directory. Both are logged in
// the install header, so the next session always knows which file to read.
// ---------------------------------------------------------------------------
static HANDLE
VaranOpenLog()
{
    HANDLE h;

    // 1. C:\varan\varan-jit-fault.log  (CreateDirectory is idempotent; ERROR_ALREADY_EXISTS is fine)
    CreateDirectoryW(L"C:\\varan", nullptr);
    h = CreateFileW(L"C:\\varan\\varan-jit-fault.log", FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        snprintf(gVaranLogPath, sizeof(gVaranLogPath), "C:\\varan\\varan-jit-fault.log");
        SetFilePointer(h, 0, nullptr, FILE_END);
        return h;
    }

    // 2. next to the executable (works when C:\ is not writable by this token)
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        for (DWORD i = n; i > 0; i--) {
            if (exe[i - 1] == L'\\' || exe[i - 1] == L'/') { exe[i] = L'\0'; break; }
        }
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%svaran-jit-fault.log", exe);
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            WideCharToMultiByte(CP_ACP, 0, path, -1, gVaranLogPath,
                                (int)sizeof(gVaranLogPath), nullptr, nullptr);
            SetFilePointer(h, 0, nullptr, FILE_END);
            return h;
        }
    }

    snprintf(gVaranLogPath, sizeof(gVaranLogPath), "<NO LOG FILE -- stderr only>");
    return INVALID_HANDLE_VALUE;
}

void
js::jit::VaranInstallFaultReporter(JSContext* cx)
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    gVaranRt = cx->runtime();

    gVaranLog = VaranOpenLog();

    // ★ THE INSTALL HEADER IS THE POSITIVE CONTROL FOR THE WHOLE REPORTER.
    // An empty log must never again have two readings:
    //     header present, no fault block  => the filter INSTALLED and never fired
    //                                        (so the crash was not attributed to JIT code,
    //                                         or the process died without an unhandled
    //                                         exception reaching a top-level filter)
    //     no file at all                  => install or plumbing broke; nothing was armed
    // The build stamp is this translation unit's compile time. It identifies the binary
    // well enough to catch "you tested yesterday's package", which is its only job --
    // it does NOT prove the rest of the tree was rebuilt in the same cycle.
    SYSTEMTIME st;
    GetLocalTime(&st);
    char hdr[MAX_PATH + 256];
    snprintf(hdr, sizeof(hdr),
             "\n===== VARAN FAULT REPORTER INSTALLED =====\n"
             "when   : %04u-%02u-%02u %02u:%02u:%02u\n"
             "pid    : %lu\n"
             "build  : " __DATE__ " " __TIME__ "  (VaranFaultReporter.cpp compile stamp)\n"
             "log    : %s\n"
             "=========================================\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long)GetCurrentProcessId(), gVaranLogPath);
    VaranEmit(hdr);
    if (gVaranLog != INVALID_HANDLE_VALUE)
        FlushFileBuffers(gVaranLog);

    gVaranPrevFilter = SetUnhandledExceptionFilter(VaranFaultFilter);
}

#else  // not (XP_WIN && _M_ARM)

void
js::jit::VaranInstallFaultReporter(JSContext*)
{
    // No last-chance filter off Windows-ARM. The resolver above is still available (the x86
    // simulator host self-tests it).
}

#endif
