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
constexpr char kRepeaterPingNative[] = "RPTPING";
constexpr char kMasterPongNative[] = "MSTPONG";
constexpr char kMasterAckNative[] = "RPTACK";
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

constexpr uint16_t kDefaultPort = 62031;
constexpr uint16_t kLocalPort = 0; // ephemeral
// MMDVMHost HOMEBREW_DATA_PACKET_LENGTH: DMRD header + 33 byte payload + BER/RSSI.
constexpr size_t kWirePacketLength = 55;
constexpr uint64_t kKeepAliveIntervalMs = 5000;
constexpr uint64_t kNetworkTimeoutMs = 15000;

// The options byte at offset 15 of a DMRD packet, using the MMDVMHost layout the
// master sends on the native MMDVM port (62031) -- the same layout DroidStar and
// MMDVMHost use in both directions:
//   bit7    slot        (1 => timeslot 2)
//   bit6    call type   (1 => private / unit call)
//   bit5    data sync   (Voice LC Header / Terminator)
//   bit4    voice sync
//   bit0-3  voice frame index N (0..5), or data type on data-sync frames
// (e.g. 0xA1 = TS2 + dataSync + VoiceHeader(1), 0x81..0x85 = TS2 voice N=1..5,
//  0x90 = TS2 voice sync, 0xA2 = TS2 + dataSync + Terminator(2)).)
//
// NOTE: parsing this with the go-brandmeister bit0 layout makes the call type
// flip between group/private as N changes (bit1 == N & 2), which resets the
// audio pipeline every few frames.
enum class OptionsMask : uint8_t {
    SlotTs2 = 0x80,
    Private = 0x40,
    DataSync = 0x20,
    VoiceSync = 0x10,
    LowNibble = 0x0F,
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
