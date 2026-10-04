#pragma once

#include "bmrf/dmr/frame.hpp"
#include "bmrf/rf/burst.hpp"

namespace bmrf::rf {

// Turns a raw DMR burst into the unified DMRFrame model (RF source).
//
// Stage RF-1 implements the voice path (3 x 72-bit AMBE frames -> 33-byte
// Homebrew-compatible payload). Full Link Control (source/destination IDs)
// requires the BPTC(196,96) decoder and is a later stage; until then RF voice
// frames carry no source/destination.
class RfDecoder {
public:
    // Returns true when `out` was filled with a frame worth processing.
    static bool decode(const DMRBurst& burst, DMRFrame& out);

    // True if the burst's sync pattern indicates a data / LC burst.
    static bool isDataBurst(const DMRBurst& burst);
};

} // namespace bmrf::rf
