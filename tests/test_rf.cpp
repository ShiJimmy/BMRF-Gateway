#include "test_framework.hpp"

#include "bmrf/rf/dmr_sync.hpp"
#include "bmrf/rf/fec.hpp"
#include "bmrf/rf/rf_decoder.hpp"

#include <array>
#include <cstring>

using namespace bmrf;
using namespace bmrf::rf;

namespace {

std::array<uint8_t, 7> encodeHamming74(uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3)
{
    std::array<uint8_t, 7> code{d0, d1, d2, d3, 0, 0, 0};
    code[4] = static_cast<uint8_t>(d0 ^ d1 ^ d2);
    code[5] = static_cast<uint8_t>(d1 ^ d2 ^ d3);
    code[6] = static_cast<uint8_t>(d0 ^ d1 ^ d3);
    return code;
}

std::array<uint8_t, 24> patternToDibits(const char* pattern)
{
    std::array<uint8_t, 24> dibits{};
    for (int i = 0; i < 24; ++i)
        dibits[static_cast<size_t>(i)] = pattern[i] == '3' ? 2 : 0; // (dibit|1): only MSB matters
    return dibits;
}

} // namespace

BMRF_TEST(hamming74_corrects_single_bit)
{
    for (uint8_t data = 0; data < 16; ++data) {
        const uint8_t d0 = (data >> 0) & 1, d1 = (data >> 1) & 1, d2 = (data >> 2) & 1,
                      d3 = (data >> 3) & 1;
        const auto original = encodeHamming74(d0, d1, d2, d3);

        for (int flip = 0; flip < 7; ++flip) {
            auto corrupted = original;
            corrupted[static_cast<size_t>(flip)] ^= 1;
            BMRF_CHECK(fec::hamming7x4Decode(corrupted.data()));
            for (int i = 0; i < 4; ++i)
                BMRF_CHECK_EQ(corrupted[static_cast<size_t>(i)], original[static_cast<size_t>(i)]);
        }
    }
}

BMRF_TEST(crc_ccitt_zeroes_after_append)
{
    const std::array<uint8_t, 8> message{1, 0, 1, 1, 0, 0, 1, 1};
    const uint16_t crc = fec::crcCcitt(message.data(), static_cast<int>(message.size()));

    std::array<uint8_t, 24> withCrc{};
    std::memcpy(withCrc.data(), message.data(), message.size());
    for (int i = 0; i < 16; ++i)
        withCrc[static_cast<size_t>(8 + i)] = (crc >> (15 - i)) & 1;

    BMRF_CHECK_EQ(fec::crcCcitt(withCrc.data(), 24), 0);
}

BMRF_TEST(dmr_sync_classification)
{
    const auto bsVoice = patternToDibits("131111333113313313113313");
    const auto bsData = patternToDibits("313333111331131131331131");
    const auto msVoice = patternToDibits("133313311131311113313331");
    const auto msData = patternToDibits("311131133313133331131113");

    BMRF_CHECK(classifySync(bsVoice.data()) == SyncType::BsVoice);
    BMRF_CHECK(classifySync(bsData.data()) == SyncType::BsData);
    BMRF_CHECK(classifySync(msVoice.data()) == SyncType::MsVoice);
    BMRF_CHECK(classifySync(msData.data()) == SyncType::MsData);
}

BMRF_TEST(rf_decoder_voice_round_trip)
{
    // Known 27 AMBE bytes and their inverse packing into 36 dibits per frame.
    std::array<uint8_t, 27> ambe{};
    for (size_t i = 0; i < ambe.size(); ++i)
        ambe[i] = static_cast<uint8_t>(i * 7 + 3);

    DMRBurst burst;
    burst.timeslot = 2;
    burst.color_code = 1;
    burst.sync = SyncType::BsVoice;
    burst.has_metrics = true;
    burst.rssi_dbm = -80.0f;
    burst.timestamp_ns = 123;

    for (int frame = 0; frame < 3; ++frame) {
        for (int byte = 0; byte < 9; ++byte) {
            const uint8_t value = ambe[static_cast<size_t>(frame * 9 + byte)];
            for (int k = 0; k < 4; ++k) {
                const uint8_t msb = (value >> (7 - 2 * k)) & 1;
                const uint8_t lsb = (value >> (6 - 2 * k)) & 1;
                const uint8_t dibit = static_cast<uint8_t>((msb << 1) | lsb);

                const int local = byte * 4 + k;
                int index;
                if (frame == 0)
                    index = local;
                else if (frame == 1)
                    index = (local < 18) ? 36 + local : 78 + (local - 18);
                else
                    index = 96 + local;

                burst.dibits[static_cast<size_t>(index)] = dibit;
            }
        }
    }

    DMRFrame frame;
    BMRF_CHECK(RfDecoder::decode(burst, frame));
    BMRF_CHECK(frame.source == DMRFrame::Source::Rf);
    BMRF_CHECK(frame.has_voice);
    BMRF_CHECK_EQ(frame.timeslot, 2);
    BMRF_CHECK_EQ(frame.color_code, 1);

    // Reverse the Homebrew payload reassembly and compare to the source bytes.
    std::array<uint8_t, 27> recovered{};
    std::memcpy(recovered.data(), frame.ambe.data(), 14);
    recovered[13] = static_cast<uint8_t>((frame.ambe[13] & 0xF0) | (frame.ambe[19] & 0x0F));
    std::memcpy(recovered.data() + 14, frame.ambe.data() + 20, 13);
    for (size_t i = 0; i < ambe.size(); ++i)
        BMRF_CHECK_EQ(recovered[i], ambe[i]);
}

BMRF_TEST(rf_decoder_rejects_data_burst)
{
    DMRBurst burst;
    burst.sync = SyncType::BsData;
    DMRFrame frame;
    BMRF_CHECK(!RfDecoder::decode(burst, frame));
}
