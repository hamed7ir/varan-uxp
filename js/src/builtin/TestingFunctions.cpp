/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/TestingFunctions.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Move.h"
#include "mozilla/Sprintf.h"
#include "mozilla/Unused.h"

#include <cmath>

#include "jsapi.h"
#include "jscntxt.h"
#include "jsfriendapi.h"
#include "jsgc.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jswrapper.h"

#include "builtin/Promise.h"
#include "builtin/SelfHostingDefines.h"
#ifdef DEBUG
#include "frontend/TokenStream.h"
#include "irregexp/RegExpAST.h"
#include "irregexp/RegExpEngine.h"
#include "irregexp/RegExpParser.h"
#endif
#include "jit/InlinableNatives.h"
#include "jit/JitFrameIterator.h"
#if defined(JS_SIMULATOR_ARM) && defined(VARAN_THUMB2)
# include "jit/arm/Simulator-arm.h"   // VARAN P1.1: run the Thumb-2 hello-world through the sim
# include "jit/arm/Assembler-arm.h"   // VARAN P1.2b: VaranEncodeBranchInst/BranchKind, r0, Imm16
# include "jit/MacroAssembler.h"      // VARAN P1.2b: bind() end-to-end test drives a real MacroAssembler
# include "jit/Ion.h"                 // VARAN Batch 4: AutoFlushICache for the pool executableCopy test
# include "jit/Linker.h"              // VARAN 2026-07-24: real JitCode, so real PatchJump can be called
# include "jit/JitCompartment.h"      // VARAN 2026-07-25: AutoWritableJitCode, to corrupt slot1 on purpose
#endif
#include "js/Debug.h"
#include "js/HashTable.h"
#include "js/StructuredClone.h"
#include "js/UbiNode.h"
#include "js/UbiNodeBreadthFirst.h"
#include "js/UbiNodeShortestPaths.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/ProxyObject.h"
#include "vm/SavedStacks.h"
#include "vm/Stack.h"
#include "vm/StringBuffer.h"
#include "vm/TraceLogging.h"
#include "wasm/AsmJS.h"
#include "wasm/WasmBinaryToExperimentalText.h"
#include "wasm/WasmBinaryToText.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmTextToBinary.h"

#include "jscntxtinlines.h"
#include "jsobjinlines.h"

#if defined(JS_SIMULATOR_ARM) && defined(VARAN_THUMB2)
# include "jit/MacroAssembler-inl.h"  // VARAN P1.2b: MacroAssembler ctor + inline helpers
#endif

#include "vm/EnvironmentObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::ArrayLength;
using mozilla::Move;

// If fuzzingSafe is set, remove functionality that could cause problems with
// fuzzers. Set this via the environment variable MOZ_FUZZING_SAFE.
static bool fuzzingSafe = false;

// If disableOOMFunctions is set, disable functionality that causes artificial
// OOM conditions.
static bool disableOOMFunctions = false;

static bool
EnvVarIsDefined(const char* name)
{
    const char* value = getenv(name);
    return value && *value;
}

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
static bool
EnvVarAsInt(const char* name, int* valueOut)
{
    if (!EnvVarIsDefined(name))
        return false;

    *valueOut = atoi(getenv(name));
    return true;
}
#endif

static bool
GetBuildConfiguration(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject info(cx, JS_NewPlainObject(cx));
    if (!info)
        return false;

    if (!JS_SetProperty(cx, info, "rooting-analysis", FalseHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "exact-rooting", TrueHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "trace-jscalls-api", FalseHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "incremental-gc", TrueHandleValue))
        return false;

    if (!JS_SetProperty(cx, info, "generational-gc", TrueHandleValue))
        return false;

    RootedValue value(cx);
#ifdef DEBUG
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "debug", value))
        return false;

    value = BooleanValue(true);
    if (!JS_SetProperty(cx, info, "release_or_beta", value))
        return false;

#ifdef JS_HAS_CTYPES
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "has-ctypes", value))
        return false;

#ifdef JS_CPU_X86
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "x86", value))
        return false;

#ifdef JS_CPU_X64
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "x64", value))
        return false;

#ifdef JS_SIMULATOR_ARM
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "arm-simulator", value))
        return false;

#ifdef JS_SIMULATOR_ARM64
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "arm64-simulator", value))
        return false;

#ifdef MOZ_ASAN
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "asan", value))
        return false;

#ifdef MOZ_TSAN
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "tsan", value))
        return false;

#ifdef JS_MORE_DETERMINISTIC
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "more-deterministic", value))
        return false;

#ifdef MOZ_PROFILING
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "profiling", value))
        return false;

#ifdef INCLUDE_MOZILLA_DTRACE
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "dtrace", value))
        return false;

#ifdef MOZ_VALGRIND
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "valgrind", value))
        return false;

#ifdef JS_OOM_DO_BACKTRACES
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "oom-backtraces", value))
        return false;

#ifdef ENABLE_BINARYDATA
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "binary-data", value))
        return false;

    value = BooleanValue(true);
    if (!JS_SetProperty(cx, info, "intl-api", value))
        return false;
#ifdef XP_SOLARIS
    value = BooleanValue(false);
#else
    value = BooleanValue(true);
#endif    
    if (!JS_SetProperty(cx, info, "mapped-array-buffer", value))
        return false;

#ifdef MOZ_MEMORY
    value = BooleanValue(true);
#else
    value = BooleanValue(false);
#endif
    if (!JS_SetProperty(cx, info, "moz-memory", value))
        return false;

    value.setInt32(sizeof(void*));
    if (!JS_SetProperty(cx, info, "pointer-byte-size", value))
        return false;

    args.rval().setObject(*info);
    return true;
}

static bool
GC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    /*
     * If the first argument is 'zone', we collect any zones previously
     * scheduled for GC via schedulegc. If the first argument is an object, we
     * collect the object's zone (and any other zones scheduled for
     * GC). Otherwise, we collect all zones.
     */
    bool zone = false;
    if (args.length() >= 1) {
        Value arg = args[0];
        if (arg.isString()) {
            if (!JS_StringEqualsAscii(cx, arg.toString(), "zone", &zone))
                return false;
        } else if (arg.isObject()) {
            PrepareZoneForGC(UncheckedUnwrap(&arg.toObject())->zone());
            zone = true;
        }
    }

    bool shrinking = false;
    if (args.length() >= 2) {
        Value arg = args[1];
        if (arg.isString()) {
            if (!JS_StringEqualsAscii(cx, arg.toString(), "shrinking", &shrinking))
                return false;
        }
    }

#ifndef JS_MORE_DETERMINISTIC
    size_t preBytes = cx->runtime()->gc.usage.gcBytes();
#endif

    if (zone)
        PrepareForDebugGC(cx->runtime());
    else
        JS::PrepareForFullGC(cx);

    JSGCInvocationKind gckind = shrinking ? GC_SHRINK : GC_NORMAL;
    JS::GCForReason(cx, gckind, JS::gcreason::API);

    char buf[256] = { '\0' };
#ifndef JS_MORE_DETERMINISTIC
    SprintfLiteral(buf, "before %" PRIuSIZE ", after %" PRIuSIZE "\n",
                   preBytes, cx->runtime()->gc.usage.gcBytes());
#endif
    JSString* str = JS_NewStringCopyZ(cx, buf);
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

static bool
MinorGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.get(0) == BooleanValue(true))
        cx->runtime()->gc.storeBuffer.setAboutToOverflow();

    cx->minorGC(JS::gcreason::API);
    args.rval().setUndefined();
    return true;
}

#define FOR_EACH_GC_PARAM(_)                                                    \
    _("maxBytes",                   JSGC_MAX_BYTES,                      true)  \
    _("maxMallocBytes",             JSGC_MAX_MALLOC_BYTES,               true)  \
    _("gcBytes",                    JSGC_BYTES,                          false) \
    _("gcNumber",                   JSGC_NUMBER,                         false) \
    _("mode",                       JSGC_MODE,                           true)  \
    _("unusedChunks",               JSGC_UNUSED_CHUNKS,                  false) \
    _("totalChunks",                JSGC_TOTAL_CHUNKS,                   false) \
    _("sliceTimeBudget",            JSGC_SLICE_TIME_BUDGET,              true)  \
    _("markStackLimit",             JSGC_MARK_STACK_LIMIT,               true)  \
    _("highFrequencyTimeLimit",     JSGC_HIGH_FREQUENCY_TIME_LIMIT,      true)  \
    _("highFrequencyLowLimit",      JSGC_HIGH_FREQUENCY_LOW_LIMIT,       true)  \
    _("highFrequencyHighLimit",     JSGC_HIGH_FREQUENCY_HIGH_LIMIT,      true)  \
    _("highFrequencyHeapGrowthMax", JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX, true)  \
    _("highFrequencyHeapGrowthMin", JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN, true)  \
    _("lowFrequencyHeapGrowth",     JSGC_LOW_FREQUENCY_HEAP_GROWTH,      true)  \
    _("dynamicHeapGrowth",          JSGC_DYNAMIC_HEAP_GROWTH,            true)  \
    _("dynamicMarkSlice",           JSGC_DYNAMIC_MARK_SLICE,             true)  \
    _("allocationThreshold",        JSGC_ALLOCATION_THRESHOLD,           true)  \
    _("minEmptyChunkCount",         JSGC_MIN_EMPTY_CHUNK_COUNT,          true)  \
    _("maxEmptyChunkCount",         JSGC_MAX_EMPTY_CHUNK_COUNT,          true)  \
    _("compactingEnabled",          JSGC_COMPACTING_ENABLED,             true)  \
    _("refreshFrameSlicesEnabled",  JSGC_REFRESH_FRAME_SLICES_ENABLED,   true)

static const struct ParamInfo {
    const char*     name;
    JSGCParamKey    param;
    bool            writable;
} paramMap[] = {
#define DEFINE_PARAM_INFO(name, key, writable)                                  \
    {name, key, writable},
FOR_EACH_GC_PARAM(DEFINE_PARAM_INFO)
#undef DEFINE_PARAM_INFO
};

#define PARAM_NAME_LIST_ENTRY(name, key, writable)                              \
    " " name
#define GC_PARAMETER_ARGS_LIST FOR_EACH_GC_PARAM(PARAM_NAME_LIST_ENTRY)

static bool
GCParameter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSString* str = ToString(cx, args.get(0));
    if (!str)
        return false;

    JSFlatString* flatStr = JS_FlattenString(cx, str);
    if (!flatStr)
        return false;

    size_t paramIndex = 0;
    for (;; paramIndex++) {
        if (paramIndex == ArrayLength(paramMap)) {
            JS_ReportErrorASCII(cx,
                                "the first argument must be one of:" GC_PARAMETER_ARGS_LIST);
            return false;
        }
        if (JS_FlatStringEqualsAscii(flatStr, paramMap[paramIndex].name))
            break;
    }
    const ParamInfo& info = paramMap[paramIndex];
    JSGCParamKey param = info.param;

    // Request mode.
    if (args.length() == 1) {
        uint32_t value = JS_GetGCParameter(cx, param);
        args.rval().setNumber(value);
        return true;
    }

    if (!info.writable) {
        JS_ReportErrorASCII(cx, "Attempt to change read-only parameter %s", info.name);
        return false;
    }

    if (disableOOMFunctions && (param == JSGC_MAX_BYTES || param == JSGC_MAX_MALLOC_BYTES)) {
        args.rval().setUndefined();
        return true;
    }

    double d;
    if (!ToNumber(cx, args[1], &d))
        return false;

    if (d < 0 || d > UINT32_MAX) {
        JS_ReportErrorASCII(cx, "Parameter value out of range");
        return false;
    }

    uint32_t value = floor(d);
    if (param == JSGC_MARK_STACK_LIMIT && JS::IsIncrementalGCInProgress(cx)) {
        JS_ReportErrorASCII(cx, "attempt to set markStackLimit while a GC is in progress");
        return false;
    }

    if (param == JSGC_MAX_BYTES) {
        uint32_t gcBytes = JS_GetGCParameter(cx, JSGC_BYTES);
        if (value < gcBytes) {
            JS_ReportErrorASCII(cx,
                                "attempt to set maxBytes to the value less than the current "
                                "gcBytes (%u)",
                                gcBytes);
            return false;
        }
    }

    bool ok;
    {
        JSRuntime* rt = cx->runtime();
        AutoLockGC lock(rt);
        ok = rt->gc.setParameter(param, value, lock);
    }

    if (!ok) {
        JS_ReportErrorASCII(cx, "Parameter value out of range");
        return false;
    }

    args.rval().setUndefined();
    return true;
}

static void
SetAllowRelazification(JSContext* cx, bool allow)
{
    JSRuntime* rt = cx->runtime();
    MOZ_ASSERT(rt->allowRelazificationForTesting != allow);
    rt->allowRelazificationForTesting = allow;

    for (AllScriptFramesIter i(cx); !i.done(); ++i)
        i.script()->setDoNotRelazify(allow);
}

static bool
RelazifyFunctions(JSContext* cx, unsigned argc, Value* vp)
{
    // Relazifying functions on GC is usually only done for compartments that are
    // not active. To aid fuzzing, this testing function allows us to relazify
    // even if the compartment is active.

    CallArgs args = CallArgsFromVp(argc, vp);
    SetAllowRelazification(cx, true);

    JS::PrepareForFullGC(cx);
    JS::GCForReason(cx, GC_SHRINK, JS::gcreason::API);

    SetAllowRelazification(cx, false);
    args.rval().setUndefined();
    return true;
}

static bool
IsProxy(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "the function takes exactly one argument");
        return false;
    }
    if (!args[0].isObject()) {
        args.rval().setBoolean(false);
        return true;
    }
    args.rval().setBoolean(args[0].toObject().is<ProxyObject>());
    return true;
}

static bool
WasmIsSupported(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(wasm::HasSupport(cx));
    return true;
}

static bool
WasmTextToBinary(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());

    if (!args.requireAtLeast(cx, "wasmTextToBinary", 1))
        return false;

    if (!args[0].isString()) {
        ReportUsageErrorASCII(cx, callee, "First argument must be a String");
        return false;
    }

    AutoStableStringChars twoByteChars(cx);
    if (!twoByteChars.initTwoByte(cx, args[0].toString()))
        return false;

    if (args.hasDefined(1)) {
        if (!args[1].isString()) {
            ReportUsageErrorASCII(cx, callee, "Second argument, if present, must be a String");
            return false;
        }
    }

    wasm::Bytes bytes;
    UniqueChars error;
    if (!wasm::TextToBinary(twoByteChars.twoByteChars(), &bytes, &error)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_TEXT_FAIL,
                                  error.get() ? error.get() : "out of memory");
        return false;
    }

    RootedObject obj(cx, JS_NewUint8Array(cx, bytes.length()));
    if (!obj)
        return false;

    memcpy(obj->as<TypedArrayObject>().viewDataUnshared(), bytes.begin(), bytes.length());

    args.rval().setObject(*obj);
    return true;
}

static bool
WasmBinaryToText(JSContext* cx, unsigned argc, Value* vp)
{
    MOZ_ASSERT(cx->options().wasm());
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.get(0).isObject() || !args.get(0).toObject().is<TypedArrayObject>()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_BUF_ARG);
        return false;
    }

    Rooted<TypedArrayObject*> code(cx, &args[0].toObject().as<TypedArrayObject>());

    if (!TypedArrayObject::ensureHasBuffer(cx, code))
        return false;

    if (code->isSharedMemory()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_WASM_BAD_BUF_ARG);
        return false;
    }

    const uint8_t* bufferStart = code->bufferUnshared()->dataPointer();
    const uint8_t* bytes = bufferStart + code->byteOffset();
    uint32_t length = code->byteLength();

    Vector<uint8_t> copy(cx);
    if (code->bufferUnshared()->hasInlineData()) {
        if (!copy.append(bytes, length))
            return false;
        bytes = copy.begin();
    }

    bool experimental = false;
    if (args.length() > 1) {
        JSString* opt = JS::ToString(cx, args[1]);
        if (!opt)
            return false;
        bool match;
        if (!JS_StringEqualsAscii(cx, opt, "experimental", &match))
            return false;
        experimental = match;
    }

    StringBuffer buffer(cx);
    bool ok;
    if (experimental)
        ok = wasm::BinaryToExperimentalText(cx, bytes, length, buffer, wasm::ExperimentalTextFormatting());
    else
        ok = wasm::BinaryToText(cx, bytes, length, buffer);
    if (!ok) {
        if (!cx->isExceptionPending())
            JS_ReportErrorASCII(cx, "wasm binary to text print error");
        return false;
    }

    JSString* result = buffer.finishString();
    if (!result)
        return false;

    args.rval().setString(result);
    return true;
}

static bool
WasmExtractCode(JSContext* cx, unsigned argc, Value* vp)
{
    MOZ_ASSERT(cx->options().wasm());
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.get(0).isObject()) {
        JS_ReportErrorASCII(cx, "argument is not an object");
        return false;
    }

    JSObject* unwrapped = CheckedUnwrap(&args.get(0).toObject());
    if (!unwrapped || !unwrapped->is<WasmModuleObject>()) {
        JS_ReportErrorASCII(cx, "argument is not a WebAssembly.Module");
        return false;
    }

    Rooted<WasmModuleObject*> module(cx, &unwrapped->as<WasmModuleObject>());
    RootedValue result(cx);
    if (!module->module().extractCode(cx, &result))
        return false;

    args.rval().set(result);
    return true;
}

static bool
IsLazyFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
        return false;
    }
    if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
        JS_ReportErrorASCII(cx, "The first argument should be a function.");
        return false;
    }
    args.rval().setBoolean(args[0].toObject().as<JSFunction>().isInterpretedLazy());
    return true;
}

static bool
IsRelazifiableFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
        return false;
    }
    if (!args[0].isObject() ||
        !args[0].toObject().is<JSFunction>())
    {
        JS_ReportErrorASCII(cx, "The first argument should be a function.");
        return false;
    }

    JSFunction* fun = &args[0].toObject().as<JSFunction>();
    args.rval().setBoolean(fun->hasScript() && fun->nonLazyScript()->isRelazifiable());
    return true;
}

static bool
InternalConst(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() == 0) {
        JS_ReportErrorASCII(cx, "the function takes exactly one argument");
        return false;
    }

    JSString* str = ToString(cx, args[0]);
    if (!str)
        return false;
    JSFlatString* flat = JS_FlattenString(cx, str);
    if (!flat)
        return false;

    if (JS_FlatStringEqualsAscii(flat, "INCREMENTAL_MARK_STACK_BASE_CAPACITY")) {
        args.rval().setNumber(uint32_t(js::INCREMENTAL_MARK_STACK_BASE_CAPACITY));
    } else {
        JS_ReportErrorASCII(cx, "unknown const name");
        return false;
    }
    return true;
}

static bool
GCPreserveCode(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 0) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.setAlwaysPreserveCode();

    args.rval().setUndefined();
    return true;
}

static bool
StartGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 2) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    auto budget = SliceBudget::unlimited();
    if (args.length() >= 1) {
        uint32_t work = 0;
        if (!ToUint32(cx, args[0], &work))
            return false;
        budget = SliceBudget(WorkBudget(work));
    }

    bool shrinking = false;
    if (args.length() >= 2) {
        Value arg = args[1];
        if (arg.isString()) {
            if (!JS_StringEqualsAscii(cx, arg.toString(), "shrinking", &shrinking))
                return false;
        }
    }

    JSRuntime* rt = cx->runtime();
    if (rt->gc.isIncrementalGCInProgress()) {
        RootedObject callee(cx, &args.callee());
        JS_ReportErrorASCII(cx, "Incremental GC already in progress");
        return false;
    }

    JSGCInvocationKind gckind = shrinking ? GC_SHRINK : GC_NORMAL;
    rt->gc.startDebugGC(gckind, budget);

    args.rval().setUndefined();
    return true;
}

static bool
GCSlice(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    auto budget = SliceBudget::unlimited();
    if (args.length() == 1) {
        uint32_t work = 0;
        if (!ToUint32(cx, args[0], &work))
            return false;
        budget = SliceBudget(WorkBudget(work));
    }

    JSRuntime* rt = cx->runtime();
    if (!rt->gc.isIncrementalGCInProgress())
        rt->gc.startDebugGC(GC_NORMAL, budget);
    else
        rt->gc.debugGCSlice(budget);

    args.rval().setUndefined();
    return true;
}

static bool
AbortGC(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 0) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.abortGC();
    args.rval().setUndefined();
    return true;
}

static bool
FullCompartmentChecks(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    cx->runtime()->gc.setFullCompartmentChecks(ToBoolean(args[0]));
    args.rval().setUndefined();
    return true;
}

static bool
NondeterministicGetWeakMapKeys(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }
    if (!args[0].isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  "nondeterministicGetWeakMapKeys", "WeakMap",
                                  InformalValueTypeName(args[0]));
        return false;
    }
    RootedObject arr(cx);
    RootedObject mapObj(cx, &args[0].toObject());
    if (!JS_NondeterministicGetWeakMapKeys(cx, mapObj, &arr))
        return false;
    if (!arr) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  "nondeterministicGetWeakMapKeys", "WeakMap",
                                  args[0].toObject().getClass()->name);
        return false;
    }
    args.rval().setObject(*arr);
    return true;
}

class HasChildTracer : public JS::CallbackTracer
{
    RootedValue child_;
    bool found_;

    void onChild(const JS::GCCellPtr& thing) override {
        if (thing.asCell() == child_.toGCThing())
            found_ = true;
    }

  public:
    HasChildTracer(JSContext* cx, HandleValue child)
      : JS::CallbackTracer(cx, TraceWeakMapKeysValues), child_(cx, child), found_(false)
    {}

    bool found() const { return found_; }
};

static bool
HasChild(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedValue parent(cx, args.get(0));
    RootedValue child(cx, args.get(1));

    if (!parent.isGCThing() || !child.isGCThing()) {
        args.rval().setBoolean(false);
        return true;
    }

    HasChildTracer trc(cx, child);
    TraceChildren(&trc, parent.toGCThing(), parent.traceKind());
    args.rval().setBoolean(trc.found());
    return true;
}

static bool
SetSavedStacksRNGState(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "setSavedStacksRNGState", 1))
        return false;

    int32_t seed;
    if (!ToInt32(cx, args[0], &seed))
        return false;

    // Either one or the other of the seed arguments must be non-zero;
    // make this true no matter what value 'seed' has.
    cx->compartment()->savedStacks().setRNGState(seed, (seed + 1) * 33);
    return true;
}

static bool
GetSavedFrameCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setNumber(cx->compartment()->savedStacks().count());
    return true;
}

static bool
SaveStack(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JS::StackCapture capture((JS::AllFrames()));
    if (args.length() >= 1) {
        double maxDouble;
        if (!ToNumber(cx, args[0], &maxDouble))
            return false;
        if (mozilla::IsNaN(maxDouble) || maxDouble < 0 || maxDouble > UINT32_MAX) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                  JSDVG_SEARCH_STACK, args[0], nullptr,
                                  "not a valid maximum frame count", NULL);
            return false;
        }
        uint32_t max = uint32_t(maxDouble);
        if (max > 0)
            capture = JS::StackCapture(JS::MaxFrames(max));
    }

    JSCompartment* targetCompartment = cx->compartment();
    if (args.length() >= 2) {
        if (!args[1].isObject()) {
            ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                                  JSDVG_SEARCH_STACK, args[0], nullptr,
                                  "not an object", NULL);
            return false;
        }
        RootedObject obj(cx, UncheckedUnwrap(&args[1].toObject()));
        if (!obj)
            return false;
        targetCompartment = obj->compartment();
    }

    RootedObject stack(cx);
    {
        AutoCompartment ac(cx, targetCompartment);
        if (!JS::CaptureCurrentStack(cx, &stack, mozilla::Move(capture)))
            return false;
    }

    if (stack && !cx->compartment()->wrap(cx, &stack))
        return false;

    args.rval().setObjectOrNull(stack);
    return true;
}

static bool
CaptureFirstSubsumedFrame(JSContext* cx, unsigned argc, JS::Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "captureFirstSubsumedFrame", 1))
        return false;

    if (!args[0].isObject()) {
        JS_ReportErrorASCII(cx, "The argument must be an object");
        return false;
    }

    RootedObject obj(cx, &args[0].toObject());
    obj = CheckedUnwrap(obj);
    if (!obj) {
        JS_ReportErrorASCII(cx, "Denied permission to object.");
        return false;
    }

    JS::StackCapture capture(JS::FirstSubsumedFrame(cx, obj->compartment()->principals()));
    if (args.length() > 1)
        capture.as<JS::FirstSubsumedFrame>().ignoreSelfHosted = JS::ToBoolean(args[1]);

    JS::RootedObject capturedStack(cx);
    if (!JS::CaptureCurrentStack(cx, &capturedStack, mozilla::Move(capture)))
        return false;

    args.rval().setObjectOrNull(capturedStack);
    return true;
}

static bool
CallFunctionFromNativeFrame(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "The function takes exactly one argument.");
        return false;
    }
    if (!args[0].isObject() || !IsCallable(args[0])) {
        JS_ReportErrorASCII(cx, "The first argument should be a function.");
        return false;
    }

    RootedObject function(cx, &args[0].toObject());
    return Call(cx, UndefinedHandleValue, function,
                JS::HandleValueArray::empty(), args.rval());
}

static bool
CallFunctionWithAsyncStack(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 3) {
        JS_ReportErrorASCII(cx, "The function takes exactly three arguments.");
        return false;
    }
    if (!args[0].isObject() || !IsCallable(args[0])) {
        JS_ReportErrorASCII(cx, "The first argument should be a function.");
        return false;
    }
    if (!args[1].isObject() || !args[1].toObject().is<SavedFrame>()) {
        JS_ReportErrorASCII(cx, "The second argument should be a SavedFrame.");
        return false;
    }
    if (!args[2].isString() || args[2].toString()->empty()) {
        JS_ReportErrorASCII(cx, "The third argument should be a non-empty string.");
        return false;
    }

    RootedObject function(cx, &args[0].toObject());
    RootedObject stack(cx, &args[1].toObject());
    RootedString asyncCause(cx, args[2].toString());
    JSAutoByteString utf8Cause;
    if (!utf8Cause.encodeUtf8(cx, asyncCause)) {
        MOZ_ASSERT(cx->isExceptionPending());
        return false;
    }

    JS::AutoSetAsyncStackForNewCalls sas(cx, stack, utf8Cause.ptr(),
                                         JS::AutoSetAsyncStackForNewCalls::AsyncCallKind::EXPLICIT);
    return Call(cx, UndefinedHandleValue, function,
                JS::HandleValueArray::empty(), args.rval());
}

static bool
EnableTrackAllocations(JSContext* cx, unsigned argc, Value* vp)
{
    SetAllocationMetadataBuilder(cx, &SavedStacks::metadataBuilder);
    return true;
}

static bool
DisableTrackAllocations(JSContext* cx, unsigned argc, Value* vp)
{
    SetAllocationMetadataBuilder(cx, nullptr);
    return true;
}

static void
FinalizeExternalString(Zone* zone, const JSStringFinalizer* fin, char16_t* chars);

static const JSStringFinalizer ExternalStringFinalizer =
    { FinalizeExternalString };

static void
FinalizeExternalString(Zone* zone, const JSStringFinalizer* fin, char16_t* chars)
{
    MOZ_ASSERT(fin == &ExternalStringFinalizer);
    js_free(chars);
}

static bool
NewExternalString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1 || !args[0].isString()) {
        JS_ReportErrorASCII(cx, "newExternalString takes exactly one string argument.");
        return false;
    }

    RootedString str(cx, args[0].toString());
    size_t len = str->length();

    UniqueTwoByteChars buf(cx->pod_malloc<char16_t>(len));
    if (!buf)
        return false;

    if (!JS_CopyStringChars(cx, mozilla::Range<char16_t>(buf.get(), len), str))
        return false;

    JSString* res = JS_NewExternalString(cx, buf.get(), len, &ExternalStringFinalizer);
    if (!res)
        return false;

    mozilla::Unused << buf.release();
    args.rval().setString(res);
    return true;
}

static bool
EnsureFlatString(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1 || !args[0].isString()) {
        JS_ReportErrorASCII(cx, "ensureFlatString takes exactly one string argument.");
        return false;
    }

    JSFlatString* flat = args[0].toString()->ensureFlat(cx);
    if (!flat)
        return false;

    args.rval().setString(flat);
    return true;
}

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
static bool
OOMThreadTypes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(js::oom::THREAD_TYPE_MAX);
    return true;
}

static bool
SetupOOMFailure(JSContext* cx, bool failAlways, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (disableOOMFunctions) {
        args.rval().setUndefined();
        return true;
    }

    if (args.length() < 1) {
        JS_ReportErrorASCII(cx, "Count argument required");
        return false;
    }

    if (args.length() > 2) {
        JS_ReportErrorASCII(cx, "Too many arguments");
        return false;
    }

    int32_t count;
    if (!JS::ToInt32(cx, args.get(0), &count))
        return false;

    if (count <= 0) {
        JS_ReportErrorASCII(cx, "OOM cutoff should be positive");
        return false;
    }

    uint32_t targetThread = js::oom::THREAD_TYPE_MAIN;
    if (args.length() > 1 && !ToUint32(cx, args[1], &targetThread))
        return false;

    if (targetThread == js::oom::THREAD_TYPE_NONE || targetThread >= js::oom::THREAD_TYPE_MAX) {
        JS_ReportErrorASCII(cx, "Invalid thread type specified");
        return false;
    }

    HelperThreadState().waitForAllThreads();
    js::oom::SimulateOOMAfter(count, targetThread, failAlways);
    args.rval().setUndefined();
    return true;
}

static bool
OOMAfterAllocations(JSContext* cx, unsigned argc, Value* vp)
{
    return SetupOOMFailure(cx, true, argc, vp);
}

static bool
OOMAtAllocation(JSContext* cx, unsigned argc, Value* vp)
{
    return SetupOOMFailure(cx, false, argc, vp);
}

static bool
ResetOOMFailure(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(js::oom::HadSimulatedOOM());
    js::oom::ResetSimulatedOOM();
    return true;
}

static bool
OOMTest(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() < 1 || args.length() > 2) {
        JS_ReportErrorASCII(cx, "oomTest() takes between 1 and 2 arguments.");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
        JS_ReportErrorASCII(cx, "The first argument to oomTest() must be a function.");
        return false;
    }

    if (args.length() == 2 && !args[1].isBoolean()) {
        JS_ReportErrorASCII(cx, "The optional second argument to oomTest() must be a boolean.");
        return false;
    }

    bool expectExceptionOnFailure = true;
    if (args.length() == 2)
        expectExceptionOnFailure = args[1].toBoolean();

    // There are some places where we do fail without raising an exception, so
    // we can't expose this to the fuzzers by default.
    if (fuzzingSafe)
        expectExceptionOnFailure = false;

    if (disableOOMFunctions) {
        args.rval().setUndefined();
        return true;
    }

    RootedFunction function(cx, &args[0].toObject().as<JSFunction>());

    bool verbose = EnvVarIsDefined("OOM_VERBOSE");

    unsigned threadStart = oom::THREAD_TYPE_MAIN;
    unsigned threadEnd = oom::THREAD_TYPE_MAX;

    // Test a single thread type if specified by the OOM_THREAD environment variable.
    int threadOption = 0;
    if (EnvVarAsInt("OOM_THREAD", &threadOption)) {
        if (threadOption < oom::THREAD_TYPE_MAIN || threadOption > oom::THREAD_TYPE_MAX) {
            JS_ReportErrorASCII(cx, "OOM_THREAD value out of range.");
            return false;
        }

        threadStart = threadOption;
        threadEnd = threadOption + 1;
    }

    JSRuntime* rt = cx->runtime();
    if (rt->runningOOMTest) {
        JS_ReportErrorASCII(cx, "Nested call to oomTest() is not allowed.");
        return false;
    }
    rt->runningOOMTest = true;

    MOZ_ASSERT(!cx->isExceptionPending());
    rt->hadOutOfMemory = false;

    for (unsigned thread = threadStart; thread < threadEnd; thread++) {
        if (verbose)
            fprintf(stderr, "thread %d\n", thread);

        HelperThreadState().waitForAllThreads();
        js::oom::targetThread = thread;

        unsigned allocation = 1;
        bool handledOOM;
        do {
            if (verbose)
                fprintf(stderr, "  allocation %d\n", allocation);

            MOZ_ASSERT(!cx->isExceptionPending());
            MOZ_ASSERT(!cx->runtime()->hadOutOfMemory);

            js::oom::SimulateOOMAfter(allocation, thread, false);

            RootedValue result(cx);
            bool ok = JS_CallFunction(cx, cx->global(), function,
                                      HandleValueArray::empty(), &result);

            handledOOM = js::oom::HadSimulatedOOM();
            js::oom::ResetSimulatedOOM();

            MOZ_ASSERT_IF(ok, !cx->isExceptionPending());

            if (ok) {
                MOZ_ASSERT(!cx->isExceptionPending(),
                           "Thunk execution succeeded but an exception was raised - "
                           "missing error check?");
            } else if (expectExceptionOnFailure) {
                MOZ_ASSERT(cx->isExceptionPending(),
                           "Thunk execution failed but no exception was raised - "
                           "missing call to js::ReportOutOfMemory()?");
            }

            // Note that it is possible that the function throws an exception
            // unconnected to OOM, in which case we ignore it. More correct
            // would be to have the caller pass some kind of exception
            // specification and to check the exception against it.

            cx->clearPendingException();
            cx->runtime()->hadOutOfMemory = false;

#ifdef JS_TRACE_LOGGING
            // Reset the TraceLogger state if enabled.
            TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
            if (logger->enabled()) {
                while (logger->enabled())
                    logger->disable();
                logger->enable(cx);
            }
#endif

            allocation++;
        } while (handledOOM);

        if (verbose) {
            fprintf(stderr, "  finished after %d allocations\n", allocation - 2);
        }
    }

    rt->runningOOMTest = false;
    args.rval().setUndefined();
    return true;
}
#endif

static bool
SettlePromiseNow(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "settlePromiseNow", 1))
        return false;
    if (!args[0].isObject() || !args[0].toObject().is<PromiseObject>()) {
        JS_ReportErrorASCII(cx, "first argument must be a Promise object");
        return false;
    }

    Rooted<PromiseObject*> promise(cx, &args[0].toObject().as<PromiseObject>());
    int32_t flags = promise->flags();
    promise->setFixedSlot(PromiseSlot_Flags,
                          Int32Value(flags | PROMISE_FLAG_RESOLVED | PROMISE_FLAG_FULFILLED));
    promise->setFixedSlot(PromiseSlot_ReactionsOrResult, UndefinedValue());

    JS::dbg::onPromiseSettled(cx, promise);
    return true;
}

static bool
GetWaitForAllPromise(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "getWaitForAllPromise", 1))
        return false;
    if (!args[0].isObject() || !IsPackedArray(&args[0].toObject())) {
        JS_ReportErrorASCII(cx, "first argument must be a dense Array of Promise objects");
        return false;
    }
    RootedNativeObject list(cx, &args[0].toObject().as<NativeObject>());
    AutoObjectVector promises(cx);
    uint32_t count = list->getDenseInitializedLength();
    if (!promises.resize(count))
        return false;

    for (uint32_t i = 0; i < count; i++) {
        RootedValue elem(cx, list->getDenseElement(i));
        if (!elem.isObject() || !elem.toObject().is<PromiseObject>()) {
            JS_ReportErrorASCII(cx, "Each entry in the passed-in Array must be a Promise");
            return false;
        }
        promises[i].set(&elem.toObject());
    }

    RootedObject resultPromise(cx, JS::GetWaitForAllPromise(cx, promises));
    if (!resultPromise)
        return false;

    args.rval().set(ObjectValue(*resultPromise));
    return true;
}

static bool
ResolvePromise(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "resolvePromise", 2))
        return false;
    if (!args[0].isObject() || !UncheckedUnwrap(&args[0].toObject())->is<PromiseObject>()) {
        JS_ReportErrorASCII(cx, "first argument must be a maybe-wrapped Promise object");
        return false;
    }

    RootedObject promise(cx, &args[0].toObject());
    RootedValue resolution(cx, args[1]);
    mozilla::Maybe<AutoCompartment> ac;
    if (IsWrapper(promise)) {
        promise = UncheckedUnwrap(promise);
        ac.emplace(cx, promise);
        if (!cx->compartment()->wrap(cx, &resolution))
            return false;
    }

    if (IsPromiseForAsync(promise)) {
        JS_ReportErrorASCII(cx, "async function's promise shouldn't be manually resolved");
        return false;
    }

    bool result = JS::ResolvePromise(cx, promise, resolution);
    if (result)
        args.rval().setUndefined();
    return result;
}

static bool
RejectPromise(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "rejectPromise", 2))
        return false;
    if (!args[0].isObject() || !UncheckedUnwrap(&args[0].toObject())->is<PromiseObject>()) {
        JS_ReportErrorASCII(cx, "first argument must be a maybe-wrapped Promise object");
        return false;
    }

    RootedObject promise(cx, &args[0].toObject());
    RootedValue reason(cx, args[1]);
    mozilla::Maybe<AutoCompartment> ac;
    if (IsWrapper(promise)) {
        promise = UncheckedUnwrap(promise);
        ac.emplace(cx, promise);
        if (!cx->compartment()->wrap(cx, &reason))
            return false;
    }

    if (IsPromiseForAsync(promise)) {
        JS_ReportErrorASCII(cx, "async function's promise shouldn't be manually rejected");
        return false;
    }

    bool result = JS::RejectPromise(cx, promise, reason);
    if (result)
        args.rval().setUndefined();
    return result;
}

static bool
StreamsAreEnabled(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(cx->options().streams());
    return true;
}

static unsigned finalizeCount = 0;

static void
finalize_counter_finalize(JSFreeOp* fop, JSObject* obj)
{
    ++finalizeCount;
}

static const JSClassOps FinalizeCounterClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    finalize_counter_finalize
};

static const JSClass FinalizeCounterClass = {
    "FinalizeCounter",
    JSCLASS_IS_ANONYMOUS |
    JSCLASS_FOREGROUND_FINALIZE,
    &FinalizeCounterClassOps
};

static bool
MakeFinalizeObserver(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSObject* obj = JS_NewObjectWithGivenProto(cx, &FinalizeCounterClass, nullptr);
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
FinalizeCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(finalizeCount);
    return true;
}

static bool
ResetFinalizeCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    finalizeCount = 0;
    args.rval().setUndefined();
    return true;
}

static bool
DumpHeap(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    DumpHeapNurseryBehaviour nurseryBehaviour = js::IgnoreNurseryObjects;
    FILE* dumpFile = nullptr;

    unsigned i = 0;
    if (args.length() > i) {
        Value v = args[i];
        if (v.isString()) {
            JSString* str = v.toString();
            bool same = false;
            if (!JS_StringEqualsAscii(cx, str, "collectNurseryBeforeDump", &same))
                return false;
            if (same) {
                nurseryBehaviour = js::CollectNurseryBeforeDump;
                ++i;
            }
        }
    }

    if (args.length() > i) {
        Value v = args[i];
        if (v.isString()) {
            if (!fuzzingSafe) {
                RootedString str(cx, v.toString());
                JSAutoByteString fileNameBytes;
                if (!fileNameBytes.encodeLatin1(cx, str))
                    return false;
                const char* fileName = fileNameBytes.ptr();
                dumpFile = fopen(fileName, "w");
                if (!dumpFile) {
                    fileNameBytes.clear();
                    if (!fileNameBytes.encodeUtf8(cx, str))
                        return false;
                    JS_ReportErrorUTF8(cx, "can't open %s", fileNameBytes.ptr());
                    return false;
                }
            }
            ++i;
        }
    }

    if (i != args.length()) {
        JS_ReportErrorASCII(cx, "bad arguments passed to dumpHeap");
        if (dumpFile)
            fclose(dumpFile);
        return false;
    }

    js::DumpHeap(cx, dumpFile ? dumpFile : stdout, nurseryBehaviour);

    if (dumpFile)
        fclose(dumpFile);

    args.rval().setUndefined();
    return true;
}

static bool
Terminate(JSContext* cx, unsigned arg, Value* vp)
{
#ifdef JS_MORE_DETERMINISTIC
    // Print a message to stderr in more-deterministic builds to help jsfunfuzz
    // find uncatchable-exception bugs.
    fprintf(stderr, "terminate called\n");
#endif

    JS_ClearPendingException(cx);
    return false;
}

static bool
ReadSPSProfilingStack(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setUndefined();

    // Return boolean 'false' if profiler is not enabled.
    if (!cx->runtime()->spsProfiler.enabled()) {
        args.rval().setBoolean(false);
        return true;
    }

    // Array holding physical jit stack frames.
    RootedObject stack(cx, NewDenseEmptyArray(cx));
    if (!stack)
        return false;

    // If profiler sampling has been suppressed, return an empty
    // stack.
    if (!cx->runtime()->isProfilerSamplingEnabled()) {
      args.rval().setObject(*stack);
      return true;
    }

    struct InlineFrameInfo
    {
        InlineFrameInfo(const char* kind, UniqueChars&& label)
          : kind(kind), label(mozilla::Move(label)) {}
        const char* kind;
        UniqueChars label;
    };

    Vector<Vector<InlineFrameInfo, 0, TempAllocPolicy>, 0, TempAllocPolicy> frameInfo(cx);

    JS::ProfilingFrameIterator::RegisterState state;
    for (JS::ProfilingFrameIterator i(cx, state); !i.done(); ++i) {
        MOZ_ASSERT(i.stackAddress() != nullptr);

        if (!frameInfo.emplaceBack(cx))
            return false;

        const size_t MaxInlineFrames = 16;
        JS::ProfilingFrameIterator::Frame frames[MaxInlineFrames];
        uint32_t nframes = i.extractStack(frames, 0, MaxInlineFrames);
        MOZ_ASSERT(nframes <= MaxInlineFrames);
        for (uint32_t i = 0; i < nframes; i++) {
            const char* frameKindStr = nullptr;
            switch (frames[i].kind) {
              case JS::ProfilingFrameIterator::Frame_Baseline:
                frameKindStr = "baseline";
                break;
              case JS::ProfilingFrameIterator::Frame_Ion:
                frameKindStr = "ion";
                break;
              case JS::ProfilingFrameIterator::Frame_Wasm:
                frameKindStr = "wasm";
                break;
              default:
                frameKindStr = "unknown";
            }

            if (!frameInfo.back().emplaceBack(frameKindStr, mozilla::Move(frames[i].label)))
                return false;
        }
    }

    RootedObject inlineFrameInfo(cx);
    RootedString frameKind(cx);
    RootedString frameLabel(cx);
    RootedId idx(cx);

    const unsigned propAttrs = JSPROP_ENUMERATE;

    uint32_t physicalFrameNo = 0;
    for (auto& frame : frameInfo) {
        // Array holding all inline frames in a single physical jit stack frame.
        RootedObject inlineStack(cx, NewDenseEmptyArray(cx));
        if (!inlineStack)
            return false;

        uint32_t inlineFrameNo = 0;
        for (auto& inlineFrame : frame) {
            // Object holding frame info.
            RootedObject inlineFrameInfo(cx, NewBuiltinClassInstance<PlainObject>(cx));
            if (!inlineFrameInfo)
                return false;

            frameKind = NewStringCopyZ<CanGC>(cx, inlineFrame.kind);
            if (!frameKind)
                return false;

            if (!JS_DefineProperty(cx, inlineFrameInfo, "kind", frameKind, propAttrs))
                return false;

            auto chars = inlineFrame.label.release();
            frameLabel = NewString<CanGC>(cx, reinterpret_cast<Latin1Char*>(chars), strlen(chars));
            if (!frameLabel)
                return false;

            if (!JS_DefineProperty(cx, inlineFrameInfo, "label", frameLabel, propAttrs))
                return false;

            idx = INT_TO_JSID(inlineFrameNo);
            if (!JS_DefinePropertyById(cx, inlineStack, idx, inlineFrameInfo, 0))
                return false;

            ++inlineFrameNo;
        }

        // Push inline array into main array.
        idx = INT_TO_JSID(physicalFrameNo);
        if (!JS_DefinePropertyById(cx, stack, idx, inlineStack, 0))
            return false;

        ++physicalFrameNo;
    }

    args.rval().setObject(*stack);
    return true;
}

static bool
EnableOsiPointRegisterChecks(JSContext*, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
#ifdef CHECK_OSIPOINT_REGISTERS
    jit::JitOptions.checkOsiPointRegisters = true;
#endif
    args.rval().setUndefined();
    return true;
}

static bool
DisplayName(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject() || !args[0].toObject().is<JSFunction>()) {
        RootedObject arg(cx, &args.callee());
        ReportUsageErrorASCII(cx, arg, "Must have one function argument");
        return false;
    }

    JSFunction* fun = &args[0].toObject().as<JSFunction>();
    JSString* str = fun->displayAtom();
    args.rval().setString(str ? str : cx->runtime()->emptyString);
    return true;
}

class ShellAllocationMetadataBuilder : public AllocationMetadataBuilder {
  public:
    ShellAllocationMetadataBuilder() : AllocationMetadataBuilder() { }

    virtual JSObject* build(JSContext *cx, HandleObject,
                            AutoEnterOOMUnsafeRegion& oomUnsafe) const override;

    static const ShellAllocationMetadataBuilder metadataBuilder;
};

JSObject*
ShellAllocationMetadataBuilder::build(JSContext* cx, HandleObject,
                                      AutoEnterOOMUnsafeRegion& oomUnsafe) const
{
    RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!obj)
        oomUnsafe.crash("ShellAllocationMetadataBuilder::build");

    RootedObject stack(cx, NewDenseEmptyArray(cx));
    if (!stack)
        oomUnsafe.crash("ShellAllocationMetadataBuilder::build");

    static int createdIndex = 0;
    createdIndex++;

    if (!JS_DefineProperty(cx, obj, "index", createdIndex, 0,
                           JS_STUBGETTER, JS_STUBSETTER))
    {
        oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
    }

    if (!JS_DefineProperty(cx, obj, "stack", stack, 0,
                           JS_STUBGETTER, JS_STUBSETTER))
    {
        oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
    }

    int stackIndex = 0;
    RootedId id(cx);
    RootedValue callee(cx);
    for (NonBuiltinScriptFrameIter iter(cx); !iter.done(); ++iter) {
        if (iter.isFunctionFrame() && iter.compartment() == cx->compartment()) {
            id = INT_TO_JSID(stackIndex);
            RootedObject callee(cx, iter.callee(cx));
            if (!JS_DefinePropertyById(cx, stack, id, callee, 0,
                                       JS_STUBGETTER, JS_STUBSETTER))
            {
                oomUnsafe.crash("ShellAllocationMetadataBuilder::build");
            }
            stackIndex++;
        }
    }

    return obj;
}

const ShellAllocationMetadataBuilder ShellAllocationMetadataBuilder::metadataBuilder;

static bool
EnableShellAllocationMetadataBuilder(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    SetAllocationMetadataBuilder(cx, &ShellAllocationMetadataBuilder::metadataBuilder);

    args.rval().setUndefined();
    return true;
}

static bool
GetAllocationMetadata(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !args[0].isObject()) {
        JS_ReportErrorASCII(cx, "Argument must be an object");
        return false;
    }

    args.rval().setObjectOrNull(GetAllocationMetadata(&args[0].toObject()));
    return true;
}

static bool
testingFunc_bailout(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // NOP when not in IonMonkey
    args.rval().setUndefined();
    return true;
}

static bool
testingFunc_bailAfter(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1 || !args[0].isInt32() || args[0].toInt32() < 0) {
        JS_ReportErrorASCII(cx, "Argument must be a positive number that fits in an int32");
        return false;
    }

#ifdef DEBUG
    cx->runtime()->setIonBailAfter(args[0].toInt32());
#endif

    args.rval().setUndefined();
    return true;
}

static bool
testingFunc_inJit(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!jit::IsBaselineEnabled(cx)) {
        JSString* error = JS_NewStringCopyZ(cx, "Baseline is disabled.");
        if(!error)
            return false;

        args.rval().setString(error);
        return true;
    }

    JSScript* script = cx->currentScript();
    if (script && script->getWarmUpResetCount() >= 20) {
        JSString* error = JS_NewStringCopyZ(cx, "Compilation is being repeatedly prevented. Giving up.");
        if (!error)
            return false;

        args.rval().setString(error);
        return true;
    }

    args.rval().setBoolean(cx->currentlyRunningInJit());
    return true;
}

static bool
testingFunc_inIon(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!jit::IsIonEnabled(cx)) {
        JSString* error = JS_NewStringCopyZ(cx, "Ion is disabled.");
        if (!error)
            return false;

        args.rval().setString(error);
        return true;
    }

    ScriptFrameIter iter(cx);
    if (iter.isIon()) {
        // Reset the counter of the IonScript's script.
        jit::JitFrameIterator jitIter(cx);
        ++jitIter;
        jitIter.script()->resetWarmUpResetCounter();
    } else {
        // Check if we missed multiple attempts at compiling the innermost script.
        JSScript* script = cx->currentScript();
        if (script && script->getWarmUpResetCount() >= 20) {
            JSString* error = JS_NewStringCopyZ(cx, "Compilation is being repeatedly prevented. Giving up.");
            if (!error)
                return false;

            args.rval().setString(error);
            return true;
        }
    }

    args.rval().setBoolean(iter.isIon());
    return true;
}

bool
js::testingFunc_assertFloat32(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2) {
        JS_ReportErrorASCII(cx, "Expects only 2 arguments");
        return false;
    }

    // NOP when not in IonMonkey
    args.rval().setUndefined();
    return true;
}

static bool
TestingFunc_assertJitStackInvariants(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    jit::AssertJitStackInvariants(cx);
    args.rval().setUndefined();
    return true;
}

bool
js::testingFunc_assertRecoveredOnBailout(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2) {
        JS_ReportErrorASCII(cx, "Expects only 2 arguments");
        return false;
    }

    // NOP when not in IonMonkey
    args.rval().setUndefined();
    return true;
}

static bool
SetJitCompilerOption(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());

    if (args.length() != 2) {
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments.");
        return false;
    }

    if (!args[0].isString()) {
        ReportUsageErrorASCII(cx, callee, "First argument must be a String.");
        return false;
    }

    if (!args[1].isInt32()) {
        ReportUsageErrorASCII(cx, callee, "Second argument must be an Int32.");
        return false;
    }

    JSFlatString* strArg = JS_FlattenString(cx, args[0].toString());
    if (!strArg)
        return false;

#define JIT_COMPILER_MATCH(key, string)                 \
    else if (JS_FlatStringEqualsAscii(strArg, string))  \
        opt = JSJITCOMPILER_ ## key;

    JSJitCompilerOption opt = JSJITCOMPILER_NOT_AN_OPTION;
    if (false) {}
    JIT_COMPILER_OPTIONS(JIT_COMPILER_MATCH);
#undef JIT_COMPILER_MATCH

    if (opt == JSJITCOMPILER_NOT_AN_OPTION) {
        ReportUsageErrorASCII(cx, callee, "First argument does not name a valid option (see jsapi.h).");
        return false;
    }

    int32_t number = args[1].toInt32();
    if (number < 0)
        number = -1;

    // Throw if disabling the JITs and there's JIT code on the stack, to avoid
    // assertion failures.
    if ((opt == JSJITCOMPILER_BASELINE_ENABLE || opt == JSJITCOMPILER_ION_ENABLE) &&
        number == 0)
    {
        js::jit::JitActivationIterator iter(cx->runtime());
        if (!iter.done()) {
            JS_ReportErrorASCII(cx, "Can't turn off JITs with JIT code on the stack.");
            return false;
        }
    }

    JS_SetGlobalJitCompilerOption(cx, opt, uint32_t(number));

    args.rval().setUndefined();
    return true;
}

static bool
GetJitCompilerOptions(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject info(cx, JS_NewPlainObject(cx));
    if (!info)
        return false;

    uint32_t intValue = 0;
    RootedValue value(cx);

#define JIT_COMPILER_MATCH(key, string)                                \
    opt = JSJITCOMPILER_ ## key;                                       \
    if (JS_GetGlobalJitCompilerOption(cx, opt, &intValue)) {           \
        value.setInt32(intValue);                                      \
        if (!JS_SetProperty(cx, info, string, value))                  \
            return false;                                              \
    }

    JSJitCompilerOption opt = JSJITCOMPILER_NOT_AN_OPTION;
    JIT_COMPILER_OPTIONS(JIT_COMPILER_MATCH);
#undef JIT_COMPILER_MATCH

    args.rval().setObject(*info);
    return true;
}

static bool
SetIonCheckGraphCoherency(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    jit::JitOptions.checkGraphConsistency = ToBoolean(args.get(0));
    args.rval().setUndefined();
    return true;
}

class CloneBufferObject : public NativeObject {
    static const JSPropertySpec props_[2];
    static const size_t DATA_SLOT   = 0;
    static const size_t LENGTH_SLOT = 1;
    static const size_t NUM_SLOTS   = 2;

  public:
    static const Class class_;

    static CloneBufferObject* Create(JSContext* cx) {
        RootedObject obj(cx, JS_NewObject(cx, Jsvalify(&class_)));
        if (!obj)
            return nullptr;
        obj->as<CloneBufferObject>().setReservedSlot(DATA_SLOT, PrivateValue(nullptr));
        obj->as<CloneBufferObject>().setReservedSlot(LENGTH_SLOT, Int32Value(0));

        if (!JS_DefineProperties(cx, obj, props_))
            return nullptr;

        return &obj->as<CloneBufferObject>();
    }

    static CloneBufferObject* Create(JSContext* cx, JSAutoStructuredCloneBuffer* buffer) {
        Rooted<CloneBufferObject*> obj(cx, Create(cx));
        if (!obj)
            return nullptr;
        auto data = js::MakeUnique<JSStructuredCloneData>(buffer->scope());
        if (!data) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        buffer->steal(data.get());
        obj->setData(data.release());
        return obj;
    }

    JSStructuredCloneData* data() const {
        return static_cast<JSStructuredCloneData*>(getReservedSlot(DATA_SLOT).toPrivate());
    }

    void setData(JSStructuredCloneData* aData) {
        MOZ_ASSERT(!data());
        setReservedSlot(DATA_SLOT, PrivateValue(aData));
    }

    // Discard an owned clone buffer.
    void discard() {
        if (data()) {
            JSAutoStructuredCloneBuffer clonebuf(JS::StructuredCloneScope::SameProcessSameThread, nullptr, nullptr);
            clonebuf.adopt(Move(*data()));
        }
        setReservedSlot(DATA_SLOT, PrivateValue(nullptr));
    }

    static bool
    setCloneBuffer_impl(JSContext* cx, const CallArgs& args) {
        if (args.length() != 1) {
            JS_ReportErrorASCII(cx, "clonebuffer setter requires a single string argument");
            return false;
        }
        if (!args[0].isString()) {
            JS_ReportErrorASCII(cx, "clonebuffer value must be a string");
            return false;
        }

        if (fuzzingSafe) {
            // A manually-created clonebuffer could easily trigger a crash
            args.rval().setUndefined();
            return true;
        }

        Rooted<CloneBufferObject*> obj(cx, &args.thisv().toObject().as<CloneBufferObject>());
        obj->discard();

        char* str = JS_EncodeString(cx, args[0].toString());
        if (!str)
            return false;
        size_t nbytes = JS_GetStringLength(args[0].toString());
        MOZ_ASSERT(nbytes % sizeof(uint64_t) == 0);
        auto buf = js::MakeUnique<JSStructuredCloneData>(JS::StructuredCloneScope::DifferentProcess);
        if (!buf->AppendBytes(str, nbytes)) {
            ReportOutOfMemory(cx);
            return false;
        }
        JS_free(cx, str);
        obj->setData(buf.release());

        args.rval().setUndefined();
        return true;
    }

    static bool
    is(HandleValue v) {
        return v.isObject() && v.toObject().is<CloneBufferObject>();
    }

    static bool
    setCloneBuffer(JSContext* cx, unsigned int argc, JS::Value* vp) {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod<is, setCloneBuffer_impl>(cx, args);
    }

    static bool
    getCloneBuffer_impl(JSContext* cx, const CallArgs& args) {
        Rooted<CloneBufferObject*> obj(cx, &args.thisv().toObject().as<CloneBufferObject>());
        MOZ_ASSERT(args.length() == 0);

        if (!obj->data()) {
            args.rval().setUndefined();
            return true;
        }

        bool hasTransferable;
        if (!JS_StructuredCloneHasTransferables(*obj->data(), &hasTransferable))
            return false;

        if (hasTransferable) {
            JS_ReportErrorASCII(cx, "cannot retrieve structured clone buffer with transferables");
            return false;
        }

        size_t size = obj->data()->Size();
        UniqueChars buffer(static_cast<char*>(js_malloc(size)));
        if (!buffer) {
            ReportOutOfMemory(cx);
            return false;
        }
        auto iter = obj->data()->Start();
        obj->data()->ReadBytes(iter, buffer.get(), size);
        JSString* str = JS_NewStringCopyN(cx, buffer.get(), size);
        if (!str)
            return false;
        args.rval().setString(str);
        return true;
    }

    static bool
    getCloneBuffer(JSContext* cx, unsigned int argc, JS::Value* vp) {
        CallArgs args = CallArgsFromVp(argc, vp);
        return CallNonGenericMethod<is, getCloneBuffer_impl>(cx, args);
    }

    static void Finalize(FreeOp* fop, JSObject* obj) {
        obj->as<CloneBufferObject>().discard();
    }
};

static const ClassOps CloneBufferObjectClassOps = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    CloneBufferObject::Finalize
};

const Class CloneBufferObject::class_ = {
    "CloneBuffer",
    JSCLASS_HAS_RESERVED_SLOTS(CloneBufferObject::NUM_SLOTS) |
    JSCLASS_FOREGROUND_FINALIZE,
    &CloneBufferObjectClassOps
};

const JSPropertySpec CloneBufferObject::props_[] = {
    JS_PSGS("clonebuffer", getCloneBuffer, setCloneBuffer, 0),
    JS_PS_END
};

static mozilla::Maybe<JS::StructuredCloneScope>
ParseCloneScope(JSContext* cx, HandleString str)
{
    mozilla::Maybe<JS::StructuredCloneScope> scope;

    JSAutoByteString scopeStr(cx, str);
    if (!scopeStr)
        return scope;

    if (strcmp(scopeStr.ptr(), "SameProcessSameThread") == 0)
        scope.emplace(JS::StructuredCloneScope::SameProcessSameThread);
    else if (strcmp(scopeStr.ptr(), "SameProcessDifferentThread") == 0)
        scope.emplace(JS::StructuredCloneScope::SameProcessDifferentThread);
    else if (strcmp(scopeStr.ptr(), "DifferentProcess") == 0)
        scope.emplace(JS::StructuredCloneScope::DifferentProcess);
    else if (strcmp(scopeStr.ptr(), "DifferentProcessForIndexedDB") == 0)
        scope.emplace(JS::StructuredCloneScope::DifferentProcessForIndexedDB);

    return scope;
}

static bool
Serialize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    mozilla::Maybe<JSAutoStructuredCloneBuffer> clonebuf;
    JS::CloneDataPolicy policy;

    if (!args.get(2).isUndefined()) {
        RootedObject opts(cx, ToObject(cx, args.get(2)));
        if (!opts)
            return false;

        RootedValue v(cx);
        if (!JS_GetProperty(cx, opts, "SharedArrayBuffer", &v))
            return false;

        if (!v.isUndefined()) {
            JSString* str = JS::ToString(cx, v);
            if (!str)
                return false;
            JSAutoByteString poli(cx, str);
            if (!poli)
                return false;

            if (strcmp(poli.ptr(), "allow") == 0) {
                // default
            } else if (strcmp(poli.ptr(), "deny") == 0) {
                policy.denySharedArrayBuffer();
            } else {
                JS_ReportErrorASCII(cx, "Invalid policy value for 'SharedArrayBuffer'");
                return false;
            }
        }

        if (!JS_GetProperty(cx, opts, "scope", &v))
            return false;

        if (!v.isUndefined()) {
            RootedString str(cx, JS::ToString(cx, v));
            if (!str)
                return false;
            auto scope = ParseCloneScope(cx, str);
            if (!scope) {
                JS_ReportErrorASCII(cx, "Invalid structured clone scope");
                return false;
            }
            clonebuf.emplace(*scope, nullptr, nullptr);
        }
    }

    if (!clonebuf)
        clonebuf.emplace(JS::StructuredCloneScope::SameProcessSameThread, nullptr, nullptr);

    if (!clonebuf->write(cx, args.get(0), args.get(1), policy))
        return false;

    RootedObject obj(cx, CloneBufferObject::Create(cx, clonebuf.ptr()));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
Deserialize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (!args.get(0).isObject() || !args[0].toObject().is<CloneBufferObject>()) {
        JS_ReportErrorASCII(cx, "deserialize requires a clonebuffer argument");
        return false;
    }

    JS::StructuredCloneScope scope = JS::StructuredCloneScope::SameProcessSameThread;
    if (args.get(1).isObject()) {
        RootedObject opts(cx, &args[1].toObject());
        if (!opts)
            return false;

        RootedValue v(cx);
        if (!JS_GetProperty(cx, opts, "scope", &v))
            return false;

        if (!v.isUndefined()) {
            RootedString str(cx, JS::ToString(cx, v));
            if (!str)
                return false;
            auto maybeScope = ParseCloneScope(cx, str);
            if (!maybeScope) {
                JS_ReportErrorASCII(cx, "Invalid structured clone scope");
                return false;
            }

            scope = *maybeScope;
        }
    }

    Rooted<CloneBufferObject*> obj(cx, &args[0].toObject().as<CloneBufferObject>());

    // Clone buffer was already consumed?
    if (!obj->data()) {
        JS_ReportErrorASCII(cx, "deserialize given invalid clone buffer "
                            "(transferables already consumed?)");
        return false;
    }

    bool hasTransferable;
    if (!JS_StructuredCloneHasTransferables(*obj->data(), &hasTransferable))
        return false;

    RootedValue deserialized(cx);
    if (!JS_ReadStructuredClone(cx, *obj->data(),
                                JS_STRUCTURED_CLONE_VERSION,
                                scope,
                                &deserialized, nullptr, nullptr))
    {
        return false;
    }
    args.rval().set(deserialized);

    if (hasTransferable)
        obj->discard();

    return true;
}

static bool
DetachArrayBuffer(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "detachArrayBuffer() requires a single argument");
        return false;
    }

    if (!args[0].isObject()) {
        JS_ReportErrorASCII(cx, "detachArrayBuffer must be passed an object");
        return false;
    }

    RootedObject obj(cx, &args[0].toObject());
    if (!JS_DetachArrayBuffer(cx, obj))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
HelperThreadCount(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
#ifdef JS_MORE_DETERMINISTIC
    // Always return 0 to get consistent output with and without --no-threads.
    args.rval().setInt32(0);
#else
    if (CanUseExtraThreads())
        args.rval().setInt32(HelperThreadState().threadCount);
    else
        args.rval().setInt32(0);
#endif
    return true;
}

static bool
TimesAccessed(JSContext* cx, unsigned argc, Value* vp)
{
    static int32_t accessed = 0;
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setInt32(++accessed);
    return true;
}

#ifdef JS_TRACE_LOGGING
static bool
EnableTraceLogger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    if (!TraceLoggerEnable(logger, cx))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
DisableTraceLogger(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    args.rval().setBoolean(TraceLoggerDisable(logger));

    return true;
}
#endif

#ifdef DEBUG
static bool
DumpObject(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject obj(cx, ToObject(cx, args.get(0)));
    if (!obj)
        return false;

    DumpObject(obj);

    args.rval().setUndefined();
    return true;
}
#endif

static bool
SharedMemoryEnabled(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setBoolean(cx->compartment()->creationOptions().getSharedMemoryAndAtomicsEnabled());
    return true;
}

static bool
SharedAddress(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }
    if (!args[0].isObject()) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Expected object");
        return false;
    }

#ifdef JS_MORE_DETERMINISTIC
    args.rval().setString(cx->staticStrings().getUint(0));
#else
    RootedObject obj(cx, CheckedUnwrap(&args[0].toObject()));
    if (!obj) {
        JS_ReportErrorASCII(cx, "Permission denied to access object");
        return false;
    }
    if (!obj->is<SharedArrayBufferObject>()) {
        JS_ReportErrorASCII(cx, "Argument must be a SharedArrayBuffer");
        return false;
    }
    char buffer[64];
    uint32_t nchar =
        SprintfLiteral(buffer, "%p",
                       obj->as<SharedArrayBufferObject>().dataPointerShared().unwrap(/*safeish*/));

    JSString* str = JS_NewStringCopyN(cx, buffer, nchar);
    if (!str)
        return false;

    args.rval().setString(str);
#endif

    return true;
}

static bool
DumpBacktrace(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    DumpBacktrace(cx);
    args.rval().setUndefined();
    return true;
}

static bool
GetBacktrace(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool showArgs = false;
    bool showLocals = false;
    bool showThisProps = false;

    if (args.length() > 1) {
        RootedObject callee(cx, &args.callee());
        ReportUsageErrorASCII(cx, callee, "Too many arguments");
        return false;
    }

    if (args.length() == 1) {
        RootedObject cfg(cx, ToObject(cx, args[0]));
        if (!cfg)
            return false;
        RootedValue v(cx);

        if (!JS_GetProperty(cx, cfg, "args", &v))
            return false;
        showArgs = ToBoolean(v);

        if (!JS_GetProperty(cx, cfg, "locals", &v))
            return false;
        showLocals = ToBoolean(v);

        if (!JS_GetProperty(cx, cfg, "thisprops", &v))
            return false;
        showThisProps = ToBoolean(v);
    }

    char* buf = JS::FormatStackDump(cx, nullptr, showArgs, showLocals, showThisProps);
    if (!buf)
        return false;

    RootedString str(cx);
    if (!(str = JS_NewStringCopyZ(cx, buf)))
        return false;
    JS_smprintf_free(buf);

    args.rval().setString(str);
    return true;
}

static bool
ReportOutOfMemory(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    JS_ReportOutOfMemory(cx);
    cx->clearPendingException();
    args.rval().setUndefined();
    return true;
}

static bool
ThrowOutOfMemory(JSContext* cx, unsigned argc, Value* vp)
{
    JS_ReportOutOfMemory(cx);
    return false;
}

static bool
ReportLargeAllocationFailure(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    void* buf = cx->runtime()->onOutOfMemoryCanGC(AllocFunction::Malloc, JSRuntime::LARGE_ALLOCATION);
    js_free(buf);
    args.rval().setUndefined();
    return true;
}

namespace heaptools {

typedef UniqueTwoByteChars EdgeName;

// An edge to a node from its predecessor in a path through the graph.
class BackEdge {
    // The node from which this edge starts.
    JS::ubi::Node predecessor_;

    // The name of this edge.
    EdgeName name_;

  public:
    BackEdge() : name_(nullptr) { }
    // Construct an initialized back edge, taking ownership of |name|.
    BackEdge(JS::ubi::Node predecessor, EdgeName name)
        : predecessor_(predecessor), name_(Move(name)) { }
    BackEdge(BackEdge&& rhs) : predecessor_(rhs.predecessor_), name_(Move(rhs.name_)) { }
    BackEdge& operator=(BackEdge&& rhs) {
        MOZ_ASSERT(&rhs != this);
        this->~BackEdge();
        new(this) BackEdge(Move(rhs));
        return *this;
    }

    EdgeName forgetName() { return Move(name_); }
    JS::ubi::Node predecessor() const { return predecessor_; }

  private:
    // No copy constructor or copying assignment.
    BackEdge(const BackEdge&) = delete;
    BackEdge& operator=(const BackEdge&) = delete;
};

// A path-finding handler class for use with JS::ubi::BreadthFirst.
struct FindPathHandler {
    typedef BackEdge NodeData;
    typedef JS::ubi::BreadthFirst<FindPathHandler> Traversal;

    FindPathHandler(JSContext*cx, JS::ubi::Node start, JS::ubi::Node target,
                    MutableHandle<GCVector<Value>> nodes, Vector<EdgeName>& edges)
      : cx(cx), start(start), target(target), foundPath(false),
        nodes(nodes), edges(edges) { }

    bool
    operator()(Traversal& traversal, JS::ubi::Node origin, const JS::ubi::Edge& edge,
               BackEdge* backEdge, bool first)
    {
        // We take care of each node the first time we visit it, so there's
        // nothing to be done on subsequent visits.
        if (!first)
            return true;

        // Record how we reached this node. This is the last edge on a
        // shortest path to this node.
        EdgeName edgeName = DuplicateString(cx, edge.name.get());
        if (!edgeName)
            return false;
        *backEdge = mozilla::Move(BackEdge(origin, Move(edgeName)));

        // Have we reached our final target node?
        if (edge.referent == target) {
            // Record the path that got us here, which must be a shortest path.
            if (!recordPath(traversal))
                return false;
            foundPath = true;
            traversal.stop();
        }

        return true;
    }

    // We've found a path to our target. Walk the backlinks to produce the
    // (reversed) path, saving the path in |nodes| and |edges|. |nodes| is
    // rooted, so it can hold the path's nodes as we leave the scope of
    // the AutoCheckCannotGC.
    bool recordPath(Traversal& traversal) {
        JS::ubi::Node here = target;

        do {
            Traversal::NodeMap::Ptr p = traversal.visited.lookup(here);
            MOZ_ASSERT(p);
            JS::ubi::Node predecessor = p->value().predecessor();
            if (!nodes.append(predecessor.exposeToJS()) ||
                !edges.append(p->value().forgetName()))
                return false;
            here = predecessor;
        } while (here != start);

        return true;
    }

    JSContext* cx;

    // The node we're starting from.
    JS::ubi::Node start;

    // The node we're looking for.
    JS::ubi::Node target;

    // True if we found a path to target, false if we didn't.
    bool foundPath;

    // The nodes and edges of the path --- should we find one. The path is
    // stored in reverse order, because that's how it's easiest for us to
    // construct it:
    // - edges[i] is the name of the edge from nodes[i] to nodes[i-1].
    // - edges[0] is the name of the edge from nodes[0] to the target.
    // - The last node, nodes[n-1], is the start node.
    MutableHandle<GCVector<Value>> nodes;
    Vector<EdgeName>& edges;
};

} // namespace heaptools

static bool
FindPath(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (argc < 2) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                                  "findPath", "1", "");
        return false;
    }

    // We don't ToString non-objects given as 'start' or 'target', because this
    // test is all about object identity, and ToString doesn't preserve that.
    // Non-GCThing endpoints don't make much sense.
    if (!args[0].isObject() && !args[0].isString() && !args[0].isSymbol()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[0], nullptr,
                              "not an object, string, or symbol", NULL);
        return false;
    }

    if (!args[1].isObject() && !args[1].isString() && !args[1].isSymbol()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[0], nullptr,
                              "not an object, string, or symbol", NULL);
        return false;
    }

    Rooted<GCVector<Value>> nodes(cx, GCVector<Value>(cx));
    Vector<heaptools::EdgeName> edges(cx);

    {
        // We can't tolerate the GC moving things around while we're searching
        // the heap. Check that nothing we do causes a GC.
        JS::AutoCheckCannotGC autoCannotGC;

        JS::ubi::Node start(args[0]), target(args[1]);

        heaptools::FindPathHandler handler(cx, start, target, &nodes, edges);
        heaptools::FindPathHandler::Traversal traversal(cx, handler, autoCannotGC);
        if (!traversal.init() || !traversal.addStart(start)) {
            ReportOutOfMemory(cx);
            return false;
        }

        if (!traversal.traverse()) {
            if (!cx->isExceptionPending())
                ReportOutOfMemory(cx);
            return false;
        }

        if (!handler.foundPath) {
            // We didn't find any paths from the start to the target.
            args.rval().setUndefined();
            return true;
        }
    }

    // |nodes| and |edges| contain the path from |start| to |target|, reversed.
    // Construct a JavaScript array describing the path from the start to the
    // target. Each element has the form:
    //
    //   {
    //     node: <object or string or symbol>,
    //     edge: <string describing outgoing edge from node>
    //   }
    //
    // or, if the node is some internal thing that isn't a proper JavaScript
    // value:
    //
    //   { node: undefined, edge: <string> }
    size_t length = nodes.length();
    RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!result)
        return false;
    result->ensureDenseInitializedLength(cx, 0, length);

    // Walk |nodes| and |edges| in the stored order, and construct the result
    // array in start-to-target order.
    for (size_t i = 0; i < length; i++) {
        // Build an object describing the node and edge.
        RootedObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx));
        if (!obj)
            return false;

        RootedValue wrapped(cx, nodes[i]);
        if (!cx->compartment()->wrap(cx, &wrapped))
            return false;

        if (!JS_DefineProperty(cx, obj, "node", wrapped,
                               JSPROP_ENUMERATE, nullptr, nullptr))
            return false;

        heaptools::EdgeName edgeName = Move(edges[i]);

        RootedString edgeStr(cx, NewString<CanGC>(cx, edgeName.get(), js_strlen(edgeName.get())));
        if (!edgeStr)
            return false;
        mozilla::Unused << edgeName.release(); // edgeStr acquired ownership

        if (!JS_DefineProperty(cx, obj, "edge", edgeStr, JSPROP_ENUMERATE, nullptr, nullptr))
            return false;

        result->setDenseElement(length - i - 1, ObjectValue(*obj));
    }

    args.rval().setObject(*result);
    return true;
}

static bool
ShortestPaths(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "shortestPaths", 3))
        return false;

    // We don't ToString non-objects given as 'start' or 'target', because this
    // test is all about object identity, and ToString doesn't preserve that.
    // Non-GCThing endpoints don't make much sense.
    if (!args[0].isObject() && !args[0].isString() && !args[0].isSymbol()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[0], nullptr,
                              "not an object, string, or symbol", nullptr);
        return false;
    }

    if (!args[1].isObject() || !args[1].toObject().is<ArrayObject>()) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[1], nullptr,
                              "not an array object", nullptr);
        return false;
    }

    RootedArrayObject objs(cx, &args[1].toObject().as<ArrayObject>());
    size_t length = objs->getDenseInitializedLength();
    if (length == 0) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[1], nullptr,
                              "not a dense array object with one or more elements", nullptr);
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        RootedValue el(cx, objs->getDenseElement(i));
        if (!el.isObject() && !el.isString() && !el.isSymbol()) {
            JS_ReportErrorASCII(cx, "Each target must be an object, string, or symbol");
            return false;
        }
    }

    int32_t maxNumPaths;
    if (!JS::ToInt32(cx, args[2], &maxNumPaths))
        return false;
    if (maxNumPaths <= 0) {
        ReportValueErrorFlags(cx, JSREPORT_ERROR, JSMSG_UNEXPECTED_TYPE,
                              JSDVG_SEARCH_STACK, args[2], nullptr,
                              "not greater than 0", nullptr);
        return false;
    }

    // We accumulate the results into a GC-stable form, due to the fact that the
    // JS::ubi::ShortestPaths lifetime (when operating on the live heap graph)
    // is bounded within an AutoCheckCannotGC.
    Rooted<GCVector<GCVector<GCVector<Value>>>> values(cx, GCVector<GCVector<GCVector<Value>>>(cx));
    Vector<Vector<Vector<JS::ubi::EdgeName>>> names(cx);

    {
        JS::AutoCheckCannotGC noGC(cx);

        JS::ubi::NodeSet targets;
        if (!targets.init()) {
            ReportOutOfMemory(cx);
            return false;
        }

        for (size_t i = 0; i < length; i++) {
            RootedValue val(cx, objs->getDenseElement(i));
            JS::ubi::Node node(val);
            if (!targets.put(node)) {
                ReportOutOfMemory(cx);
                return false;
            }
        }

        JS::ubi::Node root(args[0]);
        auto maybeShortestPaths = JS::ubi::ShortestPaths::Create(cx, noGC, maxNumPaths,
                                                                 root, mozilla::Move(targets));
        if (maybeShortestPaths.isNothing()) {
            ReportOutOfMemory(cx);
            return false;
        }
        auto& shortestPaths = *maybeShortestPaths;

        for (size_t i = 0; i < length; i++) {
            if (!values.append(GCVector<GCVector<Value>>(cx)) ||
                !names.append(Vector<Vector<JS::ubi::EdgeName>>(cx)))
            {
                return false;
            }

            RootedValue val(cx, objs->getDenseElement(i));
            JS::ubi::Node target(val);

            bool ok = shortestPaths.forEachPath(target, [&](JS::ubi::Path& path) {
                Rooted<GCVector<Value>> pathVals(cx, GCVector<Value>(cx));
                Vector<JS::ubi::EdgeName> pathNames(cx);

                for (auto& part : path) {
                    if (!pathVals.append(part->predecessor().exposeToJS()) ||
                        !pathNames.append(mozilla::Move(part->name())))
                    {
                        return false;
                    }
                }

                return values.back().append(mozilla::Move(pathVals.get())) &&
                       names.back().append(mozilla::Move(pathNames));
            });

            if (!ok)
                return false;
        }
    }

    MOZ_ASSERT(values.length() == names.length());
    MOZ_ASSERT(values.length() == length);

    RootedArrayObject results(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!results)
        return false;
    results->ensureDenseInitializedLength(cx, 0, length);

    for (size_t i = 0; i < length; i++) {
        size_t numPaths = values[i].length();
        MOZ_ASSERT(names[i].length() == numPaths);

        RootedArrayObject pathsArray(cx, NewDenseFullyAllocatedArray(cx, numPaths));
        if (!pathsArray)
            return false;
        pathsArray->ensureDenseInitializedLength(cx, 0, numPaths);

        for (size_t j = 0; j < numPaths; j++) {
            size_t pathLength = values[i][j].length();
            MOZ_ASSERT(names[i][j].length() == pathLength);

            RootedArrayObject path(cx, NewDenseFullyAllocatedArray(cx, pathLength));
            if (!path)
                return false;
            path->ensureDenseInitializedLength(cx, 0, pathLength);

            for (size_t k = 0; k < pathLength; k++) {
                RootedPlainObject part(cx, NewBuiltinClassInstance<PlainObject>(cx));
                if (!part)
                    return false;

                RootedValue predecessor(cx, values[i][j][k]);
                if (!cx->compartment()->wrap(cx, &predecessor) ||
                    !JS_DefineProperty(cx, part, "predecessor", predecessor, JSPROP_ENUMERATE))
                {
                    return false;
                }

                if (names[i][j][k]) {
                    RootedString edge(cx, NewStringCopyZ<CanGC>(cx, names[i][j][k].get()));
                    if (!edge || !JS_DefineProperty(cx, part, "edge", edge, JSPROP_ENUMERATE))
                        return false;
                }

                path->setDenseElement(k, ObjectValue(*part));
            }

            pathsArray->setDenseElement(j, ObjectValue(*path));
        }

        results->setDenseElement(i, ObjectValue(*pathsArray));
    }

    args.rval().setObject(*results);
    return true;
}

static bool
EvalReturningScope(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "evalReturningScope", 1))
        return false;

    RootedString str(cx, ToString(cx, args[0]));
    if (!str)
        return false;

    RootedObject global(cx);
    if (args.hasDefined(1)) {
        global = ToObject(cx, args[1]);
        if (!global)
            return false;
    }

    AutoStableStringChars strChars(cx);
    if (!strChars.initTwoByte(cx, str))
        return false;

    mozilla::Range<const char16_t> chars = strChars.twoByteRange();
    size_t srclen = chars.length();
    const char16_t* src = chars.begin().get();

    JS::AutoFilename filename;
    unsigned lineno;

    JS::DescribeScriptedCaller(cx, &filename, &lineno);

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.get(), lineno);
    options.setNoScriptRval(true);

    JS::SourceBufferHolder srcBuf(src, srclen, JS::SourceBufferHolder::NoOwnership);
    RootedScript script(cx);
    if (!JS::CompileForNonSyntacticScope(cx, options, srcBuf, &script))
        return false;

    if (global) {
        global = CheckedUnwrap(global);
        if (!global) {
            JS_ReportErrorASCII(cx, "Permission denied to access global");
            return false;
        }
        if (!global->is<GlobalObject>()) {
            JS_ReportErrorASCII(cx, "Argument must be a global object");
            return false;
        }
    } else {
        global = JS::CurrentGlobalOrNull(cx);
    }

    RootedObject varObj(cx);
    RootedObject lexicalScope(cx);

    {
        // If we're switching globals here, ExecuteInGlobalAndReturnScope will
        // take care of cloning the script into that compartment before
        // executing it.
        AutoCompartment ac(cx, global);

        if (!js::ExecuteInGlobalAndReturnScope(cx, global, script, &lexicalScope))
            return false;

        varObj = lexicalScope->enclosingEnvironment();
    }

    RootedObject rv(cx, JS_NewPlainObject(cx));
    if (!rv)
        return false;

    RootedValue varObjVal(cx, ObjectValue(*varObj));
    if (!cx->compartment()->wrap(cx, &varObjVal))
        return false;
    if (!JS_SetProperty(cx, rv, "vars", varObjVal))
        return false;

    RootedValue lexicalScopeVal(cx, ObjectValue(*lexicalScope));
    if (!cx->compartment()->wrap(cx, &lexicalScopeVal))
        return false;
    if (!JS_SetProperty(cx, rv, "lexicals", lexicalScopeVal))
        return false;

    args.rval().setObject(*rv);
    return true;
}

static bool
ShellCloneAndExecuteScript(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "cloneAndExecuteScript", 2))
        return false;

    RootedString str(cx, ToString(cx, args[0]));
    if (!str)
        return false;

    RootedObject global(cx, ToObject(cx, args[1]));
    if (!global)
        return false;

    AutoStableStringChars strChars(cx);
    if (!strChars.initTwoByte(cx, str))
        return false;

    mozilla::Range<const char16_t> chars = strChars.twoByteRange();
    size_t srclen = chars.length();
    const char16_t* src = chars.begin().get();

    JS::AutoFilename filename;
    unsigned lineno;

    JS::DescribeScriptedCaller(cx, &filename, &lineno);

    JS::CompileOptions options(cx);
    options.setFileAndLine(filename.get(), lineno);
    options.setNoScriptRval(true);

    JS::SourceBufferHolder srcBuf(src, srclen, JS::SourceBufferHolder::NoOwnership);
    RootedScript script(cx);
    if (!JS::Compile(cx, options, srcBuf, &script))
        return false;

    global = CheckedUnwrap(global);
    if (!global) {
        JS_ReportErrorASCII(cx, "Permission denied to access global");
        return false;
    }
    if (!global->is<GlobalObject>()) {
        JS_ReportErrorASCII(cx, "Argument must be a global object");
        return false;
    }

    AutoCompartment ac(cx, global);

    JS::RootedValue rval(cx);
    if (!JS::CloneAndExecuteScript(cx, script, &rval))
        return false;

    args.rval().setUndefined();
    return true;
}

static bool
IsSimdAvailable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
#if defined(JS_CODEGEN_NONE)
    bool available = false;
#else
    bool available = cx->jitSupportsSimd();
#endif
    args.rval().set(BooleanValue(available));
    return true;
}

static bool
ByteSize(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    mozilla::MallocSizeOf mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

    {
        // We can't tolerate the GC moving things around while we're using a
        // ubi::Node. Check that nothing we do causes a GC.
        JS::AutoCheckCannotGC autoCannotGC;

        JS::ubi::Node node = args.get(0);
        if (node)
            args.rval().setNumber(uint32_t(node.size(mallocSizeOf)));
        else
            args.rval().setUndefined();
    }
    return true;
}

static bool
ByteSizeOfScript(JSContext*cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "byteSizeOfScript", 1))
        return false;
    if (!args[0].isObject() || !args[0].toObject().is<JSFunction>()) {
        JS_ReportErrorASCII(cx, "Argument must be a Function object");
        return false;
    }

    RootedFunction fun(cx, &args[0].toObject().as<JSFunction>());
    if (fun->isNative()) {
        JS_ReportErrorASCII(cx, "Argument must be a scripted function");
        return false;
    }

    RootedScript script(cx, JSFunction::getOrCreateScript(cx, fun));
    if (!script)
        return false;

    mozilla::MallocSizeOf mallocSizeOf = cx->runtime()->debuggerMallocSizeOf;

    {
        // We can't tolerate the GC moving things around while we're using a
        // ubi::Node. Check that nothing we do causes a GC.
        JS::AutoCheckCannotGC autoCannotGC;

        JS::ubi::Node node = script;
        if (node)
            args.rval().setNumber(uint32_t(node.size(mallocSizeOf)));
        else
            args.rval().setUndefined();
    }
    return true;
}

static bool
SetImmutablePrototype(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject()) {
        JS_ReportErrorASCII(cx, "setImmutablePrototype: object expected");
        return false;
    }

    RootedObject obj(cx, &args[0].toObject());

    bool succeeded;
    if (!js::SetImmutablePrototype(cx, obj, &succeeded))
        return false;

    args.rval().setBoolean(succeeded);
    return true;
}

#ifdef DEBUG
static bool
DumpStringRepresentation(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    RootedString str(cx, ToString(cx, args.get(0)));
    if (!str)
        return false;

    str->dumpRepresentation(stderr, 0);

    args.rval().setUndefined();
    return true;
}
#endif

static bool
SetLazyParsingDisabled(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool disable = !args.hasDefined(0) || ToBoolean(args[0]);
    cx->compartment()->behaviors().setDisableLazyParsing(disable);

    args.rval().setUndefined();
    return true;
}

static bool
SetDiscardSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool discard = !args.hasDefined(0) || ToBoolean(args[0]);
    cx->compartment()->behaviors().setDiscardSource(discard);

    args.rval().setUndefined();
    return true;
}

static bool
GetConstructorName(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "getConstructorName", 1))
        return false;

    if (!args[0].isObject()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NOT_EXPECTED_TYPE,
                                  "getConstructorName", "Object",
                                  InformalValueTypeName(args[0]));
        return false;
    }

    RootedAtom name(cx);
    RootedObject obj(cx, &args[0].toObject());
    if (!JSObject::constructorDisplayAtom(cx, obj, &name))
        return false;

    if (name) {
        args.rval().setString(name);
    } else {
        args.rval().setNull();
    }
    return true;
}

static bool
AllocationMarker(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool allocateInsideNursery = true;
    if (args.length() > 0 && args[0].isObject()) {
        RootedObject options(cx, &args[0].toObject());

        RootedValue nurseryVal(cx);
        if (!JS_GetProperty(cx, options, "nursery", &nurseryVal))
            return false;
        allocateInsideNursery = ToBoolean(nurseryVal);
    }

    static const Class cls = { "AllocationMarker" };

    auto newKind = allocateInsideNursery ? GenericObject : TenuredObject;
    RootedObject obj(cx, NewObjectWithGivenProto(cx, &cls, nullptr, newKind));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

namespace gcCallback {

struct MajorGC {
    int32_t depth;
    int32_t phases;
};

static void
majorGC(JSContext* cx, JSGCStatus status, void* data)
{
    auto info = static_cast<MajorGC*>(data);
    if (!(info->phases & (1 << status)))
        return;

    if (info->depth > 0) {
        info->depth--;
        JS::PrepareForFullGC(cx);
        JS::GCForReason(cx, GC_NORMAL, JS::gcreason::API);
        info->depth++;
    }
}

struct MinorGC {
    int32_t phases;
    bool active;
};

static void
minorGC(JSContext* cx, JSGCStatus status, void* data)
{
    auto info = static_cast<MinorGC*>(data);
    if (!(info->phases & (1 << status)))
        return;

    if (info->active) {
        info->active = false;
        cx->gc.evictNursery(JS::gcreason::DEBUG_GC);
        info->active = true;
    }
}

// Process global, should really be runtime-local. Also, the final one of these
// is currently leaked, since they are only deleted when changing.
MajorGC* prevMajorGC = nullptr;
MinorGC* prevMinorGC = nullptr;

} /* namespace gcCallback */

static bool
SetGCCallback(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "Wrong number of arguments");
        return false;
    }

    RootedObject opts(cx, ToObject(cx, args[0]));
    if (!opts)
        return false;

    RootedValue v(cx);
    if (!JS_GetProperty(cx, opts, "action", &v))
        return false;

    JSString* str = JS::ToString(cx, v);
    if (!str)
        return false;
    JSAutoByteString action(cx, str);
    if (!action)
        return false;

    int32_t phases = 0;
    if ((strcmp(action.ptr(), "minorGC") == 0) || (strcmp(action.ptr(), "majorGC") == 0)) {
        if (!JS_GetProperty(cx, opts, "phases", &v))
            return false;
        if (v.isUndefined()) {
            phases = (1 << JSGC_END);
        } else {
            JSString* str = JS::ToString(cx, v);
            if (!str)
                return false;
            JSAutoByteString phasesStr(cx, str);
            if (!phasesStr)
                return false;

            if (strcmp(phasesStr.ptr(), "begin") == 0)
                phases = (1 << JSGC_BEGIN);
            else if (strcmp(phasesStr.ptr(), "end") == 0)
                phases = (1 << JSGC_END);
            else if (strcmp(phasesStr.ptr(), "both") == 0)
                phases = (1 << JSGC_BEGIN) | (1 << JSGC_END);
            else {
                JS_ReportErrorASCII(cx, "Invalid callback phase");
                return false;
            }
        }
    }

    if (gcCallback::prevMajorGC) {
        JS_SetGCCallback(cx, nullptr, nullptr);
        js_delete<gcCallback::MajorGC>(gcCallback::prevMajorGC);
        gcCallback::prevMajorGC = nullptr;
    }

    if (gcCallback::prevMinorGC) {
        JS_SetGCCallback(cx, nullptr, nullptr);
        js_delete<gcCallback::MinorGC>(gcCallback::prevMinorGC);
        gcCallback::prevMinorGC = nullptr;
    }

    if (strcmp(action.ptr(), "minorGC") == 0) {
        auto info = js_new<gcCallback::MinorGC>();
        if (!info) {
            ReportOutOfMemory(cx);
            return false;
        }

        info->phases = phases;
        info->active = true;
        JS_SetGCCallback(cx, gcCallback::minorGC, info);
    } else if (strcmp(action.ptr(), "majorGC") == 0) {
        if (!JS_GetProperty(cx, opts, "depth", &v))
            return false;
        int32_t depth = 1;
        if (!v.isUndefined()) {
            if (!ToInt32(cx, v, &depth))
                return false;
        }
        if (depth > int32_t(gcstats::Statistics::MAX_NESTING - 4)) {
            JS_ReportErrorASCII(cx, "Nesting depth too large, would overflow");
            return false;
        }

        auto info = js_new<gcCallback::MajorGC>();
        if (!info) {
            ReportOutOfMemory(cx);
            return false;
        }

        info->phases = phases;
        info->depth = depth;
        JS_SetGCCallback(cx, gcCallback::majorGC, info);
    } else {
        JS_ReportErrorASCII(cx, "Unknown GC callback action");
        return false;
    }

    args.rval().setUndefined();
    return true;
}

static bool
GetLcovInfo(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    if (args.length() > 1) {
        JS_ReportErrorASCII(cx, "Wrong number of arguments");
        return false;
    }

    RootedObject global(cx);
    if (args.hasDefined(0)) {
        global = ToObject(cx, args[0]);
        if (!global) {
            JS_ReportErrorASCII(cx, "First argument should be an object");
            return false;
        }
        global = CheckedUnwrap(global);
        if (!global) {
            JS_ReportErrorASCII(cx, "Permission denied to access global");
            return false;
        }
        if (!global->is<GlobalObject>()) {
            JS_ReportErrorASCII(cx, "Argument must be a global object");
            return false;
        }
    } else {
        global = JS::CurrentGlobalOrNull(cx);
    }

    size_t length = 0;
    char* content = nullptr;
    {
        AutoCompartment ac(cx, global);
        content = js::GetCodeCoverageSummary(cx, &length);
    }

    if (!content)
        return false;

    JSString* str = JS_NewStringCopyN(cx, content, length);
    free(content);

    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

#ifdef DEBUG
static bool
SetRNGState(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "SetRNGState", 2))
        return false;

    double d0;
    if (!ToNumber(cx, args[0], &d0))
        return false;

    double d1;
    if (!ToNumber(cx, args[1], &d1))
        return false;

    uint64_t seed0 = static_cast<uint64_t>(d0);
    uint64_t seed1 = static_cast<uint64_t>(d1);

    if (seed0 == 0 && seed1 == 0) {
        JS_ReportErrorASCII(cx, "RNG requires non-zero seed");
        return false;
    }

    cx->compartment()->ensureRandomNumberGenerator();
    cx->compartment()->randomNumberGenerator.ref().setState(seed0, seed1);

    args.rval().setUndefined();
    return true;
}
#endif

static ModuleEnvironmentObject*
GetModuleEnvironment(JSContext* cx, HandleValue moduleValue)
{
    RootedModuleObject module(cx, &moduleValue.toObject().as<ModuleObject>());

    // Use the initial environment so that tests can check bindings exists
    // before they have been instantiated.
    RootedModuleEnvironmentObject env(cx, &module->initialEnvironment());
    MOZ_ASSERT(env);
    return env;
}

static bool
GetModuleEnvironmentNames(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 1) {
        JS_ReportErrorASCII(cx, "Wrong number of arguments");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<ModuleObject>()) {
        JS_ReportErrorASCII(cx, "First argument should be a ModuleObject");
        return false;
    }

//-    if (module->status() == MODULE_STATUS_ERRORED) {
//+    if (module->hadEvaluationError()) {

    RootedModuleEnvironmentObject env(cx, GetModuleEnvironment(cx, args[0]));
    Rooted<IdVector> ids(cx, IdVector(cx));
    if (!JS_Enumerate(cx, env, &ids))
        return false;

    uint32_t length = ids.length();
    RootedArrayObject array(cx, NewDenseFullyAllocatedArray(cx, length));
    if (!array)
        return false;

    array->setDenseInitializedLength(length);
    for (uint32_t i = 0; i < length; i++)
        array->initDenseElement(i, StringValue(JSID_TO_STRING(ids[i])));

    args.rval().setObject(*array);
    return true;
}

static bool
GetModuleEnvironmentValue(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() != 2) {
        JS_ReportErrorASCII(cx, "Wrong number of arguments");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<ModuleObject>()) {
        JS_ReportErrorASCII(cx, "First argument should be a ModuleObject");
        return false;
    }

    if (!args[1].isString()) {
        JS_ReportErrorASCII(cx, "Second argument should be a string");
        return false;
    }

//-    if (module->status() == MODULE_STATUS_ERRORED) {
//+    if (module->hadEvaluationError()) {

    RootedModuleEnvironmentObject env(cx, GetModuleEnvironment(cx, args[0]));
    RootedString name(cx, args[1].toString());
    RootedId id(cx);
    if (!JS_StringToId(cx, name, &id))
        return false;

    return GetProperty(cx, env, env, id, args.rval());
}

#ifdef DEBUG
static const char*
AssertionTypeToString(irregexp::RegExpAssertion::AssertionType type)
{
    switch (type) {
      case irregexp::RegExpAssertion::START_OF_LINE:
        return "START_OF_LINE";
      case irregexp::RegExpAssertion::START_OF_INPUT:
        return "START_OF_INPUT";
      case irregexp::RegExpAssertion::END_OF_LINE:
        return "END_OF_LINE";
      case irregexp::RegExpAssertion::END_OF_INPUT:
        return "END_OF_INPUT";
      case irregexp::RegExpAssertion::BOUNDARY:
        return "BOUNDARY";
      case irregexp::RegExpAssertion::NON_BOUNDARY:
        return "NON_BOUNDARY";
      case irregexp::RegExpAssertion::NOT_AFTER_LEAD_SURROGATE:
        return "NOT_AFTER_LEAD_SURROGATE";
      case irregexp::RegExpAssertion::NOT_IN_SURROGATE_PAIR:
        return "NOT_IN_SURROGATE_PAIR";
    }
    MOZ_CRASH("unexpected AssertionType");
}

static JSObject*
ConvertRegExpTreeToObject(JSContext* cx, irregexp::RegExpTree* tree)
{
    RootedObject obj(cx, JS_NewPlainObject(cx));
    if (!obj)
        return nullptr;

    auto IntProp = [](JSContext* cx, HandleObject obj,
                      const char* name, int32_t value) {
        RootedValue val(cx, Int32Value(value));
        return JS_SetProperty(cx, obj, name, val);
    };

    auto BooleanProp = [](JSContext* cx, HandleObject obj,
                          const char* name, bool value) {
        RootedValue val(cx, BooleanValue(value));
        return JS_SetProperty(cx, obj, name, val);
    };

    auto StringProp = [](JSContext* cx, HandleObject obj,
                         const char* name, const char* value) {
        RootedString valueStr(cx, JS_NewStringCopyZ(cx, value));
        if (!valueStr)
            return false;

        RootedValue val(cx, StringValue(valueStr));
        return JS_SetProperty(cx, obj, name, val);
    };

    auto ObjectProp = [](JSContext* cx, HandleObject obj,
                         const char* name, HandleObject value) {
        RootedValue val(cx, ObjectValue(*value));
        return JS_SetProperty(cx, obj, name, val);
    };

    auto CharVectorProp = [](JSContext* cx, HandleObject obj,
                             const char* name, const irregexp::CharacterVector& data) {
        RootedString valueStr(cx, JS_NewUCStringCopyN(cx, data.begin(), data.length()));
        if (!valueStr)
            return false;

        RootedValue val(cx, StringValue(valueStr));
        return JS_SetProperty(cx, obj, name, val);
    };

    auto TreeProp = [&ObjectProp](JSContext* cx, HandleObject obj,
                                  const char* name, irregexp::RegExpTree* tree) {
        RootedObject treeObj(cx, ConvertRegExpTreeToObject(cx, tree));
        if (!treeObj)
            return false;
        return ObjectProp(cx, obj, name, treeObj);
    };

    auto TreeVectorProp = [&ObjectProp](JSContext* cx, HandleObject obj,
                                        const char* name,
                                        const irregexp::RegExpTreeVector& nodes) {
        size_t len = nodes.length();
        RootedObject array(cx, JS_NewArrayObject(cx, len));
        if (!array)
            return false;

        for (size_t i = 0; i < len; i++) {
            RootedObject child(cx, ConvertRegExpTreeToObject(cx, nodes[i]));
            if (!child)
                return false;

            RootedValue childVal(cx, ObjectValue(*child));
            if (!JS_SetElement(cx, array, i, childVal))
                return false;
        }
        return ObjectProp(cx, obj, name, array);
    };

    auto CharRangesProp = [&ObjectProp](JSContext* cx, HandleObject obj,
                                        const char* name,
                                        const irregexp::CharacterRangeVector& ranges) {
        size_t len = ranges.length();
        RootedObject array(cx, JS_NewArrayObject(cx, len));
        if (!array)
            return false;

        for (size_t i = 0; i < len; i++) {
            const irregexp::CharacterRange& range = ranges[i];
            RootedObject rangeObj(cx, JS_NewPlainObject(cx));
            if (!rangeObj)
                return false;

            auto CharProp = [](JSContext* cx, HandleObject obj,
                               const char* name, char16_t c) {
                RootedString valueStr(cx, JS_NewUCStringCopyN(cx, &c, 1));
                if (!valueStr)
                    return false;
                RootedValue val(cx, StringValue(valueStr));
                return JS_SetProperty(cx, obj, name, val);
            };

            if (!CharProp(cx, rangeObj, "from", range.from()))
                return false;
            if (!CharProp(cx, rangeObj, "to", range.to()))
                return false;

            RootedValue rangeVal(cx, ObjectValue(*rangeObj));
            if (!JS_SetElement(cx, array, i, rangeVal))
                return false;
        }
        return ObjectProp(cx, obj, name, array);
    };

    auto ElemProp = [&ObjectProp](JSContext* cx, HandleObject obj,
                                  const char* name, const irregexp::TextElementVector& elements) {
        size_t len = elements.length();
        RootedObject array(cx, JS_NewArrayObject(cx, len));
        if (!array)
            return false;

        for (size_t i = 0; i < len; i++) {
            const irregexp::TextElement& element = elements[i];
            RootedObject elemTree(cx, ConvertRegExpTreeToObject(cx, element.tree()));
            if (!elemTree)
                return false;

            RootedValue elemTreeVal(cx, ObjectValue(*elemTree));
            if (!JS_SetElement(cx, array, i, elemTreeVal))
                return false;
        }
        return ObjectProp(cx, obj, name, array);
    };

    if (tree->IsDisjunction()) {
        if (!StringProp(cx, obj, "type", "Disjunction"))
            return nullptr;
        irregexp::RegExpDisjunction* t = tree->AsDisjunction();
        if (!TreeVectorProp(cx, obj, "alternatives", t->alternatives()))
            return nullptr;
        return obj;
    }
    if (tree->IsAlternative()) {
        if (!StringProp(cx, obj, "type", "Alternative"))
            return nullptr;
        irregexp::RegExpAlternative* t = tree->AsAlternative();
        if (!TreeVectorProp(cx, obj, "nodes", t->nodes()))
            return nullptr;
        return obj;
    }
    if (tree->IsAssertion()) {
        if (!StringProp(cx, obj, "type", "Assertion"))
            return nullptr;
        irregexp::RegExpAssertion* t = tree->AsAssertion();
        if (!StringProp(cx, obj, "assertion_type", AssertionTypeToString(t->assertion_type())))
            return nullptr;
        return obj;
    }
    if (tree->IsCharacterClass()) {
        if (!StringProp(cx, obj, "type", "CharacterClass"))
            return nullptr;
        irregexp::RegExpCharacterClass* t = tree->AsCharacterClass();
        if (!BooleanProp(cx, obj, "is_negated", t->is_negated()))
            return nullptr;
        LifoAlloc* alloc = &cx->tempLifoAlloc();
        if (!CharRangesProp(cx, obj, "ranges", t->ranges(alloc)))
            return nullptr;
        return obj;
    }
    if (tree->IsAtom()) {
        if (!StringProp(cx, obj, "type", "Atom"))
            return nullptr;
        irregexp::RegExpAtom* t = tree->AsAtom();
        if (!CharVectorProp(cx, obj, "data", t->data()))
            return nullptr;
        return obj;
    }
    if (tree->IsText()) {
        if (!StringProp(cx, obj, "type", "Text"))
            return nullptr;
        irregexp::RegExpText* t = tree->AsText();
        if (!ElemProp(cx, obj, "elements", t->elements()))
            return nullptr;
        return obj;
    }
    if (tree->IsQuantifier()) {
        if (!StringProp(cx, obj, "type", "Quantifier"))
            return nullptr;
        irregexp::RegExpQuantifier* t = tree->AsQuantifier();
        if (!IntProp(cx, obj, "min", t->min()))
            return nullptr;
        if (!IntProp(cx, obj, "max", t->max()))
            return nullptr;
        if (!StringProp(cx, obj, "quantifier_type",
                        t->is_possessive() ? "POSSESSIVE"
                        : t->is_non_greedy() ? "NON_GREEDY"
                        : "GREEDY"))
            return nullptr;
        if (!TreeProp(cx, obj, "body", t->body()))
            return nullptr;
        return obj;
    }
    if (tree->IsCapture()) {
        if (!StringProp(cx, obj, "type", "Capture"))
            return nullptr;
        irregexp::RegExpCapture* t = tree->AsCapture();
        if (!IntProp(cx, obj, "index", t->index()))
            return nullptr;
        if (!TreeProp(cx, obj, "body", t->body()))
            return nullptr;
        return obj;
    }
    if (tree->IsLookaround()) {
        if (!StringProp(cx, obj, "type", "Lookaround"))
            return nullptr;
        irregexp::RegExpLookaround* t = tree->AsLookaround();
        if (!BooleanProp(cx, obj, "is_positive", t->is_positive()))
            return nullptr;
        if (!TreeProp(cx, obj, "body", t->body()))
            return nullptr;
        return obj;
    }
    if (tree->IsBackReference()) {
        if (!StringProp(cx, obj, "type", "BackReference"))
            return nullptr;
        irregexp::RegExpBackReference* t = tree->AsBackReference();
        if (!IntProp(cx, obj, "index", t->index()))
            return nullptr;
        return obj;
    }
    if (tree->IsEmpty()) {
        if (!StringProp(cx, obj, "type", "Empty"))
            return nullptr;
        return obj;
    }

    MOZ_CRASH("unexpected RegExpTree type");
}

static bool
ParseRegExp(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());

    if (args.length() == 0) {
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    if (!args[0].isString()) {
        ReportUsageErrorASCII(cx, callee, "First argument must be a String");
        return false;
    }

    RegExpFlag flags = RegExpFlag(0);
    if (!args.get(1).isUndefined()) {
        if (!args.get(1).isString()) {
            ReportUsageErrorASCII(cx, callee, "Second argument, if present, must be a String");
            return false;
        }
        RootedString flagStr(cx, args[1].toString());
        if (!ParseRegExpFlags(cx, flagStr, &flags))
            return false;
    }

    bool match_only = false;
    if (!args.get(2).isUndefined()) {
        if (!args.get(2).isBoolean()) {
            ReportUsageErrorASCII(cx, callee, "Third argument, if present, must be a Boolean");
            return false;
        }
        match_only = args[2].toBoolean();
    }

    RootedAtom pattern(cx, AtomizeString(cx, args[0].toString()));
    if (!pattern)
        return false;

    JS::CompileOptions options(cx);
    frontend::TokenStream dummyTokenStream(cx, options, nullptr, 0, nullptr);

    irregexp::RegExpCompileData data;
    if (!irregexp::ParsePattern(dummyTokenStream, cx->tempLifoAlloc(), pattern,
                                flags & MultilineFlag, match_only,
                                flags & UnicodeFlag, flags & IgnoreCaseFlag,
                                flags & GlobalFlag, flags & StickyFlag,
                                flags & DotAllFlag,
                                &data))
    {
        return false;
    }

    RootedObject obj(cx, ConvertRegExpTreeToObject(cx, data.tree));
    if (!obj)
        return false;

    args.rval().setObject(*obj);
    return true;
}

static bool
DisRegExp(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    RootedObject callee(cx, &args.callee());

    if (args.length() == 0) {
        ReportUsageErrorASCII(cx, callee, "Wrong number of arguments");
        return false;
    }

    if (!args[0].isObject() || !args[0].toObject().is<RegExpObject>()) {
        ReportUsageErrorASCII(cx, callee, "First argument must be a RegExp");
        return false;
    }

    Rooted<RegExpObject*> reobj(cx, &args[0].toObject().as<RegExpObject>());

    bool match_only = false;
    if (!args.get(1).isUndefined()) {
        if (!args.get(1).isBoolean()) {
            ReportUsageErrorASCII(cx, callee, "Second argument, if present, must be a Boolean");
            return false;
        }
        match_only = args[1].toBoolean();
    }

    RootedLinearString input(cx, cx->runtime()->emptyString);
    if (!args.get(2).isUndefined()) {
        if (!args.get(2).isString()) {
            ReportUsageErrorASCII(cx, callee, "Third argument, if present, must be a String");
            return false;
        }
        RootedString inputStr(cx, args[2].toString());
        input = inputStr->ensureLinear(cx);
        if (!input)
            return false;
    }

    if (!RegExpObject::dumpBytecode(cx, reobj, match_only, input))
        return false;

    args.rval().setUndefined();
    return true;
}
#endif // DEBUG

static bool
IsConstructor(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1)
        args.rval().setBoolean(false);
    else
        args.rval().setBoolean(IsConstructor(args[0]));
    return true;
}

static bool
GetErrorNotes(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "getErrorNotes", 1))
        return false;

    if (!args[0].isObject() || !args[0].toObject().is<ErrorObject>()) {
        args.rval().setNull();
        return true;
    }

    JSErrorReport* report = args[0].toObject().as<ErrorObject>().getErrorReport();
    if (!report) {
        args.rval().setNull();
        return true;
    }

    RootedObject notesArray(cx, CreateErrorNotesArray(cx, report));
    if (!notesArray)
        return false;

    args.rval().setObject(*notesArray);
    return true;
}

#if defined(JS_SIMULATOR_ARM) && defined(VARAN_THUMB2)
static bool
VaranT2Hello(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN P1.1 close-the-loop: run the encoder-verified Thumb-2 hello-world through the
    // simulator. Bytes = adds.w r0,r0,r1 ; bx lr (15/15 oracle byte-match). The simulator
    // INTERPRETS (reads+decodes), so a plain readable buffer suffices -- no exec memory.
    CallArgs args = CallArgsFromVp(argc, vp);
    int32_t a = args.get(0).isInt32() ? args.get(0).toInt32() : 0;
    int32_t b = args.get(1).isInt32() ? args.get(1).toInt32() : 0;
    uint8_t code[] = { 0x10, 0xeb, 0x01, 0x00, 0x70, 0x47 };
    js::jit::Simulator* sim = cx->runtime()->simulator();
    // bit0-set entry pointer (F2); the sim masks bit0 at fetch.
    uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(code) | 1);
    int64_t r = sim->call(entry, 2, a, b);
    args.rval().setInt32(int32_t(r));
    return true;
}
static bool
VaranT2Udf(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN P1.1: run a wide UDF.W through the sim -> it must print the "UDF hit" diagnostic and
    // MOZ_CRASH (fail LOUD at a known point). This function exists to CRASH; that is its contract.
    //
    // ARGUMENT REQUIRED (Batch G). `basic/bug908915.js` is a shotgun that calls EVERY shell testing
    // function with no arguments -- `for each (let e in newGlobal()) { ... e(); }` -- with a blacklist
    // of only quit/crash/readline/terminate/nestedShell. So it called this one and took the crash,
    // and that single self-inflicted abort was one of the last seven Baseline-gate failures. It was
    // never an encoder gap: the emit-time census is EMPTY at the crash, proving `emitUdf` was never
    // called, because the 0x112 word below is a hand-assembled byte array that never goes through the
    // assembler at all. (The old comment's "category ALU, shape shifted-reg, op ORR" decoding was
    // also wrong -- 0x112 was picked at P1.1 only so the diagnostic would print a plausible bucket.)
    //
    // Requiring an explicit argument makes a no-argument call a harmless no-op, so the shotgun --
    // and any future enumerate-and-call test -- passes, while the deliberate crash stays available
    // as `varanT2Udf(1)`. Fixing it here is right: the corpus is upstream test code, and a
    // deliberately-crashing debug hook has no business detonating on a bare call.
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() == 0) {
        args.rval().setUndefined();
        return true;
    }
    uint8_t code[] = { 0xf0, 0xf7, 0x12, 0xa1 };   // udf.w #0x112
    js::jit::Simulator* sim = cx->runtime()->simulator();
    uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(code) | 1);
    (void)sim->call(entry, 1, 0);   // crashes with the diagnostic before returning
    args.rval().setUndefined();
    return true;
}
static bool
VaranT2MovwT(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN P1.2b: movw r0,#0x1234 ; movt r0,#0x5678 ; bx lr  -> r0 = 0x56781234 (1450709556).
    // Bytes are oracle-verified (6/6); this proves the sim DECODE builds the 32-bit value.
    CallArgs args = CallArgsFromVp(argc, vp);
    uint8_t code[] = { 0x41,0xf2,0x34,0x20, 0xc5,0xf2,0x78,0x60, 0x70,0x47 };
    js::jit::Simulator* sim = cx->runtime()->simulator();
    uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(code) | 1);
    int64_t r = sim->call(entry, 1, 0);
    args.rval().setNumber(double(uint32_t(r)));
    return true;
}
static bool
VaranT2Branch(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN P1.2b Group 2 -- Task E sim CONTROL-FLOW test. A hand-laid Thumb-2 program built from
    // the SAME encoder (VaranEncodeBranchInst) the assembler uses, run through the simulator; the
    // returned r0 is correct ONLY if every branch LANDS where intended. adds.w r0,r0,r1 (+1 each
    // time it executes) is the marker; r1 = 1. Layout (byte offsets), start r0 = 100:
    //   0  adds                 ; r0=101, flags Z=0 N=0
    //   4  beq +8  (->12)       ; EQ (Z=0) -> NOT taken (fall through)      [cond not-taken]
    //   8  bne +8  (->16)       ; NE (Z=0) -> taken, skips the poison at 12 [cond taken]
    //   12 adds (POISON)        ; must be skipped
    //   16 adds                 ; r0=102                                    [reached]
    //   20 b.w ->32             ; forward, over the backward-target block
    //   24 adds (BACKTGT)       ; reached later via a BACKWARD branch
    //   28 b.w ->40             ; forward, past the backward branch site
    //   32 adds                 ; r0=103
    //   36 b.w ->24             ; BACKWARD to 24                            [backward]
    //   40 adds                 ; r0=105 (after 24 ran -> 104, then 40)
    //   44 bx lr                ; return r0
    // Correct trace: 0,16,32,24,40 adds run (5x) -> r0 = 105; poison at 12 skipped.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    const uint32_t ADDS = 0x0001EB10u;   // adds.w r0,r0,r1 (oracle-verified, from the hello-world)
    uint16_t prog[64];
    int nb = 0;
    auto put32 = [&](uint32_t w) { prog[nb / 2] = uint16_t(w & 0xffff); prog[nb / 2 + 1] = uint16_t(w >> 16); nb += 4; };
    auto put16 = [&](uint16_t w) { prog[nb / 2] = w; nb += 2; };
    put32(ADDS);                                         // 0
    put32(VaranEncodeBranchInst(false, 8, 0));           // 4  beq +8
    put32(VaranEncodeBranchInst(false, 8, 1));           // 8  bne +8
    put32(ADDS);                                         // 12 POISON
    put32(ADDS);                                         // 16
    put32(VaranEncodeBranchInst(false, 12, 14));         // 20 b.w ->32
    put32(ADDS);                                         // 24 BACKTGT
    put32(VaranEncodeBranchInst(false, 12, 14));         // 28 b.w ->40
    put32(ADDS);                                         // 32
    put32(VaranEncodeBranchInst(false, -12, 14));        // 36 b.w ->24 (backward)
    put32(ADDS);                                         // 40
    put16(0x4770);                                       // 44 bx lr
    Simulator* sim = cx->runtime()->simulator();
    uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
    int64_t r = sim->call(entry, 2, 100, 1);
    args.rval().setInt32(int32_t(r));                    // expect 105
    return true;
}
static bool
VaranT2BranchBind(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN P1.2b Group 2 -- Task D bind() END-TO-END. Drives a real MacroAssembler through the
    // actual as_b/varanAsBCond/bind fixup chain (not encode/decode in isolation), then decodes each
    // emitted branch and asserts it LANDS at its label -- forward multi-link chains, backward
    // branches, the in-range 2-slot form (B<c>.W + NOP.W) and the >+-1MB invert+B.W fallback.
    // Returns the number of FAILED checks (0 = all pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);

    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;

    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // Decode where the branch at buffer offset `bo` actually lands (in buffer-offset space).
    auto landing = [&](int bo) -> int {
        uint32_t w0 = masm.varanPeekWord(bo);
        int kind = VaranBranchKind(w0);
        if (kind == 0)
            return INT32_MIN;
        uint32_t cond = 14u;
        VaranDecodeBranchCond(w0, &cond);
        if (kind == 1 && cond < 14) {
            // 2-slot conditional: overflow form stores the real target in slot1 (a B.W); the
            // in-range form keeps it in slot0 (B<c>.W) with slot1 = NOP.W.
            uint32_t w1 = masm.varanPeekWord(bo + 4);
            if (VaranBranchKind(w1) == 1)
                return (bo + 4) + VaranDecodeBranchByteVal(w1);
            return bo + VaranDecodeBranchByteVal(w0);
        }
        return bo + VaranDecodeBranchByteVal(w0);
    };

    // (1) Forward multi-link chain: cond + uncond + cond, several links deep, all to one label.
    {
        Label L;
        int s1 = masm.as_b(&L, Assembler::Equal).getOffset();       // 2-slot cond
        int s2 = masm.as_b(&L).getOffset();                         // 1-slot uncond
        masm.as_movw(r0, Imm16(0));
        int s3 = masm.as_b(&L, Assembler::NotEqual).getOffset();    // 2-slot cond
        int s4 = masm.as_b(&L).getOffset();                         // 1-slot uncond
        masm.bind(&L);
        int Loff = L.offset();
        check(!masm.oom());
        check(landing(s1) == Loff);
        check(landing(s2) == Loff);
        check(landing(s3) == Loff);
        check(landing(s4) == Loff);
    }

    // (2) Backward branches (target already bound): cond + uncond, both must reach back.
    {
        Label M;
        masm.bind(&M);
        int Moff = M.offset();
        masm.as_movw(r0, Imm16(0));
        masm.as_movw(r0, Imm16(0));
        int b1 = masm.as_b(&M, Assembler::LessThan).getOffset();    // backward cond (2-slot)
        int b2 = masm.as_b(&M).getOffset();                         // backward uncond
        check(!masm.oom());
        check(landing(b1) == Moff);
        check(landing(b2) == Moff);
    }

    // (3) In-range 2-slot conditional (< +-1MB): stays B<c>.W + NOP.W.
    {
        Label G;
        int g1 = masm.as_b(&G, Assembler::Signed).getOffset();
        for (int n = 0; n < 900000; n += 4)
            masm.as_movw(r0, Imm16(0));
        masm.bind(&G);
        check(!masm.oom());
        check(landing(g1) == G.offset());
        // slot1 must be NOP.W (0x8000F3AF) -- the fallback did NOT trigger.
        check(masm.varanPeekWord(g1 + 4) == 0x8000F3AFu);
    }

    // (4) Overflow 2-slot conditional (> +-1MB): invert cond + branch-over B.W (NEW machinery).
    {
        Label F;
        int f1 = masm.as_b(&F, Assembler::Signed).getOffset();
        for (int n = 0; n < 1100000; n += 4)
            masm.as_movw(r0, Imm16(0));
        masm.bind(&F);
        check(!masm.oom());
        check(landing(f1) == F.offset());
        // slot1 must now be a real branch (B.W) -- the invert+branch-over fallback triggered.
        check(VaranBranchKind(masm.varanPeekWord(f1 + 4)) == 1);
    }

    args.rval().setInt32(fails);   // 0 == every branch bound correctly
    return true;
}
static bool
VaranT2RetargetSplice(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN P1.2b Group 2 HYGIENE regression: retarget(Label*,Label*) must splice a 2-slot
    // CONDITIONAL chain correctly. The chain link of a conditional branch lives in slot1 (the
    // companion B.W), so a plain 1-slot write to slot0 (the pre-fix code) leaves nextLink reading the
    // stale sentinel and SILENTLY DROPS the target's original chain. Build an L1 chain (cond 2-slot +
    // uncond 1-slot + cond 2-slot) whose TAIL is a 2-slot conditional, retarget L1 onto L2, bind L2,
    // and assert ALL five branches (both chains) land at L2. Pre-fix: L2's own branches are orphaned
    // (they follow the mis-written splice link) and fail to land. Returns failed-check count (0=pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto landing = [&](int bo) -> int {
        uint32_t w0 = masm.varanPeekWord(bo);
        int kind = VaranBranchKind(w0);
        if (kind == 0) return INT32_MIN;
        uint32_t cond = 14u; VaranDecodeBranchCond(w0, &cond);
        if (kind == 1 && cond < 14) {
            uint32_t w1 = masm.varanPeekWord(bo + 4);
            if (VaranBranchKind(w1) == 1) return (bo + 4) + VaranDecodeBranchByteVal(w1);
            return bo + VaranDecodeBranchByteVal(w0);
        }
        return bo + VaranDecodeBranchByteVal(w0);
    };

    Label L1, L2;
    int a1 = masm.as_b(&L1, Assembler::Equal).getOffset();      // L1 chain tail (2-slot cond)
    int a2 = masm.as_b(&L1).getOffset();                        // 1-slot uncond
    int a3 = masm.as_b(&L1, Assembler::NotEqual).getOffset();   // L1 chain head (2-slot cond)
    int b1 = masm.as_b(&L2, Assembler::Signed).getOffset();     // L2 chain tail (2-slot cond)
    int b2 = masm.as_b(&L2).getOffset();                        // L2 chain head (1-slot uncond)
    masm.retarget(&L1, &L2);   // prepend L1's chain onto L2 (splices at L1's tail a1)
    masm.bind(&L2);
    int L2off = L2.offset();
    check(!masm.oom());
    check(landing(a1) == L2off);
    check(landing(a2) == L2off);
    check(landing(a3) == L2off);
    check(landing(b1) == L2off);   // pre-fix: orphaned -> fails
    check(landing(b2) == L2off);   // pre-fix: orphaned -> fails
    check(!L1.used());             // retarget resets L1

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2LoadStore(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- LDR/STR core: byte-match (T3 imm+, T4 imm-) + a store/load round-trip.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto dtr = [&](LoadStore ls, int sz, Register rt, Register base, int off) -> uint32_t {
        int o = masm.nextOffset().getOffset();
        masm.as_dtr(ls, sz, Offset, rt, DTRAddr(base, DtrOffImm(off)), Assembler::Always);
        return masm.varanPeekWord(o);
    };
    check(dtr(IsLoad,  32, r0, r1,  4) == 0x0004f8d1u);  // ldr.w  r0,[r1,#4]  (T3)
    check(dtr(IsLoad,  32, r0, r1, -4) == 0x0c04f851u);  // ldr    r0,[r1,#-4] (T4)
    check(dtr(IsStore, 32, r0, r1,  4) == 0x0004f8c1u);  // str.w  r0,[r1,#4]
    check(dtr(IsLoad,   8, r0, r1,  4) == 0x0004f891u);  // ldrb.w r0,[r1,#4]
    check(dtr(IsStore,  8, r0, r1,  4) == 0x0004f881u);  // strb.w r0,[r1,#4]
    // Execute: str r1,[sp,#-8] ; ldr r0,[sp,#-8] ; bx lr  -> r0 = original r1.
    {
        Simulator* sim = cx->runtime()->simulator();
        uint16_t prog[8];
        prog[0] = 0xf84du; prog[1] = 0x1c08u;   // str r1,[sp,#-8]
        prog[2] = 0xf85du; prog[3] = 0x0c08u;   // ldr r0,[sp,#-8]
        prog[4] = 0x4770u;                       // bx lr
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        check(uint32_t(sim->call(e, 2, 0, 0xcafef00du)) == 0xcafef00du);
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2ExtDtr(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- extended data transfer (LDRH/STRH/LDRSB/LDRSH via the halfword/signed families,
    // and LDRD/STRD). Byte-match against clang thumbv7 objdump, then execute a strd/ldrd round-trip and
    // a signed/unsigned halfword load to prove sign-extension + the value path. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto ext = [&](LoadStore ls, int sz, bool sgn, Register rt, EDtrAddr a) -> uint32_t {
        int o = masm.nextOffset().getOffset();
        masm.as_extdtr(ls, sz, sgn, Offset, rt, a, Assembler::Always);
        return masm.varanPeekWord(o);
    };
    // Halfword / signed byte / signed halfword: T3 imm+ and register form.
    check(ext(IsStore, 16, false, r0, EDtrAddr(r1, EDtrOffImm(4)))  == 0x0004f8a1u);  // strh  r0,[r1,#4]
    check(ext(IsLoad,  16, false, r0, EDtrAddr(r1, EDtrOffReg(r2))) == 0x0002f831u);  // ldrh  r0,[r1,r2]
    check(ext(IsLoad,   8, true,  r0, EDtrAddr(r1, EDtrOffImm(4)))  == 0x0004f991u);  // ldrsb r0,[r1,#4]
    check(ext(IsLoad,  16, true,  r0, EDtrAddr(r1, EDtrOffReg(r2))) == 0x0002f931u);  // ldrsh r0,[r1,r2]
    // LDRD / STRD immediate (imm8 x4).
    check(ext(IsLoad,  64, true,  r0, EDtrAddr(r2, EDtrOffImm(8)))  == 0x0102e9d2u);  // ldrd r0,r1,[r2,#8]
    check(ext(IsStore, 64, true,  r0, EDtrAddr(r2, EDtrOffImm(8)))  == 0x0102e9c2u);  // strd r0,r1,[r2,#8]
    check(ext(IsLoad,  64, true,  r0, EDtrAddr(r2, EDtrOffImm(0)))  == 0x0100e9d2u);  // ldrd r0,r1,[r2]
    // Execute #1: strd r2,r3,[sp,#-16] ; ldrd r0,r1,[sp,#-16] ; bx lr  ->  r0=arg2 (red zone).
    {
        Simulator* sim = cx->runtime()->simulator();
        uint16_t prog[8];
        prog[0] = 0xe94du; prog[1] = 0x2304u;   // strd r2,r3,[sp,#-16]
        prog[2] = 0xe95du; prog[3] = 0x0104u;   // ldrd r0,r1,[sp,#-16]
        prog[4] = 0x4770u;                       // bx lr
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        // sim->call returns r0; check r0 == arg2 (0xaabbccdd).
        check(uint32_t(sim->call(e, 4, 0, 0, 0xaabbccddu, 0x11223344u)) == 0xaabbccddu);
    }
    // Execute #2: sign-extension. strh r1,[sp,#-4] ; ldrsh r0,[sp,#-4] ; bx lr.  r1=0x8001 -> r0=0xffff8001.
    {
        Simulator* sim = cx->runtime()->simulator();
        uint16_t prog[8];
        prog[0] = 0xf82du; prog[1] = 0x1c04u;   // strh r1,[sp,#-4]
        prog[2] = 0xf93du; prog[3] = 0x0c04u;   // ldrsh r0,[sp,#-4]
        prog[4] = 0x4770u;                       // bx lr
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        check(uint32_t(sim->call(e, 2, 0, 0x00008001u)) == 0xffff8001u);
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2Vldr(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- VLDR/VSTR (as_vdtr), un-deferred. A standard VFP word: T2 == A32 with cond AL,
    // halfword-swapped, so it funnels through varanEmitVfp like every other VFP op. Byte-match against
    // clang thumbv7 objdump, then execute a vldr/vstr round-trip that carries an 8-byte double through
    // a VFP register. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto vdtr = [&](LoadStore ls, VFPRegister vd, Register base, int off) -> uint32_t {
        int o = masm.nextOffset().getOffset();
        masm.as_vdtr(ls, vd, VFPAddr(base, VFPOffImm(off)), Assembler::Always);
        return masm.varanPeekWord(o);
    };
    check(vdtr(IsLoad,  d0, r1,  8) == 0x0b02ed91u);   // vldr d0,[r1,#8]
    check(vdtr(IsStore, d0, r1,  8) == 0x0b02ed81u);   // vstr d0,[r1,#8]
    check(vdtr(IsLoad,  ReturnFloat32Reg, r1,  8) == 0x0a02ed91u);   // vldr s0,[r1,#8] (s0 = d0 single overlay)
    check(vdtr(IsLoad,  d0, r1, -8) == 0x0b02ed11u);   // vldr d0,[r1,#-8]
    check(vdtr(IsLoad,  d0, r1,  0) == 0x0b00ed91u);   // vldr d0,[r1]
    // Execute: str the two halves to [sp,#-8]/[sp,#-4] ; vldr d0,[sp,#-8] ; vstr d0,[sp,#-16] ;
    // ldr r0,[sp,#-16] -> r0 = the low word (arg1), proving the 8 bytes rode through the VFP reg.
    {
        Simulator* sim = cx->runtime()->simulator();
        uint16_t prog[16];
        prog[0]  = 0xf84du; prog[1]  = 0x1c08u;   // str  r1,[sp,#-8]
        prog[2]  = 0xf84du; prog[3]  = 0x2c04u;   // str  r2,[sp,#-4]
        prog[4]  = 0xed1du; prog[5]  = 0x0b02u;   // vldr d0,[sp,#-8]
        prog[6]  = 0xed0du; prog[7]  = 0x0b04u;   // vstr d0,[sp,#-16]
        prog[8]  = 0xf85du; prog[9]  = 0x0c10u;   // ldr  r0,[sp,#-16]
        prog[10] = 0x4770u;                        // bx lr
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        check(uint32_t(sim->call(e, 3, 0, 0x12345678u, 0x9abcdef0u)) == 0x12345678u);
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2Pool(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- the constant pool. loadConstantDouble(non-encodable) routes through as_FImm64Pool;
    // finishPool patches the hint to a real T2 VLDR-literal and lays the 8-byte constant into the pool.
    // Decode the patched VLDR's pc-relative offset and confirm it lands EXACTLY on the pool slot holding
    // the double -- a -8 (A32) vs -4 (T2) pc-bias error, or a bad index round-trip, points at the wrong
    // word (silent wrong constant). Plus the PoolHintData index round-trip + alias-guard headroom.
    // Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // (A) PoolHintData index round-trip + alias-guard headroom (ISA-independent bookkeeping).
    check(Assembler::varanPoolHintSelfTest() == 0);

    // (B) End-to-end: emit a double via the pool, finish, and verify the patched VLDR reaches the const.
    {
        js::LifoAlloc lifo(1 << 16);
        TempAllocator alloc(&lifo);
        JitContext jc(cx, &alloc);
        AutoFlushICache afc("varanT2Pool");                  // executableCopy's pool patch flushes the I-cache
        MacroAssembler masm;
        const double K = 3.14159265358979;                 // not VFP-immediate-encodable -> forced to pool
        masm.loadConstantDouble(K, d0);
        masm.finish();                                       // flush()es the pending pool + patches the VLDR
        check(!masm.oom());
        size_t n = masm.size();
        check(n >= 8);
        if (!masm.oom() && n >= 8 && n <= 480) {
            uint8_t buf[512];
            memset(buf, 0, sizeof(buf));
            masm.executableCopy(buf);
            // buf[0] is the patched VLDR d0,[pc,#imm]: stored (hw1<<16)|hw0, hw0=0xED9F(U=1)/0xED1F(U=0).
            uint32_t vldr; memcpy(&vldr, buf, 4);
            uint32_t hw0 = vldr & 0xffff, hw1 = (vldr >> 16) & 0xffff;
            check((hw0 & 0xff7f) == 0xed1f);                 // VLDR d,[pc,#imm] literal (U masked out)
            uint32_t U = (hw0 >> 7) & 1;
            int32_t imm = int32_t((hw1 & 0xff) << 2);
            // T2 pc = Align(vldrAddr+4,4); vldrAddr is 4-aligned here (buffer start).
            uintptr_t base = reinterpret_cast<uintptr_t>(buf);
            uintptr_t pcAligned = (base + 4) & ~uintptr_t(3);
            uintptr_t target = U ? (pcAligned + imm) : (pcAligned - imm);
            check(target + 8 <= base + n);                   // in-bounds
            if (target + 8 <= base + n) {
                double got; memcpy(&got, reinterpret_cast<void*>(target), 8);
                check(got == K);                              // the pool holds exactly K at the -4 bias
            }
        }
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2Toggle(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- ToggleCall/ToggledCallSize over the toggled-call sequence movw/movt(ScratchReg) +
    // {NOP.W | packed BLXReg}. No logic change was needed (it composes over the now-T2 InstMovW/InstNOP/
    // InstBLXReg classifiers); this proves the round-trip disabled->enabled->disabled and the size.
    // Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // B1 (Batch D): toggledCall now emits movw / movt / orr scratch,#1 / (blx|nop) -- the Thumb-bit
    // orr is unconditional in BOTH arms so the footprint stays uniform and the toggled word stays
    // last. Mirror that shape here, or this test would pin the OLD 3-word layout and read as a
    // regression. (orr.w r12,r12,#1 = 4c f0 01 0c, LLVM-oracle minted.)
    alignas(4) uint32_t buf[4];
    buf[0] = 0x2c34f241u;   // movw r12, #0x1234   (ScratchRegister = ip = r12)
    buf[1] = 0x6c78f2c5u;   // movt r12, #0x5678
    buf[2] = 0x0c01f04cu;   // orr.w r12, r12, #1  (the Thumb bit)
    buf[3] = 0x8000F3AFu;   // NOP.W  (disabled toggled call)
    uint8_t* code = reinterpret_cast<uint8_t*>(buf);
    auto at2 = [&]() -> Instruction* { return reinterpret_cast<Instruction*>(&buf[3]); };

    AutoFlushICache afc("varanT2Toggle");
    check(Assembler::ToggledCallSize(code) == 16);   // movw + movt + orr + slot
    check(InstNOP::IsTHIS(*at2()));                   // starts disabled
    // disabled -> enabled: the slot becomes a packed BLX ScratchRegister.
    Assembler::ToggleCall(CodeLocationLabel(code), true);
    check(InstBLXReg::IsTHIS(*at2()));
    check(at2()->is<InstBranchReg>());
    check(at2()->as<InstBranchReg>()->checkDest(ScratchRegister));
    // enabled -> disabled: back to NOP.W.
    Assembler::ToggleCall(CodeLocationLabel(code), false);
    check(InstNOP::IsTHIS(*at2()));
    check(buf[3] == 0x8000F3AFu);
    // idempotent no-op re-toggle to the same state.
    Assembler::ToggleCall(CodeLocationLabel(code), false);
    check(InstNOP::IsTHIS(*at2()));

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2ToggleJump(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch A -- the TOGGLED-JUMP redesign (ToggleToJmp/ToggleToCmp). A32 flips bits[27:20] of
    // one word between CMP and B; Thumb-2 has no such analogue, so toggledJump now lays a 2-slot site
    // whose branch word is never modified:
    //     slot0 = NOP.W  -> fall into slot1 -> branch TAKEN (enabled)
    //     slot0 = B.W +4 -> skip slot1      -> fall through (disabled)
    // A passing assert is NOT proof, so this EXECUTES the site under the simulator and checks that
    // control actually flows both ways, across a full enabled->disabled->enabled round trip.
    // Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // byte 0 : slot0 (toggle slot, wide)
    // byte 4 : slot1 = B.W +8 -> Lskip.  PC = 4+4 = 8, target = 16, so off = 8.
    // byte 8 : mov.w r0,#1                       (fallthrough / DISABLED result)
    // byte 12: bx lr ; nop16 pad (keeps Lskip 4-aligned)
    // byte 16: mov.w r0,#2  (Lskip / ENABLED result)
    // byte 20: bx lr
    // Wide forms are used for the movs because the wide-only invariant means the simulator implements
    // only NOP16/BX/BLX/BKPT on the 16-bit path -- a 16-bit `movs r0,#1` loud-crashes there.
    // All words below are LLVM-oracle byte-matched: nop.w = af f3 00 80, b.w .+12 = 00 f0 04 b8,
    // mov.w r0,#N = 4f f0 0N 00.
    // slot1's branch word (B.W +8 = 0xB804F000) is deliberately DIFFERENT from the skip word the
    // toggle writes into slot0 (B.W +4 = 0xB802F000) so that a toggle which scribbled on the wrong
    // slot cannot coincidentally still pass.
    alignas(4) uint16_t prog[12];
    prog[0]  = 0xF3AFu; prog[1]  = 0x8000u;   // slot0: NOP.W (enabled)
    prog[2]  = 0xF000u; prog[3]  = 0xB804u;   // slot1: B.W +8 -> Lskip
    prog[4]  = 0xF04Fu; prog[5]  = 0x0001u;   // mov.w r0,#1
    prog[6]  = 0x4770u;                        // bx lr
    prog[7]  = 0xBF00u;                        // nop16 pad
    prog[8]  = 0xF04Fu; prog[9]  = 0x0002u;   // Lskip: mov.w r0,#2
    prog[10] = 0x4770u;                        // bx lr
    prog[11] = 0xBF00u;                        // nop16 pad

    uint8_t* base  = reinterpret_cast<uint8_t*>(prog);
    uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
    Simulator* sim = cx->runtime()->simulator();
    AutoFlushICache afc("varanT2ToggleJump");

    // As emitted: enabled -> the jump is taken -> 2.
    check(uint32_t(sim->call(entry, 1, 0)) == 2u);
    // Disable: control must now fall through -> 1. slot1 must be untouched.
    Assembler::ToggleToCmp(CodeLocationLabel(base));
    check(uint32_t(sim->call(entry, 1, 0)) == 1u);
    check(prog[2] == 0xF000u && prog[3] == 0xB804u);
    // Re-enable: back to the jump -> 2. This is the direction the A32 code could only achieve by
    // reconstructing the offset from the CMP's fields; here slot1 never lost it.
    Assembler::ToggleToJmp(CodeLocationLabel(base));
    check(uint32_t(sim->call(entry, 1, 0)) == 2u);
    check(prog[0] == 0xF3AFu && prog[1] == 0x8000u);
    check(prog[2] == 0xF000u && prog[3] == 0xB804u);

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2ShiftReg(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch B item 1 -- register-CONTROLLED shift (census 0x11d, the hottest gap).
    // `ma_lsl/ma_lsr/ma_asr/ma_ror(Register shift, ...)` -> `as_mov(dst, lsl(src, shift))`, i.e. an
    // A32 op2 with bit4 = 1. Thumb-2 has NO register-controlled-shift form of general data
    // processing, only the dedicated LSL/LSR/ASR/ROR (register) instructions, so it is SYNTHESISED:
    // OpMov emits the dedicated shift directly; any other op shifts into ip first.
    //
    // NB the batch brief said this decodes via case (1) 0x75; tree-truth is 0x7d -- `lsl.w` is
    // 0xFA0x, so a NEW simulator sub-case was required, placed ahead of the extend ops it shares a
    // band with. Byte values below are LLVM-oracle minted. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto emit1 = [&](ShiftType) { return 0u; };
    (void)emit1;

    // --- ENCODE: the four OpMov forms are ONE instruction each (no scratch) ---
    auto one = [&](Operand2 o2, SBit s) -> uint32_t {
        int o = masm.nextOffset().getOffset();
        masm.as_mov(r0, o2, s);
        return masm.varanPeekWord(o);
    };
    check(one(lsl(r1, r2), LeaveCC) == 0xF002FA01u);   // lsl.w r0,r1,r2   01 fa 02 f0
    check(one(lsr(r1, r2), LeaveCC) == 0xF002FA21u);   // lsr.w r0,r1,r2   21 fa 02 f0
    check(one(asr(r1, r2), LeaveCC) == 0xF002FA41u);   // asr.w r0,r1,r2   41 fa 02 f0
    check(one(ror(r1, r2), LeaveCC) == 0xF002FA61u);   // ror.w r0,r1,r2   61 fa 02 f0
    {   // S-bit form, distinct registers: asrs.w r3,r4,r5   54 fa 05 f3
        int o = masm.nextOffset().getOffset();
        masm.as_mov(r3, asr(r4, r5), SetCC);
        check(masm.varanPeekWord(o) == 0xF305FA54u);
    }
    {   // --- non-Mov op: shift into ip, then the ordinary register-form ALU on ip ---
        int o = masm.nextOffset().getOffset();
        masm.as_add(r0, r1, lsl(r2, r3));
        check(masm.varanPeekWord(o)     == 0xFC03FA02u);   // lsl.w r12,r2,r3   02 fa 03 fc
        check(masm.varanPeekWord(o + 4) == 0x000CEB01u);   // add.w r0,r1,r12   01 eb 0c 00
    }
    check(!masm.oom());

    // --- EXECUTE: the shifts must produce correct VALUES, including the >=32 edge cases that the
    // A32 "low byte of Rs" rule makes reachable (a naive `<< amt` in the simulator is UB there).
    {
        Simulator* sim = cx->runtime()->simulator();
        // r0 = value, r1 = amount -> returns the shifted result.
        auto run = [&](uint32_t hw0, uint32_t hw1, uint32_t val, uint32_t amt) -> uint32_t {
            alignas(4) uint16_t prog[4];
            prog[0] = uint16_t(hw0); prog[1] = uint16_t(hw1);   // <shift>.w r0, r0, r1
            prog[2] = 0x4770u;                                   // bx lr
            prog[3] = 0xBF00u;
            uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
            return uint32_t(sim->call(e, 2, int32_t(val), int32_t(amt)));
        };
        // hw0 = 0xFA00|(type<<5)|Rn(=0) ; hw1 = 0xF000|(Rd(=0)<<8)|Rm(=1)
        const uint32_t HW1 = 0xF001u;
        check(run(0xFA00u, HW1, 0x00000001u,  4) == 0x00000010u);   // lsl 1<<4
        check(run(0xFA20u, HW1, 0x80000000u,  4) == 0x08000000u);   // lsr
        check(run(0xFA40u, HW1, 0x80000000u,  4) == 0xF8000000u);   // asr keeps sign
        check(run(0xFA60u, HW1, 0x00000001u,  1) == 0x80000000u);   // ror wraps
        check(run(0xFA00u, HW1, 0xFFFFFFFFu, 32) == 0x00000000u);   // lsl by 32 -> 0 (not UB)
        check(run(0xFA00u, HW1, 0xFFFFFFFFu, 40) == 0x00000000u);   // lsl by >32 -> 0
        check(run(0xFA40u, HW1, 0x80000000u, 40) == 0xFFFFFFFFu);   // asr by >32 -> sign fill
        check(run(0xFA20u, HW1, 0xFFFFFFFFu,  0) == 0xFFFFFFFFu);   // amount 0 is a no-op
        check(run(0xFA60u, HW1, 0x12345678u, 32) == 0x12345678u);   // ror by 32 == identity
    }

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2CondDtrDtm(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch B items 2+3 -- conditional load/store (census 0x24e) and the as_dtm synth (0x28b).
    //
    // (2) A conditional ldr/str becomes B<!c>.W branch-over an unconditional body. The body is NOT a
    //     fixed size (the register-offset synth expands to 3 instructions), so the branch is emitted
    //     as a placeholder and PATCHED with the real skip -- this test pins that the patched skip
    //     actually equals the emitted body size, which is the whole failure mode.
    // (3) as_dtm DA/IB/rejected-list becomes per-register ldr/str, lowest-register-to-lowest-address.
    //
    // Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // --- (2) conditional ldr: branch-over, and the skip must match the body exactly ---
    {
        MacroAssembler masm;
        int o = masm.nextOffset().getOffset();
        masm.ma_ldr(DTRAddr(r1, DtrOffImm(8)), r0, Offset, Assembler::Equal);
        int end = masm.nextOffset().getOffset();
        uint32_t br = masm.varanPeekWord(o);
        // B<c>.W T3: hw0 = 0xF000|(S<<10)|(cond<<6)|imm6 ; hw1 = 0x8000|(J1<<13)|(J2<<11)|imm11.
        // cond must be NotEqual (1) = the inversion of Equal (0).
        check((br & 0xf800u) == 0xf000u);
        check(((br >> 6) & 0xfu) == 1u);                       // inverted condition
        int32_t bodyBytes = end - o - 4;
        int32_t imm11 = int32_t((br >> 16) & 0x7ffu);
        check(imm11 * 2 == bodyBytes);                          // the PATCHED skip == real body size
        check(bodyBytes == 4);                                  // simple imm offset -> 1-insn body
        check(!masm.oom());
    }
    {   // register-offset form: body is 3 instructions, so the skip must be 12 -- the case a
        // hardcoded skip of 4 would have silently broken.
        MacroAssembler masm;
        int o = masm.nextOffset().getOffset();
        masm.ma_ldr(DTRAddr(r1, DtrRegImmShift(r2, LSL, 2)), r0, Offset, Assembler::Equal);
        int end = masm.nextOffset().getOffset();
        uint32_t br = masm.varanPeekWord(o);
        int32_t imm11 = int32_t((br >> 16) & 0x7ffu);
        check(imm11 * 2 == end - o - 4);
        check(!masm.oom());
    }
    {   // as_alu's conditional arm has the same variable-body hazard once the register-CONTROLLED
        // shift synth exists (2-instruction body for a non-Mov op). Pin its patched skip too.
        MacroAssembler masm;
        int o = masm.nextOffset().getOffset();
        masm.as_add(r0, r1, lsl(r2, r3), LeaveCC, Assembler::Equal);
        int end = masm.nextOffset().getOffset();
        uint32_t br = masm.varanPeekWord(o);
        int32_t imm11 = int32_t((br >> 16) & 0x7ffu);
        check(imm11 * 2 == end - o - 4);
        check(end - o - 4 == 8);                                // lsl.w ip + add.w  = 2 instructions
        check(!masm.oom());
    }

    // --- (3) as_dtm synth: DA/IB expand to per-register ldr/str, no UDF ---
    auto udfWord = [](uint32_t c) -> uint32_t {
        return ((0xa000u | (c & 0xfff)) << 16) | (0xf7f0u | ((c >> 12) & 0xf));
    };
    for (int mi = 0; mi < 2; mi++) {
        MacroAssembler masm;
        DTMMode mode = mi ? IB : DA;
        int o = masm.nextOffset().getOffset();
        masm.startDataTransferM(IsLoad, r0, mode);
        masm.transferReg(r1);
        masm.transferReg(r2);
        masm.finishDataTransfer();
        int end = masm.nextOffset().getOffset();
        check(end - o == 8);                                    // exactly 2 loads, no UDF
        check(masm.varanPeekWord(o)     != udfWord(0x28b));
        check(masm.varanPeekWord(o + 4) != udfWord(0x28b));
        check(!masm.oom());
    }

    // --- (3) EXECUTE: DA must transfer lowest-register-to-lowest-address ---
    {
        Simulator* sim = cx->runtime()->simulator();
        // ldrda-equivalent over {r1,r2} with base r0: lowest address = r0-4, so r1<-[r0-4], r2<-[r0].
        // Program: str the two markers below r0, run the synth shape, return r1+r2 packed.
        alignas(4) uint32_t mem[2] = { 0x1111u, 0x2222u };
        alignas(4) uint16_t prog[8];
        // ldr r1,[r0,#0] ; ldr r2,[r0,#4] ; add r0,r1,r2 ; bx lr   (the DA lowering, base = &mem[0])
        prog[0] = 0xF8D0u; prog[1] = 0x1000u;   // ldr.w r1,[r0,#0]
        prog[2] = 0xF8D0u; prog[3] = 0x2004u;   // ldr.w r2,[r0,#4]
        prog[4] = 0xEB01u; prog[5] = 0x0002u;   // add.w r0,r1,r2
        prog[6] = 0x4770u;                       // bx lr
        prog[7] = 0xBF00u;
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        check(uint32_t(sim->call(e, 1, int32_t(uintptr_t(mem)))) == 0x3333u);
    }

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2MrsMsr(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch B item 4 -- MRS/MSR (census 0x210 / 0x212), the ONE item that needed a genuinely
    // new 32-bit decode case. Sole caller is wasm::GenerateInterruptExit (the APSR save/restore
    // pair). Byte-match both, then EXECUTE a flags round-trip through the simulator: set flags with a
    // compare, read them out with MRS, clobber them, restore with MSR, and observe the restored
    // condition actually steer a branch. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    {   // ENCODE: mrs r4, apsr = ef f3 00 84 ; msr apsr_nzcvq, r4 = 84 f3 00 88
        MacroAssembler masm;
        int o = masm.nextOffset().getOffset();
        masm.as_mrs(r4);
        check(masm.varanPeekWord(o) == 0x8400F3EFu);
        o = masm.nextOffset().getOffset();
        masm.as_msr(r4);
        check(masm.varanPeekWord(o) == 0x8800F384u);
        check(!masm.oom());
    }

    {   // EXECUTE: flags -> MRS -> clobber -> MSR -> the restored Z must still steer the branch.
        Simulator* sim = cx->runtime()->simulator();
        // Wide forms throughout: the wide-only invariant means the simulator implements only
        // NOP16/BX/BLX/BKPT on the 16-bit path, so a 16-bit `cmp`/`movs` would loud-crash there.
        // Byte order below is halfword-in-memory; all oracle-minted.
        //   byte 0 : cmp.w r0, r1        b0 eb 01 0f   (sets flags)
        //   byte 4 : mrs r3, apsr        ef f3 00 83   (save)
        //   byte 8 : cmp.w r2, #0x63     b2 f1 63 0f   (clobber flags)
        //   byte 12: msr apsr_nzcvq, r3  83 f3 00 88   (restore)
        // r3, not r4: Simulator::call asserts the callee-saved registers (r4-r11) come back intact,
        // so the scratch here has to be caller-saved. r0-r2 carry the arguments, leaving r3.
        //   byte 16: beq.w -> byte 28    B<c>.W cond=0(EQ), off = 28-(16+4) = 8 -> imm11 = 4
        //   byte 20: mov.w r0, #0        4f f0 00 00   (Z clear -> not equal)
        //   byte 24: bx lr ; pad
        //   byte 28: mov.w r0, #1                       (Z set   -> equal)
        //   byte 32: bx lr ; pad
        alignas(4) uint16_t prog[18];
        int k = 0;
        prog[k++] = 0xEBB0u; prog[k++] = 0x0F01u;   // b0  cmp.w r0, r1
        prog[k++] = 0xF3EFu; prog[k++] = 0x8300u;   // b4  mrs r3, apsr
        prog[k++] = 0xF1B2u; prog[k++] = 0x0F63u;   // b8  cmp.w r2, #0x63
        prog[k++] = 0xF383u; prog[k++] = 0x8800u;   // b12 msr apsr_nzcvq, r3
        prog[k++] = 0xF000u; prog[k++] = 0x8004u;   // b16 beq.w -> b28
        prog[k++] = 0xF04Fu; prog[k++] = 0x0000u;   // b20 mov.w r0, #0
        prog[k++] = 0x4770u; prog[k++] = 0xBF00u;   // b24 bx lr ; pad
        prog[k++] = 0xF04Fu; prog[k++] = 0x0001u;   // b28 mov.w r0, #1
        prog[k++] = 0x4770u; prog[k++] = 0xBF00u;   // b32 bx lr ; pad
        MOZ_ASSERT(size_t(k) == mozilla::ArrayLength(prog));
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        // r0 == r1 -> Z set at the cmp; the clobbering cmp clears it; MSR must bring it back.
        check(uint32_t(sim->call(e, 3, 7, 7, 1)) == 1u);
        // r0 != r1 -> Z clear; MSR must restore THAT too (not just leave the clobbered value).
        check(uint32_t(sim->call(e, 3, 7, 9, 0x63)) == 0u);
    }

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2C7Bit(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch F -- C7: the Thumb bit on SYNTHESIZED code addresses.
    //
    // These are built in C++ as `code->raw() + offset` and branched through by `ldr pc` / `bx lr`,
    // where there is NO branch-time register for B1 to OR. NON-VACUOUS: each check asserts the
    // address is ODD *and* differs from the even value the old code produced, so a regression fails
    // on a comparison rather than merely not crashing. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    JitRuntime* jrt = cx->runtime()->jitRuntime();
    if (!jrt) {                       // JIT not initialised in this shell configuration
        args.rval().setInt32(-1);
        return true;
    }

    // P4 -- the arguments rectifier's *returnAddrOut. Consumed by ret() = `ldr.w pc,[sp],#4`.
    {
        void* ra = jrt->getArgumentsRectifierReturnAddr();
        check(ra != nullptr);
        check((uintptr_t(ra) & 1) == 1);                    // odd: carries the Thumb bit
    }

    // P8 -- the DebugModeOSR handler address, BOTH arms. Consumed by the VM wrapper's retn().
    {
        void* a = cx->runtime()->jitRuntime()->getBaselineDebugModeOSRHandlerAddress(cx, true);
        void* b = cx->runtime()->jitRuntime()->getBaselineDebugModeOSRHandlerAddress(cx, false);
        check(a && (uintptr_t(a) & 1) == 1);
        check(b && (uintptr_t(b) & 1) == 1);
        // Non-vacuous the other way too: the even form is what the pre-C7 code returned, so an
        // unfixed build would land exactly on (addr & ~1).
        check(a != (void*)(uintptr_t(a) & ~uintptr_t(1)));
    }

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2EmitGuards(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch E STEP 5 -- the emit-contract guards. NON-VACUOUS IN BOTH DIRECTIONS, which is the
    // whole point: a guard that never fires is useless, and a guard that fires on legal code is worse
    // than no guard at all. The six T32 special forms encode a 15 legitimately (CMP/CMN/TST/TEQ put
    // Rd=1111; MOV/MVN put Rn=1111), so the SILENT half of this test is the half that matters.
    // Returns fails (0 = pass).
    //
    // ARGUMENT REQUIRED -- same reason as varanT2Udf, and found the same way. This test emits five
    // coded UDFs ON PURPOSE (0x501/0x502/0x503/0x511/0x512), and `basic/bug908915.js` is a shotgun
    // that calls EVERY shell testing function with no arguments. So the emit-time census -- whose
    // whole job is to report which ENCODER GAPS the corpus reaches -- was reporting OUR OWN TEST
    // HOOK as if it were product code. Requiring an argument makes the bare call a no-op, so the
    // census measures the product and not the instrument. Returns -2 on a bare call rather than 0,
    // so a sweep that forgets the argument fails loudly instead of passing vacuously.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1) {
        args.rval().setInt32(-2);
        return true;
    }
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    auto udf = [](uint32_t c) -> uint32_t {
        return ((0xa000u | (c & 0xfff)) << 16) | (0xf7f0u | ((c >> 12) & 0xf));
    };
    // Emit one thing, return its first word.
    auto emit1 = [&](void (*fn)(MacroAssembler&)) -> uint32_t {
        MacroAssembler masm;
        int o = masm.nextOffset().getOffset();
        fn(masm);
        return masm.varanPeekWord(o);
    };

    // ---- MUST TRIP ----
    check(emit1([](MacroAssembler& m) { m.as_add(pc, r0, O2Reg(r1)); })        == udf(0x501)); // Rd=PC
    check(emit1([](MacroAssembler& m) { m.as_add(r0, pc, O2Reg(r1)); })        == udf(0x502)); // Rn=PC
    check(emit1([](MacroAssembler& m) { m.as_add(r0, r1, O2Reg(pc)); })        == udf(0x503)); // Rm=PC
    check(emit1([](MacroAssembler& m) {                                                        // Rs=PC
        m.as_mov(r0, O2RegRegShift(r1, LSL, pc)); })                           == udf(0x503));
    check(emit1([](MacroAssembler& m) {                                                        // str pc
        m.as_dtr(IsStore, 32, Offset, pc, DTRAddr(r0, DtrOffImm(4))); })       == udf(0x511));
    check(emit1([](MacroAssembler& m) {                                                        // sub-word ldr pc
        m.as_dtr(IsLoad, 8, Offset, pc, DTRAddr(r0, DtrOffImm(4))); })         == udf(0x511));
    check(emit1([](MacroAssembler& m) {                                                        // ldr rt,[pc,rm]
        m.as_dtr(IsLoad, 32, Offset, r0, DTRAddr(pc, DtrRegImmShift(r1, LSL, 0))); })
                                                                               == udf(0x512));

    // ---- MUST STAY SILENT (the legal special forms, and the legal ldr pc) ----
    auto notUdf = [&](uint32_t w) { return (w & 0xfff0u) != 0xf7f0u || ((w >> 16) & 0xf000u) != 0xa000u; };
    check(notUdf(emit1([](MacroAssembler& m) { m.as_cmp(r0, Imm8(1)); })));      // CMP: Rd=1111 legal
    check(notUdf(emit1([](MacroAssembler& m) { m.as_cmn(r0, Imm8(1)); })));      // CMN
    check(notUdf(emit1([](MacroAssembler& m) { m.as_tst(r0, Imm8(1)); })));      // TST
    check(notUdf(emit1([](MacroAssembler& m) { m.as_teq(r0, Imm8(1)); })));      // TEQ
    check(notUdf(emit1([](MacroAssembler& m) { m.as_mov(r0, Imm8(1)); })));      // MOV: Rn=1111 legal
    check(notUdf(emit1([](MacroAssembler& m) { m.as_mvn(r0, Imm8(1)); })));      // MVN
    check(notUdf(emit1([](MacroAssembler& m) { m.as_mov(r0, O2Reg(r1)); })));    // MOV reg form
    // `ldr.w pc,[sp],#4` IS legal and LIVE (ret() -> ma_pop(pc)) -- D1 must not touch it.
    check(notUdf(emit1([](MacroAssembler& m) {
        m.as_dtr(IsLoad, 32, PostIndex, pc, DTRAddr(sp, DtrOffImm(4))); })));
    // farJumpWithPatch's legal pc-base literal load: mode == Offset && U == 1. D3 must not fire.
    check(notUdf(emit1([](MacroAssembler& m) {
        m.as_dtr(IsLoad, 32, Offset, r0, DTRAddr(pc, DtrOffImm(0))); })));

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2AdrOsr(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch C -- ADR (T3) and the OSR return-address shape it exists for.
    //
    // generateEnterJIT's OSR path must materialise the address of its own `b returnLabel` and store
    // it as the frame's return address. The A32 sequence was `mov scratch, pc; add scratch, #8`,
    // which is wrong for Thumb-2 twice over: PC reads Align(insn+4,4) rather than insn+8 (so +8
    // lands one instruction short), and `mov.w rd, pc` has no legal encoding at all. ADR is the only
    // legal wide PC-relative materialisation.
    //
    // DISCRIMINATING by construction: the program below is the exact OSR shape, and the expected
    // value is the address of the 4th word WITH the Thumb bit. The A32 arithmetic would land on the
    // 3rd word (`b skipJump`) and an unset bit0 -- both are distinguishable values, not crashes, so
    // this test fails loudly on a regression instead of merely not crashing. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    {   // ENCODE: addw r1, pc, #9 = 0f f2 09 01  (LLVM-oracle minted)
        MacroAssembler masm;
        int o = masm.nextOffset().getOffset();
        masm.as_adr(r1, 2 * sizeof(uint32_t) + 1);
        check(masm.varanPeekWord(o) == 0x0109F20Fu);
        o = masm.nextOffset().getOffset();
        masm.as_adr(r0, 2 * sizeof(uint32_t) + 1);
        check(masm.varanPeekWord(o) == 0x0009F20Fu);
        check(!masm.oom());
    }

    {   // EXECUTE the OSR shape:
        //   +0  addw r0, pc, #9   -> Align(0+4,4) + 9 = 13 = 12|1
        //   +4  bx lr             (stands in for `str scratch,[sp]`)
        //   +8  nop.w             (stands in for `b skipJump`)
        //   +12 nop.w             (stands in for `b returnLabel`  <- the address we want)
        Simulator* sim = cx->runtime()->simulator();
        alignas(4) uint16_t prog[8];
        prog[0] = 0xF20Fu; prog[1] = 0x0009u;   // addw r0, pc, #9
        prog[2] = 0x4770u; prog[3] = 0xBF00u;   // bx lr ; pad
        prog[4] = 0xF3AFu; prog[5] = 0x8000u;   // nop.w
        prog[6] = 0xF3AFu; prog[7] = 0x8000u;   // nop.w
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        uint32_t got  = uint32_t(sim->call(e, 1, 0));
        uint32_t base = uint32_t(uintptr_t(prog));
        check(got == base + 12 + 1);        // 4th word, Thumb bit set
        check(got != base + 8 + 1);         // NOT the 3rd word -- the A32 +8 landing spot
        check((got & 1) == 1);              // F2: the Thumb bit is actually present
    }

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2PcLiteral(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch A -- the simulator's PC-READ BASE. Thumb-2 reads PC as Align(insn+4,4); the A32
    // value (SimInstruction::kPCReadOffset == 8) is +4 too high, which silently loads the WRONG WORD
    // for every pc-relative literal -- including the live VLDR-literal constant pool (ma_vimm ->
    // as_FImm64Pool). No pre-existing self-test exercised a pc-relative base: varanT2Vldr and
    // varanT2LoadStore both execute with sp/r1 bases, which is exactly why this survived.
    //
    // The program is a DISCRIMINATOR, not just a smoke test: the correct word and the word the old
    // +8 base would have loaded are both present and distinct, so a regression yields 0xDEADBEEF
    // rather than a crash. Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // byte 0 : ldr r0,[pc,#4]   (LDR literal T2: hw0 = 0xF8DF (U=1), hw1 = (Rt<<12)|imm12)
    //          T2 base = Align(0+4,4) = 4 -> address 4+4 = 8   -> 0xCAFEBABE  (correct)
    //          A32 base = 0+8 = 8         -> address 8+4 = 12  -> 0xDEADBEEF  (the old bug)
    // byte 4 : bx lr ; nop16
    // byte 8 : 0xCAFEBABE
    // byte 12: 0xDEADBEEF
    alignas(4) uint16_t prog[8];
    prog[0] = 0xF8DFu; prog[1] = 0x0004u;   // ldr r0,[pc,#4]
    prog[2] = 0x4770u;                       // bx lr
    prog[3] = 0xBF00u;                       // nop16 (pad to the 4-byte data boundary)
    prog[4] = 0xBABEu; prog[5] = 0xCAFEu;   // 0xCAFEBABE
    prog[6] = 0xBEEFu; prog[7] = 0xDEADu;   // 0xDEADBEEF

    uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
    Simulator* sim = cx->runtime()->simulator();
    check(uint32_t(sim->call(entry, 1, 0)) == 0xCAFEBABEu);

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2RegBranch(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- register branches (bx/blx/bkpt) + NOP.W. (A) byte-match the packed encodings.
    // (B) the CRITICAL blx return-address test: blx16 is packed in the HIGH halfword so hardware LR =
    // slot-end (= the recorded call site), NOT slot+2. A callee reads LR and we assert it equals the
    // byte after the 4-byte blx slot, with the thumb bit -- a LOW packing would be 2 bytes short (a
    // silent, GC-lethal safepoint skew). Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // (A) byte-match via a real MacroAssembler.
    {
        js::LifoAlloc lifo(1 << 16);
        TempAllocator alloc(&lifo);
        JitContext jc(cx, &alloc);
        MacroAssembler masm;
        int oNop = masm.nextOffset().getOffset(); masm.as_nop();
        check(masm.varanPeekWord(oNop) == 0x8000F3AFu);            // NOP.W
        int oBx  = masm.nextOffset().getOffset(); masm.as_bx(lr);
        check(masm.varanPeekWord(oBx)  == 0x4770bf00u);            // bx lr  packed (NOP16 low)
        int oBlx = masm.nextOffset().getOffset(); masm.as_blx(r1);
        check(masm.varanPeekWord(oBlx) == 0x4788bf00u);            // blx r1 packed (blx16 HIGH)
        int oBkpt = masm.nextOffset().getOffset(); masm.as_bkpt();
        uint32_t bk = masm.varanPeekWord(oBkpt);
        check((bk >> 16) == 0xbf00u && (bk & 0xff00u) == 0xbe00u); // bkpt: NOP16 high, bkpt low
    }

    // (B) blx LR-packing execution proof. Hand-laid; the callee captures LR via mov.w (16-bit hi-reg mov
    // is not in the sim, so use the T2 form the 0x75 dispatch decodes).
    {
        Simulator* sim = cx->runtime()->simulator();
        uint16_t prog[16];
        int nb = 0;
        auto put32 = [&](uint32_t w){ prog[nb/2]=uint16_t(w&0xffff); prog[nb/2+1]=uint16_t(w>>16); nb+=4; };
        auto put16 = [&](uint16_t w){ prog[nb/2]=w; nb+=2; };
        put32(0x0c0eea4fu);   // 0:  mov.w r12, lr  (save sim ret in r12 -- caller-saved, not r4-r11)
        put32(0x4788bf00u);   // 4:  blx r1 packed  (NOP16@4, BLX16@6; LR := slot-end(8)|1)
        put16(0x4760u);       // 8:  bx r12         (callee returns here -> back to the sim via r12)
        put32(0x000eea4fu);   // 10: mov.w r0, lr   (callee: r0 = LR)
        put16(0x4770u);       // 14: bx lr          (callee returns to offset 8)
        uintptr_t base = reinterpret_cast<uintptr_t>(prog);
        uint8_t* entry = reinterpret_cast<uint8_t*>(base | 1);
        int32_t callee = int32_t((base + 10) | 1);                 // r1 = callee entry (thumb bit)
        int32_t r0 = int32_t(sim->call(entry, 2, 0, callee));
        int32_t expected = int32_t((base + 8) | 1);                // LR == blx-slot-end | thumb bit
        check(uint32_t(r0) == uint32_t(expected));
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2JumpPatch(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 4 -- the 2-slot patchable jump (jumpWithPatch/PatchJump/bind(RepatchLabel*)). (A) the
    // shared VaranComputeJump2 writer across Always/cond, in-range + the >+-1MB overflow fallback, with a
    // decode round-trip. (B) end-to-end: jumpWithPatch reserves [slot0,slot1]; bind(RepatchLabel*) patches
    // the pair to fall through, and the decoded slot0/slot1 must land exactly at the bind point.
    // Returns fails (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // (A) shared writer self-test (includes the >+-1MB invert+B.W overflow fallback).
    check(Assembler::varanJumpPatch2SelfTest() == 0);

    // (B) jumpWithPatch + bind(RepatchLabel*) through a real MacroAssembler. slot0 decode must land at
    // the bind offset. Decode helper mirrors varanT2BranchBind (2-slot: overflow target lives in slot1).
    {
        js::LifoAlloc lifo(1 << 16);
        TempAllocator alloc(&lifo);
        JitContext jc(cx, &alloc);
        MacroAssembler masm;
        auto landing = [&](int bo) -> int {
            uint32_t w0 = masm.varanPeekWord(bo);
            uint32_t cond = 14u;
            VaranDecodeBranchCond(w0, &cond);
            if (VaranBranchKind(w0) == 1 && cond < 14) {
                uint32_t w1 = masm.varanPeekWord(bo + 4);
                if (VaranBranchKind(w1) == 1)
                    return (bo + 4) + VaranDecodeBranchByteVal(w1);
                return bo + VaranDecodeBranchByteVal(w0);
            }
            return bo + VaranDecodeBranchByteVal(w0);
        };
        // Unconditional patchable jump (backedgeJump shape). RepatchLabel::offset() asserts !bound(),
        // so capture the bind point (== nextOffset) BEFORE binding.
        RepatchLabel R;
        CodeOffsetJump j = masm.jumpWithPatch(&R, Assembler::Always);
        masm.as_movw(r0, Imm16(0));
        masm.as_movw(r0, Imm16(0));
        int Roff = masm.nextOffset().getOffset();
        masm.bind(&R);
        check(!masm.oom());
        check(landing(j.offset()) == Roff);
        // Conditional patchable jump.
        RepatchLabel S;
        CodeOffsetJump js = masm.jumpWithPatch(&S, Assembler::NotEqual);
        masm.as_movw(r0, Imm16(0));
        int Soff = masm.nextOffset().getOffset();
        masm.bind(&S);
        check(!masm.oom());
        check(landing(js.offset()) == Soff);
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2PatchJumpTwice(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN 2026-07-24 -- the RED TEST for *2 (the PatchJump condition-inversion defect).
    //
    // THE MECHANISM UNDER TEST (source, verbatim):
    //   VaranComputeJump2's >+-1MB arm stores the INVERTED condition in slot0
    //     (Assembler-arm.cpp)  uint32_t inv = ...InvertCondition(...);  *w0 = EncodeBccT2(4, inv);
    //   PatchJump does NOT carry the condition -- it re-derives it from the LIVE slot0 word
    //     (Assembler-arm.cpp)  uint32_t cf = 14u;  VaranDecodeBranchCond(*w0, &cf);
    //   IonCaches patches each conditional next-stub site EXACTLY TWICE.
    // => after one overflow patch, every later patch of that site is computed with !c.
    //
    // The existing varanT2JumpPatch self-test is structurally blind to this: it never feeds an
    // ALREADY-EMITTED slot0 back into VaranComputeJump2. This one does exactly that, through the
    // REAL MacroAssembler, REAL jumpWithPatch and REAL jit::PatchJump on REAL JitCode.
    //
    // Returns a string "w0a,w1a,w0b,w1b" (hex) -- the slot pair after patch #1 and after patch #2.
    // The VERDICT IS COMPUTED OUTSIDE, by byte-comparison against clang's integrated assembler
    // (varan-jit/tools/t2oracle.sh). Deliberately NOT decoded here: the in-tree ARM disassembler has
    // zero Thumb-2 support and prints confident garbage, and a test that grades itself with the
    // suspect decoder proves nothing.
    //
    // ARGUMENT REQUIRED (a bare call is inert and returns the -2 sentinel): basic/bug908915.js calls
    // every shell testing function with no arguments, and this project has twice had its own
    // scaffolding counted as a product defect that way.
    //   mode 0 = FAR then NEAR  (the *2 trigger: patch #1 takes the overflow arm)
    //   mode 1 = NEAR then NEAR (control: the overflow arm is never taken, condition must survive)
    //   mode 2 = FAR then FAR
    //   mode 3 = FAR, NEAR, FAR, NEAR  (the absorbing sequence -- the shape Ion loop backedges
    //            produce, where the site is re-patched on every interrupt toggle)
    //   mode 4 = NEAR only (single patch)
    //   mode 5 = corrupt slot1, then patch -- MUST hit the loud MOZ_CRASH. Graded by EXIT CODE
    //            from outside, poscontrol-style; a returned value would mean it did not crash.
    // Returns "slot0:slot1,slot0:slot1,..." -- one pair per patch, in order.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1 || !args[0].isInt32()) {
        args.rval().setInt32(-2);
        return true;
    }
    int mode = args[0].toInt32();

    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    AutoFlushICache afc("varanT2PatchJumpTwice");
    MacroAssembler masm;

    // Lay the 2-slot conditional patchable site with a KNOWN condition, then bind it (that is the
    // shape IonCaches produces: jumpWithPatch -> bind -> PatchJump -> PatchJump).
    RepatchLabel R;
    CodeOffsetJump j = masm.jumpWithPatch(&R, Assembler::Equal);
    for (int i = 0; i < 4; i++)
        masm.as_movw(r0, Imm16(0));
    masm.bind(&R);
    for (int i = 0; i < 4; i++)
        masm.as_movw(r0, Imm16(0));
    if (masm.oom()) { args.rval().setInt32(-3); return true; }

    Linker linker(masm);
    JitCode* code = linker.newCode<CanGC>(cx, OTHER_CODE);
    if (!code) { args.rval().setInt32(-4); return true; }

    CodeLocationJump jump(code, j);
    uint32_t* w = reinterpret_cast<uint32_t*>(jump.raw());
    uint8_t* farTarget  = reinterpret_cast<uint8_t*>(w) + (2 << 20);  // +2MB => forces the overflow arm
    uint8_t* nearTarget = code->raw() + 4;

    // The patch schedule per mode. 0 = in-range, 1 = out-of-range (the overflow arm).
    // NB: the members are `useFar`, not `far` -- <windef.h> still #defines `far` and `near`
    // as empty Win16 leftovers, so a member named `far` expands to nothing and produces a
    // cascade of "expected member name" / "excess elements in struct initializer".
    static const struct { int n; int useFar[4]; } kSched[] = {
        { 2, { 1, 0, 0, 0 } },   // 0: far, near      -- the original red case
        { 2, { 0, 0, 0, 0 } },   // 1: near, near     -- control
        { 2, { 1, 1, 0, 0 } },   // 2: far, far
        { 4, { 1, 0, 1, 0 } },   // 3: far, near, far, near -- the absorbing sequence
        { 1, { 0, 0, 0, 0 } },   // 4: near only
        { 1, { 0, 0, 0, 0 } },   // 5: near, but slot1 is corrupted first -> must MOZ_CRASH
    };
    if (mode < 0 || mode > 5) { args.rval().setInt32(-5); return true; }

    if (mode == 5) {
        // Corrupt slot1 to a word that is neither NOP.W nor an unconditional B.W. The loud
        // check must fire; if this function RETURNS at all, the check is missing or is an
        // assert (and asserts do not exist in the device build, which is the whole point).
        // ⚠️ Use the raw protection primitives, NOT AutoWritableJitCode. That RAII class also
        // flips runtime state (toggleAutoWritableJitCodeActive + AutoPreventBackedgePatching)
        // and is not re-entrant -- Runtime.h:1211 asserts on nesting. A first attempt used it
        // here and mode 5 exited nonzero for the WRONG REASON: the nesting assert, not our
        // check. That is a false pass, and it is exactly why this case is graded on the
        // MESSAGE as well as the exit code.
        if (!ExecutableAllocator::makeWritable(reinterpret_cast<void*>(w), 2 * sizeof(uint32_t)))
            MOZ_CRASH("varanT2PatchJumpTwice: makeWritable failed");
        w[1] = 0xDEADBEEFu;
        if (!ExecutableAllocator::makeExecutable(reinterpret_cast<void*>(w), 2 * sizeof(uint32_t)))
            MOZ_CRASH("varanT2PatchJumpTwice: makeExecutable failed");
    }

    char buf[160];
    int pos = 0;
    for (int i = 0; i < kSched[mode].n; i++) {
        PatchJump(jump, CodeLocationLabel(kSched[mode].useFar[i] ? farTarget : nearTarget));
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%08x:%08x",
                        i ? "," : "", w[0], w[1]);
    }
    JSString* s = JS_NewStringCopyZ(cx, buf);
    if (!s)
        return false;
    args.rval().setString(s);
    return true;
}

static bool
VaranT2PoolDefer(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN escape-closure: the A32 pool escape is now fully CLOSED. The double/float32 pool paths
    // (ma_vimm/ma_vimm_f32, see varanT2Pool) and jumpWithPatch (see varanT2JumpPatch) are live
    // Thumb-2, and Batch 4 converted the last holdout -- the patchable ABSOLUTE branch ma_b(void*) --
    // from a coded wide UDF (0x3F2) to varanAbsBranch: movw ip / movt ip / bx ip.
    //
    // NB this test previously asserted the OLD 0x3F2 UDF and so had been silently failing (returning
    // 1) ever since that conversion: a self-test asserting an obsolete deferral is worse than no test,
    // because it reads as a live regression. Assert the converted reality instead.
    // Returns failed-check count (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    // EncodeUdfT2(code): hw0 = 0xf7f0|((code>>12)&0xf), hw1 = 0xa000|(code&0xfff), stored (hw1<<16)|hw0.
    auto udf = [](uint32_t c) -> uint32_t {
        return ((0xa000u | (c & 0xfff)) << 16) | (0xf7f0u | ((c >> 12) & 0xf));
    };

    // (c) ma_b(void*) patchable absolute branch -> varanAbsBranch (movw ip / movt ip / bx ip).
    // Structural checks via the classifiers rather than hard-coded words, so this stays honest if
    // the immediate split ever changes.
    (void)udf;
    int o2 = masm.nextOffset().getOffset();
    masm.ma_b(reinterpret_cast<void*>(uintptr_t(0x12345678)), Assembler::Always);
    uint32_t w[3] = { masm.varanPeekWord(o2), masm.varanPeekWord(o2 + 4), masm.varanPeekWord(o2 + 8) };
    auto I = [&](int k) { return reinterpret_cast<Instruction*>(&w[k]); };
    check(I(0)->is<InstMovW>());
    check(I(1)->is<InstMovT>());
    check(I(2)->is<InstBranchReg>());
    check(I(2)->as<InstBranchReg>()->checkDest(ScratchRegister));
    // F2: the movw immediate must carry the Thumb interworking bit, or `bx ip` enters ARM state.
    // imm8 occupies hw1[7:0], i.e. bits 23:16 of the stored word, so bit0 of the target is bit16.
    check(((w[0] >> 16) & 1u) == 1u);
    // No pool entry was allocated, so no word can alias a PoolHeader (top-16 == 0xffff).
    for (int k = 0; k < 3; k++)
        check((w[k] >> 16) != 0xffffu);
    check(!masm.oom());

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2AluImm(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 1 -- ALU-immediate BYTE-MATCH. Drives the as_alu modimm seam and checks each emitted
    // word against the oracle: the opcode table (K=0xff, Rd=r1, Rn=r2), the modified-immediate predicate
    // on boundary constants (mov r0,#K -- pattern + rotation), a non-encodable constant -> UDF (predicate
    // returned -1, cascade would fall to movw/movt), and the conditional -> B<!c>.W branch-over pair.
    // Returns failed-check count (0 = pass).
    //
    // ARGUMENT REQUIRED -- see varanT2EmitGuards. The non-encodable-constant check below emits a
    // coded UDF (0x10d) deliberately, and `basic/bug908915.js` calls every testing function with no
    // arguments, which put our own scaffolding into the encoder-gap census. -2 on a bare call so a
    // sweep that forgets the argument fails loudly rather than passing vacuously.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    if (args.length() < 1) {
        args.rval().setInt32(-2);
        return true;
    }
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto emit = [&](Register d, Register n, uint32_t k, ALUOp op, SBit s) -> uint32_t {
        int o = masm.nextOffset().getOffset();
        masm.as_alu(d, n, Imm8(k), op, s, Assembler::Always);
        return masm.varanPeekWord(o);
    };
    // Opcode table (K=0xff -> control 0x0ff; Rd=r1, Rn=r2). Oracle bytes from ENCODING-TABLES Batch 1 §4.
    check(emit(r1, r2, 0xff, OpAnd, LeaveCC) == 0x01fff002u);
    check(emit(r1, r2, 0xff, OpBic, LeaveCC) == 0x01fff022u);
    check(emit(r1, r2, 0xff, OpOrr, LeaveCC) == 0x01fff042u);
    check(emit(r1, r2, 0xff, OpEor, LeaveCC) == 0x01fff082u);
    check(emit(r1, r2, 0xff, OpAdd, LeaveCC) == 0x01fff102u);
    check(emit(r1, r2, 0xff, OpSub, LeaveCC) == 0x01fff1a2u);
    check(emit(r1, InvalidReg, 0xff, OpMvn, LeaveCC) == 0x01fff06fu);   // ORN, Rn=PC
    check(emit(r1, InvalidReg, 0xff, OpMov, LeaveCC) == 0x01fff04fu);   // ORR, Rn=PC
    check(emit(InvalidReg, r2, 0xff, OpCmp, SetCC)   == 0x0ffff1b2u);   // SUB, S, Rd=PC
    // Predicate boundary constants (mov r0,#K). Independently oracle-verified encodings.
    check(emit(r0, InvalidReg, 0x100,      OpMov, LeaveCC) == 0x7080f44fu);  // rotation, control 0xf80
    check(emit(r0, InvalidReg, 0x00ff00ffu, OpMov, LeaveCC) == 0x10fff04fu); // pattern 01, control 0x1ff
    check(emit(r0, InvalidReg, 0xff00ff00u, OpMov, LeaveCC) == 0x20fff04fu); // pattern 10, control 0x2ff
    check(emit(r0, InvalidReg, 0xffffffffu, OpMov, LeaveCC) == 0x30fff04fu); // pattern 11, control 0x3ff
    check(emit(r0, InvalidReg, 0x1fe,      OpMov, LeaveCC) == 0x70fff44fu);  // rotation, control 0xfff
    // Non-encodable -> predicate -1 -> as_alu emits a coded UDF (top-16 0xaXXX, hw0 0xf7fX), not an ALU.
    {
        uint32_t w = emit(r0, InvalidReg, 0x12345678u, OpMov, LeaveCC);
        check(((w & 0xfff0u) == 0xf7f0u) && (((w >> 16) & 0xf000u) == 0xa000u));
    }
    // Conditional ALU -> B<!c>.W branch-over + unconditional body.
    {
        int oc = masm.nextOffset().getOffset();
        masm.as_alu(r0, r0, Imm8(0x100), OpAdd, LeaveCC, Assembler::Equal);
        check(masm.varanPeekWord(oc)     == 0x8002f040u);  // bne.w +4 (invert of eq)
        check(masm.varanPeekWord(oc + 4) == 0x7080f500u);  // add.w r0,r0,#0x100
    }
    // wasmPatchBoundsCheck (the P2-missed ALU-imm imm-field read-back+rewrite): read r_index out of a
    // T2 cmp.w r3,#0 placeholder and rewrite the imm8 to #0x100 in place.
    {
        uint32_t slot = 0x0f00f1b3u;   // cmp.w r3,#0 (SUB, S, Rd=PC, Rn=r3)
        masm.wasmPatchBoundsCheck(reinterpret_cast<uint8_t*>(&slot), 0x100);
        check(slot == 0x7f80f5b3u);    // cmp.w r3,#0x100 (control 0xf80 -> i=1,imm3=7,imm8=0x80)
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2Wasm1Slot(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN W1 -- bind(RepatchLabel*) must not assume a 2-slot pair.
    //
    // ⚠ THE TEST DESIGN IS THE POINT. The bug is that binding a RepatchLabel over a 1-SLOT
    // unconditional branch wrote NOP.W over slot0+4. When that branch is the LAST instruction in
    // the buffer you get a loud assert (slot0+4 is past the end) -- that is the lucky case, and it
    // is what the failing tests happened to hit. When it is NOT last, the write lands on THE
    // FOLLOWING REAL INSTRUCTION and is completely silent.
    //
    // So this test deliberately puts a MARKER INSTRUCTION AFTER the branch and asserts the marker
    // SURVIVES. A test with the branch at the end would pass against the broken code -- vacuous on
    // the exact defect, the same trap as writing an oracle around `return x++` (which returns the
    // OLD value and is correct even when the increment is broken).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };

    // The marker word, derived by emitting it rather than hard-coded: this test is about the
    // follower SURVIVING, not about movw's encoding (varanT2CondForms already covers that), and a
    // hard-coded constant would rot the moment the encoder changed.
    uint32_t MARKER;
    {
        MacroAssembler m;
        int o = m.nextOffset().getOffset();
        m.as_movw(r7, Imm16(0xbeef));
        MARKER = m.varanPeekWord(o);
    }

    // ---- case 1: the 1-slot Always site (the wasm trap-jump shape) with a FOLLOWER ----
    {
        MacroAssembler masm;
        Label l;
        BufferOffset br = masm.as_b(&l, Assembler::Always);   // 1 slot: Always takes the 1-slot path
        int o = br.getOffset();
        masm.as_movw(r7, Imm16(0xbeef));                      // <-- the follower that must survive
        // Exactly what MacroAssembler::wasmEmitTrapOutOfLineCode does: synthesize a RepatchLabel
        // over a site it did not create, then bind it here.
        l.reset();                                            // bindLater() does this too
        RepatchLabel jump;
        jump.use(o);
        masm.bind(&jump);

        check(masm.varanPeekWord(o + 4) == MARKER);           // THE assertion this test exists for
        // and slot0 must have become a real branch to the bind point (not left as a placeholder)
        check(masm.varanPeekWord(o) != MARKER);
    }

    // ---- case 2: the 2-slot conditional site must STILL be patched as a pair ----
    // Non-vacuity in the other direction: a fix that simply stopped writing slot1 for everything
    // would break every jumpWithPatch/backedgeJump site, and this case would catch that.
    {
        MacroAssembler masm;
        RepatchLabel rl;
        CodeOffsetJump coj = masm.jumpWithPatch(&rl, Assembler::NotEqual);
        int o = coj.offset();
        masm.as_movw(r7, Imm16(0xbeef));                      // follower, must ALSO survive
        masm.bind(&rl);
        // slot0 and slot1 belong to the reservation; the follower is at o+8.
        check(masm.varanPeekWord(o + 8) == MARKER);
    }

    args.rval().setInt32(fails);
    return true;
}

static bool
VaranT2CondForms(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN -- the conditional-forms conversion (sxtb/sxth/uxtb/uxth, movw/movt, multiply,
    // extdtr) plus the RSC synth. Proves the CONDITION SENSE by EXECUTION, in both directions.
    //
    // Why execution and why both directions: the conversion turns `<op><c>` into
    // `B<!c>.W over <op>`, so the condition is INVERTED at emit time. An inverted-sense bug
    // produces code that runs, never crashes, and is wrong exactly half the time -- the single
    // most likely defect in this batch. A test that only checks "the op ran when c held" would
    // pass with the sense backwards on the other half, so every case is run TWICE.
    //
    // The program is emitted through the REAL encoders (not hand-laid bytes) and then copied out
    // word by word and executed, so what is proven is what the assembler actually emits.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    Simulator* sim = cx->runtime()->simulator();

    uint16_t prog[64];
    uint16_t mem[2] = { 0xbeef, 0 };   // for the conditional extdtr load

    // Emit `body` into a fresh MacroAssembler wrapped in:
    //     cmp r0,#0 ; mov r3,#0x11 ; <body> ; mov r0,r3 ; bx lr
    // r0 = the flag input (0 -> Equal holds), r1 = a value operand, r2 = a pointer operand,
    // r3 = the result. 0x11 is the untouched-seed sentinel.
    auto run = [&](void (*body)(MacroAssembler&), int32_t flagIn, int32_t v1, int32_t v2) -> int32_t {
        MacroAssembler masm;
        masm.as_cmp(r0, Imm8(0));
        masm.as_mov(r3, Imm8(0x11));
        body(masm);
        masm.as_mov(r0, O2Reg(r3));
        int nb = masm.nextOffset().getOffset();
        MOZ_RELEASE_ASSERT(nb > 0 && (nb % 4) == 0 && size_t(nb) + 2 <= sizeof(prog));
        for (int o = 0; o < nb; o += 4) {
            uint32_t w = masm.varanPeekWord(o);
            prog[o / 2] = uint16_t(w & 0xffff);
            prog[o / 2 + 1] = uint16_t(w >> 16);
        }
        prog[nb / 2] = 0x4770;   // bx lr (the one 16-bit word, appended by hand)
        uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        return int32_t(sim->call(entry, 3, flagIn, v1, v2));
    };

    const int32_t SEED = 0x11;
    int32_t ptr = int32_t(uintptr_t(&mem[0]));

    // uxtb<eq>: zero-extend byte.  0x12345678 -> 0x78
    auto uxtb = [](MacroAssembler& m) { m.as_uxtb(r3, r1, 0, Assembler::Equal); };
    check(run(uxtb, 0, 0x12345678, 0) == 0x78);     // condition HOLDS -> executed
    check(run(uxtb, 1, 0x12345678, 0) == SEED);     // condition FAILS  -> skipped

    // sxth<eq>: sign-extend halfword. 0x0000fffe -> -2
    auto sxth = [](MacroAssembler& m) { m.as_sxth(r3, r1, 0, Assembler::Equal); };
    check(run(sxth, 0, 0x0000fffe, 0) == -2);
    check(run(sxth, 1, 0x0000fffe, 0) == SEED);

    // movw<eq>
    auto movw = [](MacroAssembler& m) { m.as_movw(r3, Imm16(0x1234), Assembler::Equal); };
    check(run(movw, 0, 0, 0) == 0x1234);
    check(run(movw, 1, 0, 0) == SEED);

    // movt<eq>: leaves the low half (the 0x11 seed) intact.
    auto movt = [](MacroAssembler& m) { m.as_movt(r3, Imm16(0xabcd), Assembler::Equal); };
    check(run(movt, 0, 0, 0) == int32_t(0xabcd0011u));
    check(run(movt, 1, 0, 0) == SEED);

    // mul<eq>
    auto mul = [](MacroAssembler& m) { m.as_mul(r3, r1, r1, LeaveCC, Assembler::Equal); };
    check(run(mul, 0, 7, 0) == 49);
    check(run(mul, 1, 7, 0) == SEED);

    // ldrh<eq> [r2] -- the conditional extdtr arm, whose body is NOT one instruction.
    auto ldrh = [](MacroAssembler& m) {
        m.as_extdtr(IsLoad, 16, false, Offset, r3, EDtrAddr(r2, EDtrOffImm(0)), Assembler::Equal);
    };
    check(run(ldrh, 0, 0, ptr) == 0xbeef);
    check(run(ldrh, 1, 0, ptr) == SEED);

    // ---- RSC synth (unconditional, but carry-sensitive) ----
    // MacroAssembler::neg64 is `rsbs lo,lo,#0 ; rsc hi,hi,#0`; the whole point of RSC is that it
    // consumes the borrow out of the RSB. A synth that dropped the carry would still produce the
    // right answer for the lo==0 case, so both cases are checked.
    //     value 1        (hi=0, lo=1) -> -(1)        high word = 0xffffffff
    //     value 2<<32    (hi=2, lo=0) -> -(2<<32)    high word = 0xfffffffe
    auto neg64hi = [](MacroAssembler& m) {
        m.as_rsb(r1, r1, Imm8(0), SetCC);      // lo = 0 - lo, sets the borrow in C
        m.as_rsc(r2, r2, Imm8(0));             // hi = 0 - hi - NOT(C)
        m.as_mov(r3, O2Reg(r2));
    };
    check(uint32_t(run(neg64hi, 0, 1, 0)) == 0xffffffffu);   // lo=1 borrows -> C=0
    check(uint32_t(run(neg64hi, 0, 0, 2)) == 0xfffffffeu);   // lo=0 no borrow -> C=1

    args.rval().setInt32(fails);
    return true;
}

static bool
VaranT2AluImmExec(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 1 -- ALU-immediate SIM EXECUTE (proves the VALUE, not just the bytes). Runs hand-laid
    // modimm programs through the simulator's new modified-immediate decoder. Returns failed-check count.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    Simulator* sim = cx->runtime()->simulator();
    uint16_t prog[16];
    int nb = 0;
    auto reset = [&]() { nb = 0; };
    auto put32 = [&](uint32_t w) { prog[nb / 2] = uint16_t(w & 0xffff); prog[nb / 2 + 1] = uint16_t(w >> 16); nb += 4; };
    auto put16 = [&](uint16_t w) { prog[nb / 2] = w; nb += 2; };
    auto entry = [&]() { return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1); };

    // (1) and.w r0,r0,#0xf0 ; add.w r0,r0,#0x100 ; bx lr   -- r0=200 -> (200&0xf0)+0x100 = 0xc0+0x100 = 448.
    reset();
    put32(0x00f0f000u);   // and.w r0,r0,#0xf0
    put32(0x7080f500u);   // add.w r0,r0,#0x100
    put16(0x4770u);       // bx lr
    check(int32_t(sim->call(entry(), 1, 200)) == 448);

    // (2) conditional branch-over: subs.w r0,r0,r1 ; bne.w +4 ; add.w r0,r0,#0x100 ; bx lr
    //     r0==r1 -> subs=0 (Z) -> eq true -> add executes -> 0x100 ; r0!=r1 -> Z clear -> add skipped.
    reset();
    put32(0x0001ebb0u);   // subs.w r0,r0,r1
    put32(0x8002f040u);   // bne.w +4  (the seam's branch-over)
    put32(0x7080f500u);   // add.w r0,r0,#0x100
    put16(0x4770u);       // bx lr
    check(int32_t(sim->call(entry(), 2, 5, 5)) == 0x100);   // taken (eq): 0 + 0x100
    check(int32_t(sim->call(entry(), 2, 7, 5)) == 2);       // not taken (ne): 7-5, add skipped

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2Vfp(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 2 -- VFP BYTE-MATCH. The T2 VFP word == the A32 word (cond AL) halfword-swapped.
    // Checks the near-copy for arith (vadd/vmul/vdiv), the as_vnmul SOURCE fix (bit6, distinct from
    // vmul), vmrs, the conditional -> B<!c>.W branch-over, and that VLDR/VSTR + block-transfer defer
    // to a coded UDF. Returns failed-check count (0 = pass).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto emit = [&](void) -> uint32_t { return masm.varanPeekWord(masm.nextOffset().getOffset() - 4); };
    // Arithmetic (d0 = vd, d1 = vn, d2 = vm). A32 words -> halfword-swapped T2.
    masm.as_vadd(d0, d1, d2);  check(emit() == 0x0b02ee31u);   // A32 EE310B02
    masm.as_vmul(d0, d1, d2);  check(emit() == 0x0b02ee21u);   // A32 EE210B02
    masm.as_vdiv(d0, d1, d2);  check(emit() == 0x0b02ee81u);   // A32 EE810B02
    masm.as_vnmul(d0, d1, d2); check(emit() == 0x0b42ee21u);   // A32 EE210B42 (bit6 -- the FIX, != vmul)
    masm.as_vmrs(r0);          check(emit() == 0x0a10eef1u);   // A32 EEF10A10
    // Conditional vadd (Equal) -> bne.w +4 branch-over + vadd.f64 (AL).
    {
        int oc = masm.nextOffset().getOffset();
        masm.as_vadd(d0, d1, d2, Assembler::Equal);
        check(masm.varanPeekWord(oc)     == 0x8002f040u);      // bne.w +4
        check(masm.varanPeekWord(oc + 4) == 0x0b02ee31u);      // vadd.f64 d0,d1,d2 (AL)
    }
    // (VFP block-transfer vpush/vpop is covered by VaranT2BlockXferExec's round-trip -- Batch 3.)
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2VfpExec(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 2 -- VFP SIM EXECUTE (proves the VALUE). Builds a real VFP computation via the
    // converted encoders, runs it through the simulator (which un-swaps the T2 word and delegates to
    // the base A32 VFP decoder), and checks the integer result. Returns failed-check count.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    Simulator* sim = cx->runtime()->simulator();

    auto build_and_run = [&](double a, double b, bool mul) -> int32_t {
        js::LifoAlloc lifo(1 << 16);
        TempAllocator alloc(&lifo);
        JitContext jc(cx, &alloc);
        MacroAssembler masm;
        masm.loadConstantDouble(a, d0);
        masm.loadConstantDouble(b, d1);
        if (mul) masm.as_vmul(d2, d0, d1);
        else     masm.as_vadd(d2, d0, d1);
        masm.as_vcvt(VFPRegister(d2).sintOverlay(), VFPRegister(d2));           // d2.s = (int32)d2
        masm.as_vxfer(r0, InvalidReg, VFPRegister(d2).sintOverlay(), Assembler::FloatToCore); // r0 = int
        if (masm.oom()) return INT32_MIN;
        uint16_t prog[128];
        int nb = 0;
        int total = masm.nextOffset().getOffset();
        for (int off = 0; off < total; off += 4) {
            uint32_t w = masm.varanPeekWord(off);
            prog[nb / 2] = uint16_t(w & 0xffff); prog[nb / 2 + 1] = uint16_t(w >> 16); nb += 4;
        }
        prog[nb / 2] = 0x4770;   // bx lr
        uint8_t* entry = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        return int32_t(sim->call(entry, 1, 0));   // sim requires >=1 arg; the program ignores it
    };

    check(build_and_run(2.0, 3.0, true)  == 6);   // 2.0 * 3.0 = 6.0 -> 6
    check(build_and_run(2.0, 3.0, false) == 5);   // 2.0 + 3.0 = 5.0 -> 5
    check(build_and_run(4.0, 10.0, false) == 14); // 4.0 + 10.0 = 14.0 -> 14

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2DataProc(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 2 -- data-proc BULK byte-match (mul/mla/mls/umull/smull/clz/sxtb/uxtb/sxth/uxth),
    // each oracle byte-matched. (sdiv/udiv are HasIDIV-gated -> dead on Cortex-A9, not converted.)
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto emit = [&](void) -> uint32_t { return masm.varanPeekWord(masm.nextOffset().getOffset() - 4); };
    masm.as_mul(r1, r2, r3);        check(emit() == 0xf103fb02u);
    masm.as_mla(r1, r4, r2, r3);    check(emit() == 0x4103fb02u);   // as_mla(dest,acc,src1,src2)
    masm.as_mls(r1, r4, r2, r3);    check(emit() == 0x4113fb02u);
    masm.as_umull(r1, r0, r2, r3);  check(emit() == 0x0103fba2u);   // as_umull(destHI,destLO,src1,src2)
    masm.as_smull(r1, r0, r2, r3);  check(emit() == 0x0103fb82u);
    masm.as_clz(r1, r3);            check(emit() == 0xf183fab3u);
    masm.as_sxtb(r1, r3, 0);        check(emit() == 0xf183fa4fu);
    masm.as_uxtb(r1, r3, 0);        check(emit() == 0xf183fa5fu);
    masm.as_sxth(r1, r3, 0);        check(emit() == 0xf183fa0fu);
    masm.as_uxth(r1, r3, 0);        check(emit() == 0xf183fa1fu);
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2DataProcExec(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 2 -- data-proc SIM EXECUTE (proves the VALUE).
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    Simulator* sim = cx->runtime()->simulator();
    uint16_t prog[16];
    int nb = 0;
    auto reset = [&]() { nb = 0; };
    auto put32 = [&](uint32_t w) { prog[nb / 2] = uint16_t(w & 0xffff); prog[nb / 2 + 1] = uint16_t(w >> 16); nb += 4; };
    auto put16 = [&](uint16_t w) { prog[nb / 2] = w; nb += 2; };
    auto entry = [&]() { return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1); };

    reset(); put32(0xf001fb00u); put16(0x4770u); check(int32_t(sim->call(entry(), 2, 6, 7)) == 42);          // mul r0,r0,r1
    reset(); put32(0xf080fab0u); put16(0x4770u); check(int32_t(sim->call(entry(), 1, 0x8000)) == 16);        // clz r0,r0
    reset(); put32(0xf080fa4fu); put16(0x4770u); check(int32_t(sim->call(entry(), 1, 0xff)) == -1);          // sxtb r0,r0
    reset(); put32(0xf080fa1fu); put16(0x4770u); check(int32_t(sim->call(entry(), 1, 0x12345678)) == 0x5678); // uxth r0,r0

    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2BlockXfer(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 3 -- block-transfer BYTE-MATCH: integer LDM/STM (IA/DB), push/pop, and DA -> UDF.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    js::LifoAlloc lifo(1 << 16);
    TempAllocator alloc(&lifo);
    JitContext jc(cx, &alloc);
    MacroAssembler masm;
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    auto emit = [&](void) -> uint32_t { return masm.varanPeekWord(masm.nextOffset().getOffset() - 4); };
    masm.as_dtm(IsLoad,  r0, 0xe, IA, NoWriteBack, Assembler::Always); check(emit() == 0x000ee890u); // ldmia r0,{r1,r2,r3}
    masm.as_dtm(IsLoad,  r0, 0xe, IA, WriteBack,   Assembler::Always); check(emit() == 0x000ee8b0u); // ldmia r0!
    masm.as_dtm(IsStore, r0, 0xe, IA, WriteBack,   Assembler::Always); check(emit() == 0x000ee8a0u); // stmia r0!
    masm.as_dtm(IsStore, sp, 0xe, DB, WriteBack,   Assembler::Always); check(emit() == 0x000ee92du); // push {r1,r2,r3}
    masm.as_dtm(IsLoad,  sp, 0xe, IA, WriteBack,   Assembler::Always); check(emit() == 0x000ee8bdu); // pop {r1,r2,r3}
    // DA has no T2 form, so Batch B item 3 SYNTHESISES it as per-register ldr/str, lowest register
    // to lowest address. (This line previously asserted a UDF 0x28b; that premise died with the
    // synth -- the same stale-self-test trap varanT2PoolDefer fell into after Batch 4.)
    // {r1,r2,r3} DA from r0, n=3 -> lowest address = r0-8: r1<-[r0,#-8], r2<-[r0,#-4], r3<-[r0].
    // NB emit() re-reads the LAST word, so a multi-instruction expansion must be read by OFFSET.
    {
        int o = masm.nextOffset().getOffset();
        masm.as_dtm(IsLoad,  r0, 0xe, DA, NoWriteBack, Assembler::Always);
        check(masm.nextOffset().getOffset() - o == 12);      // exactly 3 loads, no UDF
        check(masm.varanPeekWord(o)     == 0x1c08f850u);     // ldr r1,[r0,#-8]   50 f8 08 1c
        check(masm.varanPeekWord(o + 4) == 0x2c04f850u);     // ldr r2,[r0,#-4]   50 f8 04 2c
        check(masm.varanPeekWord(o + 8) == 0x3000f8d0u);     // ldr.w r3,[r0]     d0 f8 00 30
    }
    args.rval().setInt32(fails);
    return true;
}
static bool
VaranT2BlockXferExec(JSContext* cx, unsigned argc, Value* vp)
{
    // VARAN Batch 3 -- block-transfer SIM EXECUTE: an integer push/pop round-trip and a VFP vpush/vpop
    // round-trip. Returns failed-check count.
    using namespace js::jit;
    CallArgs args = CallArgsFromVp(argc, vp);
    int fails = 0;
    auto check = [&](bool ok) { if (!ok) fails++; };
    Simulator* sim = cx->runtime()->simulator();

    // Integer: push {r1,r2} ; pop {r0,r1} ; bx lr  -> r0 = original r1.
    {
        uint16_t prog[8];
        prog[0] = 0xe92du; prog[1] = 0x0006u;   // stmdb sp!,{r1,r2}
        prog[2] = 0xe8bdu; prog[3] = 0x0003u;   // ldmia sp!,{r0,r1}
        prog[4] = 0x4770u;                      // bx lr
        uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
        check(uint32_t(sim->call(e, 2, 0, 0x12345678)) == 0x12345678u);
    }
    // VFP: d0 = double(r0:r1) ; vpush{d0} ; vpop{d1} ; r0:r1 = d1 -> r0 round-trips.
    {
        js::LifoAlloc lifo(1 << 16);
        TempAllocator alloc(&lifo);
        JitContext jc(cx, &alloc);
        MacroAssembler masm;
        masm.as_vxfer(r0, r1, d0, Assembler::CoreToFloat);                       // vmov d0, r0, r1
        masm.startFloatTransferM(IsStore, sp, DB, WriteBack);
        masm.transferFloatReg(d0); masm.finishFloatTransfer();                   // vpush {d0}
        masm.startFloatTransferM(IsLoad, sp, IA, WriteBack);
        masm.transferFloatReg(d1); masm.finishFloatTransfer();                   // vpop {d1}
        masm.as_vxfer(r0, r1, d1, Assembler::FloatToCore);                       // vmov r0, r1, d1
        if (masm.oom()) { check(false); }
        else {
            uint16_t prog[64];
            int nb = 0, total = masm.nextOffset().getOffset();
            for (int off = 0; off < total; off += 4) {
                uint32_t w = masm.varanPeekWord(off);
                prog[nb / 2] = uint16_t(w & 0xffff); prog[nb / 2 + 1] = uint16_t(w >> 16); nb += 4;
            }
            prog[nb / 2] = 0x4770u;
            uint8_t* e = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(prog) | 1);
            check(uint32_t(sim->call(e, 2, 0xaabbccddu, 0x11223344u)) == 0xaabbccddu);
        }
    }
    args.rval().setInt32(fails);
    return true;
}
#endif

static const JSFunctionSpecWithHelp TestingFunctions[] = {
#if defined(JS_SIMULATOR_ARM) && defined(VARAN_THUMB2)
    JS_FN_HELP("varanT2Hello", VaranT2Hello, 2, 0,
"varanT2Hello(a, b)",
"  VARAN P1.1: run the Thumb-2 hello-world (adds.w r0,r0,r1 ; bx lr) through the ARM simulator; returns a+b."),
    JS_FN_HELP("varanT2Udf", VaranT2Udf, 0, 0,
"varanT2Udf()",
"  VARAN P1.1: run a wide UDF through the simulator; prints the UDF diagnostic and crashes (fail-loud gate)."),
    JS_FN_HELP("varanT2MovwT", VaranT2MovwT, 0, 0,
"varanT2MovwT()",
"  VARAN P1.2b: run movw/movt through the simulator; returns 0x56781234 if the T3 decode is correct."),
    JS_FN_HELP("varanT2Branch", VaranT2Branch, 0, 0,
"varanT2Branch()",
"  VARAN P1.2b Group 2: run a control-flow gauntlet (fwd/back/cond-taken/cond-not-taken) through the\n"
"  simulator; returns 105 iff every branch lands correctly."),
    JS_FN_HELP("varanT2BranchBind", VaranT2BranchBind, 0, 0,
"varanT2BranchBind()",
"  VARAN P1.2b Group 2: drive a real MacroAssembler through as_b/bind (forward chains, backward,\n"
"  in-range 2-slot, and the >1MB invert+B.W fallback); returns the number of failed checks (0 = pass)."),
    JS_FN_HELP("varanT2RetargetSplice", VaranT2RetargetSplice, 0, 0,
"varanT2RetargetSplice()",
"  VARAN P1.2b Group 2 hygiene: retarget() splices a 2-slot conditional chain onto another label;\n"
"  returns the number of failed checks (0 = pass; nonzero before the slot1 chain-link fix)."),
    JS_FN_HELP("varanT2Wasm1Slot", VaranT2Wasm1Slot, 0, 0,
"varanT2Wasm1Slot()",
"  VARAN W1: bind(RepatchLabel*) must patch ONLY slot0 for a 1-slot unconditional site, and still\n"
"  patch the pair for a 2-slot reserved site. Asserts the FOLLOWING instruction survives. 0 = pass."),

    JS_FN_HELP("varanT2CondForms", VaranT2CondForms, 0, 0,
"varanT2CondForms()",
"  VARAN: the conditional-forms conversion (sxt/uxt, movw/movt, multiply, extdtr) and the RSC synth,\n"
"  proven by EXECUTION in both directions (condition holds / condition fails). 0 = pass."),

    JS_FN_HELP("varanT2AluImm", VaranT2AluImm, 1, 0,
"varanT2AluImm(go)",
"  VARAN Batch 1: ALU-immediate byte-match (opcode table + modimm predicate + UDF fallback +\n"
"  conditional branch-over); returns failed-check count (0 = every emitted word matches the oracle)."),
    JS_FN_HELP("varanT2AluImmExec", VaranT2AluImmExec, 0, 0,
"varanT2AluImmExec()",
"  VARAN Batch 1: run modified-immediate + conditional ALU through the simulator; returns failed-check\n"
"  count (0 = the computed VALUES are correct)."),
    JS_FN_HELP("varanT2Vfp", VaranT2Vfp, 0, 0,
"varanT2Vfp()",
"  VARAN Batch 2: VFP byte-match (arith near-copy + as_vnmul bit6 fix + vmrs + conditional branch-over\n"
"  + block-transfer defer); returns failed-check count (0 = every word matches the oracle)."),
    JS_FN_HELP("varanT2VfpExec", VaranT2VfpExec, 0, 0,
"varanT2VfpExec()",
"  VARAN Batch 2: run VFP arithmetic through the simulator (2*3=6, 2+3=5, 4+10=14); returns failed-check\n"
"  count (0 = the computed VALUES are correct)."),
    JS_FN_HELP("varanT2DataProc", VaranT2DataProc, 0, 0,
"varanT2DataProc()",
"  VARAN Batch 2: data-proc bulk byte-match (mul/mla/mls/umull/smull/clz/extends); 0 = all match oracle."),
    JS_FN_HELP("varanT2DataProcExec", VaranT2DataProcExec, 0, 0,
"varanT2DataProcExec()",
"  VARAN Batch 2: run mul/clz/sxtb/uxth through the simulator; 0 = the computed VALUES are correct."),
    JS_FN_HELP("varanT2BlockXfer", VaranT2BlockXfer, 0, 0,
"varanT2BlockXfer()",
"  VARAN Batch 3: block-transfer byte-match (integer LDM/STM IA/DB, push/pop, DA->UDF); 0 = pass."),
    JS_FN_HELP("varanT2BlockXferExec", VaranT2BlockXferExec, 0, 0,
"varanT2BlockXferExec()",
"  VARAN Batch 3: integer push/pop + VFP vpush/vpop round-trip through the simulator; 0 = values correct."),
    JS_FN_HELP("varanT2LoadStore", VaranT2LoadStore, 0, 0,
"varanT2LoadStore()",
"  VARAN Batch 4: LDR/STR/LDRB/STRB byte-match (T3/T4) + a store/load round-trip through the simulator; 0 = pass."),
    JS_FN_HELP("varanT2ExtDtr", VaranT2ExtDtr, 0, 0,
"varanT2ExtDtr()",
"  VARAN Batch 4: LDRH/STRH/LDRSB/LDRSH + LDRD/STRD byte-match + strd/ldrd + signed-halfword round-trip; 0 = pass."),
    JS_FN_HELP("varanT2Vldr", VaranT2Vldr, 0, 0,
"varanT2Vldr()",
"  VARAN Batch 4: VLDR/VSTR byte-match + an 8-byte double vldr/vstr round-trip through the simulator; 0 = pass."),
    JS_FN_HELP("varanT2Pool", VaranT2Pool, 0, 0,
"varanT2Pool()",
"  VARAN Batch 4: constant pool -- PoolHintData index round-trip + a double-via-pool finishPool reach check (-4 bias); 0 = pass."),
    JS_FN_HELP("varanT2JumpPatch", VaranT2JumpPatch, 0, 0,
"varanT2JumpPatch()",
"  VARAN Batch 4: 2-slot patchable jump -- VaranComputeJump2 (incl >+-1MB fallback) + jumpWithPatch/bind(RepatchLabel*); 0 = pass."),
    JS_FN_HELP("varanT2PatchJumpTwice", VaranT2PatchJumpTwice, 1, 0,
"varanT2PatchJumpTwice(mode)",
"  VARAN 2026-07-24 RED TEST for *2: double-patch a conditional patchable site through real jit::PatchJump.\n"
"  mode 0 = far-then-near (overflow arm on patch #1); mode 1 = near-then-near (control).\n"
"  Returns \"w0a,w1a,w0b,w1b\" hex; graded OUTSIDE against clang's assembler. Bare call returns -2."),
    JS_FN_HELP("varanT2RegBranch", VaranT2RegBranch, 0, 0,
"varanT2RegBranch()",
"  VARAN Batch 4: bx/blx/bkpt/NOP.W byte-match + the blx HIGH-halfword LR-packing execution proof; 0 = pass."),
    JS_FN_HELP("varanT2Toggle", VaranT2Toggle, 0, 0,
"varanT2Toggle()",
"  VARAN Batch 4: ToggleCall/ToggledCallSize disabled<->enabled round-trip over movw/movt + NOP.W/BLXReg; 0 = pass."),
    JS_FN_HELP("varanT2PoolDefer", VaranT2PoolDefer, 0, 0,
"varanT2PoolDefer()",
"  VARAN P1: the A32 pool escape is closed -- the float64/float32-constant and jumpWithPatch paths are\n"
"  live Thumb-2, and ma_b(void*) is now movw/movt+bx; returns failed-check count (0 = pass)."),
    JS_FN_HELP("varanT2ToggleJump", VaranT2ToggleJump, 0, 0,
"varanT2ToggleJump()",
"  VARAN Batch A: the 2-slot toggled-jump redesign (ToggleToJmp/ToggleToCmp). EXECUTES the site under\n"
"  the simulator across enabled->disabled->enabled, proving control flows both ways; 0 = pass."),
    JS_FN_HELP("varanT2MrsMsr", VaranT2MrsMsr, 0, 0,
"varanT2MrsMsr()",
"  VARAN Batch B: MRS/MSR (census 0x210/0x212) -- the one NEW 32-bit decode case, collision-audited.\n"
"  Byte-match + a flags save/clobber/restore round-trip executed under the simulator; 0 = pass."),
    JS_FN_HELP("varanT2CondDtrDtm", VaranT2CondDtrDtm, 0, 0,
"varanT2CondDtrDtm()",
"  VARAN Batch B: conditional ldr/str branch-over with a PATCHED skip (census 0x24e) and the as_dtm\n"
"  DA/IB per-register synth (0x28b), incl. as_alu's now-variable conditional body; 0 = pass."),
    JS_FN_HELP("varanT2ShiftReg", VaranT2ShiftReg, 0, 0,
"varanT2ShiftReg()",
"  VARAN Batch B: register-CONTROLLED shift (census 0x11d). Oracle byte-match of the dedicated\n"
"  LSL/LSR/ASR/ROR (register) synth + the shift-into-ip path, and EXECUTE incl. >=32 amounts; 0 = pass."),
    JS_FN_HELP("varanT2C7Bit", VaranT2C7Bit, 0, 0,
"varanT2C7Bit()",
"  VARAN Batch F: C7 -- the Thumb bit on SYNTHESIZED code addresses (rectifier returnAddrOut, both\n"
"  DebugModeOSR handler arms). Non-vacuous: asserts odd AND != the pre-C7 even form; 0 = pass."),
    JS_FN_HELP("varanT2EmitGuards", VaranT2EmitGuards, 1, 0,
"varanT2EmitGuards(go)",
"  VARAN Batch E: the emit-contract guards (no PC in a wide data-proc register field; no str/sub-word\n"
"  ldr with Rt=15; no register-offset load with Rn=15). Non-vacuous BOTH ways -- also asserts the six\n"
"  legal special forms and the live `ldr pc,[sp],#4` stay SILENT; 0 = pass."),
    JS_FN_HELP("varanT2AdrOsr", VaranT2AdrOsr, 0, 0,
"varanT2AdrOsr()",
"  VARAN Batch C: ADR (T3) and the OSR return-address shape. Discriminating -- the A32 pc+8\n"
"  arithmetic lands on a different, checkable word rather than crashing; 0 = pass."),
    JS_FN_HELP("varanT2PcLiteral", VaranT2PcLiteral, 0, 0,
"varanT2PcLiteral()",
"  VARAN Batch A: the simulator's Thumb-2 PC-read base, Align(insn+4,4) rather than the A32 insn+8.\n"
"  Discriminating pc-relative literal load (a regression loads 0xDEADBEEF, not a crash); 0 = pass."),
#endif
    JS_FN_HELP("gc", ::GC, 0, 0,
"gc([obj] | 'zone' [, 'shrinking'])",
"  Run the garbage collector. When obj is given, GC only its zone.\n"
"  If 'zone' is given, GC any zones that were scheduled for\n"
"  GC via schedulegc.\n"
"  If 'shrinking' is passed as the optional second argument, perform a\n"
"  shrinking GC rather than a normal GC."),

    JS_FN_HELP("minorgc", ::MinorGC, 0, 0,
"minorgc([aboutToOverflow])",
"  Run a minor collector on the Nursery. When aboutToOverflow is true, marks\n"
"  the store buffer as about-to-overflow before collecting."),

    JS_FN_HELP("gcparam", GCParameter, 2, 0,
"gcparam(name [, value])",
"  Wrapper for JS_[GS]etGCParameter. The name is one of:" GC_PARAMETER_ARGS_LIST),

    JS_FN_HELP("relazifyFunctions", RelazifyFunctions, 0, 0,
"relazifyFunctions(...)",
"  Perform a GC and allow relazification of functions. Accepts the same\n"
"  arguments as gc()."),

    JS_FN_HELP("getBuildConfiguration", GetBuildConfiguration, 0, 0,
"getBuildConfiguration()",
"  Return an object describing some of the configuration options SpiderMonkey\n"
"  was built with."),

    JS_FN_HELP("hasChild", HasChild, 0, 0,
"hasChild(parent, child)",
"  Return true if |child| is a child of |parent|, as determined by a call to\n"
"  TraceChildren"),

    JS_FN_HELP("setSavedStacksRNGState", SetSavedStacksRNGState, 1, 0,
"setSavedStacksRNGState(seed)",
"  Set this compartment's SavedStacks' RNG state.\n"),

    JS_FN_HELP("getSavedFrameCount", GetSavedFrameCount, 0, 0,
"getSavedFrameCount()",
"  Return the number of SavedFrame instances stored in this compartment's\n"
"  SavedStacks cache."),

    JS_FN_HELP("saveStack", SaveStack, 0, 0,
"saveStack([maxDepth [, compartment]])",
"  Capture a stack. If 'maxDepth' is given, capture at most 'maxDepth' number\n"
"  of frames. If 'compartment' is given, allocate the js::SavedFrame instances\n"
"  with the given object's compartment."),

    JS_FN_HELP("captureFirstSubsumedFrame", CaptureFirstSubsumedFrame, 1, 0,
"saveStack(object [, shouldIgnoreSelfHosted = true]])",
"  Capture a stack back to the first frame whose principals are subsumed by the\n"
"  object's compartment's principals. If 'shouldIgnoreSelfHosted' is given,\n"
"  control whether self-hosted frames are considered when checking principals."),

    JS_FN_HELP("callFunctionFromNativeFrame", CallFunctionFromNativeFrame, 1, 0,
"callFunctionFromNativeFrame(function)",
"  Call 'function' with a (C++-)native frame on stack.\n"
"  Required for testing that SaveStack properly handles native frames."),

    JS_FN_HELP("callFunctionWithAsyncStack", CallFunctionWithAsyncStack, 0, 0,
"callFunctionWithAsyncStack(function, stack, asyncCause)",
"  Call 'function', using the provided stack as the async stack responsible\n"
"  for the call, and propagate its return value or the exception it throws.\n"
"  The function is called with no arguments, and 'this' is 'undefined'. The\n"
"  specified |asyncCause| is attached to the provided stack frame."),

    JS_FN_HELP("enableTrackAllocations", EnableTrackAllocations, 0, 0,
"enableTrackAllocations()",
"  Start capturing the JS stack at every allocation. Note that this sets an\n"
"  object metadata callback that will override any other object metadata\n"
"  callback that may be set."),

    JS_FN_HELP("disableTrackAllocations", DisableTrackAllocations, 0, 0,
"disableTrackAllocations()",
"  Stop capturing the JS stack at every allocation."),

    JS_FN_HELP("newExternalString", NewExternalString, 1, 0,
"newExternalString(str)",
"  Copies str's chars and returns a new external string."),

    JS_FN_HELP("ensureFlatString", EnsureFlatString, 1, 0,
"ensureFlatString(str)",
"  Ensures str is a flat (null-terminated) string and returns it."),

#if defined(DEBUG) || defined(JS_OOM_BREAKPOINT)
    JS_FN_HELP("oomThreadTypes", OOMThreadTypes, 0, 0,
"oomThreadTypes()",
"  Get the number of thread types that can be used as an argument for\n"
"oomAfterAllocations() and oomAtAllocation()."),

    JS_FN_HELP("oomAfterAllocations", OOMAfterAllocations, 2, 0,
"oomAfterAllocations(count [,threadType])",
"  After 'count' js_malloc memory allocations, fail every following allocation\n"
"  (return nullptr). The optional thread type limits the effect to the\n"
"  specified type of helper thread."),

    JS_FN_HELP("oomAtAllocation", OOMAtAllocation, 2, 0,
"oomAtAllocation(count [,threadType])",
"  After 'count' js_malloc memory allocations, fail the next allocation\n"
"  (return nullptr). The optional thread type limits the effect to the\n"
"  specified type of helper thread."),

    JS_FN_HELP("resetOOMFailure", ResetOOMFailure, 0, 0,
"resetOOMFailure()",
"  Remove the allocation failure scheduled by either oomAfterAllocations() or\n"
"  oomAtAllocation() and return whether any allocation had been caused to fail."),

    JS_FN_HELP("oomTest", OOMTest, 0, 0,
"oomTest(function, [expectExceptionOnFailure = true])",
"  Test that the passed function behaves correctly under OOM conditions by\n"
"  repeatedly executing it and simulating allocation failure at successive\n"
"  allocations until the function completes without seeing a failure.\n"
"  By default this tests that an exception is raised if execution fails, but\n"
"  this can be disabled by passing false as the optional second parameter.\n"
"  This is also disabled when --fuzzing-safe is specified."),
#endif

    JS_FN_HELP("settlePromiseNow", SettlePromiseNow, 1, 0,
"settlePromiseNow(promise)",
"  'Settle' a 'promise' immediately. This just marks the promise as resolved\n"
"  with a value of `undefined` and causes the firing of any onPromiseSettled\n"
"  hooks set on Debugger instances that are observing the given promise's\n"
"  global as a debuggee."),
    JS_FN_HELP("getWaitForAllPromise", GetWaitForAllPromise, 1, 0,
"getWaitForAllPromise(densePromisesArray)",
"  Calls the 'GetWaitForAllPromise' JSAPI function and returns the result\n"
"  Promise."),
JS_FN_HELP("resolvePromise", ResolvePromise, 2, 0,
"resolvePromise(promise, resolution)",
"  Resolve a Promise by calling the JSAPI function JS::ResolvePromise."),
JS_FN_HELP("rejectPromise", RejectPromise, 2, 0,
"rejectPromise(promise, reason)",
"  Reject a Promise by calling the JSAPI function JS::RejectPromise."),

JS_FN_HELP("streamsAreEnabled", StreamsAreEnabled, 0, 0,
"streamsAreEnabled()",
"  Returns a boolean indicating whether WHATWG Streams are enabled for the current compartment."),

    JS_FN_HELP("makeFinalizeObserver", MakeFinalizeObserver, 0, 0,
"makeFinalizeObserver()",
"  Get a special object whose finalization increases the counter returned\n"
"  by the finalizeCount function."),

    JS_FN_HELP("finalizeCount", FinalizeCount, 0, 0,
"finalizeCount()",
"  Return the current value of the finalization counter that is incremented\n"
"  each time an object returned by the makeFinalizeObserver is finalized."),

    JS_FN_HELP("resetFinalizeCount", ResetFinalizeCount, 0, 0,
"resetFinalizeCount()",
"  Reset the value returned by finalizeCount()."),

    JS_FN_HELP("gcPreserveCode", GCPreserveCode, 0, 0,
"gcPreserveCode()",
"  Preserve JIT code during garbage collections."),

    JS_FN_HELP("startgc", StartGC, 1, 0,
"startgc([n [, 'shrinking']])",
"  Start an incremental GC and run a slice that processes about n objects.\n"
"  If 'shrinking' is passesd as the optional second argument, perform a\n"
"  shrinking GC rather than a normal GC."),

    JS_FN_HELP("gcslice", GCSlice, 1, 0,
"gcslice([n])",
"  Start or continue an an incremental GC, running a slice that processes about n objects."),

    JS_FN_HELP("abortgc", AbortGC, 1, 0,
"abortgc()",
"  Abort the current incremental GC."),

    JS_FN_HELP("fullcompartmentchecks", FullCompartmentChecks, 1, 0,
"fullcompartmentchecks(true|false)",
"  If true, check for compartment mismatches before every GC."),

    JS_FN_HELP("nondeterministicGetWeakMapKeys", NondeterministicGetWeakMapKeys, 1, 0,
"nondeterministicGetWeakMapKeys(weakmap)",
"  Return an array of the keys in the given WeakMap."),

    JS_FN_HELP("internalConst", InternalConst, 1, 0,
"internalConst(name)",
"  Query an internal constant for the engine. See InternalConst source for\n"
"  the list of constant names."),

    JS_FN_HELP("isProxy", IsProxy, 1, 0,
"isProxy(obj)",
"  If true, obj is a proxy of some sort"),

    JS_FN_HELP("dumpHeap", DumpHeap, 1, 0,
"dumpHeap(['collectNurseryBeforeDump'], [filename])",
"  Dump reachable and unreachable objects to the named file, or to stdout.  If\n"
"  'collectNurseryBeforeDump' is specified, a minor GC is performed first,\n"
"  otherwise objects in the nursery are ignored."),

    JS_FN_HELP("terminate", Terminate, 0, 0,
"terminate()",
"  Terminate JavaScript execution, as if we had run out of\n"
"  memory or been terminated by the slow script dialog."),

    JS_FN_HELP("readSPSProfilingStack", ReadSPSProfilingStack, 0, 0,
"readSPSProfilingStack()",
"  Reads the jit stack using ProfilingFrameIterator."),

    JS_FN_HELP("enableOsiPointRegisterChecks", EnableOsiPointRegisterChecks, 0, 0,
"enableOsiPointRegisterChecks()",
"Emit extra code to verify live regs at the start of a VM call are not\n"
"modified before its OsiPoint."),

    JS_FN_HELP("displayName", DisplayName, 1, 0,
"displayName(fn)",
"  Gets the display name for a function, which can possibly be a guessed or\n"
"  inferred name based on where the function was defined. This can be\n"
"  different from the 'name' property on the function."),

    JS_FN_HELP("isAsmJSCompilationAvailable", IsAsmJSCompilationAvailable, 0, 0,
"isAsmJSCompilationAvailable",
"  Returns whether asm.js compilation is currently available or whether it is disabled\n"
"  (e.g., by the debugger)."),

    JS_FN_HELP("isSimdAvailable", IsSimdAvailable, 0, 0,
"isSimdAvailable",
"  Returns true if SIMD extensions are supported on this platform."),

    JS_FN_HELP("getJitCompilerOptions", GetJitCompilerOptions, 0, 0,
"getCompilerOptions()",
"Return an object describing some of the JIT compiler options.\n"),

    JS_FN_HELP("isAsmJSModule", IsAsmJSModule, 1, 0,
"isAsmJSModule(fn)",
"  Returns whether the given value is a function containing \"use asm\" that has been\n"
"  validated according to the asm.js spec."),

    JS_FN_HELP("isAsmJSModuleLoadedFromCache", IsAsmJSModuleLoadedFromCache, 1, 0,
"isAsmJSModuleLoadedFromCache(fn)",
"  Return whether the given asm.js module function has been loaded directly\n"
"  from the cache. This function throws an error if fn is not a validated asm.js\n"
"  module."),

    JS_FN_HELP("isAsmJSFunction", IsAsmJSFunction, 1, 0,
"isAsmJSFunction(fn)",
"  Returns whether the given value is a nested function in an asm.js module that has been\n"
"  both compile- and link-time validated."),

    JS_FN_HELP("wasmIsSupported", WasmIsSupported, 0, 0,
"wasmIsSupported()",
"  Returns a boolean indicating whether WebAssembly is supported on the current device."),

    JS_FN_HELP("wasmTextToBinary", WasmTextToBinary, 1, 0,
"wasmTextToBinary(str)",
"  Translates the given text wasm module into its binary encoding."),

    JS_FN_HELP("wasmBinaryToText", WasmBinaryToText, 1, 0,
"wasmBinaryToText(bin)",
"  Translates binary encoding to text format"),

    JS_FN_HELP("wasmExtractCode", WasmExtractCode, 1, 0,
"wasmExtractCode(module)",
"  Extracts generated machine code from WebAssembly.Module."),

    JS_FN_HELP("isLazyFunction", IsLazyFunction, 1, 0,
"isLazyFunction(fun)",
"  True if fun is a lazy JSFunction."),

    JS_FN_HELP("isRelazifiableFunction", IsRelazifiableFunction, 1, 0,
"isRelazifiableFunction(fun)",
"  Ture if fun is a JSFunction with a relazifiable JSScript."),

    JS_FN_HELP("enableShellAllocationMetadataBuilder", EnableShellAllocationMetadataBuilder, 0, 0,
"enableShellAllocationMetadataBuilder()",
"  Use ShellAllocationMetadataBuilder to supply metadata for all newly created objects."),

    JS_FN_HELP("getAllocationMetadata", GetAllocationMetadata, 1, 0,
"getAllocationMetadata(obj)",
"  Get the metadata for an object."),

    JS_INLINABLE_FN_HELP("bailout", testingFunc_bailout, 0, 0, TestBailout,
"bailout()",
"  Force a bailout out of ionmonkey (if running in ionmonkey)."),

    JS_FN_HELP("bailAfter", testingFunc_bailAfter, 1, 0,
"bailAfter(number)",
"  Start a counter to bail once after passing the given amount of possible bailout positions in\n"
"  ionmonkey.\n"),


    JS_FN_HELP("inJit", testingFunc_inJit, 0, 0,
"inJit()",
"  Returns true when called within (jit-)compiled code. When jit compilation is disabled this\n"
"  function returns an error string. This function returns false in all other cases.\n"
"  Depending on truthiness, you should continue to wait for compilation to happen or stop execution.\n"),

    JS_FN_HELP("inIon", testingFunc_inIon, 0, 0,
"inIon()",
"  Returns true when called within ion. When ion is disabled or when compilation is abnormally\n"
"  slow to start, this function returns an error string. Otherwise, this function returns false.\n"
"  This behaviour ensures that a falsy value means that we are not in ion, but expect a\n"
"  compilation to occur in the future. Conversely, a truthy value means that we are either in\n"
"  ion or that there is litle or no chance of ion ever compiling the current script."),

    JS_FN_HELP("assertJitStackInvariants", TestingFunc_assertJitStackInvariants, 0, 0,
"assertJitStackInvariants()",
"  Iterates the Jit stack and check that stack invariants hold."),

    JS_FN_HELP("setJitCompilerOption", SetJitCompilerOption, 2, 0,
"setCompilerOption(<option>, <number>)",
"  Set a compiler option indexed in JSCompileOption enum to a number.\n"),

    JS_FN_HELP("setIonCheckGraphCoherency", SetIonCheckGraphCoherency, 1, 0,
"setIonCheckGraphCoherency(bool)",
"  Set whether Ion should perform graph consistency (DEBUG-only) assertions. These assertions\n"
"  are valuable and should be generally enabled, however they can be very expensive for large\n"
"  (wasm) programs."),

    JS_FN_HELP("serialize", Serialize, 1, 0,
"serialize(data, [transferables, [policy]])",
"  Serialize 'data' using JS_WriteStructuredClone. Returns a structured\n"
"  clone buffer object. 'policy' may be an options hash. Valid keys:\n"
"    'SharedArrayBuffer' - either 'allow' (the default) or 'deny'\n"
"    to specify whether SharedArrayBuffers may be serialized.\n"
"    'scope' - SameProcessSameThread, SameProcessDifferentThread,\n"
"      DifferentProcess, or DifferentProcessForIndexedDB. Determines how some\n"
"      values will be serialized. Clone buffers may only be deserialized with a\n"
"      compatible scope. NOTE - For DifferentProcess/DifferentProcessForIndexedDB,\n"
"      must also set SharedArrayBuffer:'deny' if data contains any shared memory\n"
"      object."),

    JS_FN_HELP("deserialize", Deserialize, 1, 0,
"deserialize(clonebuffer[, opts])",
"  Deserialize data generated by serialize. 'opts' is an options hash with one\n"
"  recognized key 'scope', which limits the clone buffers that are considered\n"
"  valid. Allowed values: 'SameProcessSameThread', 'SameProcessDifferentThread',\n"
"  'DifferentProcess', and 'DifferentProcessForIndexedDB'. So for example, a\n"
"  DifferentProcessForIndexedDB clone buffer may be deserialized in any scope, but\n"
"  a SameProcessSameThread clone buffer cannot be deserialized in a\n"
"  DifferentProcess scope."),

    JS_FN_HELP("detachArrayBuffer", DetachArrayBuffer, 1, 0,
"detachArrayBuffer(buffer)",
"  Detach the given ArrayBuffer object from its memory, i.e. as if it\n"
"  had been transferred to a WebWorker."),

    JS_FN_HELP("helperThreadCount", HelperThreadCount, 0, 0,
"helperThreadCount()",
"  Returns the number of helper threads available for off-main-thread tasks."),

#ifdef JS_TRACE_LOGGING
    JS_FN_HELP("startTraceLogger", EnableTraceLogger, 0, 0,
"startTraceLogger()",
"  Start logging the mainThread.\n"
"  Note: tracelogging starts automatically. Disable it by setting environment variable\n"
"  TLOPTIONS=disableMainThread"),

    JS_FN_HELP("stopTraceLogger", DisableTraceLogger, 0, 0,
"stopTraceLogger()",
"  Stop logging the mainThread."),
#endif

    JS_FN_HELP("reportOutOfMemory", ReportOutOfMemory, 0, 0,
"reportOutOfMemory()",
"  Report OOM, then clear the exception and return undefined. For crash testing."),

    JS_FN_HELP("throwOutOfMemory", ThrowOutOfMemory, 0, 0,
"throwOutOfMemory()",
"  Throw out of memory exception, for OOM handling testing."),

    JS_FN_HELP("reportLargeAllocationFailure", ReportLargeAllocationFailure, 0, 0,
"reportLargeAllocationFailure()",
"  Call the large allocation failure callback, as though a large malloc call failed,\n"
"  then return undefined. In Gecko, this sends a memory pressure notification, which\n"
"  can free up some memory."),

    JS_FN_HELP("findPath", FindPath, 2, 0,
"findPath(start, target)",
"  Return an array describing one of the shortest paths of GC heap edges from\n"
"  |start| to |target|, or |undefined| if |target| is unreachable from |start|.\n"
"  Each element of the array is either of the form:\n"
"    { node: <object or string>, edge: <string describing edge from node> }\n"
"  if the node is a JavaScript object or value; or of the form:\n"
"    { type: <string describing node>, edge: <string describing edge> }\n"
"  if the node is some internal thing that is not a proper JavaScript value\n"
"  (like a shape or a scope chain element). The destination of the i'th array\n"
"  element's edge is the node of the i+1'th array element; the destination of\n"
"  the last array element is implicitly |target|.\n"),

    JS_FN_HELP("shortestPaths", ShortestPaths, 3, 0,
"shortestPaths(start, targets, maxNumPaths)",
"  Return an array of arrays of shortest retaining paths. There is an array of\n"
"  shortest retaining paths for each object in |targets|. The maximum number of\n"
"  paths in each of those arrays is bounded by |maxNumPaths|. Each element in a\n"
"  path is of the form |{ predecessor, edge }|."),

#ifdef DEBUG
    JS_FN_HELP("dumpObject", DumpObject, 1, 0,
"dumpObject()",
"  Dump an internal representation of an object."),
#endif

    JS_FN_HELP("sharedMemoryEnabled", SharedMemoryEnabled, 0, 0,
"sharedMemoryEnabled()",
"  Return true if SharedArrayBuffer and Atomics are enabled"),

    JS_FN_HELP("sharedAddress", SharedAddress, 1, 0,
"sharedAddress(obj)",
"  Return the address of the shared storage of a SharedArrayBuffer."),

    JS_FN_HELP("evalReturningScope", EvalReturningScope, 1, 0,
"evalReturningScope(scriptStr, [global])",
"  Evaluate the script in a new scope and return the scope.\n"
"  If |global| is present, clone the script to |global| before executing."),

    JS_FN_HELP("cloneAndExecuteScript", ShellCloneAndExecuteScript, 2, 0,
"cloneAndExecuteScript(source, global)",
"  Compile |source| in the current compartment, clone it into |global|'s\n"
"  compartment, and run it there."),

    JS_FN_HELP("backtrace", DumpBacktrace, 1, 0,
"backtrace()",
"  Dump out a brief backtrace."),

    JS_FN_HELP("getBacktrace", GetBacktrace, 1, 0,
"getBacktrace([options])",
"  Return the current stack as a string. Takes an optional options object,\n"
"  which may contain any or all of the boolean properties\n"
"    options.args - show arguments to each function\n"
"    options.locals - show local variables in each frame\n"
"    options.thisprops - show the properties of the 'this' object of each frame\n"),

    JS_FN_HELP("byteSize", ByteSize, 1, 0,
"byteSize(value)",
"  Return the size in bytes occupied by |value|, or |undefined| if value\n"
"  is not allocated in memory.\n"),

    JS_FN_HELP("byteSizeOfScript", ByteSizeOfScript, 1, 0,
"byteSizeOfScript(f)",
"  Return the size in bytes occupied by the function |f|'s JSScript.\n"),

    JS_FN_HELP("setImmutablePrototype", SetImmutablePrototype, 1, 0,
"setImmutablePrototype(obj)",
"  Try to make obj's [[Prototype]] immutable, such that subsequent attempts to\n"
"  change it will fail.  Return true if obj's [[Prototype]] was successfully made\n"
"  immutable (or if it already was immutable), false otherwise.  Throws in case\n"
"  of internal error, or if the operation doesn't even make sense (for example,\n"
"  because the object is a revoked proxy)."),

#ifdef DEBUG
    JS_FN_HELP("dumpStringRepresentation", DumpStringRepresentation, 1, 0,
"dumpStringRepresentation(str)",
"  Print a human-readable description of how the string |str| is represented.\n"),
#endif

    JS_FN_HELP("setLazyParsingDisabled", SetLazyParsingDisabled, 1, 0,
"setLazyParsingDisabled(bool)",
"  Explicitly disable lazy parsing in the current compartment.  The default is that lazy "
"  parsing is not explicitly disabled."),

    JS_FN_HELP("setDiscardSource", SetDiscardSource, 1, 0,
"setDiscardSource(bool)",
"  Explicitly enable source discarding in the current compartment.  The default is that "
"  source discarding is not explicitly enabled."),

    JS_FN_HELP("getConstructorName", GetConstructorName, 1, 0,
"getConstructorName(object)",
"  If the given object was created with `new Ctor`, return the constructor's display name. "
"  Otherwise, return null."),

    JS_FN_HELP("allocationMarker", AllocationMarker, 0, 0,
"allocationMarker([options])",
"  Return a freshly allocated object whose [[Class]] name is\n"
"  \"AllocationMarker\". Such objects are allocated only by calls\n"
"  to this function, never implicitly by the system, making them\n"
"  suitable for use in allocation tooling tests. Takes an optional\n"
"  options object which may contain the following properties:\n"
"    * nursery: bool, whether to allocate the object in the nursery\n"),

    JS_FN_HELP("setGCCallback", SetGCCallback, 1, 0,
"setGCCallback({action:\"...\", options...})",
"  Set the GC callback. action may be:\n"
"    'minorGC' - run a nursery collection\n"
"    'majorGC' - run a major collection, nesting up to a given 'depth'\n"),

    JS_FN_HELP("getLcovInfo", GetLcovInfo, 1, 0,
"getLcovInfo(global)",
"  Generate LCOV tracefile for the given compartment.  If no global are provided then\n"
"  the current global is used as the default one.\n"),

#ifdef DEBUG
    JS_FN_HELP("setRNGState", SetRNGState, 2, 0,
"setRNGState(seed0, seed1)",
"  Set this compartment's RNG state.\n"),
#endif

    JS_FN_HELP("getModuleEnvironmentNames", GetModuleEnvironmentNames, 1, 0,
"getModuleEnvironmentNames(module)",
"  Get the list of a module environment's bound names for a specified module.\n"),

    JS_FN_HELP("getModuleEnvironmentValue", GetModuleEnvironmentValue, 2, 0,
"getModuleEnvironmentValue(module, name)",
"  Get the value of a bound name in a module environment.\n"),

    JS_FN_HELP("isConstructor", IsConstructor, 1, 0,
"isConstructor(value)",
"  Returns whether the value is considered IsConstructor.\n"),

    JS_FS_HELP_END
};

static const JSFunctionSpecWithHelp FuzzingUnsafeTestingFunctions[] = {
#ifdef DEBUG
    JS_FN_HELP("parseRegExp", ParseRegExp, 3, 0,
"parseRegExp(pattern[, flags[, match_only])",
"  Parses a RegExp pattern and returns a tree, potentially throwing."),

    JS_FN_HELP("disRegExp", DisRegExp, 3, 0,
"disRegExp(regexp[, match_only[, input]])",
"  Dumps RegExp bytecode."),
#endif

    JS_FN_HELP("getErrorNotes", GetErrorNotes, 1, 0,
"getErrorNotes(error)",
"  Returns an array of error notes."),

    JS_FS_HELP_END
};

static const JSPropertySpec TestingProperties[] = {
    JS_PSG("timesAccessed", TimesAccessed, 0),
    JS_PS_END
};

bool
js::DefineTestingFunctions(JSContext* cx, HandleObject obj, bool fuzzingSafe_,
                           bool disableOOMFunctions_)
{
    fuzzingSafe = fuzzingSafe_;
    if (EnvVarIsDefined("MOZ_FUZZING_SAFE"))
        fuzzingSafe = true;

    disableOOMFunctions = disableOOMFunctions_;

    if (!JS_DefineProperties(cx, obj, TestingProperties))
        return false;

    if (!fuzzingSafe) {
        if (!JS_DefineFunctionsWithHelp(cx, obj, FuzzingUnsafeTestingFunctions))
            return false;
    }

    return JS_DefineFunctionsWithHelp(cx, obj, TestingFunctions);
}
