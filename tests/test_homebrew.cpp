#include "test_framework.hpp"

#include "bmrf/brandmeister/rx.hpp"
#include "bmrf/brandmeister/tx.hpp"
#include "bmrf/common/sha256.hpp"

#include <array>
#include <string>
#include <vector>

using namespace bmrf;
using namespace bmrf::brandmeister;

namespace {

std::vector<uint8_t> makeDmrData(uint8_t options, uint32_t src, uint32_t dst,
                                 const std::vector<uint8_t>& payload)
{
    DMRData data;
    data.sequence = 0x2A;
    data.source_id = src;
    data.destination_id = dst;
    data.repeater_id = 0x000001;
    data.options = options;
    data.stream_id = 0xDEADBEEF;
    for (size_t i = 0; i < payload.size() && i < kDmrPayloadBytes; ++i)
        data.data[i] = payload[i];
    data.ber = 0x05;
    data.rssi = 0x64;
    return HomebrewEncoder::buildDmrData(data);
}

std::vector<uint8_t> makeControl(const char* signature, size_t length)
{
    std::vector<uint8_t> out(length, 0);
    for (size_t i = 0; signature[i] != '\0' && i < length; ++i)
        out[i] = static_cast<uint8_t>(signature[i]);
    return out;
}

} // namespace

BMRF_TEST(sha256_known_vector)
{
    BMRF_CHECK_EQ(sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3),
                  std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

BMRF_TEST(homebrew_parses_dmrd_voice)
{
    // go-brandmeister options: slot2 (bit0) + voice + N=3 (high nibble)
    const std::vector<uint8_t> bytes = makeDmrData(0x31, 0x123456, 0x00005B, {});
    const Packet packet = HomebrewParser::parse(bytes.data(), bytes.size());

    BMRF_CHECK(packet.type == PacketType::DmrData);
    BMRF_CHECK(packet.dmr_data.has_value());
    BMRF_CHECK_EQ(packet.dmr_data->source_id, 0x123456u);
    BMRF_CHECK_EQ(packet.dmr_data->destination_id, 0x5Bu);
    BMRF_CHECK_EQ(packet.dmr_data->slot(), 2);
    BMRF_CHECK(!packet.dmr_data->isPrivate());
    BMRF_CHECK(!packet.dmr_data->dataSync());
    BMRF_CHECK(!packet.dmr_data->voiceSync());
    BMRF_CHECK_EQ(packet.dmr_data->voiceFrameIndex(), 3);
    BMRF_CHECK(packet.dmr_data->has_metrics);
    BMRF_CHECK_EQ(packet.dmr_data->ber, 5);
    BMRF_CHECK_EQ(packet.dmr_data->rssi, 100);

    const DMRFrame frame = HomebrewParser::toFrame(*packet.dmr_data, 1000);
    BMRF_CHECK(frame.source == DMRFrame::Source::BrandMeister);
    BMRF_CHECK(frame.data_type == DataType::Voice);
    BMRF_CHECK(frame.has_voice);
    BMRF_CHECK_EQ(frame.sequence, 3u);
    BMRF_CHECK_EQ(frame.timeslot, 2);
    BMRF_CHECK(frame.call_type == CallType::Group);
    BMRF_CHECK_EQ(frame.timestamp_ns, 1000ULL);
}

BMRF_TEST(homebrew_parses_voice_header_lc)
{
    std::vector<uint8_t> payload(kDmrPayloadBytes, 0);
    payload[0] = 0x00; // group, no protect
    payload[1] = 0x00; // feature set
    payload[2] = 0x20; // service options
    payload[3] = 0x00;
    payload[4] = 0x00;
    payload[5] = 0x5B; // destination 91
    payload[6] = 0x12;
    payload[7] = 0x34;
    payload[8] = 0x56; // source 0x123456

    // go-brandmeister options: slot2 (bit0) + data frame type (0x08) + data type
    // VoiceHeader=1 (high nibble) => 0x19.
    const std::vector<uint8_t> bytes =
        makeDmrData(0x19, 0x000001, 0x00005B, payload);
    const Packet packet = HomebrewParser::parse(bytes.data(), bytes.size());
    const DMRFrame frame = HomebrewParser::toFrame(*packet.dmr_data, 0);

    BMRF_CHECK(frame.data_type == DataType::VoiceHeader);
    BMRF_CHECK(frame.voice_lc.has_value());
    BMRF_CHECK_EQ(frame.voice_lc->service_options, 0x20);
    BMRF_CHECK_EQ(frame.voice_lc->source_id, 0x123456u);
    BMRF_CHECK_EQ(frame.voice_lc->destination_id, 0x5Bu);
    BMRF_CHECK(frame.call_type == CallType::Group);
    BMRF_CHECK_EQ(frame.source_id, 0x123456u);
    BMRF_CHECK_EQ(frame.destination_id, 0x5Bu);
}

BMRF_TEST(homebrew_private_call_flag)
{
    // go-brandmeister options: private (bit1) + voice + N=0.
    const std::vector<uint8_t> bytes = makeDmrData(0x02, 0x111111, 0x222222, {});
    const Packet packet = HomebrewParser::parse(bytes.data(), bytes.size());
    const DMRFrame frame = HomebrewParser::toFrame(*packet.dmr_data, 0);

    BMRF_CHECK(packet.dmr_data->isPrivate());
    BMRF_CHECK(frame.call_type == CallType::Private);
    BMRF_CHECK_EQ(static_cast<int>(frame.flco), static_cast<int>(Flco::UnitToUnitVoice));
}

BMRF_TEST(homebrew_control_packets)
{
    {
        std::vector<uint8_t> ack = {'M', 'S', 'T', 'A', 'C', 'K'};
        for (int i = 0; i < 8; ++i)
            ack.push_back(static_cast<uint8_t>('0' + i));
        const std::array<uint8_t, 8> nonce{1, 2, 3, 4, 5, 6, 7, 8};
        ack.insert(ack.end(), nonce.begin(), nonce.end());

        const Packet packet = HomebrewParser::parse(ack.data(), ack.size());
        BMRF_CHECK(packet.type == PacketType::MasterAck);
        BMRF_CHECK(packet.has_nonce);
        for (int i = 0; i < 8; ++i)
            BMRF_CHECK_EQ(packet.nonce[static_cast<size_t>(i)], nonce[static_cast<size_t>(i)]);
    }
    {
        const auto value = makeControl("MSTNAK", 6);
        BMRF_CHECK(HomebrewParser::parse(value.data(), value.size()).type == PacketType::MasterNak);
    }
    {
        const auto value = makeControl("MSTCL", 5);
        BMRF_CHECK(HomebrewParser::parse(value.data(), value.size()).type == PacketType::MasterClose);
    }
    {
        const auto value = makeControl("MSTPING01234567", 15);
        BMRF_CHECK(HomebrewParser::parse(value.data(), value.size()).type == PacketType::MasterPing);
    }
    {
        const auto value = makeControl("RPTPONG", 7);
        BMRF_CHECK(HomebrewParser::parse(value.data(), value.size()).type == PacketType::RepeaterPong);
    }
    {
        const auto value = makeControl("XYZ", 3);
        BMRF_CHECK(HomebrewParser::parse(value.data(), value.size()).type == PacketType::Unknown);
    }
}

BMRF_TEST(homebrew_encoder_lengths)
{
    BMRF_CHECK_EQ(HomebrewEncoder::hexId(0x123456), std::string("00123456"));
    BMRF_CHECK_EQ(HomebrewEncoder::buildLogin(0x123456).size(), size_t(12));
    BMRF_CHECK_EQ(HomebrewEncoder::buildPing(0x123456).size(), size_t(15));

    const std::array<uint8_t, 8> nonce{};
    BMRF_CHECK_EQ(HomebrewEncoder::buildKey(0x123456, nonce, "secret").size(), size_t(76));

    RepeaterConfig config;
    config.callsign = "BMRF";
    config.id = 0x123456;
    config.color_code = 1;
    BMRF_CHECK_EQ(HomebrewEncoder::buildConfig(config).size(), size_t(302));

    BMRF_CHECK_EQ(makeDmrData(0x80, 1, 2, {}).size(), size_t(55));
}
