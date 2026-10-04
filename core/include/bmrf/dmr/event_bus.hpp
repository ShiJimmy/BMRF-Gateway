#pragma once

#include "bmrf/dmr/event.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace bmrf {

// Thread-safe fan-out event bus. The UI subscribes and polls; the RF decoder,
// CallManager and BrandMeister client publish. Subscribers never block a
// publisher (bounded per-subscriber queue, oldest events dropped on overflow).
class EventBus {
public:
    class Subscription {
    public:
        // Returns false when the queue is empty.
        bool tryPop(DMREvent& out);
        // Drains the whole queue.
        std::vector<DMREvent> popAll();
        // Blocks until an event arrives, the subscription closes or the timeout
        // expires.
        bool waitPop(DMREvent& out, uint32_t timeoutMs = 500);
        size_t size() const;
        void close();

    private:
        friend class EventBus;

        struct Channel {
            mutable std::mutex mutex;
            std::condition_variable cv;
            std::deque<DMREvent> queue;
            size_t maxQueue = 4096;
            bool closed = false;
        };

        explicit Subscription(std::shared_ptr<Channel> channel)
            : m_channel(std::move(channel))
        {
        }

        std::shared_ptr<Channel> m_channel;
    };

    std::unique_ptr<Subscription> subscribe(size_t maxQueue = 4096);

    void publish(const DMREvent& event);
    void clear();
    size_t subscriberCount() const;

private:
    mutable std::mutex m_mutex;
    std::vector<std::weak_ptr<Subscription::Channel>> m_channels;
};

} // namespace bmrf
