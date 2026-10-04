// rf_voice_dump - brute-force the DMR level->dibit mapping and write one WAV per
// candidate, so the correct mapping can be identified by listening.
//
// Framing/sync uses a fixed mapping; only the AMBE retransform changes per WAV.

#include "bmrf/rf/aligned_demod.hpp"
#include "bmrf/rf/dmr_phy.hpp"
#include "bmrf/rf/dmr_sync.hpp"
#include "bmrf/rf/rf_decoder.hpp"
#include "bmrf/vocoder/ambe_decoder.hpp"

extern "C" void bmrf_mbelib_stats(unsigned long long* frames, unsigned long long* errs);

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace bmrf;
using namespace bmrf::rf;

namespace {

// Mapping used for framing/sync by the liquid demodulator (sync-calibrated).
const std::array<int, 4> kFrameMap = {3, 2, 0, 1};

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

void writeWav(const std::string& path, const std::vector<int16_t>& pcm)
{
    std::ofstream out(path, std::ios::binary);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    const uint32_t sampleRate = 8000;
    const uint32_t byteRate = sampleRate * 2;
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(1);
    u32(sampleRate);
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
        std::printf("usage: rf_voice_dump <file.cu8> [sampleRate=240000] [offsetHz=100000] "
                    "[timeslot=1]\n");
        return 2;
    }
    const std::string path = argv[1];
    const uint32_t fs = argc >= 3 ? static_cast<uint32_t>(std::atoi(argv[2])) : 240000u;
    const double offset = argc >= 4 ? std::atof(argv[3]) : 100000.0;
    const int wantedSlot = argc >= 5 ? std::atoi(argv[4]) : 1;

    const std::vector<uint8_t> iq = readFile(path);
    if (iq.size() < 4) {
        std::printf("cannot read %s\n", path.c_str());
        return 1;
    }
    const size_t samples = iq.size() / 2;
    std::printf("file=%s samples=%zu\n", path.c_str(), samples);

    // Locate the strongest-signal window (the transmission) and process only it.
    const size_t block = fs / 10;
    size_t bestBlock = 0;
    double bestRms = 0.0;
    for (size_t b = 0; b * block < samples; ++b) {
        const size_t begin = b * block;
        const size_t end = std::min(samples, begin + block);
        double acc = 0.0;
        for (size_t n = begin; n < end; ++n) {
            const double i = static_cast<double>(iq[2 * n]) - 127.5;
            const double q = static_cast<double>(iq[2 * n + 1]) - 127.5;
            acc += i * i + q * q;
        }
        const double rms = std::sqrt(acc / (2.0 * std::max<size_t>(1, end - begin)));
        if (rms > bestRms) {
            bestRms = rms;
            bestBlock = b;
        }
    }
    (void)bestBlock;
    const size_t from = 0;
    const size_t to = samples;
    std::printf("loudest %.2fs rms=%.1f; processing [%.2f..%.2f]s\n", bestBlock * 0.1, bestRms,
                from / static_cast<double>(fs), to / static_cast<double>(fs));

    (void)offset;
    AlignedDemodConfig aconfig;
    aconfig.sample_rate = fs;
    aconfig.symbol_rate = 4800;
    AlignedDmrDemod demod(aconfig);
    std::vector<uint8_t> dibits;
    {
        const size_t chunk = 32768;
        for (size_t s = from; s + chunk <= to; s += chunk)
            demod.processBlock(iq.data() + s * 2, chunk, dibits);
        if ((to - from) % chunk)
            demod.processBlock(iq.data() + (to - (to - from) % chunk) * 2, (to - from) % chunk,
                               dibits);
    }

    {
        int minErr = 25;
        for (size_t i = 0; i + 24 <= dibits.size(); ++i) {
            int e = 25;
            classifySyncCorrelate(dibits.data() + i, 25, &e);
            if (e < minErr)
                minErr = e;
        }
        std::printf("dibits=%zu minSyncErr=%d\n", dibits.size(), minErr);
    }

    DmrPhy phy;
    std::vector<DMRBurst> bursts;
    phy.feed(dibits.data(), dibits.size(), bursts);
    std::printf("total bursts=%zu\n", bursts.size());

    (void)kFrameMap;
    (void)wantedSlot;
    std::array<int, 4> perm{0, 1, 2, 3};
    std::sort(perm.begin(), perm.end());
    int index = 0;
    do {
        // Independent decoder per timeslot so the two interleaved slots never mix.
        auto dec1 = createAmbeDecoder();
        auto dec2 = createAmbeDecoder();
        std::vector<int16_t> pcm1;
        std::vector<int16_t> pcm2;
        unsigned long long f0 = 0, e0 = 0;
        bmrf_mbelib_stats(&f0, &e0);

        for (const auto& burst : bursts) {
            if (burst.sync == SyncType::BsData || burst.sync == SyncType::MsData)
                continue;

            DMRBurst converted = burst;
            for (auto& d : converted.dibits)
                d = static_cast<uint8_t>(perm[d & 0x03]);

            DMRFrame frame;
            if (!RfDecoder::decode(converted, frame))
                continue;

            const bool slot2 = (burst.timeslot == 2);
            int16_t buffer[480] = {0};
            const int n = (slot2 ? dec2 : dec1)
                              ->decodeBurst(frame.ambe.data(), frame.ambe.size(), 0, buffer, 480);
            if (n > 0) {
                if (slot2)
                    pcm2.insert(pcm2.end(), buffer, buffer + n);
                else
                    pcm1.insert(pcm1.end(), buffer, buffer + n);
            }
        }

        char name[128];
        std::snprintf(name, sizeof(name), "voice_%d%d%d%d_s1.wav", perm[0], perm[1], perm[2], perm[3]);
        writeWav(name, pcm1);
        std::snprintf(name, sizeof(name), "voice_%d%d%d%d_s2.wav", perm[0], perm[1], perm[2], perm[3]);
        writeWav(name, pcm2);
        unsigned long long f1 = 0, e1 = 0;
        bmrf_mbelib_stats(&f1, &e1);
        const unsigned long long df = f1 - f0;
        const unsigned long long de = e1 - e0;
        std::printf("  perm=[%d %d %d %d] s1=%zu s2=%zu ambeFrames=%llu eccErrs=%llu avg=%.3f\n",
                    perm[0], perm[1], perm[2], perm[3], pcm1.size(), pcm2.size(), df, de,
                    df ? static_cast<double>(de) / static_cast<double>(df) : 0.0);
        ++index;
    } while (std::next_permutation(perm.begin(), perm.end()));

    std::printf("wrote %d WAVs\n", index);
    return 0;
}
