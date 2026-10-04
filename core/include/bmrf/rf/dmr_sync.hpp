#pragma once

#include <cstdint>

namespace bmrf::rf {

// DMR 48-bit sync codewords, represented the same way DSD/DSD-FME do: 24 dibits
// reduced to a 24-character string of '1'/'3' via (dibit | 1). The remaining
// half of the 48-bit codeword is the bit-inverse, so 24 dibits are sufficient
// to classify the four patterns.
enum class SyncType : uint8_t {
    None = 0,
    BsVoice,
    BsData,
    MsVoice,
    MsData,
};

const char* toString(SyncType type);

// Classify 24 sync dibits (values 0..3). Returns SyncType::None if no match.
SyncType classifySync(const uint8_t dibits[24]);

// Correlating classifier: accepts the best match whose 24-bit Hamming distance
// is <= maxErrors. `errors` (optional) receives the distance.
SyncType classifySyncCorrelate(const uint8_t dibits[24], int maxErrors, int* errors);

} // namespace bmrf::rf
