#pragma once

#include "bmrf/common/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bmrf::brandmeister {

// Homebrew protocol signatures. Sourced from BrandMeister/go-brandmeister
// (homebrew/const.go) and MMDVMHost (DMRNetwork.cpp).
namespace sig {
constexpr char kDmrData[] = "DMRD";
constexpr char kRepeaterConfig[] = "RPTC";
constexpr char kRepeaterLogin[] = "RPTL";
constexpr char kRepeaterKey[] = "RPTK";
constexpr char kRepeaterPong[] = "RPTPONG";
constexpr char kRepeaterClose[] = "RPTCL";
constexpr char kRepeaterRssi[] = "RPTRSSI";
constexpr char kRepeaterInterrupt[] = "RPTINTR";
constexpr char kRepeaterIdle[] = "RPTIDLE";
constexpr char kRepeaterWake[] = "RPTWAKE";
constexpr char kMaster[] = "MST";
constexpr char kMasterAck[] = "MSTACK";
constexpr char kMasterNak[] = "MSTNAK";
constexpr char kMasterPing[] = "MSTPING";
constexpr char kMasterClose[] = "MSTCL";
} // namespace sig

constexpr uint16_t kDefaultPort = 62030;
constexpr uint16_t kLocalPort = 0; // ephemeral
// MMDVMHost HOMEBREW_DATA_PACKET_LENGTH: DMRD header + 33 byte payload + BER/RSSI.
constexpr size_t kWirePacketLength = 55;
constexpr uint64_t kKeepAliveIntervalMs = 5000;
constexpr uint64_t kNetworkTimeoutMs = 15000;

// The options byte at offset 15 of a DMRD packet. BrandMeister's master uses the
// go-brandmeister (BrandMeister official) layout, verified against live packets:
//   bit0    slot        (1 => timeslot 2)
//   bit1    call type   (1 => private / unit call)
//   bit2-3  frame type  (0 = voice, 1 = voice sync, 2 = data)
//   bit4-7  voice frame index N (0..5) for voice, or data type for data frames
// (e.g. 0x18 = data + VoiceHeader(1), 0x24..0x54 = voice N=0..5, 0x28 = data +
//  TerminatorLC(2)). This differs from MMDVMHost's older layout.
enum class OptionsMask : uint8_t {
    SlotTs2 = 0x01,
    Private = 0x02,
    FrameTypeMask = 0x0C,
    FrameTypeVoice = 0x00,
    FrameTypeVoiceSync = 0x04,
    FrameTypeData = 0x08,
};

// Decoded 55-byte DMRD data packet.
struct DMRData {
    std::array<uint8_t, 4> signature{{'D', 'M', 'R', 'D'}};
    uint8_t sequence = 0;
    uint32_t source_id = 0;      // 24 bit
    uint32_t destination_id = 0; // 24 bit
    uint32_t repeater_id = 0;
    uint8_t options = 0;
    uint32_t stream_id = 0;
    std::array<uint8_t, kDmrPayloadBytes> data{};
    uint8_t ber = 0;
    uint8_t rssi = 0;
    bool has_metrics = false; // true when BER/RSSI were present on the wire

    int slot() const;
    bool isPrivate() const;
    bool dataSync() const;
    bool voiceSync() const;
    uint8_t voiceFrameIndex() const;
    uint8_t dataType() const;
};

} // namespace bmrf::brandmeister
