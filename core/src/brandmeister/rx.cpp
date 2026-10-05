#include "bmrf/brandmeister/rx.hpp"

#include <cstring>

namespace bmrf::brandmeister {

namespace {

uint32_t readId24(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 16) |
           (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

uint32_t readU32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

bool startsWith(const uint8_t* data, size_t len, const char* signature, size_t signatureLen)
{
    return len >= signatureLen && std::memcmp(data, signature, signatureLen) == 0;
}

} // namespace

Packet HomebrewParser::parse(const uint8_t* data, size_t len)
{
    Packet packet;
    if (data == nullptr || len == 0)
        return packet;

    packet.raw.assign(data, data + len);

    if (startsWith(data, len, sig::kDmrData, 4)) {
        if (len < 53)
            return packet; // truncated DMRD; leave as Unknown
        DMRData dmr;
        std::memcpy(dmr.signature.data(), data, 4);
        dmr.sequence = data[4];
        dmr.source_id = readId24(data + 5);
        dmr.destination_id = readId24(data + 8);
        dmr.repeater_id = readU32(data + 11);
        dmr.options = data[15];
        dmr.stream_id = readU32(data + 16);
        std::memcpy(dmr.data.data(), data + 20, kDmrPayloadBytes);
        if (len >= kWirePacketLength) {
            dmr.ber = data[53];
            dmr.rssi = data[54];
            dmr.has_metrics = true;
        }
        packet.type = PacketType::DmrData;
        packet.dmr_data = dmr;
        return packet;
    }

    if (startsWith(data, len, sig::kMasterAck, 6)) {
        packet.type = PacketType::MasterAck;
        // ASCII dialect: "MSTACK" + 8 byte id echo + optional 8 byte nonce.
        if (len >= 6 + 8 + 8) {
            std::memcpy(packet.nonce.data(), data + 6 + 8, 8);
            packet.has_nonce = true;
        }
        return packet;
    }

    if (startsWith(data, len, sig::kMasterAckNative, 6)) {
        packet.type = PacketType::MasterAck;
        // Native dialect: "RPTACK" + 4 byte salt (login ack) or 4 byte id echo.
        if (len >= 6 + 4) {
            std::memcpy(packet.nonce.data(), data + 6, 4);
            packet.has_nonce = true;
        }
        return packet;
    }

    if (startsWith(data, len, sig::kMasterNak, 6)) {
        packet.type = PacketType::MasterNak;
        return packet;
    }

    if (startsWith(data, len, sig::kMasterClose, 5)) {
        packet.type = PacketType::MasterClose;
        return packet;
    }

    if (startsWith(data, len, sig::kMasterPing, 7)) {
        packet.type = PacketType::MasterPing;
        return packet;
    }

    if (startsWith(data, len, sig::kRepeaterPong, 7)) {
        packet.type = PacketType::RepeaterPong;
        return packet;
    }

    if (startsWith(data, len, sig::kMasterPongNative, 7)) {
        packet.type = PacketType::RepeaterPong;
        return packet;
    }

    return packet;
}

DMRFrame HomebrewParser::toFrame(const DMRData& d, uint64_t timestamp_ns)
{
    DMRFrame frame;
    frame.source = DMRFrame::Source::BrandMeister;
    frame.timestamp_ns = timestamp_ns;
    frame.timeslot = d.slot();
    frame.color_code = -1;
    frame.source_id = d.source_id;
    frame.destination_id = d.destination_id;
    frame.flco = d.isPrivate() ? static_cast<uint8_t>(Flco::UnitToUnitVoice)
                               : static_cast<uint8_t>(Flco::GroupVoice);
    frame.call_type = d.isPrivate() ? CallType::Private : CallType::Group;
    frame.sequence = d.sequence;
    frame.ambe = d.data;
    frame.raw_payload.assign(d.data.begin(), d.data.end());
    if (d.has_metrics) {
        frame.rssi_dbm = static_cast<float>(d.rssi);
        frame.ber_percent = static_cast<float>(d.ber);
    }

    if (d.dataSync()) {
        const DataType type = static_cast<DataType>(d.dataType());
        frame.data_type = type;

        if (type == DataType::VoiceHeader || type == DataType::TerminatorLC) {
            VoiceLC lc;
            if (parseVoiceLC(d.data, lc)) {
                frame.voice_lc = lc;
                frame.flco = lc.flco;
                frame.service_options = lc.service_options;
                frame.source_id = lc.source_id;
                frame.destination_id = lc.destination_id;
                frame.call_type = lc.protect ? CallType::Private : CallType::Group;
            }
        } else if (type == DataType::EmbeddedData) {
            VoiceLC lc;
            lc.flco = d.data[0] & 0x7F;
            lc.protect = (d.data[0] & 0x80) != 0;
            lc.feature_set_id = d.data[1];
            lc.destination_id = readId24(&d.data[2]);
            lc.source_id = readId24(&d.data[5]);
            frame.voice_lc = lc;
        }
    } else if (d.voiceSync()) {
        frame.data_type = DataType::VoiceSync;
        frame.sequence = 0;
        frame.has_voice = true;
    } else {
        frame.data_type = DataType::Voice;
        frame.sequence = d.voiceFrameIndex();
        frame.has_voice = true;
    }

    return frame;
}

bool HomebrewParser::parseVoiceLC(const std::array<uint8_t, kDmrPayloadBytes>& payload,
                                  VoiceLC& out)
{
    out.flco = payload[0] & 0x7F;
    out.protect = (payload[0] & 0x80) != 0;
    out.feature_set_id = payload[1];
    out.service_options = payload[2];
    out.destination_id = readId24(&payload[3]);
    out.source_id = readId24(&payload[6]);
    return true;
}

} // namespace bmrf::brandmeister
