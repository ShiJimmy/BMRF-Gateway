#pragma once

#include <cstdint>

namespace bmrf::rf::fec {

// Hamming(7,4) as used by the DMR CACH TACT field. Corrects a single-bit error
// in place and returns true when the corrected word is a valid codeword.
// `bits` holds 7 hard-decision bits (0/1); on success bits[0..3] are the data.
bool hamming7x4Decode(uint8_t bits[7]);

// DMR CRC-CCITT (x^16 + x^12 + x^5 + 1), used by Full Link Control.
uint16_t crcCcitt(const uint8_t* bits, int length);

// DMR 5-bit CRC used by the Full Link Control header.
uint8_t crc5(const uint8_t* bits, int length);

} // namespace bmrf::rf::fec
