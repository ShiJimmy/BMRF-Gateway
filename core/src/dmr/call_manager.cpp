#include "bmrf/dmr/call_manager.hpp"

#include "bmrf/dmr/event_bus.hpp"

#include <utility>

namespace bmrf {

namespace {

int voiceIndexFor(const DMRFrame& frame)
{
    switch (frame.data_type) {
    case DataType::Voice:
        return static_cast<int>(frame.sequence % static_cast<uint32_t>(kVoiceFramesPerSuperframe));
    case DataType::VoiceSync:
        return 0;
    case DataType::VoiceFrameA: return 0;
    case DataType::VoiceFrameB: return 1;
    case DataType::VoiceFrameC: return 2;
    case DataType::VoiceFrameD: return 3;
    case DataType::VoiceFrameE: return 4;
    case DataType::VoiceFrameF: return 5;
    default: return -1;
    }
}

} // namespace

CallManager::CallManager(EventBus* bus, CallManagerConfig config)
    : m_bus(bus), m_config(config)
{
}

void CallManager::setEventBus(EventBus* bus)
{
    m_bus = bus;
}

std::vector<DMREvent> CallManager::onFrame(const DMRFrame& frame)
{
    std::vector<DMREvent> events;
    if (frame.source_id == 0 && frame.destination_id == 0)
        return events;

    CallKey key;
    key.source_id = frame.source_id;
    key.destination_id = frame.destination_id;
    key.call_type = frame.call_type;
    key.timeslot = frame.timeslot;

    auto it = m_entries.find(key);
    const bool newCall = (it == m_entries.end());

    if (newCall) {
        ++m_totalCalls;
        CallEntry entry;
        DMRCall& call = entry.call;
        call.call_id = m_nextCallId++;
        call.source_id = frame.source_id;
        call.destination_id = frame.destination_id;
        call.call_type = frame.call_type;
        if (frame.timeslot > 0)
            call.timeslot = frame.timeslot;
        if (frame.color_code >= 0)
            call.color_code = frame.color_code;
        call.flco = frame.flco;
        call.service_options = frame.service_options;
        call.voice_lc = frame.voice_lc;
        call.start_timestamp_ns = frame.timestamp_ns;
        call.last_frame_timestamp_ns = frame.timestamp_ns;
        call.source = (frame.source == DMRFrame::Source::Rf) ? CallSource::Rf
                                                             : CallSource::BrandMeister;
        call.state = CallState::CallStart;
        if (frame.rssi_dbm)
            call.rssi = frame.rssi_dbm;
        if (frame.snr_db)
            call.snr = frame.snr_db;
        if (frame.ber_percent)
            call.ber = frame.ber_percent;

        it = m_entries.emplace(key, std::move(entry)).first;

        DMREvent event;
        event.type = DMREventType::CallStarted;
        event.call = it->second.call;
        event.frame = frame;
        event.timestamp_ns = frame.timestamp_ns;
        events.push_back(std::move(event));
    }

    CallEntry& entry = it->second;
    DMRCall& call = entry.call;
    call.last_frame_timestamp_ns = frame.timestamp_ns;

    if (frame.voice_lc) {
        call.voice_lc = frame.voice_lc;
        call.call_type = frame.voice_lc->protect ? CallType::Private : CallType::Group;
    }
    if (frame.flco != 0 || !call.flco)
        call.flco = frame.flco;
    if (frame.service_options != 0)
        call.service_options = frame.service_options;
    if (frame.color_code >= 0)
        call.color_code = frame.color_code;
    if (frame.rssi_dbm)
        call.rssi = frame.rssi_dbm;
    if (frame.snr_db)
        call.snr = frame.snr_db;
    if (frame.ber_percent)
        call.ber = frame.ber_percent;

    if (frame.data_type == DataType::TerminatorLC) {
        call.state = CallState::CallEnd;

        DMREvent event;
        event.type = DMREventType::CallEnded;
        event.call = call;
        event.frame = frame;
        event.timestamp_ns = frame.timestamp_ns;
        events.push_back(std::move(event));

        m_entries.erase(it);
        publish(events);
        return events;
    }

    if (isVoiceFrame(frame.data_type)) {
        const int index = voiceIndexFor(frame);
        if (index >= 0) {
            if (entry.hasVoiceIndex) {
                const int expected =
                    (entry.lastVoiceIndex + 1) % kVoiceFramesPerSuperframe;
                const int diff =
                    (index - expected + kVoiceFramesPerSuperframe) % kVoiceFramesPerSuperframe;
                if (diff > 0) {
                    call.lost_frame_count += static_cast<uint64_t>(diff);
                    m_totalLost += static_cast<uint64_t>(diff);
                }
            }
            entry.lastVoiceIndex = index;
            entry.hasVoiceIndex = true;
        }
        call.frame_count++;
        ++m_totalFrames;
        call.state = CallState::Active;

        DMREvent event;
        event.type = DMREventType::VoiceFrameReceived;
        event.call = call;
        event.frame = frame;
        event.timestamp_ns = frame.timestamp_ns;
        events.push_back(std::move(event));
    } else {
        call.state = CallState::Active;

        DMREvent event;
        event.type = DMREventType::FrameReceived;
        event.call = call;
        event.frame = frame;
        event.timestamp_ns = frame.timestamp_ns;
        events.push_back(std::move(event));
    }

    DMREvent updated;
    updated.type = DMREventType::CallUpdated;
    updated.call = call;
    updated.timestamp_ns = frame.timestamp_ns;
    events.push_back(std::move(updated));

    publish(events);
    return events;
}

std::vector<DMREvent> CallManager::tick(uint64_t now_ns)
{
    std::vector<DMREvent> events;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        const DMRCall& call = it->second.call;
        const bool expired = now_ns > call.last_frame_timestamp_ns &&
                             (now_ns - call.last_frame_timestamp_ns) >= m_config.call_timeout_ns;
        if (expired) {
            DMREvent event;
            event.type = DMREventType::CallEnded;
            event.call = call;
            event.call.state = CallState::CallEnd;
            event.timestamp_ns = now_ns;
            events.push_back(std::move(event));
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    publish(events);
    return events;
}

std::vector<DMRCall> CallManager::activeCalls() const
{
    std::vector<DMRCall> calls;
    calls.reserve(m_entries.size());
    for (const auto& pair : m_entries)
        calls.push_back(pair.second.call);
    return calls;
}

size_t CallManager::activeCallCount() const
{
    return m_entries.size();
}

CallManagerStats CallManager::stats() const
{
    CallManagerStats result;
    result.total_calls = m_totalCalls;
    result.total_frames = m_totalFrames;
    result.total_lost = m_totalLost;
    result.active_calls = m_entries.size();
    return result;
}

void CallManager::reset()
{
    m_entries.clear();
}

void CallManager::publish(const std::vector<DMREvent>& events) const
{
    if (m_bus == nullptr)
        return;
    for (const auto& event : events)
        m_bus->publish(event);
}

} // namespace bmrf
