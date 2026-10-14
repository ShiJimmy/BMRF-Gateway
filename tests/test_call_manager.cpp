#include "test_framework.hpp"

#include "bmrf/dmr/call_manager.hpp"
#include "bmrf/dmr/event_bus.hpp"

using namespace bmrf;

namespace {

DMRFrame makeFrame(DataType type, uint32_t src, uint32_t dst, CallType callType,
                   uint64_t timestampNs, uint32_t sequence = 0)
{
    DMRFrame frame;
    frame.source = DMRFrame::Source::BrandMeister;
    frame.timestamp_ns = timestampNs;
    frame.timeslot = 2;
    frame.source_id = src;
    frame.destination_id = dst;
    frame.call_type = callType;
    frame.data_type = type;
    frame.sequence = sequence;
    frame.has_voice = isVoiceFrame(type);
    return frame;
}

} // namespace

BMRF_TEST(call_manager_starts_and_counts)
{
    EventBus bus;
    auto subscription = bus.subscribe();
    CallManager manager(&bus);

    const auto startEvents =
        manager.onFrame(makeFrame(DataType::VoiceHeader, 100, 91, CallType::Group, 1000000000ULL));
    BMRF_CHECK_EQ(startEvents.size(), size_t(3)); // CallStarted + FrameReceived + CallUpdated
    BMRF_CHECK(startEvents.front().type == DMREventType::CallStarted);
    BMRF_CHECK_EQ(manager.activeCallCount(), size_t(1));

    manager.onFrame(makeFrame(DataType::Voice, 100, 91, CallType::Group, 1060000000ULL, 0));
    manager.onFrame(makeFrame(DataType::Voice, 100, 91, CallType::Group, 1120000000ULL, 1));
    const auto voiceEvents =
        manager.onFrame(makeFrame(DataType::Voice, 100, 91, CallType::Group, 1180000000ULL, 3));

    BMRF_CHECK(!voiceEvents.empty());
    BMRF_CHECK(voiceEvents.front().type == DMREventType::VoiceFrameReceived);
    BMRF_CHECK_EQ(voiceEvents.front().call.frame_count, 3ULL);
    BMRF_CHECK_EQ(voiceEvents.front().call.lost_frame_count, 1ULL);

    const auto expired = manager.tick(1180000000ULL + 3000000000ULL);
    BMRF_CHECK_EQ(expired.size(), size_t(1));
    BMRF_CHECK(expired.front().type == DMREventType::CallEnded);
    BMRF_CHECK_EQ(manager.activeCallCount(), size_t(0));

    BMRF_CHECK(subscription->size() > 0);
}

BMRF_TEST(call_manager_terminator_ends_call)
{
    CallManager manager;
    manager.onFrame(makeFrame(DataType::Voice, 200, 91, CallType::Group, 1000ULL, 0));
    const auto events =
        manager.onFrame(makeFrame(DataType::TerminatorLC, 200, 91, CallType::Group, 2000ULL));

    BMRF_CHECK_EQ(events.size(), size_t(1));
    BMRF_CHECK(events.front().type == DMREventType::CallEnded);
    BMRF_CHECK_EQ(manager.activeCallCount(), size_t(0));
}

// The RF (DSDcc) path feeds one DMRFrame per burst with a per-burst voice
// sequence cycling [0..5] (Homebrew voice number N). No frames are missing, so
// the lost-frame counter must stay zero (regression for the 80% false-loss bug).
BMRF_TEST(call_manager_burst_sequence_no_false_loss)
{
    CallManager manager;
    for (uint32_t i = 0; i < 60; ++i) {
        manager.onFrame(makeFrame(DataType::Voice, 1234567, 73, CallType::Group,
                                  1000000000ULL + i * 60000000ULL, i % 6));
    }
    const CallManagerStats stats = manager.stats();
    BMRF_CHECK_EQ(stats.total_frames, 60ULL);
    BMRF_CHECK_EQ(stats.total_lost, 0ULL);
}

BMRF_TEST(call_manager_separates_timeslots)
{
    CallManager manager;
    manager.onFrame(makeFrame(DataType::Voice, 300, 91, CallType::Group, 1000ULL, 0));

    DMRFrame other = makeFrame(DataType::Voice, 300, 91, CallType::Group, 1000ULL, 0);
    other.timeslot = 1;
    manager.onFrame(other);

    BMRF_CHECK_EQ(manager.activeCallCount(), size_t(2));
}
