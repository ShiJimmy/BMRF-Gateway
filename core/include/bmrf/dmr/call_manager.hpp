#pragma once

#include "bmrf/dmr/event.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bmrf {

class EventBus;

struct CallManagerConfig {
    // A call is considered ended when no frame arrives within this window.
    uint64_t call_timeout_ns = 2000000000ULL; // 2 s
};

// Session-wide call statistics for the dashboard.
struct CallManagerStats {
    uint64_t total_calls = 0;
    uint64_t total_frames = 0;
    uint64_t total_lost = 0;
    uint64_t active_calls = 0;
};

// Aggregates DMRFrame into DMRCall instances and emits DMREvent (design doc §9).
//
// The manager is not thread-safe by itself: feed it from a single dispatcher
// thread (or guard externally). Events are published to the attached EventBus.
class CallManager {
public:
    explicit CallManager(EventBus* bus = nullptr, CallManagerConfig config = {});

    void setEventBus(EventBus* bus);
    EventBus* eventBus() const { return m_bus; }

    // Feed one frame. Returns the events generated and publishes them to the bus.
    std::vector<DMREvent> onFrame(const DMRFrame& frame);

    // Expire inactive calls. Call this periodically (e.g. every 100 ms).
    std::vector<DMREvent> tick(uint64_t now_ns);

    std::vector<DMRCall> activeCalls() const;
    size_t activeCallCount() const;
    CallManagerStats stats() const;
    void reset();

private:
    struct CallKey {
        uint32_t source_id = 0;
        uint32_t destination_id = 0;
        CallType call_type = CallType::Unknown;
        int timeslot = 0;

        bool operator==(const CallKey& other) const
        {
            return source_id == other.source_id &&
                   destination_id == other.destination_id &&
                   call_type == other.call_type &&
                   timeslot == other.timeslot;
        }
    };

    struct CallKeyHash {
        size_t operator()(const CallKey& key) const noexcept
        {
            size_t h = static_cast<size_t>(key.source_id) * 1000003u;
            h ^= static_cast<size_t>(key.destination_id) * 1000033u;
            h ^= static_cast<size_t>(key.call_type) * 31u;
            h ^= static_cast<size_t>(key.timeslot + 1) * 17u;
            return h;
        }
    };

    struct CallEntry {
        DMRCall call;
        int lastVoiceIndex = -1;
        bool hasVoiceIndex = false;
    };

    void publish(const std::vector<DMREvent>& events) const;

    EventBus* m_bus = nullptr;
    CallManagerConfig m_config;
    uint64_t m_nextCallId = 1;
    uint64_t m_totalCalls = 0;
    uint64_t m_totalFrames = 0;
    uint64_t m_totalLost = 0;
    std::unordered_map<CallKey, CallEntry, CallKeyHash> m_entries;
};

} // namespace bmrf
