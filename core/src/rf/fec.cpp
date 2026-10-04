#include "bmrf/rf/fec.hpp"

namespace bmrf::rf::fec {

namespace {

// Standard Hamming(7,4) parity-check matrix: columns are the 3-bit indices of
// the 7 received positions (1..7), with syndrome 0 meaning "no error".
// positions 1..3 are parity, 4..7 are data.
constexpr uint8_t kParityCheck[3][7] = {
    {1, 1, 1, 0, 1, 0, 0},
    {0, 1, 1, 1, 0, 1, 0},
    {1, 1, 0, 1, 0, 0, 1},
};

} // namespace

namespace {

uint8_t syndromeOf(const uint8_t* bits)
{
    uint8_t syndrome = 0;
    for (int row = 0; row < 3; ++row) {
        uint8_t parity = 0;
        for (int col = 0; col < 7; ++col)
            parity ^= (bits[col] & 1) & kParityCheck[row][col];
        syndrome = static_cast<uint8_t>((syndrome << 1) | (parity & 1));
    }
    return syndrome;
}

} // namespace

bool hamming7x4Decode(uint8_t bits[7])
{
    const uint8_t syndrome = syndromeOf(bits);
    if (syndrome != 0) {
        // The H columns are not ordered by value, so map the syndrome back to
        // the bit position whose unit-error produces it.
        int corrected = -1;
        for (int i = 0; i < 7; ++i) {
            uint8_t unit = 0;
            for (int row = 0; row < 3; ++row)
                unit = static_cast<uint8_t>((unit << 1) | (kParityCheck[row][i] & 1));
            if (unit == syndrome) {
                corrected = i;
                break;
            }
        }
        if (corrected < 0)
            return false; // uncorrectable
        bits[corrected] ^= 1;
    }
    return syndromeOf(bits) == 0;
}

uint16_t crcCcitt(const uint8_t* bits, int length)
{
    uint16_t crc = 0x0000;
    for (int i = 0; i < length; ++i) {
        const uint16_t feedback = static_cast<uint16_t>(((crc >> 15) ^ (bits[i] & 1)) & 1);
        crc = static_cast<uint16_t>(crc << 1);
        if (feedback)
            crc ^= 0x1021; // x^16 + x^12 + x^5 + 1
    }
    return static_cast<uint16_t>(crc & 0xFFFF);
}

uint8_t crc5(const uint8_t* bits, int length)
{
    uint8_t crc = 0x00;
    for (int i = 0; i < length; ++i) {
        const uint8_t feedback = static_cast<uint8_t>(((crc >> 4) ^ (bits[i] & 1)) & 1);
        crc = static_cast<uint8_t>((crc << 1) & 0x1F);
        if (feedback)
            crc ^= 0x05; // x^5 + x^2 + 1 (shortened)
    }
    return static_cast<uint8_t>(crc & 0x1F);
}

} // namespace bmrf::rf::fec
