/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_IonOptimizationLevels_h
#define jit_IonOptimizationLevels_h

#include "mozilla/EnumeratedArray.h"

#include "jsbytecode.h"
#include "jstypes.h"

#include "jit/JitOptions.h"
#include "js/TypeDecls.h"

namespace js {
namespace jit {

enum class OptimizationLevel : uint8_t
{
    Normal,
    Wasm,
    Count,
    DontCompile
};

#ifdef JS_JITSPEW
inline const char*
OptimizationLevelString(OptimizationLevel level)
{
    switch (level) {
      case OptimizationLevel::DontCompile:
        return "Optimization_DontCompile";
      case OptimizationLevel::Normal:
        return "Optimization_Normal";
      case OptimizationLevel::Wasm:
        return "Optimization_Wasm";
      case OptimizationLevel::Count:;
    }
    MOZ_CRASH("Invalid OptimizationLevel");
}
#endif

class OptimizationInfo
{
  public:
    OptimizationLevel level_;

    // Toggles whether Effective Address Analysis is performed.
    bool eaa_;

    // Toggles whether Alignment Mask Analysis is performed.
    bool ama_;

    // Toggles whether Edge Case Analysis is used.
    bool edgeCaseAnalysis_;

    // Toggles whether redundant checks get removed.
    bool eliminateRedundantChecks_;

    // Toggles whether interpreted scripts get inlined.
    bool inlineInterpreted_;

    // Toggles whether native scripts get inlined.
    bool inlineNative_;

    // Toggles whether global value numbering is used.
    bool gvn_;

    // Toggles whether loop invariant code motion is performed.
    bool licm_;

    // Toggles whether Range Analysis is used.
    bool rangeAnalysis_;

    // Toggles whether loop unrolling is performed.
    bool loopUnrolling_;

    // Toggles whether instruction reordering is performed.
    bool reordering_;

    // Toggles whether Truncation based on Range Analysis is used.
    bool autoTruncate_;

    // Toggles whether sincos is used.
    bool sincos_;

    // Describes which register allocator to use.
    IonRegisterAllocator registerAllocator_;

    // The maximum total bytecode size of an inline call site. We use a lower
    // value if off-thread compilation is not available, to avoid stalling the
    // main thread.
    uint32_t inlineMaxBytecodePerCallSiteOffThread_;
    uint32_t inlineMaxBytecodePerCallSiteMainThread_;

    // The maximum value we allow for baselineScript->inlinedBytecodeLength_
    // when inlining.
    uint16_t inlineMaxCalleeInlinedBytecodeLength_;

    // The maximum bytecode length we'll inline in a single compilation.
    uint32_t inlineMaxTotalBytecodeLength_;

    // The maximum bytecode length the caller may have,
    // before we stop inlining large functions in that caller.
    uint32_t inliningMaxCallerBytecodeLength_;

    // The maximum inlining depth.
    uint32_t maxInlineDepth_;

    // Toggles whether scalar replacement is used.
    bool scalarReplacement_;

    // The maximum inlining depth for functions.
    //
    // Inlining small functions has almost no compiling overhead
    // and removes the otherwise needed call overhead.
    // The value is currently very low.
    // Actually it is only needed to make sure we don't blow out the stack.
    uint32_t smallFunctionMaxInlineDepth_;

    // How many invocations or loop iterations are needed before functions
    // are compiled.
    uint32_t compilerWarmUpThreshold_;

    // Default compiler warmup threshold, unless it is overridden.
// Varan 2026-07-31: REVERTED to upstream 1000. Lowering this to 300 was tried on device
    // (m6-device/ion300-pkg4) and is a MEASURED REGRESSION, not an improvement:
    //   JS cost per entry   7.02 ms (Ion@1000)  ->  10.53 / 20.20 / 13.62 ms (Ion@300)
    //   worst single JS entry     30.3 s        ->  85.0 s
    // i.e. 1.5x to 2.9x WORSE. That also refutes the theory it was meant to test: if page-load
    // code never reached Ion, lowering the trigger would have changed nothing. Ion IS reached,
    // and on this device it is a net LOSS -- so the cost of getting into Ion exceeds what Ion
    // buys back. Do not re-lower this without first explaining WHY Ion is net-negative here;
    // the leading suspect is that Ion compilation is not actually happening off-thread.
    static const uint32_t CompilerWarmupThreshold = 1000;

    // How many invocations or loop iterations are needed before small functions
    // are compiled.
    uint32_t compilerSmallFunctionWarmUpThreshold_;

    // Default small function compiler warmup threshold, unless it is overridden.
    static const uint32_t CompilerSmallFunctionWarmupThreshold = 100;

    // How many invocations or loop iterations are needed before calls
    // are inlined, as a fraction of compilerWarmUpThreshold.
    double inliningWarmUpThresholdFactor_;

    // How many invocations or loop iterations are needed before a function
    // is hot enough to recompile the outerScript to inline that function,
    // as a multiplication of inliningWarmUpThreshold.
    uint32_t inliningRecompileThresholdFactor_;

    OptimizationInfo()
    { }

    void initNormalOptimizationInfo();
    void initWasmOptimizationInfo();

    OptimizationLevel level() const {
        return level_;
    }

    bool inlineInterpreted() const {
        return inlineInterpreted_ && !JitOptions.disableInlining;
    }

    bool inlineNative() const {
        return inlineNative_ && !JitOptions.disableInlining;
    }

    uint32_t compilerWarmUpThreshold(JSScript* script, jsbytecode* pc = nullptr) const;

    bool gvnEnabled() const {
        return gvn_ && !JitOptions.disableGvn;
    }

    bool licmEnabled() const {
        return licm_ && !JitOptions.disableLicm;
    }

    bool rangeAnalysisEnabled() const {
        return rangeAnalysis_ && !JitOptions.disableRangeAnalysis;
    }

    bool loopUnrollingEnabled() const {
        return loopUnrolling_ && !JitOptions.disableLoopUnrolling;
    }

    bool instructionReorderingEnabled() const {
        return reordering_ && !JitOptions.disableInstructionReordering;
    }

    bool autoTruncateEnabled() const {
        return autoTruncate_ && rangeAnalysisEnabled();
    }

    bool sincosEnabled() const {
        return sincos_ && !JitOptions.disableSincos;
    }

    bool eaaEnabled() const {
        return eaa_ && !JitOptions.disableEaa;
    }

    bool amaEnabled() const {
        return ama_ && !JitOptions.disableAma;
    }

    bool edgeCaseAnalysisEnabled() const {
        return edgeCaseAnalysis_ && !JitOptions.disableEdgeCaseAnalysis;
    }

    bool eliminateRedundantChecksEnabled() const {
        return eliminateRedundantChecks_;
    }

    bool flowAliasAnalysisEnabled() const {
        return !JitOptions.disableFlowAA;
    }

    IonRegisterAllocator registerAllocator() const {
        if (JitOptions.forcedRegisterAllocator.isSome())
            return JitOptions.forcedRegisterAllocator.ref();
        return registerAllocator_;
    }

    bool scalarReplacementEnabled() const {
        return scalarReplacement_ && !JitOptions.disableScalarReplacement;
    }

    uint32_t smallFunctionMaxInlineDepth() const {
        return smallFunctionMaxInlineDepth_;
    }

    bool isSmallFunction(JSScript* script) const;

    uint32_t maxInlineDepth() const {
        return maxInlineDepth_;
    }

    uint32_t inlineMaxBytecodePerCallSite(bool offThread) const {
        return (offThread || !JitOptions.limitScriptSize)
               ? inlineMaxBytecodePerCallSiteOffThread_
               : inlineMaxBytecodePerCallSiteMainThread_;
    }

    uint16_t inlineMaxCalleeInlinedBytecodeLength() const {
        return inlineMaxCalleeInlinedBytecodeLength_;
    }

    uint32_t inlineMaxTotalBytecodeLength() const {
        return inlineMaxTotalBytecodeLength_;
    }

    uint32_t inliningMaxCallerBytecodeLength() const {
        return inliningMaxCallerBytecodeLength_;
    }

    uint32_t inliningWarmUpThreshold() const {
        uint32_t compilerWarmUpThreshold = compilerWarmUpThreshold_;
        if (JitOptions.forcedDefaultIonWarmUpThreshold.isSome())
            compilerWarmUpThreshold = JitOptions.forcedDefaultIonWarmUpThreshold.ref();
#if defined(_M_ARM)
        // Varan 2026-07-31: PIN THE INLINING THRESHOLD TO ITS DEFAULT-DERIVED VALUE.
        //
        // This derivation is why the 1000 -> 300 warm-up experiment was NOT a single-variable
        // experiment, and the result was misread because of it. CompilerWarmupThreshold feeds
        // THREE things: the compile trigger itself, this inlining trigger (x0.125, so 125 -> 37)
        // and inliningRecompileThreshold (x4 on top, so 500 -> 148). Dropping the trigger to 300
        // therefore ALSO said "inline callees that are 3.4x colder" -- which enlarges every MIR
        // graph and makes each main-thread IonBuilder::build() more expensive, at the same time
        // as it made compiles ~3x more numerous. Two variables moved in the same harmful
        // direction and the measurement could not tell them apart.
        //
        // Pinned to CompilerWarmupThreshold * factor -- i.e. exactly today's shipped behaviour at
        // the default trigger -- so that any FUTURE warm-up experiment moves one variable only.
        // This is deliberately NOT a behaviour change at the default; it is a change to what the
        // knob means when someone turns it.
        return uint32_t(CompilerWarmupThreshold * inliningWarmUpThresholdFactor_);
#else
        return compilerWarmUpThreshold * inliningWarmUpThresholdFactor_;
#endif
    }

    uint32_t inliningRecompileThreshold() const {
        return inliningWarmUpThreshold() * inliningRecompileThresholdFactor_;
    }
};

class OptimizationLevelInfo
{
  private:
    mozilla::EnumeratedArray<OptimizationLevel, OptimizationLevel::Count, OptimizationInfo> infos_;

  public:
    OptimizationLevelInfo();

    const OptimizationInfo* get(OptimizationLevel level) const {
        return &infos_[level];
    }

    OptimizationLevel nextLevel(OptimizationLevel level) const;
    OptimizationLevel firstLevel() const;
    bool isLastLevel(OptimizationLevel level) const;
    OptimizationLevel levelForScript(JSScript* script, jsbytecode* pc = nullptr) const;
};

extern OptimizationLevelInfo IonOptimizations;

} // namespace jit
} // namespace js

#endif /* jit_IonOptimizationLevels_h */
