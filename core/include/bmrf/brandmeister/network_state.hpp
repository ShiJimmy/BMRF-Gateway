#pragma once

#include <cstdint>

namespace bmrf::brandmeister {

// BrandMeister client state machine (design doc §14).
//   DISCONNECTED -> CONNECTING -> REGISTERED -> ACTIVE
//   any state     -> RECONNECTING -> CONNECTING
enum class NetworkState : uint8_t {
    Disconnected = 0,
    Connecting,
    Registered,
    Active,
    Reconnecting,
};

inline const char* toString(NetworkState state)
{
    switch (state) {
    case NetworkState::Disconnected: return "DISCONNECTED";
    case NetworkState::Connecting:   return "CONNECTING";
    case NetworkState::Registered:   return "REGISTERED";
    case NetworkState::Active:       return "ACTIVE";
    case NetworkState::Reconnecting: return "RECONNECTING";
    default:                         return "UNKNOWN";
    }
}

} // namespace bmrf::brandmeister
