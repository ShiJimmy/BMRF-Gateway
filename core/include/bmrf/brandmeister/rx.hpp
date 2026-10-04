#pragma once

#include "bmrf/brandmeister/homebrew.hpp"
#include "bmrf/dmr/frame.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bmrf::brandmeister {

enum class PacketType : uint8_t {
    DmrData,
    MasterAck,
    MasterNak,
    MasterPing,
    MasterClose,
    RepeaterPong,
    Unknown,
};

inline const char* toString(PacketType type)
{
    switch (type) {
    case PacketType::DmrData:      return "DmrData";
    case PacketType::MasterAck:    return "MasterAck";
    case PacketType::MasterNak:    return "MasterNak";
    case PacketType::MasterPing:   return "MasterPing";
    case PacketType::MasterClose:  return "MasterClose";
    case PacketType::RepeaterPong: return "RepeaterPong";
    default:                       return "Unknown";
    }
}

struct Packet {
    PacketType type = PacketType::Unknown;
    std::vector<uint8_t> raw;
    std::optional<DMRData> dmr_data;
    std::array<uint8_t, 8> nonce{};
    bool has_nonce = false;
    uint32_t repeater_id = 0;
};

// Stateless Homebrew RX parser. This is the "BM Network Parser" of design doc
// §7: it turns a raw UDP datagram into either a DMRD packet or a control packet.
class HomebrewParser {
public:
    static Packet parse(const uint8_t* data, size_t len);

    // Convert a decoded DMRD packet into the unified DMRFrame model. Parses the
    // embedded Link Control when present.
    static DMRFrame toFrame(const DMRData& d, uint64_t timestamp_ns);

    // Parse a 9-byte Full Link Control from a voice header / terminator payload.
    static bool parseVoiceLC(const std::array<uint8_t, kDmrPayloadBytes>& payload,
                             VoiceLC& out);
};

} // namespace bmrf::brandmeister
