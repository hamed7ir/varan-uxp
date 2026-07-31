/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "VaranPhases.h"

#include <stdio.h>
#include <stdlib.h>

namespace mozilla {
namespace varan {

static uint32_t  gDepth[PHASE_COUNT] = { 0, 0, 0, 0, 0, 0 };
static TimeStamp gStart[PHASE_COUNT];

static const char* const kPhaseName[PHASE_COUNT] =
  { "style", "reflow", "paint", "JS", "GC", "CC" };

namespace {

struct PhaseTotals
{
  double   mMs;      // accumulated, OUTERMOST intervals only
  double   mMaxMs;   // longest single outermost interval
  uint64_t mCount;
};

PhaseTotals gTotals[PHASE_COUNT] = {
  { 0.0, 0.0, 0 }, { 0.0, 0.0, 0 }, { 0.0, 0.0, 0 },
  { 0.0, 0.0, 0 }, { 0.0, 0.0, 0 }, { 0.0, 0.0, 0 }
};

TimeStamp gFirst;      // first time anything was recorded -- the denominator
TimeStamp gLastDump;

} // anonymous namespace

bool
PhasesEnabled()
{
  // Read once. getenv on every reflow would itself be measurable, and this file must not
  // become part of what it is measuring.
  static const bool sEnabled = !!getenv("VARAN_PHASES");
  return sEnabled;
}

void
PhaseEnter(PhaseKind aKind)
{
  if (!PhasesEnabled() || aKind >= PHASE_COUNT) {
    return;
  }
  if (gDepth[aKind] == 0) {
    gStart[aKind] = TimeStamp::Now();
  }
  gDepth[aKind]++;
}

void
PhaseExit(PhaseKind aKind)
{
  if (!PhasesEnabled() || aKind >= PHASE_COUNT || gDepth[aKind] == 0) {
    return;
  }
  gDepth[aKind]--;
  if (gDepth[aKind] == 0 && !gStart[aKind].IsNull()) {
    PhaseAccumulate(aKind, TimeStamp::Now() - gStart[aKind]);
  }
}

void
PhaseAccumulate(PhaseKind aKind, const TimeDuration& aDelta)
{
  if (aKind >= PHASE_COUNT) {
    return;
  }
  double ms = aDelta.ToMilliseconds();
  if (ms < 0.0) {
    ms = 0.0;             // a clock step backwards must not become negative time
  }

  PhaseTotals& t = gTotals[aKind];
  t.mMs += ms;
  t.mCount++;
  if (ms > t.mMaxMs) {
    t.mMaxMs = ms;
  }

  if (gFirst.IsNull()) {
    gFirst = TimeStamp::Now();
  }
}

void
PhaseMaybeDump()
{
  if (!PhasesEnabled() || gFirst.IsNull()) {
    return;
  }

  TimeStamp now = TimeStamp::Now();
  if (!gLastDump.IsNull() && (now - gLastDump).ToSeconds() < 5.0) {
    return;
  }
  gLastDump = now;

  // Plain stdio, deliberately: no XPCOM, no NSPR, no allocation of consequence. This runs
  // on the paint path of a device we are trying to characterise, and it must not be able
  // to fail in an interesting way.
  FILE* f = fopen("C:\\varan\\varan-phases.log", "w");
  if (!f) {
    return;
  }

  double wall = (now - gFirst).ToSeconds();
  double accounted = 0.0;
  for (int i = 0; i < PHASE_COUNT; i++) {
    accounted += gTotals[i].mMs;
  }

  fprintf(f, "=========================================================\r\n");
  fprintf(f, " VARAN MAIN-THREAD PHASE ACCOUNTING\r\n");
  fprintf(f, " %.1f s since the first recorded phase\r\n", wall);
  fprintf(f, "=========================================================\r\n\r\n");
  fprintf(f, "  %-8s %12s %10s %12s %8s\r\n",
          "phase", "total (s)", "count", "longest(ms)", "%wall");
  for (int i = 0; i < PHASE_COUNT; i++) {
    fprintf(f, "  %-8s %12.2f %10llu %12.1f %7.1f%%\r\n",
            kPhaseName[i],
            gTotals[i].mMs / 1000.0,
            (unsigned long long)gTotals[i].mCount,
            gTotals[i].mMaxMs,
            wall > 0.0 ? (100.0 * gTotals[i].mMs / 1000.0 / wall) : 0.0);
  }
  fprintf(f, "\r\n  accounted here : %.2f s of %.1f s wall (%.1f%%)\r\n",
          accounted / 1000.0, wall,
          wall > 0.0 ? (100.0 * accounted / 1000.0 / wall) : 0.0);
  fprintf(f, "\r\n");
  fprintf(f, "  READ THIS WITH THE CHROME PROBE'S BLOCKED-TIME NUMBER.\r\n");
  fprintf(f, "  Blocked time minus (style + reflow + paint) is what is left for JS\r\n");
  fprintf(f, "  execution, DOM construction and everything else on the main thread.\r\n");
  fprintf(f, "\r\n");
  fprintf(f, "  Intervals are OUTERMOST only -- reflow nests inside reflow, and summing\r\n");
  fprintf(f, "  nested intervals would report more time than actually elapsed. Style and\r\n");
  fprintf(f, "  paint can also contain reflow, so these three are NOT disjoint and must\r\n");
  fprintf(f, "  not simply be added together and subtracted from wall time.\r\n");

  fclose(f);
}

} // namespace varan
} // namespace mozilla
