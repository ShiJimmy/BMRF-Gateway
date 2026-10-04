#include "bmrf/brandmeister/uplink.hpp"

#include <cstring>

namespace bmrf::brandmeister {

DMRData toDmrData(const DMRFrame& frame, uint32_t repeater_id, uint32_t stream_id,
                  uint8_t sequence)
{
    DMRData data;
    data.sequence = sequence;
    data.source_id = frame.source_id & 0xFFFFFFu;
    data.destination_id = frame.destination_id & 0xFFFFFFu;
    data.repeater_id = repeater_id;
    data.stream_id = stream_id;

    // Outgoing (client -> master) DMRD uses the standard MMDVMHost / hotspot
    // options layout (the same one DroidStar/MMDVMHost use and BrandMeister
    // accepts): 0x80 slot2, 0x40 private, 0x20 data sync, 0x10 voice sync, low
    // nibble = voice frame index N (or data type).
    uint8_t options = 0;
    // TEMP experiment: force uplink timeslot 2 (matches the working DroidStar).
    options |= 0x80;
    if (frame.call_type == CallType::Private)
        options |= 0x40;

    switch (frame.data_type) {
    case DataType::VoiceSync:
        options |= 0x10; // voice sync
        break;
    case DataType::Voice:
        options |= static_cast<uint8_t>(frame.sequence & 0x0F); // voice frame index
        break;
    default:
        options |= 0x20; // data sync
        options |= static_cast<uint8_t>(static_cast<uint8_t>(frame.data_type) & 0x0F);
        break;
    }
    data.options = options;

    std::memcpy(data.data.data(), frame.ambe.data(), kDmrPayloadBytes);

    if (frame.ber_percent)
        data.ber = static_cast<uint8_t>(*frame.ber_percent);
    if (frame.rssi_dbm)
        data.rssi = static_cast<uint8_t>(*frame.rssi_dbm);
    data.has_metrics = true;

    return data;
}

} // namespace bmrf::brandmeister
