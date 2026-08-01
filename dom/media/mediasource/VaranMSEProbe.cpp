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
static void
VaranWriteLine(const char* aLine)
{
  const char* path = getenv("VARAN_MSE_LOG");
  char buf[1024];
  if (!path || !*path) {
    const char* tmp = getenv("TEMP");
    if (!tmp || !*tmp) {
      tmp = getenv("TMP");
    }
    if (!tmp || !*tmp) {
      return;   // nowhere safe to write; the log module still has it
    }
    snprintf(buf, sizeof(buf), "%s\\varan-mse-invalidstate.log", tmp);
    path = buf;
  }
  FILE* f = fopen(path, "a");
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
  char line[512];
  snprintf(line, sizeof(line),
           "=== VARAN-MSE-PROBE ARMED  t=+%.3fs since process start ===  "
           "(build carries the B1 InvalidStateError capture; if no THROW lines "
           "follow, the error did NOT fire -- that is a result, not a missing probe)",
           VaranElapsedMs() / 1000.0);
  VaranWriteLine(line);
  VARAN_MSE_LOG("%s", line);
}

void
VaranReportInvalidState(const char* aSite,
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
    if (gSites[i].mSite == aSite) {
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
             "VARAN-MSE THROW InvalidStateError  site=%s  ms=%p readyState=%s  %s  "
             "t=+%.3fs since process start  n=%u  since-previous-at-site=%.3fs",
             aSite, (void*)aMediaSource, VaranReadyStateStr(aMediaSource), sbstate,
             nowMs / 1000.0, count, sinceLast / 1000.0);
  } else {
    snprintf(line, sizeof(line),
             "VARAN-MSE THROW InvalidStateError  site=%s  ms=%p readyState=%s  %s  "
             "t=+%.3fs since process start  n=%u  (first at this site)",
             aSite, (void*)aMediaSource, VaranReadyStateStr(aMediaSource), sbstate,
             nowMs / 1000.0, count);
  }
  VaranWriteLine(line);
  VARAN_MSE_LOG("%s", line);
}

} // namespace dom
} // namespace mozilla
