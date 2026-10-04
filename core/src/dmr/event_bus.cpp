#include "bmrf/dmr/event_bus.hpp"

#include <chrono>
#include <utility>

namespace bmrf {

bool EventBus::Subscription::tryPop(DMREvent& out)
{
    std::lock_guard<std::mutex> lock(m_channel->mutex);
    if (m_channel->queue.empty())
        return false;
    out = std::move(m_channel->queue.front());
    m_channel->queue.pop_front();
    return true;
}

std::vector<DMREvent> EventBus::Subscription::popAll()
{
    std::vector<DMREvent> out;
    std::lock_guard<std::mutex> lock(m_channel->mutex);
    out.reserve(m_channel->queue.size());
    while (!m_channel->queue.empty()) {
        out.push_back(std::move(m_channel->queue.front()));
        m_channel->queue.pop_front();
    }
    return out;
}

bool EventBus::Subscription::waitPop(DMREvent& out, uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_channel->mutex);
    const bool signalled = m_channel->cv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this] { return !m_channel->queue.empty() || m_channel->closed; });
    if (!signalled || m_channel->queue.empty())
        return false;
    out = std::move(m_channel->queue.front());
    m_channel->queue.pop_front();
    return true;
}

size_t EventBus::Subscription::size() const
{
    std::lock_guard<std::mutex> lock(m_channel->mutex);
    return m_channel->queue.size();
}

void EventBus::Subscription::close()
{
    std::lock_guard<std::mutex> lock(m_channel->mutex);
    m_channel->closed = true;
    m_channel->cv.notify_all();
}

std::unique_ptr<EventBus::Subscription> EventBus::subscribe(size_t maxQueue)
{
    auto channel = std::make_shared<Subscription::Channel>();
    channel->maxQueue = maxQueue == 0 ? 1 : maxQueue;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_channels.push_back(channel);
    return std::unique_ptr<Subscription>(new Subscription(channel));
}

void EventBus::publish(const DMREvent& event)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_channels.begin(); it != m_channels.end();) {
        auto channel = it->lock();
        if (!channel) {
            it = m_channels.erase(it);
            continue;
        }
        {
            std::lock_guard<std::mutex> channelLock(channel->mutex);
            if (!channel->closed) {
                while (channel->queue.size() >= channel->maxQueue)
                    channel->queue.pop_front(); // drop oldest on overflow
                channel->queue.push_back(event);
            }
        }
        channel->cv.notify_one();
        ++it;
    }
}

void EventBus::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_channels.begin(); it != m_channels.end();) {
        auto channel = it->lock();
        if (!channel) {
            it = m_channels.erase(it);
            continue;
        }
        {
            std::lock_guard<std::mutex> channelLock(channel->mutex);
            channel->queue.clear();
        }
        ++it;
    }
}

size_t EventBus::subscriberCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& weak : m_channels) {
        if (!weak.expired())
            ++count;
    }
    return count;
}

} // namespace bmrf
