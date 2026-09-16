/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsDOMNavigationTiming.h"

#include "GeckoProfiler.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIScriptSecurityManager.h"
#include "prtime.h"
#include "nsIURI.h"
#include "nsPrintfCString.h"
#include "mozilla/dom/PerformanceNavigation.h"
#include "mozilla/TimeStamp.h"

#include <stdio.h>
#include <stdlib.h>

using namespace mozilla;

// Varan: NAV phase stamps (POST-FABLE item 4) -- partition the in-page probe's gap list into
// load vs playback phases. Same VARAN_JITLOG gate + epoch-ms clock as the other nine tags
// (js/src/vm/VaranJitLog.h format family); DIAGNOSTIC, remove with the MANIFEST DIAGNOSTIC
// series. One nsDOMNavigationTiming exists per docshell, so lines fire for the top document,
// every iframe, AND chrome documents -- obj= (the timing instance) and uri= are the fields
// post-processing uses to pick out the watch page's own lines.
static FILE*
VaranJitLogFile()
{
  static FILE* sFile = nullptr;
  static bool sChecked = false;
  if (!sChecked) {
    sChecked = true;
    const char* path = getenv("VARAN_JITLOG");
    if (path && *path) {
      sFile = fopen(path, "a");
    }
  }
  return sFile;
}

static void
VaranNavLog(const char* aEvent, const void* aSelf, nsIURI* aURI)
{
  FILE* f = VaranJitLogFile();
  if (!f) {
    return;
  }
  nsAutoCString spec;
  if (aURI) {
    aURI->GetSpec(spec);
  }
  fprintf(f, "NAV t=%lld event=%s obj=%p uri=%s\n",
          (long long)(PR_Now() / PR_USEC_PER_MSEC), aEvent, aSelf,
          spec.IsEmpty() ? "?" : spec.get());
  fflush(f);
}

nsDOMNavigationTiming::nsDOMNavigationTiming()
{
  Clear();
}

nsDOMNavigationTiming::~nsDOMNavigationTiming()
{
}

void
nsDOMNavigationTiming::Clear()
{
  mNavigationType = TYPE_RESERVED;
  mNavigationStartHighRes = 0;
  mBeforeUnloadStart = TimeStamp();
  mUnloadStart = TimeStamp();
  mUnloadEnd = TimeStamp();
  mLoadEventStart = TimeStamp();
  mLoadEventEnd = TimeStamp();
  mDOMLoading = TimeStamp();
  mDOMInteractive = TimeStamp();
  mDOMContentLoadedEventStart = TimeStamp();
  mDOMContentLoadedEventEnd = TimeStamp();
  mDOMComplete = TimeStamp();

  mDocShellHasBeenActiveSinceNavigationStart = false;
}

DOMTimeMilliSec
nsDOMNavigationTiming::TimeStampToDOM(TimeStamp aStamp) const
{
  if (aStamp.IsNull()) {
    return 0;
  }

  TimeDuration duration = aStamp - mNavigationStart;
  return GetNavigationStart() + static_cast<int64_t>(duration.ToMilliseconds());
}

void
nsDOMNavigationTiming::NotifyNavigationStart(DocShellState aDocShellState)
{
  mNavigationStartHighRes = (double)PR_Now() / PR_USEC_PER_MSEC;
  mNavigationStart = TimeStamp::Now();
  mDocShellHasBeenActiveSinceNavigationStart = (aDocShellState == DocShellState::eActive);
  VaranNavLog("navigationStart", this, mLoadedURI);   // Varan NAV (uri usually still unknown here)
}

void
nsDOMNavigationTiming::NotifyFetchStart(nsIURI* aURI, Type aNavigationType)
{
  mNavigationType = aNavigationType;
  // At the unload event time we don't really know the loading uri.
  // Need it for later check for unload timing access.
  mLoadedURI = aURI;
  VaranNavLog("fetchStart", this, aURI);   // Varan NAV: binds obj= to its URI earliest
}

void
nsDOMNavigationTiming::NotifyBeforeUnload()
{
  mBeforeUnloadStart = TimeStamp::Now();
}

void
nsDOMNavigationTiming::NotifyUnloadAccepted(nsIURI* aOldURI)
{
  mUnloadStart = mBeforeUnloadStart;
  mUnloadedURI = aOldURI;
}

void
nsDOMNavigationTiming::NotifyUnloadEventStart()
{
  mUnloadStart = TimeStamp::Now();
}

void
nsDOMNavigationTiming::NotifyUnloadEventEnd()
{
  mUnloadEnd = TimeStamp::Now();
}

void
nsDOMNavigationTiming::NotifyLoadEventStart()
{
  if (!mLoadEventStart.IsNull()) {
    return;
  }
  mLoadEventStart = TimeStamp::Now();
  VaranNavLog("loadEventStart", this, mLoadedURI);   // Varan NAV
}

void
nsDOMNavigationTiming::NotifyLoadEventEnd()
{
  if (!mLoadEventEnd.IsNull()) {
    return;
  }
  mLoadEventEnd = TimeStamp::Now();
  VaranNavLog("loadEventEnd", this, mLoadedURI);   // Varan NAV
}

void
nsDOMNavigationTiming::SetDOMLoadingTimeStamp(nsIURI* aURI, TimeStamp aValue)
{
  if (!mDOMLoading.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMLoading = aValue;
}

void
nsDOMNavigationTiming::NotifyDOMLoading(nsIURI* aURI)
{
  if (!mDOMLoading.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMLoading = TimeStamp::Now();
}

void
nsDOMNavigationTiming::NotifyDOMInteractive(nsIURI* aURI)
{
  if (!mDOMInteractive.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMInteractive = TimeStamp::Now();
  VaranNavLog("domInteractive", this, aURI);   // Varan NAV
}

void
nsDOMNavigationTiming::NotifyDOMComplete(nsIURI* aURI)
{
  if (!mDOMComplete.IsNull()) {
    return;
  }
  mLoadedURI = aURI;
  mDOMComplete = TimeStamp::Now();
}

void
nsDOMNavigationTiming::NotifyDOMContentLoadedStart(nsIURI* aURI)
{
  if (!mDOMContentLoadedEventStart.IsNull()) {
    return;
  }

  mLoadedURI = aURI;
  mDOMContentLoadedEventStart = TimeStamp::Now();
  VaranNavLog("DCLstart", this, aURI);   // Varan NAV
}

void
nsDOMNavigationTiming::NotifyDOMContentLoadedEnd(nsIURI* aURI)
{
  if (!mDOMContentLoadedEventEnd.IsNull()) {
    return;
  }

  mLoadedURI = aURI;
  mDOMContentLoadedEventEnd = TimeStamp::Now();
  VaranNavLog("DCLend", this, aURI);   // Varan NAV
}

void
nsDOMNavigationTiming::NotifyNonBlankPaintForRootContentDocument()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!mNavigationStart.IsNull());

  if (!mNonBlankPaint.IsNull()) {
    return;
  }

  mNonBlankPaint = TimeStamp::Now();
  TimeDuration elapsed = mNonBlankPaint - mNavigationStart;

  if (profiler_is_active()) {
    nsAutoCString spec;
    if (mLoadedURI) {
      mLoadedURI->GetSpec(spec);
    }
    nsPrintfCString marker("Non-blank paint after %dms for URL %s, %s",
                           int(elapsed.ToMilliseconds()), spec.get(),
                           mDocShellHasBeenActiveSinceNavigationStart ? "foreground tab" : "this tab was inactive some of the time between navigation start and first non-blank paint");
    PROFILER_MARKER(marker.get());
  }
}

void
nsDOMNavigationTiming::NotifyDocShellStateChanged(DocShellState aDocShellState)
{
  mDocShellHasBeenActiveSinceNavigationStart &=
    (aDocShellState == DocShellState::eActive);
}

mozilla::TimeStamp
nsDOMNavigationTiming::GetUnloadEventStartTimeStamp() const
{
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsresult rv = ssm->CheckSameOriginURI(mLoadedURI, mUnloadedURI, false);
  if (NS_SUCCEEDED(rv)) {
    return mUnloadStart;
  }
  return mozilla::TimeStamp();
}

mozilla::TimeStamp
nsDOMNavigationTiming::GetUnloadEventEndTimeStamp() const
{
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsresult rv = ssm->CheckSameOriginURI(mLoadedURI, mUnloadedURI, false);
  if (NS_SUCCEEDED(rv)) {
    return mUnloadEnd;
  }
  return mozilla::TimeStamp();
}
