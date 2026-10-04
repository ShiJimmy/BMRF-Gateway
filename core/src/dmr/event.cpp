#include "bmrf/dmr/event.hpp"

namespace bmrf {

const char* toString(DMREventType v)
{
    switch (v) {
    case DMREventType::CallStarted:        return "CallStarted";
    case DMREventType::FrameReceived:      return "FrameReceived";
    case DMREventType::VoiceFrameReceived: return "VoiceFrameReceived";
    case DMREventType::CallUpdated:        return "CallUpdated";
    case DMREventType::CallEnded:          return "CallEnded";
    case DMREventType::DecoderError:       return "DecoderError";
    case DMREventType::NetworkConnected:   return "NetworkConnected";
    case DMREventType::NetworkDisconnected:return "NetworkDisconnected";
    case DMREventType::NetworkKeepalive:   return "NetworkKeepalive";
    default:                               return "Unknown";
    }
}

} // namespace bmrf
