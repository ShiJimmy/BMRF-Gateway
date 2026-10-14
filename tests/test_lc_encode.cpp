#include "test_framework.hpp"

#include "bmrf/dmr/lc_encode.hpp"

#include <cstdio>
#include <cstring>

using namespace bmrf;

// Basic sanity: the encoder produces a non-zero data-sync payload and rejects
// unsupported data types. (Byte-exact validation against a captured BrandMeister
// Voice LC Header is still pending; see RF uplink notes.)
BMRF_TEST(lc_encode_produces_payload)
{
    uint8_t out[33];
    BMRF_CHECK(encodeDmrLcPayload(0x00, 0x00, 0x00, 46001u, 4616466u, 1, 1, out));

    bool nonZero = false;
    for (uint8_t b : out) {
        if (b != 0U) {
            nonZero = true;
            break;
        }
    }
    BMRF_CHECK(nonZero);

    // Deterministic for identical input.
    uint8_t out2[33];
    BMRF_CHECK(encodeDmrLcPayload(0x00, 0x00, 0x00, 46001u, 4616466u, 1, 1, out2));
    BMRF_CHECK(std::memcmp(out, out2, 33) == 0);

    // Different color code changes the payload (slot type is protected).
    uint8_t out3[33];
    BMRF_CHECK(encodeDmrLcPayload(0x00, 0x00, 0x00, 46001u, 4616466u, 2, 1, out3));
    BMRF_CHECK(std::memcmp(out, out3, 33) != 0);
}

// The data-sync pattern (MS sourced) must appear in payload bytes 13..19; this
// is what a BrandMeister data-sync burst (Voice LC Header / Terminator) carries.
BMRF_TEST(lc_encode_has_data_sync_pattern)
{
    uint8_t out[33];
    BMRF_CHECK(encodeDmrLcPayload(0x00, 0x00, 0x00, 46001u, 4616466u, 1, 1, out));
    BMRF_CHECK_EQ(out[14], 0x5DU);
    BMRF_CHECK_EQ(out[15], 0x7FU);
    BMRF_CHECK_EQ(out[16], 0x77U);
    BMRF_CHECK_EQ(out[17], 0xFDU);
    BMRF_CHECK_EQ(out[18], 0x75U);
}

// Locked regression vector for the embedded-LC slices. Payload bytes 15..17 are
// the pure embedded-LC chunk, independent of AMBE and colour code. The encoder
// was originally verified byte-for-byte against a captured DroidStar voice frame.
BMRF_TEST(lc_encode_embedded_slices_stable)
{
    const uint8_t lc[9] = {0x00, 0x00, 0x00, 0x12, 0xD6, 0x87, 0x12, 0xD6, 0x87};
    const uint8_t exp1[3] = {0x21, 0x10, 0x61};
    const uint8_t exp2[3] = {0xc3, 0xc1, 0x10};
    const uint8_t exp3[3] = {0x30, 0xf2, 0xb1};
    const uint8_t exp4[3] = {0xe0, 0xc3, 0x52};
    const uint8_t* exp[4] = {exp1, exp2, exp3, exp4};

    for (int n = 1; n <= 4; ++n) {
        uint8_t payload[33];
        std::memset(payload, 0, sizeof(payload));
        applyVoiceSignalling(payload, lc, n, 1);
        std::printf("SLICE%d=%02x%02x%02x\n", n, payload[15], payload[16], payload[17]);
        BMRF_CHECK_EQ(payload[15], exp[n - 1][0]);
        BMRF_CHECK_EQ(payload[16], exp[n - 1][1]);
        BMRF_CHECK_EQ(payload[17], exp[n - 1][2]);
    }
}

BMRF_TEST(lc_encode_rejects_unknown_type)
{
    uint8_t out[33];
    BMRF_CHECK(!encodeDmrLcPayload(0x00, 0, 0, 1, 2, 1, 7, out));
}

// Locked regression vector for the Voice LC Header (src=dst=1234567, group, CC1).
BMRF_TEST(lc_encode_header_vector)
{
    uint8_t out[33];
    BMRF_CHECK(encodeDmrLcPayload(0x00, 0x00, 0x00, 1234567u, 1234567u, 0, 1, out));
    std::printf("HEADER_VEC=");
    for (int i = 0; i < 33; ++i)
        std::printf("%02x", out[i]);
    std::printf("\n");
    static const uint8_t exp[33] = {
        0x16, 0xb3, 0x11, 0x20, 0x03, 0x30, 0x58, 0xd0, 0xb3, 0x90, 0x3d,
        0xc3, 0xc0, 0x6d, 0x5d, 0x7f, 0x77, 0xfd, 0x75, 0x73, 0xaf, 0x7c,
        0x37, 0xb8, 0x16, 0xd1, 0x8a, 0xa2, 0x40, 0xc3, 0xbe, 0x06, 0xf6};
    for (int i = 0; i < 33; ++i)
        BMRF_CHECK_EQ(out[i], exp[i]);
}
