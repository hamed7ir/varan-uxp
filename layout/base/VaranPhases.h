/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Varan main-thread phase accounting.
 *
 * WHY THIS EXISTS: the Gecko profiler does NOT exist in this UXP vintage --
 * tools/profiler/moz.build exports a header and builds tasktracer, and there are no
 * profiler sources and no MOZ_GECKO_PROFILER anywhere in the configure system. A device
 * probe measured the main thread as unavailable for 72-91% of wall time on YouTube, with
 * single blocks of 10-27 s, against a control page at 7.6%. That number is large enough
 * to explain both open complaints (slow load, and video dying because MSE appendBuffer is
 * main-thread and HTMLMediaElement's STALL_MS is 3000). What it does NOT say is WHICH
 * phase is spending the time, and every previous attempt to answer that by reasoning
 * rather than measuring picked the wrong subsystem.
 *
 * Layout can be measured from chrome JS via nsIReflowObserver. Style resolution and paint
 * cannot -- there is no scriptable hook -- which is the whole reason this file exists.
 * Reflow is instrumented here too so the C++ report stands on its own and does not depend
 * on XPConnect being correct.
 *
 * OFF BY DEFAULT. Nothing is timed unless the environment variable VARAN_PHASES is set,
 * checked exactly once. When off the scope object does one already-loaded bool test and
 * the compiler can see through it; this must not become part of what it is measuring.
 *
 * NOT thread-safe on purpose: every site it wraps is main-thread-only, and a lock here
 * would be both unnecessary and a distortion of the thing being measured.
 */

#ifndef mozilla_VaranPhases_h
#define mozilla_VaranPhases_h

#include "mozilla/TimeStamp.h"

namespace mozilla {
namespace varan {

enum PhaseKind {
  PHASE_STYLE = 0,   // RestyleManager::ProcessPendingRestyles
  PHASE_REFLOW,      // PresShell::DoReflow
  PHASE_PAINT,       // PresShell::Paint
  PHASE_COUNT
};

// True only if VARAN_PHASES is set in the environment. Read once, on first use.
bool PhasesEnabled();

// Accumulate one completed interval. Re-entrant calls are handled by the scope class
// below, which only records the OUTERMOST interval per phase -- reflow nests inside
// reflow constantly, and summing nested intervals would report more time than elapsed.
void PhaseAccumulate(PhaseKind aKind, const TimeDuration& aDelta);

// Called from the paint site. Rewrites the report at most every 5 s of wall time, so a
// freeze or a kill still leaves data behind and no shutdown hook is required.
void PhaseMaybeDump();

class MOZ_RAII AutoPhase
{
public:
  explicit AutoPhase(PhaseKind aKind)
    : mKind(aKind)
    , mOutermost(false)
  {
    if (!PhasesEnabled()) {
      return;
    }
    if (sDepth[aKind] == 0) {
      mOutermost = true;
      mStart = TimeStamp::Now();
    }
    sDepth[aKind]++;
  }

  ~AutoPhase()
  {
    if (!PhasesEnabled()) {
      return;
    }
    sDepth[mKind]--;
    if (mOutermost) {
      PhaseAccumulate(mKind, TimeStamp::Now() - mStart);
    }
  }

private:
  static uint32_t sDepth[PHASE_COUNT];

  PhaseKind mKind;
  bool      mOutermost;
  TimeStamp mStart;
};

} // namespace varan
} // namespace mozilla

#endif // mozilla_VaranPhases_h
