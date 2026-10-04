#pragma once

#include <chrono>
#include <cstdint>

namespace bmrf {

// Monotonic nanoseconds since an unspecified epoch. Suitable for latency /
// call duration measurement, not for wall-clock display.
inline uint64_t nowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline uint64_t nowMs()
{
    return nowNs() / 1000000ULL;
}

inline uint64_t msToNs(uint64_t ms)
{
    return ms * 1000000ULL;
}

} // namespace bmrf
