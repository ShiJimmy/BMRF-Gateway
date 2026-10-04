#pragma once

#include "bmrf/rf/burst_source.hpp"

#include <memory>

namespace bmrf::rf {

// Burst source that reads IQ from the (already opened) RTL-SDR and runs the
// built-in 4FSK + DMR synchroniser. Returns the null source on builds without
// RTL-SDR support.
std::unique_ptr<IDmrBurstSource> createRtlSdrBurstSource();

// Default RF burst source for the current build (RTL-SDR when available).
std::unique_ptr<IDmrBurstSource> createDefaultRfBurstSource();

// Capture-only mode: read + record raw IQ without running the demodulator
// (needed to sustain high sample rates such as 1.024 MSps).
void rtlSdrBurstSourceSetCaptureOnly(bool captureOnly);

} // namespace bmrf::rf
