#pragma once

#include "bmrf/brandmeister/homebrew.hpp"
#include "bmrf/dmr/frame.hpp"

namespace bmrf::brandmeister {

// Convert a unified RF frame into a Homebrew DMRD packet payload (design doc §6:
// do not re-encode RF AMBE; carry the protocol fields through unchanged).
DMRData toDmrData(const DMRFrame& frame, uint32_t repeater_id, uint32_t stream_id,
                  uint8_t sequence);

} // namespace bmrf::brandmeister
