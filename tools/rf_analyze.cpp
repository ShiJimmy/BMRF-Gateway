// rf_analyze - offline calibration helper for the DMR 4FSK receiver.
//
// Reads raw cu8 IQ and:
//   * reports per-block RMS (to locate the transmission)
//   * estimates the discriminator DC / carrier offset
//   * runs a symbol-timing calibration search over (samples/symbol, phase) and
//     reports the best sync Hamming distance found at the loudest window
//   * runs the receiver chain and summarises sync detections

#include "bmrf/common/log.hpp"
#include "bmrf/rf/aligned_demod.hpp"
#include "bmrf/rf/dmr_phy.hpp"
#include "bmrf/rf/dmr_sync.hpp"
#include "bmrf/rf/fsk_demod.hpp"
#include "bmrf/rf/liquid_demod.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace bmrf;
using namespace bmrf::rf;

namespace {

constexpr double kPi = 6.28318530717958647692 / 2.0;

const char* kSyncPatterns[4] = {
    "131111333113313313113313", // BS voice
    "313333111331131131331131", // BS data
    "133313311131311113313331", // MS voice
    "311131133313133331131113", // MS data
};

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

// Build the per-sample FM discriminator with a running DC blocker.
std::vector<float> discriminator(const std::vector<uint8_t>& iq)
{
    const size_t samples = iq.size() / 2;
    std::vector<float> disc(samples > 0 ? samples - 1 : 0);
    double dc = 0.0;
    for (size_t n = 1; n < samples; ++n) {
        const double i0 = static_cast<double>(iq[2 * (n - 1)]) - 127.5;
        const double q0 = static_cast<double>(iq[2 * (n - 1) + 1]) - 127.5;
        const double i1 = static_cast<double>(iq[2 * n]) - 127.5;
        const double q1 = static_cast<double>(iq[2 * n + 1]) - 127.5;
        const double cross = q0 * i1 - i0 * q1;
        const double dot = i0 * i1 + q0 * q1;
        const double value = std::atan2(cross, dot);
        dc += (value - dc) * 0.001;
        disc[n - 1] = static_cast<float>(value - dc);
    }
    return disc;
}

int sliceSymbol(double phaseSum, double threshold)
{
    if (phaseSum >= threshold)
        return 3;
    if (phaseSum >= 0.0)
        return 2;
    if (phaseSum >= -threshold)
        return 1;
    return 0;
}

int syncErrorsAt(const std::vector<uint8_t>& bits, size_t pos)
{
    int best = 25;
    for (int p = 0; p < 4; ++p) {
        int diff = 0;
        for (int i = 0; i < 24; ++i) {
            const int expected = kSyncPatterns[p][i] == '3' ? 1 : 0;
            if (static_cast<int>(bits[pos + i]) != expected)
                ++diff;
        }
        best = std::min(best, diff);
    }
    return best;
}

// The float discriminator levels (0..3) are mapped to dibits via a permutation.
// The correct DMR mapping is unknown until calibrated, so try all 24.
std::vector<std::array<int, 4>> permutations()
{
    std::array<int, 4> base{0, 1, 2, 3};
    std::vector<std::array<int, 4>> out;
    std::sort(base.begin(), base.end());
    do {
        out.push_back(base);
    } while (std::next_permutation(base.begin(), base.end()));
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: rf_analyze <file.cu8> [sampleRateHz=240000]\n");
        return 2;
    }
    const std::string path = argv[1];
    const uint32_t fs = argc >= 3 ? static_cast<uint32_t>(std::atoi(argv[2])) : 240000u;

    const std::vector<uint8_t> iq = readFile(path);
    if (iq.size() < 4) {
        std::printf("cannot read %s\n", path.c_str());
        return 1;
    }
    const size_t samples = iq.size() / 2;
    std::printf("file=%s samples=%zu (%.2f s @ %u Hz)\n", path.c_str(), samples,
                static_cast<double>(samples) / fs, fs);

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
    const size_t windowStart = bestBlock * block;
    std::printf("loudest block: %.2f s rms=%.2f\n", bestBlock * 0.1, bestRms);

    std::printf("signal blocks (rms>20):\n");
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
        if (rms > 20.0)
            std::printf("  %.2fs rms=%.1f\n", b * 0.1, rms);
    }

    const std::vector<float> disc = discriminator(iq);

    // Symbol-timing calibration: search samples/symbol and phase for the lowest
    // achievable sync Hamming distance within the loud window.
    const double innerStep = 2.0 * kPi * 648.0 / 4800.0; // inner-symbol phase
    const double threshold = 2.0 * innerStep;
    const size_t winLen = std::min(windowStart + fs, std::min(samples, windowStart + 2 * fs));

    const auto perms = permutations();
    int bestErrors = 25;
    int bestSps = 0;
    int bestPhase = 0;
    int bestPerm = -1;
    std::vector<uint8_t> levels;
    for (int sps = 49; sps <= 51; ++sps) {
        for (int phase = 0; phase < sps; ++phase) {
            levels.clear();
            for (size_t s = windowStart + static_cast<size_t>(phase); s + sps < winLen;
                 s += static_cast<size_t>(sps)) {
                double sum = 0.0;
                for (int k = 0; k < sps; ++k)
                    sum += disc[s + static_cast<size_t>(k)];
                levels.push_back(static_cast<uint8_t>(sliceSymbol(sum, threshold)));
            }
            for (size_t pi = 0; pi < perms.size(); ++pi) {
                const auto& perm = perms[pi];
                std::vector<uint8_t> bits(levels.size());
                for (size_t i = 0; i < levels.size(); ++i)
                    bits[i] = static_cast<uint8_t>(perm[levels[i]]);
                for (size_t pos = 0; pos + 24 <= bits.size(); ++pos) {
                    const int e = syncErrorsAt(bits, pos);
                    if (e < bestErrors) {
                        bestErrors = e;
                        bestSps = sps;
                        bestPhase = phase;
                        bestPerm = static_cast<int>(pi);
                    }
                }
            }
        }
    }
    std::printf("best sync: sps=%d phase=%d perm=%d map=[%d %d %d %d] errors=%d (of 24)\n",
                bestSps, bestPhase, bestPerm,
                bestPerm >= 0 ? perms[bestPerm][0] : -1,
                bestPerm >= 0 ? perms[bestPerm][1] : -1,
                bestPerm >= 0 ? perms[bestPerm][2] : -1,
                bestPerm >= 0 ? perms[bestPerm][3] : -1, bestErrors);

    // Fractional samples/symbol search over the tail of the capture: if a
    // constant fractional rate yields a clean sync, a fixed NCO suffices.
    {
        const size_t winSamples = std::min(samples, static_cast<size_t>(3 * fs));
        const size_t fromSample = samples - winSamples;
        const float* win = disc.data() + fromSample;
        const size_t winLen = winSamples - 1;
        double bestRate = 0.0;
        int bestPh = 0;
        int bestErr = 25;
        const size_t searchFrom = samples > fs ? samples - fs : 0; // last 1 s
        const size_t searchTo = samples - 1;
        (void)fromSample;
        for (int r = 0; r <= 80; ++r) {
            const double rate = 47.0 + 0.05 * r;
            for (int phase = 0; phase < static_cast<int>(rate) + 1; ++phase) {
                std::vector<uint8_t> syms;
                syms.reserve((searchTo - searchFrom) / 47 + 2);
                double acc = 0.0;
                double sum = 0.0;
                for (size_t i = searchFrom + static_cast<size_t>(phase); i < searchTo; ++i) {
                    sum += disc[i];
                    acc += 1.0;
                    if (acc >= rate) {
                        int level = (sum >= threshold) ? 3
                                    : (sum >= 0) ? 2
                                    : (sum >= -threshold) ? 1
                                                          : 0;
                        static const int kMap[4] = {0, 2, 3, 1};
                        syms.push_back(static_cast<uint8_t>(kMap[level]));
                        sum = 0.0;
                        acc -= rate;
                    }
                }
                for (size_t pos = 0; pos + 24 <= syms.size(); ++pos) {
                    int e = 25;
                    (void)classifySyncCorrelate(syms.data() + pos, 25, &e);
                    if (e < bestErr) {
                        bestErr = e;
                        bestRate = rate;
                        bestPh = phase;
                    }
                }
            }
        }
        std::printf("fractional rate search: rate=%.2f phase=%d errors=%d\n", bestRate, bestPh,
                    bestErr);

        // Run a continuous NCO at the calibrated fractional rate starting from
        // the aligned search window and count the DMR bursts recovered.
        if (bestRate > 0.0) {
            std::vector<uint8_t> syms;
            syms.reserve((disc.size() - searchFrom) / 47 + 2);
            double acc = 0.0;
            double sum = 0.0;
            for (size_t i = searchFrom + static_cast<size_t>(bestPh); i < disc.size(); ++i) {
                sum += disc[i];
                acc += 1.0;
                if (acc >= bestRate) {
                    int level = (sum >= threshold) ? 3
                                : (sum >= 0) ? 2
                                : (sum >= -threshold) ? 1
                                                      : 0;
                    static const int kMap[4] = {0, 2, 3, 1};
                    syms.push_back(static_cast<uint8_t>(kMap[level]));
                    sum = 0.0;
                    acc -= bestRate;
                }
            }
            DmrPhy p;
            std::vector<DMRBurst> b;
            p.feed(syms.data(), syms.size(), b);
            std::printf("NCO(%.2f) full-capture bursts=%zu\n", bestRate, b.size());
        }
    }

    // Run the shipped block-aligned receiver chain (used by the Android app).
    setLogLevel(LogLevel::Off);
    AlignedDemodConfig aconfig;
    aconfig.sample_rate = fs;
    aconfig.symbol_rate = 4800;
    AlignedDmrDemod aligned(aconfig);
    DmrPhy phy;
    std::vector<DMRBurst> bursts;
    const size_t blk = 24000;
    int lastPhase = -1;
    for (size_t start = 0; start + blk <= samples; start += blk) {
        std::vector<uint8_t> dibits;
        aligned.processBlock(iq.data() + start * 2, blk, dibits);
        phy.feed(dibits.data(), dibits.size(), bursts);
        if (aligned.phase() != lastPhase) {
            lastPhase = aligned.phase();
        }
    }
    std::printf("aligned chain: bursts=%zu finalPhase=%d locked=%d\n", bursts.size(),
                aligned.phase(), aligned.locked() ? 1 : 0);

    // liquid-dsp chain (NCO offset removal + freqdem + RRC symbol sync).
    for (double off : {-100000.0, 100000.0}) {
        for (int sps : {2, 5, 10}) {
            LiquidDmrDemod liquidDemod(fs, 4800, off, sps);
            std::vector<uint8_t> d;
            liquidDemod.process(iq.data(), samples, d);
            DmrPhy p;
            std::vector<DMRBurst> b;
            p.feed(d.data(), d.size(), b);
            std::printf("liquid offset=%.0f sps=%d: dibits=%zu bursts=%zu\n", off, sps, d.size(),
                        b.size());
        }
    }

    return 0;
}
