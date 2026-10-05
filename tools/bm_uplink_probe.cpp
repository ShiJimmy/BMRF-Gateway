// bm_uplink_probe - replay a raw cu8 IQ capture through the real DSDcc decoder
// and the real HomebrewClient uplink to BrandMeister. Used to reproduce the
// "RF -> BM uplink not seen on BM" issue on the desktop with full control and
// fast iteration, independent of the Android app.
//
// usage: bm_uplink_probe <file.cu8> <host> <id> <callsign> <password>
//                        [sampleRate=240000] [offsetHz=40000] [capture.txt]

#include "bmrf/brandmeister/client.hpp"
#include "bmrf/brandmeister/transport.hpp"
#include "bmrf/capture/capture.hpp"
#include "bmrf/common/time.hpp"
#include "bmrf/rf/dsdcc_demod.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace bmrf;
using namespace bmrf::brandmeister;
using bmrf::rf::DsdccDmrDemod;

namespace {

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

const char* stateName(NetworkState s)
{
    switch (s) {
    case NetworkState::Disconnected: return "Disconnected";
    case NetworkState::Connecting:   return "Connecting";
    case NetworkState::Registered:   return "Registered";
    case NetworkState::Active:       return "Active";
    case NetworkState::Reconnecting: return "Reconnecting";
    default:                         return "?";
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 6) {
        std::printf("usage: bm_uplink_probe <file.cu8> <host> <id> <callsign> <password> "
                    "[sampleRate] [offsetHz] [capture.txt]\n");
        return 2;
    }
    const std::string file = argv[1];
    const std::string host = argv[2];
    const uint32_t id = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const std::string callsign = argv[4];
    const std::string password = argv[5];
    const uint32_t fs = argc >= 7 ? static_cast<uint32_t>(std::atoi(argv[6])) : 240000u;
    const double offset = argc >= 8 ? std::atof(argv[7]) : 40000.0;
    const std::string capPath = argc >= 9 ? argv[8] : std::string();
    // Optional: override every frame's destination/call type with a valid
    // talkgroup, to test whether the uplink encoding/session is accepted
    // independently of the (often poor) RF address decode.
    const uint32_t forceDst = argc >= 10 ? static_cast<uint32_t>(std::strtoul(argv[9], nullptr, 10)) : 0U;
    const uint32_t forceSrc = argc >= 11 ? static_cast<uint32_t>(std::strtoul(argv[10], nullptr, 10)) : 0U;
    const int forceCc = argc >= 12 ? std::atoi(argv[11]) : -1;   // -1 = keep decoded
    const int forceSo = argc >= 13 ? std::atoi(argv[12]) : -1;   // -1 = keep decoded

    const bool synth = (file == "synth");
    const std::vector<uint8_t> iq = synth ? std::vector<uint8_t>() : readFile(file);
    if (!synth && iq.size() < 4) {
        std::printf("cannot read %s\n", file.c_str());
        return 1;
    }
    if (!capPath.empty()) {
        std::string err;
        if (captureStart(capPath, err))
            std::printf("capture -> %s\n", capPath.c_str());
        else
            std::printf("capture start failed: %s\n", err.c_str());
    }

    ClientConfig cfg;
    cfg.master_host = host;
    cfg.master_port = 62031;
    cfg.repeater_id = id;
    cfg.password = password;
    cfg.repeater.id = id;
    cfg.repeater.callsign = callsign;
    const std::string swid = argc >= 15 ? argv[14] : std::string("20181107_Pi-Star");
    const uint32_t rfFreq = argc >= 16 ? static_cast<uint32_t>(std::strtoul(argv[15], nullptr, 10)) : 438800000U;
    cfg.repeater.rx_freq = rfFreq;
    cfg.repeater.tx_freq = rfFreq;
    cfg.repeater.tx_power = 1;
    cfg.repeater.color_code = 1;
    cfg.repeater.software_id = swid;
    cfg.repeater.package_id = "MMDVM_MMDVM_HS_Hat";
    cfg.repeater.url = "www.qrz.com";

    HomebrewClient client(std::make_unique<UdpTransport>(), cfg);
    client.start(nowMs());

    for (int i = 0; i < 500 && !client.loggedIn(); ++i) {
        client.clock(nowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("login state=%s peer=%s\n", stateName(client.state()),
                client.masterPeer().c_str());
    if (!client.loggedIn()) {
        std::printf("login FAILED\n");
        if (!capPath.empty())
            captureStop();
        return 1;
    }

    if (synth) {
        // Continuous synthetic call for ~7 s, like a real hotspot transmitting.
        const uint32_t src = forceSrc != 0 ? forceSrc : id;
        const uint32_t dst = forceDst != 0 ? forceDst : id;
        // BMRF_SYNTH_SWITCH: send 80 frames to `dst`, then switch to `id` for 80
        // frames, to test that a mid-call talkgroup change is routed correctly.
        const bool doSwitch = std::getenv("BMRF_SYNTH_SWITCH") != nullptr;
        uint32_t dst2 = id;
        if (const char* s2 = std::getenv("BMRF_SYNTH_DST2"))
            dst2 = static_cast<uint32_t>(std::strtoul(s2, nullptr, 10));
        int gapMs = 0;
        if (const char* g = std::getenv("BMRF_SYNTH_GAP"))
            gapMs = std::atoi(g);
        std::printf("synth call src=%u dst=%u cc=%d so=%d switch=%d\n", src, dst, forceCc,
                    forceSo, doSwitch ? 1 : 0);
        for (int i = 0; i < 160; ++i) {
            DMRFrame f;
            f.source = DMRFrame::Source::Rf;
            f.timestamp_ns = nowNs();
            f.timeslot = 2;
            f.source_id = src;
            f.destination_id = (doSwitch && i >= 80) ? dst2 : dst;
            f.call_type = CallType::Group;
            f.flco = 0;
            f.service_options = forceSo >= 0 ? static_cast<uint8_t>(forceSo) : 0;
            f.color_code = forceCc;
            f.has_voice = true;
            f.data_type = DataType::Voice;
            f.sequence = static_cast<uint32_t>(i % 6);
            std::memset(f.ambe.data(), 0, f.ambe.size());
            client.sendFrame(f);
            client.clock(nowMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            if (doSwitch && i == 79) {
                for (int k = 0; k < gapMs / 20; ++k) {
                    client.clock(nowMs());
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
        }
        for (int i = 0; i < 100; ++i) {
            client.clock(nowMs());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        client.stop();
        if (!capPath.empty())
            captureStop();
        std::printf("done\n");
        return 0;
    }

    DsdccDmrDemod demod(fs, offset);
    std::vector<int16_t> pcm;
    pcm.reserve(4096);
    const size_t samples = iq.size() / 2;
    const size_t chunk = 32768;
    uint8_t payload[33];
    long long sent = 0, skipped = 0, ready = 0;

    auto drain = [&]() {
        int seq = 0;
        while (demod.takePayload(payload, &seq)) {
            ++ready;
            uint32_t src = demod.sourceId();
            uint32_t dst = demod.destinationId();
            if (forceSrc != 0)
                src = forceSrc;
            if (forceDst != 0)
                dst = forceDst;
            if (src == 0 || dst == 0) {
                ++skipped;
                continue;
            }
            DMRFrame f;
            f.source = DMRFrame::Source::Rf;
            f.timestamp_ns = nowNs();
            f.timeslot = demod.timeslot();
            f.color_code = (forceCc >= 0) ? forceCc : demod.colorCode();
            f.source_id = src;
            f.destination_id = dst;
            f.call_type = CallType::Group;
            f.flco = 0;
            f.service_options = (forceSo >= 0) ? static_cast<uint8_t>(forceSo)
                                              : demod.serviceOptions();
            f.has_voice = true;
            f.data_type = DataType::Voice;
            f.sequence = static_cast<uint32_t>(seq);
            std::memcpy(f.ambe.data(), payload, f.ambe.size());

            const bool ok = client.sendFrame(f);
            ++sent;
            if (sent <= 8 || (sent % 25) == 0)
                std::printf("tx #%lld ok=%d slot=%d src=%u dst=%u ct=%d flco=0x%02x "
                            "so=0x%02x cc=%d seq=%d\n",
                            sent, ok ? 1 : 0, f.timeslot, f.source_id, f.destination_id,
                            static_cast<int>(f.call_type), f.flco, f.service_options,
                            f.color_code, seq);
        }
    };

    // Pace the replay at (approximately) real time: DMR voice bursts are sent at
    // ~60 ms intervals and BrandMeister drops streams that arrive too fast.
    const auto wallStart = std::chrono::steady_clock::now();
    size_t processed = 0;
    for (size_t s = 0; s + chunk <= samples; s += chunk) {
        demod.process(iq.data() + s * 2, chunk, pcm);
        pcm.clear();
        drain();
        client.clock(nowMs()); // process downlink + keepalive
        processed += chunk;
        const double targetS = static_cast<double>(processed) / static_cast<double>(fs);
        const double elapsedS = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wallStart).count();
        if (targetS > elapsedS) {
            const auto wait = std::chrono::duration<double>(targetS - elapsedS);
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::milliseconds>(wait));
        }
    }

    std::printf("iq done: payloads=%lld sent=%lld skipped=%lld state=%s\n", ready, sent,
                skipped, stateName(client.state()));

    // Let the RF call go idle so the client emits the Voice Terminator, then stop.
    for (int i = 0; i < 160; ++i) {
        client.clock(nowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client.stop();
    if (!capPath.empty())
        captureStop();
    std::printf("done\n");
    return 0;
}
