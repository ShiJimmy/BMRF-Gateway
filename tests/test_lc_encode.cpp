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

// Validate the embedded-LC slices against real DroidStar voice frames captured
// over the air (src=dst=4615543, group call). Payload bytes 15..17 are the pure
// embedded-LC chunk, independent of AMBE/colour code.
BMRF_TEST(lc_encode_embedded_slices_match_droidstar)
{
    const uint8_t lc[9] = {0x00, 0x00, 0x00, 0x46, 0x6d, 0x77, 0x46, 0x6d, 0x77};
    const uint8_t exp1[3] = {0x91, 0xe1, 0x42};
    const uint8_t exp2[3] = {0xb1, 0xe0, 0x53};
    const uint8_t exp3[3] = {0x71, 0x70, 0xf2};
    const uint8_t exp4[3] = {0xf1, 0x82, 0x83};
    const uint8_t* exp[4] = {exp1, exp2, exp3, exp4};

    for (int n = 1; n <= 4; ++n) {
        uint8_t payload[33];
        std::memset(payload, 0, sizeof(payload));
        applyVoiceSignalling(payload, lc, n, 1);
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

// Compare our Voice LC Header against DroidStar's captured header for the same
// LC (src=dst=4615543, group, CC1). DroidStar payload:
//   0f0a07f4774c7b4062a055a2c06d5d7f77fd7573ae4c0e50c6217ae161057a0676
BMRF_TEST(lc_encode_header_matches_droidstar)
{
    uint8_t out[33];
    BMRF_CHECK(encodeDmrLcPayload(0x00, 0x00, 0x00, 4615543u, 4615543u, 0, 1, out));
    std::printf("HEADER_SELF=");
    for (int i = 0; i < 33; ++i)
        std::printf("%02x", out[i]);
    std::printf("\n");
    static const uint8_t exp[33] = {
        0x0f, 0x0a, 0x07, 0xf4, 0x77, 0x4c, 0x7b, 0x40, 0x62, 0xa0, 0x55,
        0xa2, 0xc0, 0x6d, 0x5d, 0x7f, 0x77, 0xfd, 0x75, 0x73, 0xae, 0x4c,
        0x0e, 0x50, 0xc6, 0x21, 0x7a, 0xe1, 0x61, 0x05, 0x7a, 0x06, 0x76};
    for (int i = 0; i < 33; ++i)
        BMRF_CHECK_EQ(out[i], exp[i]);
}
