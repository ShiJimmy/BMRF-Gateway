// rf_dsd_dump - decode DMR from a raw cu8 IQ capture via the vendored DSDcc
// decoder (the same engine SDRangel uses). Writes 8 kHz PCM to a WAV and prints
// the decoded DMR identity frames.
//
// usage: rf_dsd_dump <file.cu8> [sampleRate=240000] [offsetHz=40000] [out.wav]

#include "bmrf/rf/dsdcc_demod.hpp"
#include "bmrf/vocoder/ambe_decoder.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace bmrf::rf;

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

void writeWav(const std::string& path, const std::vector<int16_t>& pcm, uint32_t rate)
{
    std::ofstream out(path, std::ios::binary);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    const uint32_t byteRate = rate * 2;
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(byteRate);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: rf_dsd_dump <file.cu8> [sampleRate=240000] [offsetHz=40000] [out.wav]\n");
        return 2;
    }
    const std::string path = argv[1];
    const uint32_t fs = argc >= 3 ? static_cast<uint32_t>(std::atoi(argv[2])) : 240000u;
    const double offset = argc >= 4 ? std::atof(argv[3]) : 40000.0;
    const std::string outPath = argc >= 5 ? argv[4] : "dsd_out.wav";

    const std::vector<uint8_t> iq = readFile(path);
    if (iq.size() < 4) {
        std::printf("cannot read %s\n", path.c_str());
        return 1;
    }
    const size_t samples = iq.size() / 2;
    std::printf("file=%s samples=%zu (%.2f s @ %u Hz) offset=%.0f\n", path.c_str(), samples,
                static_cast<double>(samples) / fs, fs, offset);

    DsdccDmrDemod demod(fs, offset);
    std::vector<int16_t> pcm;
    pcm.reserve(samples / 6);

    // Also decode the reassembled Homebrew AMBE payloads with OUR mbelib so the
    // RF -> BrandMeister AMBE bridge can be validated (should sound the same as
    // the DSDcc-native PCM).
    auto homebrewDecoder = bmrf::createAmbeDecoder();
    std::vector<int16_t> homebrewPcm;
    long long payloadCount = 0;

    const size_t chunk = 32768;
    std::string lastStatus;
    long long syncHits = 0;
    auto drainPayloads = [&]() {
        uint8_t payload[33];
        while (demod.takePayload(payload)) {
            ++payloadCount;
            int16_t buf[480] = {0};
            const int n = homebrewDecoder->decodeBurst(payload, sizeof(payload), 0, buf, 480);
            if (n > 0)
                homebrewPcm.insert(homebrewPcm.end(), buf, buf + n);
        }
    };
    for (size_t s = 0; s + chunk <= samples; s += chunk) {
        demod.process(iq.data() + s * 2, chunk, pcm);
        drainPayloads();
        const std::string st = demod.statusText();
        if (!st.empty() && st != lastStatus) {
            lastStatus = st;
            std::printf("  status: %s (inLevel=%d syncQ=%d)\n", st.c_str(), demod.inLevel(),
                        demod.symbolSyncQuality());
        }
    }
    if (samples % chunk) {
        demod.process(iq.data() + (samples - samples % chunk) * 2, samples % chunk, pcm);
        drainPayloads();
    }

    const uint32_t ar = static_cast<uint32_t>(demod.audioSampleRate());
    double rms = 0.0;
    for (int16_t v : pcm)
        rms += static_cast<double>(v) * v;
    rms = pcm.empty() ? 0.0 : std::sqrt(rms / pcm.size());

    std::printf("pcm samples=%zu (%.2f s @ %u Hz) rms=%.1f\n", pcm.size(),
                static_cast<double>(pcm.size()) / ar, ar, rms);
    writeWav(outPath, pcm, ar);
    std::printf("wrote %s\n", outPath.c_str());

    // Second WAV decoded from the reassembled Homebrew payloads via our mbelib.
    double hrms = 0.0;
    for (int16_t v : homebrewPcm)
        hrms += static_cast<double>(v) * v;
    hrms = homebrewPcm.empty() ? 0.0 : std::sqrt(hrms / homebrewPcm.size());
    std::printf("homebrew payloads=%lld pcm=%zu (%.2f s) rms=%.1f\n", payloadCount,
                homebrewPcm.size(), static_cast<double>(homebrewPcm.size()) / ar, hrms);
    std::string hbPath = outPath;
    const size_t dot = hbPath.rfind('.');
    if (dot != std::string::npos)
        hbPath = hbPath.substr(0, dot) + "_homebrew" + hbPath.substr(dot);
    else
        hbPath += "_homebrew.wav";
    writeWav(hbPath, homebrewPcm, ar);
    std::printf("wrote %s\n", hbPath.c_str());
    (void)syncHits;
    return 0;
}
