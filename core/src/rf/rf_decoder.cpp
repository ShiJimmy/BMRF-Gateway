#include "bmrf/rf/rf_decoder.hpp"

#include "bmrf/common/time.hpp"

#include <array>
#include <cstring>

namespace bmrf::rf {

namespace {

// Pack 36 dibits (one AMBE frame) into the 9-byte representation expected by
// the mbelib adapter: within each byte, dibit k contributes its MSB at bit
// (7-2k) and LSB at bit (6-2k).
void packAmbeFrame(const uint8_t* dibits, uint8_t out9[9])
{
    for (int byte = 0; byte < 9; ++byte) {
        uint8_t value = 0;
        for (int k = 0; k < 4; ++k) {
            const uint8_t dibit = dibits[byte * 4 + k] & 0x03;
            value |= static_cast<uint8_t>(((dibit >> 1) & 1) << (7 - 2 * k));
            value |= static_cast<uint8_t>((dibit & 1) << (6 - 2 * k));
        }
        out9[byte] = value;
    }
}

// Build the 33-byte Homebrew-compatible payload from the three 9-byte AMBE
// frames so the existing BM AMBE decoder can be reused unchanged.
void buildHomebrewPayload(const uint8_t ambe27[27], std::array<uint8_t, kDmrPayloadBytes>& payload)
{
    payload.fill(0);
    std::memcpy(payload.data(), ambe27, 13);
    payload[13] = static_cast<uint8_t>(ambe27[13] & 0xF0);
    payload[19] = static_cast<uint8_t>(ambe27[13] & 0x0F);
    std::memcpy(payload.data() + 20, ambe27 + 14, 13);
}

} // namespace

bool RfDecoder::isDataBurst(const DMRBurst& burst)
{
    return burst.sync == SyncType::BsData || burst.sync == SyncType::MsData;
}

bool RfDecoder::decode(const DMRBurst& burst, DMRFrame& out)
{
    if (isDataBurst(burst))
        return false; // LC/data bursts are handled in a later stage

    uint8_t ambe27[27];
    uint8_t frame1[36];
    uint8_t frame2[36];
    uint8_t frame3[36];

    std::memcpy(frame1, burst.dibits.data(), 36);
    std::memcpy(frame2, burst.dibits.data() + 36, 18);
    std::memcpy(frame2 + 18, burst.dibits.data() + 78, 18);
    std::memcpy(frame3, burst.dibits.data() + 96, 36);

    packAmbeFrame(frame1, ambe27 + 0);
    packAmbeFrame(frame2, ambe27 + 9);
    packAmbeFrame(frame3, ambe27 + 18);

    out = DMRFrame();
    out.source = DMRFrame::Source::Rf;
    out.timestamp_ns = burst.timestamp_ns != 0 ? burst.timestamp_ns : nowNs();
    out.timeslot = burst.timeslot;
    out.color_code = burst.color_code;
    out.data_type = (burst.sync == SyncType::BsVoice || burst.sync == SyncType::MsVoice)
                        ? DataType::VoiceSync
                        : DataType::Voice;
    out.sequence = 0;
    out.has_voice = true;
    buildHomebrewPayload(ambe27, out.ambe);
    out.raw_payload.assign(out.ambe.begin(), out.ambe.end());

    if (burst.has_metrics) {
        out.rssi_dbm = burst.rssi_dbm;
        out.snr_db = burst.snr_db;
        out.ber_percent = burst.ber_percent;
    }
    return true;
}

} // namespace bmrf::rf
