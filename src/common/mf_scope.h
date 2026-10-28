#pragma once

#include "common/hr.h"

#include <mfapi.h>

// MFStartup/MFShutdown считаются по ссылкам, поэтому область можно открывать в любом потоке.
class MfScope {
public:
    MfScope() { ThrowIfFailed(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup"); }
    ~MfScope() { MFShutdown(); }
    MfScope(const MfScope&) = delete;
    MfScope& operator=(const MfScope&) = delete;
};
