/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Varan: env-gated JIT/page-load event log (VARAN_JITLOG=<file path>).
 * Diagnostic instrument for the Q1/Q2 main-thread blocked-time attribution
 * run; remove after that run. Zero overhead when the env var is unset (one
 * cached getenv, no timing calls). Every line carries t=<epoch ms> so it
 * correlates with the in-page probe's Date.now() gap timestamps. */

#ifndef vm_VaranJitLog_h
#define vm_VaranJitLog_h

#include <stdio.h>
#include <stdlib.h>

#include "vm/Time.h"

namespace js {

inline FILE*
VaranJitLogFile()
{
    static FILE* file = nullptr;
    static bool checked = false;
    if (!checked) {
        checked = true;
        const char* path = getenv("VARAN_JITLOG");
        if (path && *path)
            file = fopen(path, "a");
    }
    return file;
}

/* Scope timer for sites with multiple returns: logs
 * "<tag> t=<epoch ms> ms=<dur> a=<a> b=<b>" at scope exit when dur >=
 * thresholdMs. Field meaning is per-tag, documented at the use site. */
class VaranJitLogScope
{
    FILE* file_;
    const char* tag_;
    double thresholdMs_;
    size_t a_;
    size_t b_;
    int64_t t0_;

  public:
    VaranJitLogScope(const char* tag, double thresholdMs, size_t a, size_t b)
      : file_(VaranJitLogFile()), tag_(tag), thresholdMs_(thresholdMs), a_(a), b_(b), t0_(0)
    {
        if (file_)
            t0_ = PRMJ_Now();
    }
    ~VaranJitLogScope()
    {
        if (!file_)
            return;
        double ms = double(PRMJ_Now() - t0_) / 1000.0;
        if (ms < thresholdMs_)
            return;
        fprintf(file_, "%s t=%lld ms=%.1f a=%llu b=%llu\n", tag_,
                (long long)(t0_ / 1000), ms,
                (unsigned long long)a_, (unsigned long long)b_);
        fflush(file_);
    }
};

} // namespace js

#endif // vm_VaranJitLog_h
