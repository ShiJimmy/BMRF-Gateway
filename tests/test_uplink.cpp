#include "test_framework.hpp"

#include "bmrf/brandmeister/rx.hpp"
#include "bmrf/brandmeister/tx.hpp"
#include "bmrf/brandmeister/uplink.hpp"

using namespace bmrf;
using namespace bmrf::brandmeister;

BMRF_TEST(uplink_frame_to_dmrd_round_trip)
{
    DMRFrame frame;
    frame.source = DMRFrame::Source::Rf;
    frame.timeslot = 2;
    frame.source_id = 0x123456;
    frame.destination_id = 91;
    frame.call_type = CallType::Group;
    frame.data_type = DataType::Voice;
    frame.sequence = 3;
    frame.has_voice = true;
    for (size_t i = 0; i < frame.ambe.size(); ++i)
        frame.ambe[i] = static_cast<uint8_t>(i);
    frame.ber_percent = 5.0f;
    frame.rssi_dbm = 100.0f;

    const DMRData data = toDmrData(frame, 0x000001, 0xDEADBEEF, 7);

    // MMDVMHost options layout: bit7 slot2, bit6 private, low nibble N.
    BMRF_CHECK_EQ(data.options & 0x80, 0x80);      // timeslot 2
    BMRF_CHECK_EQ(data.options & 0x40, 0);         // group
    BMRF_CHECK_EQ(data.options & 0x0F, 0x03);      // voice frame index N=3
    BMRF_CHECK_EQ(data.source_id, 0x123456u);
    BMRF_CHECK_EQ(data.destination_id, 91u);
    BMRF_CHECK_EQ(data.sequence, 7);
    BMRF_CHECK_EQ(data.ber, 5);
    BMRF_CHECK_EQ(data.rssi, 100);
    for (size_t i = 0; i < 33; ++i)
        BMRF_CHECK_EQ(data.data[i], static_cast<uint8_t>(i));

    const auto wire = HomebrewEncoder::buildDmrData(data);
    BMRF_CHECK_EQ(wire.size(), size_t(55));
}

BMRF_TEST(uplink_voice_header_uses_data_sync)
{
    DMRFrame frame;
    frame.timeslot = 1;
    frame.source_id = 1;
    frame.destination_id = 2;
    frame.call_type = CallType::Private;
    frame.data_type = DataType::VoiceHeader;

    // MMDVMHost layout: bit7 slot, bit6 private, bit5 data sync, low nibble
    // data type (Voice LC Header = 1). Uplink is currently forced to timeslot 2.
    const DMRData data = toDmrData(frame, 1, 2, 0);
    BMRF_CHECK(data.options & 0x80);              // timeslot 2 (forced)
    BMRF_CHECK(data.options & 0x40);              // private
    BMRF_CHECK(data.options & 0x20);              // data sync
    BMRF_CHECK_EQ(data.options & 0x0F, 0x01);     // voice header
}
