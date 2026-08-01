/* -*- mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// VARAN B1 -- InvalidStateError capture probe. See VaranMSEProbe.h for the why.
// CAPTURE ONLY: nothing here changes control flow. Every function returns void
// and the throw at each call site is untouched.

#include "VaranMSEProbe.h"

#include "MediaSource.h"
#include "SourceBuffer.h"
#include "mozilla/Logging.h"
#include "mozilla/TimeStamp.h"
#include "nsThreadUtils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mozilla {
namespace dom {

static LazyLogModule gVaranMSELog("VaranMSE");
#define VARAN_MSE_LOG(...) MOZ_LOG(gVaranMSELog, mozilla::LogLevel::Error, (__VA_ARGS__))

// Per-site repeat tracking. MSE DOM entry points are main-thread only, so this
// is touched from one thread; the NS_IsMainThread() guard below makes that an
// enforced precondition rather than an assumption.
// __func__ yields a distinct static string per site, so pointer identity is a
// valid key -- no strcmp, no allocation, on an error path.
static const int kMaxSites = 32;
struct VaranSiteRec {
  const char* mSite;
  int mLine;
  uint32_t mCount;
  double mLastMs;
};
static VaranSiteRec gSites[kMaxSites];
static int gSiteCount = 0;

// A cap, because "no silent caps" is a standing rule here. If the error turns
// out to fire thousands of times, per-event file I/O would itself distort the
// run -- so it stops, and SAYS it stopped, rather than quietly truncating.
static const uint32_t kMaxEvents = 200;
static uint32_t gEventCount = 0;
static bool gCapAnnounced = false;
static bool gArmed = false;

static double
VaranElapsedMs()
{
  bool inconsistent = false;
  TimeStamp start = TimeStamp::ProcessCreation(inconsistent);
  if (!start) {
    return -1.0;
  }
  return (TimeStamp::Now() - start).ToMilliseconds();
}

// Output goes to a FILE as well as the log module, deliberately: the browser is
// a windows-subsystem process (stderr invisible), and a capture that depends on
// the tester having exported the right MOZ_LOG string can come back empty for
// the wrong reason. Opened per event and closed -- events are meant to be rare,
// and an always-open handle would be lost on a hard kill.
//
// ★ WHY THIS TRIES SEVERAL PATHS INSTEAD OF ONE.
//   v1 resolved %TEMP% and returned SILENTLY if the open failed. That is a
//   silent-failure mode that destroys the property the whole design rests on:
//   "no THROW lines" is supposed to mean "armed, never fired" -- a finding --
//   but an unwritable path produces exactly the same absence of output, and it
//   also swallows the ARMED line, so the tester cannot tell the two apart.
//   This browser has only ever been run as Administrator; the very same trip
//   tests NON-ELEVATED startup, where the token differs. So the failure is not
//   hypothetical, and "check the path" is a weaker fix than "cannot fail
//   silently". Candidates are tried in order, the first that opens wins, and
//   the winner is recorded IN the ARMED line so the tester knows where to look.
static char gPath[1024];
static bool gPathResolved = false;
static bool gPathUsable = false;

static bool
VaranTryPath(const char* aPath)
{
  if (!aPath || !*aPath) {
    return false;
  }
  FILE* f = fopen(aPath, "a");
  if (!f) {
    return false;
  }
  fclose(f);
  size_t n = strlen(aPath);
  if (n >= sizeof(gPath)) {
    return false;
  }
  memcpy(gPath, aPath, n + 1);
  return true;
}

static void
VaranResolvePath()
{
  if (gPathResolved) {
    return;
  }
  gPathResolved = true;

  // 1. explicit override
  if (VaranTryPath(getenv("VARAN_MSE_LOG"))) { gPathUsable = true; return; }

  // 2-4. the usual temp locations, then the profile-independent fallbacks.
  static const char* kVars[] = { "TEMP", "TMP", "LOCALAPPDATA", "USERPROFILE" };
  char buf[1024];
  for (size_t i = 0; i < sizeof(kVars) / sizeof(kVars[0]); i++) {
    const char* base = getenv(kVars[i]);
    if (!base || !*base) {
      continue;
    }
    snprintf(buf, sizeof(buf), "%s\\varan-mse-invalidstate.log", base);
    if (VaranTryPath(buf)) { gPathUsable = true; return; }
  }

  // 5. last resort: the current directory, which for a normal launch is the
  //    install directory next to varan.exe.
  if (VaranTryPath("varan-mse-invalidstate.log")) { gPathUsable = true; return; }

  gPathUsable = false;   // announced by VaranArmMSEProbe via the log module
}

static void
VaranWriteLine(const char* aLine)
{
  VaranResolvePath();
  if (!gPathUsable) {
    return;   // the log module still carries it; the ARMED path says so
  }
  FILE* f = fopen(gPath, "a");
  if (!f) {
    return;
  }
  fputs(aLine, f);
  fputc('\n', f);
  fclose(f);
}

static const char*
VaranReadyStateStr(MediaSource* aMS)
{
  if (!aMS) {
    return "<no-mediasource>";
  }
  switch (aMS->ReadyState()) {
    case MediaSourceReadyState::Closed: return "closed";
    case MediaSourceReadyState::Open:   return "open";
    case MediaSourceReadyState::Ended:  return "ended";
    default:                            return "<unknown>";
  }
}

void
VaranArmMSEProbe()
{
  if (gArmed || !NS_IsMainThread()) {
    return;
  }
  gArmed = true;
  VaranResolvePath();

  char line[1280];
  snprintf(line, sizeof(line),
           "=== VARAN-MSE-PROBE ARMED  t=+%.3fs since process start  file=%s ===  "
           "(build carries the B1 InvalidStateError capture; if no THROW lines "
           "follow, the error did NOT fire -- that is a result, not a missing probe)",
           VaranElapsedMs() / 1000.0,
           gPathUsable ? gPath : "<NONE WRITABLE>");
  VaranWriteLine(line);
  VARAN_MSE_LOG("%s", line);

  if (!gPathUsable) {
    // The one case where the file channel cannot report its own failure. Say it
    // on the only channel left, so a capture that comes back empty is not
    // mistaken for the "armed, never fired" finding.
    VARAN_MSE_LOG("*** VARAN-MSE-PROBE: NO WRITABLE LOG PATH (tried VARAN_MSE_LOG, "
                  "TEMP, TMP, LOCALAPPDATA, USERPROFILE, CWD). The file capture is "
                  "DEAD for this run -- an empty file is NOT the 'never fired' "
                  "result. Re-run with VARAN_MSE_LOG set to a writable path.");
  }
}

void
VaranReportInvalidState(const char* aSite,
                        int aLine,
                        MediaSource* aMediaSource,
                        SourceBuffer* aSourceBuffer)
{
  if (!NS_IsMainThread()) {
    // Should not happen -- the MSE DOM surface is main-thread. Recorded rather
    // than asserted so an opt build still tells us if it ever does.
    VaranWriteLine("VARAN-MSE THROW off-main-thread (unexpected); state not sampled");
    return;
  }

  if (gEventCount >= kMaxEvents) {
    if (!gCapAnnounced) {
      gCapAnnounced = true;
      char capline[256];
      snprintf(capline, sizeof(capline),
               "=== VARAN-MSE-PROBE CAP REACHED at %u events -- further throws are "
               "NOT recorded (per-event file I/O would distort the run) ===",
               kMaxEvents);
      VaranWriteLine(capline);
      VARAN_MSE_LOG("%s", capline);
    }
    return;
  }
  gEventCount++;

  double nowMs = VaranElapsedMs();

  // Repeat count + interval since the previous throw at THIS site.
  uint32_t count = 1;
  double sinceLast = -1.0;
  int i = 0;
  for (; i < gSiteCount; i++) {
    if (gSites[i].mSite == aSite && gSites[i].mLine == aLine) {
      break;
    }
  }
  if (i < gSiteCount) {
    gSites[i].mCount++;
    count = gSites[i].mCount;
    sinceLast = nowMs - gSites[i].mLastMs;
    gSites[i].mLastMs = nowMs;
  } else if (gSiteCount < kMaxSites) {
    gSites[gSiteCount].mSite = aSite;
    gSites[gSiteCount].mLine = aLine;
    gSites[gSiteCount].mCount = 1;
    gSites[gSiteCount].mLastMs = nowMs;
    gSiteCount++;
  }

  char sbstate[128];
  if (aSourceBuffer) {
    snprintf(sbstate, sizeof(sbstate), "sb=%p updating=%s attached=%s",
             (void*)aSourceBuffer,
             aSourceBuffer->Updating() ? "true" : "false",
             aSourceBuffer->IsAttached() ? "true" : "false");
  } else {
    snprintf(sbstate, sizeof(sbstate), "sb=<none>");
  }

  char line[768];
  if (sinceLast >= 0.0) {
    snprintf(line, sizeof(line),
             "VARAN-MSE THROW InvalidStateError  site=%s:%d  ms=%p readyState=%s  %s  "
             "t=+%.3fs since process start  n=%u  since-previous-at-site=%.3fs",
             aSite, aLine, (void*)aMediaSource, VaranReadyStateStr(aMediaSource), sbstate,
             nowMs / 1000.0, count, sinceLast / 1000.0);
  } else {
    snprintf(line, sizeof(line),
             "VARAN-MSE THROW InvalidStateError  site=%s:%d  ms=%p readyState=%s  %s  "
             "t=+%.3fs since process start  n=%u  (first at this site)",
             aSite, aLine, (void*)aMediaSource, VaranReadyStateStr(aMediaSource), sbstate,
             nowMs / 1000.0, count);
  }
  VaranWriteLine(line);
  VARAN_MSE_LOG("%s", line);
}

} // namespace dom
} // namespace mozilla
