#include "bmrf/audio/jitter_buffer.hpp"

#include <chrono>
#include <utility>

namespace bmrf {

JitterBuffer::JitterBuffer(size_t capacity)
    : m_capacity(capacity == 0 ? 1 : capacity)
{
}

bool JitterBuffer::push(const VoiceBurst& burst)
{
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed)
            return false;
        while (m_queue.size() >= m_capacity) {
            m_queue.pop_front();
            ++m_dropped;
            dropped = true;
        }
        m_queue.push_back(burst);
    }
    m_cv.notify_one();
    return !dropped;
}

bool JitterBuffer::tryPop(VoiceBurst& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

bool JitterBuffer::waitPop(VoiceBurst& out, uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const bool signalled = m_cv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this] { return !m_queue.empty() || m_closed; });
    if (!signalled || m_queue.empty())
        return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void JitterBuffer::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    m_closed = false;
}

void JitterBuffer::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closed = true;
    m_cv.notify_all();
}

size_t JitterBuffer::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

size_t JitterBuffer::dropped() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dropped;
}

} // namespace bmrf
