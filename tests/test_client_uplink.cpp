#include "test_framework.hpp"

#include "bmrf/brandmeister/client.hpp"
#include "bmrf/brandmeister/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace bmrf;
using namespace bmrf::brandmeister;

namespace {

// Minimal in-memory transport that records everything sent, so we can inspect
// the DMRD datagrams the client produces.
class FakeTransport : public ITransport {
public:
    bool open(const std::string&, uint16_t, std::string&) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    bool send(const uint8_t* data, size_t len) override
    {
        m_sent.emplace_back(data, data + len);
        return true;
    }
    int recv(uint8_t*, size_t) override { return 0; }

    std::vector<std::vector<uint8_t>> m_sent;

private:
    bool m_open = false;
};

std::vector<uint8_t> masterAck(const std::vector<uint8_t>& tail)
{
    std::vector<uint8_t> v = {'R', 'P', 'T', 'A', 'C', 'K'};
    v.insert(v.end(), tail.begin(), tail.end());
    return v;
}

bool isDmrd(const std::vector<uint8_t>& b, uint32_t& dst, uint8_t& options)
{
    if (b.size() != 55 || b[0] != 'D' || b[1] != 'M' || b[2] != 'R' || b[3] != 'D')
        return false;
    dst = (static_cast<uint32_t>(b[8]) << 16) | (static_cast<uint32_t>(b[9]) << 8) | b[10];
    options = b[15];
    return true;
}

DMRFrame voiceFrame(uint32_t src, uint32_t dst, uint32_t seq)
{
    DMRFrame f;
    f.source = DMRFrame::Source::Rf;
    f.timeslot = 1;
    f.source_id = src;
    f.destination_id = dst;
    f.call_type = CallType::Group;
    f.flco = 0;
    f.has_voice = true;
    f.data_type = DataType::Voice;
    f.sequence = seq;
    return f;
}

} // namespace

// Regression: switching talkgroup mid-call must terminate the old call and send
// a fresh Voice LC Header for the new target, instead of leaking the new
// transmission into the previous talkgroup.
BMRF_TEST(client_uplink_talkgroup_change_restarts_call)
{
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* t = transport.get();

    ClientConfig cfg;
    cfg.master_host = "test";
    cfg.master_port = 62031;
    cfg.repeater_id = 0x00466d77;
    cfg.password = "p";
    cfg.repeater.id = 0x00466d77;
    cfg.repeater.callsign = "BG7KNO";

    HomebrewClient client(std::move(transport), cfg);
    client.start(1000);
    client.inject(masterAck({0x11, 0x22, 0x33, 0x44}).data(), 10, 1100); // -> RPTK
    client.inject(masterAck({0x00, 0x46, 0x6d, 0x77}).data(), 10, 1200); // -> RPTC
    client.inject(masterAck({0x00, 0x46, 0x6d, 0x77}).data(), 10, 1300); // -> Active
    BMRF_CHECK(client.state() == NetworkState::Active);

    const size_t baseline = t->m_sent.size();

    // Call to group 46001.
    for (uint32_t i = 0; i < 8; ++i)
        client.sendFrame(voiceFrame(0x00466d77, 46001, i % 6));

    size_t firstHeader = SIZE_MAX;
    for (size_t i = baseline; i < t->m_sent.size(); ++i) {
        uint32_t dst;
        uint8_t opt;
        if (isDmrd(t->m_sent[i], dst, opt) && (opt & 0x20) && (opt & 0x0F) == 0x01 &&
            dst == 46001) {
            firstHeader = i;
            break;
        }
    }
    BMRF_CHECK(firstHeader != SIZE_MAX);

    // Switch to group 46073 mid-call: the client terminates 46001 immediately
    // and holds the new header back briefly (so BrandMeister closes the old
    // session first), then starts the new call.
    for (uint32_t i = 0; i < 8; ++i)
        client.sendFrame(voiceFrame(0x00466d77, 46073, i % 6));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (uint32_t i = 0; i < 6; ++i)
        client.sendFrame(voiceFrame(0x00466d77, 46073, i % 6));

    bool sawTerminatorForOld = false;
    bool sawHeaderForNew = false;
    for (size_t i = firstHeader + 1; i < t->m_sent.size(); ++i) {
        uint32_t dst;
        uint8_t opt;
        if (!isDmrd(t->m_sent[i], dst, opt) || !(opt & 0x20))
            continue;
        const uint8_t dataType = opt & 0x0F;
        if (dataType == 0x02 && dst == 46001)
            sawTerminatorForOld = true;
        if (dataType == 0x01 && dst == 46073)
            sawHeaderForNew = true;
    }
    BMRF_CHECK(sawTerminatorForOld);
    BMRF_CHECK(sawHeaderForNew);
}
