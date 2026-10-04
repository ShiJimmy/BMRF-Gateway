#pragma once

#include "bmrf/common/types.hpp"

#include <array>
#include <cstdint>

namespace bmrf {

// One DMR voice burst queued for the audio pipeline. The 33-byte payload is the
// raw DMR voice payload; AMBE frame deinterleaving / unpacking is the decoder's
// responsibility (see IAmbeDecoder).
struct VoiceBurst {
    std::array<uint8_t, kDmrPayloadBytes> payload{};
    uint8_t voiceIndex = 0;
    uint64_t sequence = 0;
    uint64_t timestampNs = 0;
    int timeslot = 0;
    bool voiceSync = false;
};

} // namespace bmrf
