#pragma once

#include "bmrf/dmr/frame.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace bmrf {

// A logical call instance aggregated by the CallManager (design doc §3.2).
//
// NOTE: call_id is an application-internal instance id, not a DMR Air Interface
// field.
struct DMRCall {
    uint64_t call_id = 0;

    uint32_t source_id = 0;
    uint32_t destination_id = 0;

    CallType call_type = CallType::Unknown;

    std::optional<int> timeslot;
    std::optional<int> color_code;

    std::optional<uint8_t> flco;
    std::optional<uint8_t> service_options;

    std::optional<VoiceLC> voice_lc;

    uint64_t start_timestamp_ns = 0;
    uint64_t last_frame_timestamp_ns = 0;

    uint64_t frame_count = 0;
    uint64_t lost_frame_count = 0;

    CallState state = CallState::Idle;
    CallSource source = CallSource::Unknown;

    // Network metadata
    std::optional<std::string> bm_peer;
    std::optional<std::string> bm_master;

    // RF measurement
    std::optional<float> rssi;
    std::optional<float> snr;
    std::optional<float> ber;

    uint64_t durationMs() const;
};

} // namespace bmrf
