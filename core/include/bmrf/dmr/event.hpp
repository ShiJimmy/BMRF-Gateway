#pragma once

#include "bmrf/dmr/call.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace bmrf {

enum class DMREventType : uint8_t {
    CallStarted,
    FrameReceived,
    VoiceFrameReceived,
    CallUpdated,
    CallEnded,
    DecoderError,
    NetworkConnected,
    NetworkDisconnected,
    NetworkKeepalive,
};

struct DMREvent {
    DMREventType type = DMREventType::CallUpdated;
    DMRCall call;
    std::optional<DMRFrame> frame;
    std::string message; // DecoderError / network event detail
    uint64_t timestamp_ns = 0;
};

const char* toString(DMREventType v);

} // namespace bmrf
