#pragma once

#include "bmrf/audio/voice.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace bmrf {

// Bounded, thread-safe voice queue decoupling the network RX thread from the
// audio thread (design doc §8). Drops the oldest burst on overflow.
class JitterBuffer {
public:
    explicit JitterBuffer(size_t capacity = 32);

    bool push(const VoiceBurst& burst); // false when a burst had to be dropped
    bool tryPop(VoiceBurst& out);
    bool waitPop(VoiceBurst& out, uint32_t timeoutMs);

    void clear(); // also reopens a closed buffer
    void close(); // wakes waiters; further push()/waitPop() return false

    size_t size() const;
    size_t dropped() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<VoiceBurst> m_queue;
    size_t m_capacity;
    size_t m_dropped = 0;
    bool m_closed = false;
};

} // namespace bmrf
