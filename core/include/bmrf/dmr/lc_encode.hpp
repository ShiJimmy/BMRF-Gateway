#pragma once

#include <cstdint>

namespace bmrf {

// Encode a DMR Full Link Control into the 33-byte data-sync burst payload used
// by a Voice LC Header / Terminator DMRD (ETSI TS 102 361-1; the same encoding
// MMDVMHost uses). `dataType` is 1 (Voice LC Header) or 2 (Terminator with LC).
// Returns false for other types. `out` receives the 33-byte payload.
bool encodeDmrLcPayload(uint8_t flco, uint8_t featureSetId, uint8_t serviceOptions,
                        uint32_t destinationId, uint32_t sourceId, uint8_t colorCode,
                        uint8_t dataType, uint8_t out[33]);

// Apply the DMR per-burst embedded signalling to a voice burst payload (the
// 33-byte AMBE payload): `n` is the voice superframe position [0..5]. For n == 0
// the burst becomes a Voice Sync (audio sync pattern); for n in 1..5 the
// embedded Link Control slice + EMB (colour code / link control status) are
// written, so BrandMeister recognises the call's voice frames. `lc` is the
// 9-byte Link Control (flco, featureSetId, serviceOptions, dst[3], src[3]).
void applyVoiceSignalling(uint8_t payload[33], const uint8_t lc[9], int n,
                          uint8_t colorCode);

} // namespace bmrf
