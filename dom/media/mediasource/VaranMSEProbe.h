/* -*- mode: c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_VaranMSEProbe_h_
#define mozilla_dom_VaranMSEProbe_h_

// VARAN B1 -- InvalidStateError CAPTURE PROBE.  CAPTURE ONLY, NO FIX.
//
// WHY THIS EXISTS
//   InvalidStateError is the only Varan-SPECIFIC signal left in the video defect.
//   Everything else found so far reduces to "JS is expensive", which is true
//   upstream too. It is an OBSERVED ERROR, not a hypothesis -- which is exactly
//   what all eleven refuted theories were not.
//
//   So this instruments the THROW SITE and builds no theory of the mechanism.
//   Every DOM `InvalidStateError` raised anywhere in dom/media/mediasource is
//   recorded with the state that produced it, and nothing is changed.
//
// WHAT IT CAPTURES, per throw
//   - which call threw            (__func__ at the throw site)
//   - MediaSource readyState      (closed / open / ended / <detached>)
//   - SourceBuffer state          (updating, still attached to a MediaSource)
//   - elapsed since process start (see the caveat below -- NOT navigation start)
//   - repeat count and the interval since the previous throw AT THAT SITE
//
// ⚠️ THE ELAPSED FIGURE IS SINCE PROCESS CREATION, NOT NAVIGATION START.
//   Reaching real navigation-start from here means walking
//   MediaSource -> owner window -> document -> performance timing, which is a lot
//   of plumbing and null-checking on an error path for a number the trip can
//   recover anyway: record the wall-clock time of the navigation and subtract.
//   It is labelled `t=+N.NNNs since process start` so it cannot be misread.
//
// ★ IT ARMS ITSELF LOUDLY, AND THAT IS THE POINT.
//   The first MediaSource constructed writes a PROBE ARMED line. So an empty
//   capture means "armed, never fired" -- a real finding -- instead of being
//   indistinguishable from "the probe was not in this build" or "MOZ_LOG was not
//   set". This project has repeatedly been bitten by instruments that cannot
//   tell silence from absence; the batch explicitly wants an empty B1 to be a
//   finding, and that only works if absence is excluded.
//
// ★ IT DOES NOT DEPEND ON MOZ_LOG.
//   Output goes to BOTH a dedicated `VaranMSE` log module AND, unconditionally,
//   to a file. The browser is a windows-subsystem process, so stderr is
//   invisible; and a capture that silently depends on the tester exporting the
//   right MOZ_LOG string is a capture that can come back empty for the wrong
//   reason. File: %TEMP%\varan-mse-invalidstate.log (VARAN_MSE_LOG overrides).

namespace mozilla {
namespace dom {

class MediaSource;
class SourceBuffer;

// Called from the throw sites via VARAN_MSE_INVALID_STATE below.
// aMediaSource / aSourceBuffer may be null; both are only read, never held.
void VaranReportInvalidState(const char* aSite,
                             MediaSource* aMediaSource,
                             SourceBuffer* aSourceBuffer);

// Called once from the MediaSource constructor. Idempotent.
void VaranArmMSEProbe();

} // namespace dom
} // namespace mozilla

// Replaces a bare `aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR)`. The throw is
// UNCHANGED -- this only records what state produced it.
#define VARAN_MSE_INVALID_STATE(rv_, ms_, sb_)                              \
  do {                                                                      \
    mozilla::dom::VaranReportInvalidState(__func__, (ms_), (sb_));          \
    (rv_).Throw(NS_ERROR_DOM_INVALID_STATE_ERR);                            \
  } while (0)

#endif // mozilla_dom_VaranMSEProbe_h_
