#pragma once

#include "bmrf/common/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace bmrf {

// A parsed DMR Full Link Control (voice header / terminator).
struct VoiceLC {
    uint8_t flco = 0; // Full Link Control Opcode (low 7 bits of the LC options)
    bool protect = false; // private/unit call flag (bit 7 of the LC options)
    uint8_t feature_set_id = 0;
    uint8_t service_options = 0;
    uint32_t source_id = 0;
    uint32_t destination_id = 0;
};

// A single protocol / logical DMR frame as produced by either the RF decoder or
// the BrandMeister network parser. This is the core data structure of BMRF
// (design doc §3.1 / §30).
struct DMRFrame {
    enum class Source : uint8_t {
        Rf,
        BrandMeister,
    };

    Source source = Source::BrandMeister;
    uint64_t timestamp_ns = 0;

    // RF / network framing
    int timeslot = 0;   // 1 or 2, 0 = unknown
    int color_code = -1; // RF side only, -1 = unknown

    // Addressing
    uint32_t source_id = 0;
    uint32_t destination_id = 0;

    // Call classification
    CallType call_type = CallType::Unknown;
    uint8_t flco = 0;
    uint8_t service_options = 0;

    DataType data_type = DataType::Idle;

    // Link control
    std::optional<VoiceLC> voice_lc;

    // Voice
    bool has_voice = false;
    std::array<uint8_t, kAmbeBytes> ambe{};

    // Transport / sequence information
    uint32_t sequence = 0;

    // RF measurements (RF source only)
    std::optional<float> rssi_dbm;
    std::optional<float> snr_db;
    std::optional<float> ber_percent;

    // Raw / protocol data when useful for debugging
    std::vector<uint8_t> raw_payload;

    bool isVoice() const { return has_voice || isVoiceFrame(data_type); }
};

} // namespace bmrf
