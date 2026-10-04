// rf_lc_dump - decode DMR Link Control (source/target ID) from data bursts.
//
// Validates the RF -> dibit -> frame chain independently of the AMBE vocoder:
// a DMR Voice LC Header is a data burst whose 96-bit Link Control carries the
// source and target IDs. BPTC(196,96) deinterleave + matrix extraction is enough
// to read the IDs when the RF is clean (error correction omitted on purpose).

#include "bmrf/rf/aligned_demod.hpp"
#include "bmrf/rf/dmr_phy.hpp"
#include "bmrf/rf/dmr_sync.hpp"
#include "bmrf/rf/fsk_demod.hpp"
#include "bmrf/rf/liquid_demod.hpp"

#include <cmath>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace bmrf::rf;

namespace {

const uint8_t kBPTCDeInterleavingIndex[196] = {
    0, 13, 26, 39, 52, 65, 78, 91, 104, 117, 130, 143, 156, 169, 182, 195, 12, 25, 38, 51, 64, 77,
    90, 103, 116, 129, 142, 155, 168, 181, 194, 11, 24, 37, 50, 63, 76, 89, 102, 115, 128, 141,
    154, 167, 180, 193, 10, 23, 36, 49, 62, 75, 88, 101, 114, 127, 140, 153, 166, 179, 192, 9, 22,
    35, 48, 61, 74, 87, 100, 113, 126, 139, 152, 165, 178, 191, 8, 21, 34, 47, 60, 73, 86, 99, 112,
    125, 138, 151, 164, 177, 190, 7, 20, 33, 46, 59, 72, 85, 98, 111, 124, 137, 150, 163, 176, 189,
    6, 19, 32, 45, 58, 71, 84, 97, 110, 123, 136, 149, 162, 175, 188, 5, 18, 31, 44, 57, 70, 83, 96,
    109, 122, 135, 148, 161, 174, 187, 4, 17, 30, 43, 56, 69, 82, 95, 108, 121, 134, 147, 160, 173,
    186, 3, 16, 29, 42, 55, 68, 81, 94, 107, 120, 133, 146, 159, 172, 185, 2, 15, 28, 41, 54, 67, 80,
    93, 106, 119, 132, 145, 158, 171, 184, 1, 14, 27, 40, 53, 66, 79, 92, 105, 118, 131, 144, 157,
    170, 183};

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

unsigned bitsToUint(const uint8_t* bits, int off, int len)
{
    unsigned v = 0;
    for (int i = 0; i < len; ++i)
        v = (v << 1) | (bits[off + i] & 1);
    return v;
}

const uint8_t kH15[4 * 15] = {
    1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0,
    0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1};
const uint8_t kH13[4 * 13] = {
    1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0,
    1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1};

uint8_t g_corr15[16];
uint8_t g_corr13[16];

void initHamming()
{
    for (int i = 0; i < 16; ++i) {
        g_corr15[i] = 0xFF;
        g_corr13[i] = 0xFF;
    }
    g_corr15[0b1001] = 0; g_corr15[0b1101] = 1; g_corr15[0b1111] = 2; g_corr15[0b1110] = 3;
    g_corr15[0b0111] = 4; g_corr15[0b1010] = 5; g_corr15[0b0101] = 6; g_corr15[0b1011] = 7;
    g_corr15[0b1100] = 8; g_corr15[0b0110] = 9; g_corr15[0b0011] = 10; g_corr15[0b1000] = 11;
    g_corr15[0b0100] = 12; g_corr15[0b0010] = 13; g_corr15[0b0001] = 14;
    g_corr13[0b1111] = 0; g_corr13[0b1110] = 1; g_corr13[0b0111] = 2; g_corr13[0b1010] = 3;
    g_corr13[0b0101] = 4; g_corr13[0b1011] = 5; g_corr13[0b1100] = 6; g_corr13[0b0110] = 7;
    g_corr13[0b0011] = 8; g_corr13[0b1000] = 9; g_corr13[0b0100] = 10; g_corr13[0b0010] = 11;
    g_corr13[0b0001] = 12;
}

bool hamming15(uint8_t rx[15], uint8_t dec[11])
{
    int s = 0;
    for (int is = 0; is < 4; ++is) {
        int b = 0;
        for (int i = 0; i < 15; ++i)
            b ^= rx[i] * kH15[is * 15 + i];
        s |= b << (3 - is);
    }
    if (s > 0) {
        if (g_corr15[s] == 0xFF)
            return false;
        rx[g_corr15[s]] ^= 1;
    }
    for (int i = 0; i < 11; ++i)
        dec[i] = rx[i];
    return true;
}

bool hamming13(uint8_t rx[13], uint8_t dec[9])
{
    int s = 0;
    for (int is = 0; is < 4; ++is) {
        int b = 0;
        for (int i = 0; i < 13; ++i)
            b ^= rx[i] * kH13[is * 13 + i];
        s |= b << (3 - is);
    }
    if (s > 0) {
        if (g_corr13[s] == 0xFF)
            return false;
        rx[g_corr13[s]] ^= 1;
    }
    for (int i = 0; i < 9; ++i)
        dec[i] = rx[i];
    return true;
}

const uint8_t kG15[15 * 11] = {
    1,0,0,0,0,0,0,0,0,0,0, 1,0,0,1,
    0,1,0,0,0,0,0,0,0,0,0, 1,1,0,1,
    0,0,1,0,0,0,0,0,0,0,0, 1,1,1,1,
    0,0,0,1,0,0,0,0,0,0,0, 1,1,1,0,
    0,0,0,0,1,0,0,0,0,0,0, 0,1,1,1,
    0,0,0,0,0,1,0,0,0,0,0, 1,0,1,0,
    0,0,0,0,0,0,1,0,0,0,0, 0,1,0,1,
    0,0,0,0,0,0,0,1,0,0,0, 1,0,1,1,
    0,0,0,0,0,0,0,0,1,0,0, 1,1,0,0,
    0,0,0,0,0,0,0,0,0,1,0, 0,1,1,0,
    0,0,0,0,0,0,0,0,0,0,1, 0,0,1,1};
const uint8_t kG13[13 * 9] = {
    1,0,0,0,0,0,0,0,0, 1,1,1,1,
    0,1,0,0,0,0,0,0,0, 1,1,1,0,
    0,0,1,0,0,0,0,0,0, 0,1,1,1,
    0,0,0,1,0,0,0,0,0, 1,0,1,0,
    0,0,0,0,1,0,0,0,0, 0,1,0,1,
    0,0,0,0,0,1,0,0,0, 1,0,1,1,
    0,0,0,0,0,0,1,0,0, 1,1,0,0,
    0,0,0,0,0,0,0,1,0, 0,1,1,0,
    0,0,0,0,0,0,0,0,1, 0,0,1,1};

void ham15enc(const uint8_t in[11], uint8_t out[15])
{
    for (int j = 0; j < 15; ++j) {
        int v = 0;
        for (int i = 0; i < 11; ++i)
            v += in[i] * kG15[i * 15 + j];
        out[j] = static_cast<uint8_t>(v & 1);
    }
}
void ham13enc(const uint8_t in[9], uint8_t out[13])
{
    for (int j = 0; j < 13; ++j) {
        int v = 0;
        for (int i = 0; i < 9; ++i)
            v += in[i] * kG13[i * 13 + j];
        out[j] = static_cast<uint8_t>(v & 1);
    }
}

void bptcEncode(const uint8_t lc[96], uint8_t info[196])
{
    uint8_t m[13][15] = {{0}};
    for (int j = 0; j < 8; ++j)
        m[0][3 + j] = lc[j];
    int k = 8;
    for (int i = 1; i < 9; ++i)
        for (int j = 0; j < 11; ++j)
            m[i][j] = lc[k++];
    for (int i = 0; i < 9; ++i) {
        uint8_t enc[15];
        ham15enc(m[i], enc);
        for (int j = 0; j < 15; ++j)
            m[i][j] = enc[j];
    }
    for (int c = 0; c < 15; ++c) {
        uint8_t col[9], enc[13];
        for (int i = 0; i < 9; ++i)
            col[i] = m[i][c];
        ham13enc(col, enc);
        for (int i = 0; i < 13; ++i)
            m[i][c] = enc[i];
    }
    uint8_t de[196] = {0};
    k = 1;
    for (int i = 0; i < 13; ++i)
        for (int j = 0; j < 15; ++j)
            de[k++] = m[i][j];
    for (int i = 0; i < 196; ++i)
        info[i] = de[kBPTCDeInterleavingIndex[i]];
}

void buildDataBurst(const uint8_t info[196], uint8_t dibits[132])
{
    for (int i = 0; i < 132; ++i)
        dibits[i] = 0;
    for (int i = 0; i < 49; ++i)
        dibits[i] = static_cast<uint8_t>((info[2 * i] << 1) | info[2 * i + 1]);
    for (int i = 0; i < 49; ++i)
        dibits[83 + i] = static_cast<uint8_t>((info[98 + 2 * i] << 1) | info[98 + 2 * i + 1]);
}

// data burst: info[0..97] = dibits[0..48], info[98..195] = dibits[83..131].
void extractInfo(const DMRBurst& burst, uint8_t info[196])
{
    int k = 0;
    for (int i = 0; i < 49; ++i) {
        info[k++] = (burst.dibits[i] >> 1) & 1;
        info[k++] = burst.dibits[i] & 1;
    }
    for (int i = 83; i <= 131; ++i) {
        info[k++] = (burst.dibits[i] >> 1) & 1;
        info[k++] = burst.dibits[i] & 1;
    }
}

void decodeLc(const DMRBurst& burst, const char* label)
{
    uint8_t info[196];
    uint8_t de[196] = {0};
    extractInfo(burst, info);
    for (int i = 0; i < 196; ++i)
        de[kBPTCDeInterleavingIndex[i]] = info[i] & 1;

    uint8_t matrix[13][15];
    int k = 1;
    for (int i = 0; i < 13; ++i)
        for (int j = 0; j < 15; ++j)
            matrix[i][j] = de[k++];

    int irrecoverable = 0;
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            irrecoverable = 0;
        for (int i = 0; i < 9; ++i) {
            uint8_t line[15], corr[11];
            for (int j = 0; j < 15; ++j)
                line[j] = matrix[i][j];
            if (!hamming15(line, corr))
                ++irrecoverable;
            for (int j = 0; j < 11; ++j)
                matrix[i][j] = corr[j];
        }
        for (int i = 0; i < 15; ++i) {
            uint8_t col[13], corr[9];
            for (int j = 0; j < 13; ++j)
                col[j] = matrix[j][i];
            if (!hamming13(col, corr))
                ++irrecoverable;
            for (int j = 0; j < 9; ++j)
                matrix[j][i] = corr[j];
        }
    }

    uint8_t lc[96];
    int n = 0;
    for (int j = 3; j < 11; ++j)
        lc[n++] = matrix[0][j];
    for (int i = 1; i < 9; ++i)
        for (int j = 0; j < 11; ++j)
            lc[n++] = matrix[i][j];

    const unsigned pf = bitsToUint(lc, 0, 1);
    const unsigned flco = bitsToUint(lc, 2, 6);
    const unsigned fid = bitsToUint(lc, 8, 8);
    const unsigned so = bitsToUint(lc, 16, 8);
    const unsigned target = bitsToUint(lc, 24, 24);
    const unsigned source = bitsToUint(lc, 48, 24);
    const unsigned extra = bitsToUint(lc, 72, 24);

    std::printf("%s slot=%d sync=%d err=%d pf=%u flco=0x%02X fid=0x%02X so=0x%02X "
                "target=%u source=%u tail=0x%06X\n",
                label, burst.timeslot, static_cast<int>(burst.sync), irrecoverable, pf, flco, fid, so,
                target, source, extra);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: rf_lc_dump <file.cu8> [sampleRate=240000]\n");
        return 2;
    }
    initHamming();

    if (argc >= 2 && std::string(argv[1]) == "--selftest") {
        uint8_t lc[96] = {0};
        auto put = [&](int off, int len, unsigned v) {
            for (int i = 0; i < len; ++i)
                lc[off + i] = static_cast<uint8_t>((v >> (len - 1 - i)) & 1);
        };
        const unsigned wantTarget = 16777215u;
        const unsigned wantSource = 4612345u;
        put(2, 6, 0);       // flco = 0 (group)
        put(8, 8, 0);       // fid
        put(16, 8, 0);      // so
        put(24, 24, wantTarget);
        put(48, 24, wantSource);

        uint8_t info[196];
        bptcEncode(lc, info);
        uint8_t dibits[132];
        buildDataBurst(info, dibits);
        const std::string syncStr = "313333111331131131331131"; // BS data
        for (int i = 0; i < 24; ++i)
            dibits[54 + i] = static_cast<uint8_t>(syncStr[i] == '3' ? 3 : 1);

        DMRBurst b;
        b.sync = SyncType::BsData;
        b.timeslot = 1;
        std::memcpy(b.dibits.data(), dibits, 132);
        decodeLc(b, "SELFTEST(direct)");

        // Full RF chain: RRC-shaped modulation -> aligned demod -> PHY -> LC.
        std::vector<uint8_t> stream;
        for (int i = 0; i < 400; ++i)
            stream.push_back(static_cast<uint8_t>(i % 4));
        stream.insert(stream.end(), dibits, dibits + 132);
        for (int i = 0; i < 400; ++i)
            stream.push_back(static_cast<uint8_t>(i % 4));

        const double sps = 50.0;
        const double beta = 0.2;
        const int half = 8;
        const size_t taps = 2 * half * 50 + 1;
        auto rrc = [&](double t) {
            if (std::fabs(t) < 1e-9)
                return 1.0 - beta + 4.0 * beta / 3.14159265358979;
            return (std::sin(3.14159265358979 * t * (1 - beta)) +
                    4 * beta * t * std::cos(3.14159265358979 * t * (1 + beta))) /
                   (3.14159265358979 * t * (1 - std::pow(4 * beta * t, 2)));
        };
        std::vector<double> h(taps);
        double hsum = 0.0;
        for (size_t n = 0; n < taps; ++n) {
            h[n] = rrc((static_cast<double>(n) - static_cast<double>(taps / 2)) / sps);
            hsum += h[n];
        }
        for (auto& v : h)
            v /= hsum;
        const int levelOfDibit[4] = {2, 3, 1, 0}; // inverse of {3,2,0,1}
        std::vector<double> freq(stream.size() * 50 + taps, 0.0);
        for (size_t k = 0; k < stream.size(); ++k) {
            const double lvl = static_cast<double>(levelOfDibit[stream[k] & 3]) - 1.5;
            for (size_t n = 0; n < taps; ++n)
                freq[k * 50 + n] += lvl * h[n];
        }
        std::vector<uint8_t> iq;
        double phase = 0.0;
        for (double f : freq) {
            phase += 2.0 * 3.14159265358979 * (f * 2.0 * 648.0) / 240000.0;
            if (phase > 3.14159265358979)
                phase -= 2.0 * 3.14159265358979;
            if (phase < -3.14159265358979)
                phase += 2.0 * 3.14159265358979;
            iq.push_back(static_cast<uint8_t>(std::lround(127.0 + 100.0 * std::cos(phase))));
            iq.push_back(static_cast<uint8_t>(std::lround(127.0 + 100.0 * std::sin(phase))));
        }
        AlignedDemodConfig config;
        config.sample_rate = 240000;
        config.symbol_rate = 4800;
        AlignedDmrDemod demod(config);
        std::vector<uint8_t> dd;
        demod.processBlock(iq.data(), iq.size() / 2, dd);
        DmrPhy phy;
        std::vector<DMRBurst> bs;
        phy.feed(dd.data(), dd.size(), bs);
        for (const auto& bb : bs)
            if (bb.sync == SyncType::BsData || bb.sync == SyncType::MsData)
                decodeLc(bb, "SELFTEST(rf)");
        std::printf("expected target=%u source=%u\n", wantTarget, wantSource);
        return 0;
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

    // Block-aligned discriminator with adaptive thresholds.
    const double offset = argc >= 4 ? std::atof(argv[3]) : 0.0;
    (void)offset;
    // Optional 4th arg: samples/symbol override (the real RTL clock is not
    // exactly the requested rate; calibrate with rf_analyze).
    const double spsOverride = argc >= 5 ? std::atof(argv[4]) : 0.0;
    AlignedDemodConfig config;
    config.sample_rate = fs;
    config.symbol_rate = spsOverride > 1.0 ? static_cast<uint32_t>(fs / spsOverride + 0.5) : 4800;
    std::printf("demod symbol_rate=%u (sps=%.3f)\n", config.symbol_rate,
                static_cast<double>(fs) / config.symbol_rate);
    AlignedDmrDemod demod(config);
    std::vector<uint8_t> allDibits;
    const bool wholeBlock = (argc >= 6 && std::string(argv[5]) == "whole");
    if (wholeBlock) {
        demod.processBlock(iq.data(), samples, allDibits);
    } else {
        const size_t chunk = (argc >= 7) ? static_cast<size_t>(std::atoi(argv[6])) : 32768;
        for (size_t s = 0; s + chunk <= samples; s += chunk)
            demod.processBlock(iq.data() + s * 2, chunk, allDibits);
        if (samples % chunk)
            demod.processBlock(iq.data() + (samples - samples % chunk) * 2, samples % chunk,
                               allDibits);
    }
    DmrPhy phy;
    std::vector<DMRBurst> bursts;
    phy.feed(allDibits.data(), allDibits.size(), bursts);
    int minErr = 25;
    std::vector<size_t> syncPositions;
    for (size_t i = 0; i + 24 <= allDibits.size(); ++i) {
        int e = 25;
        const SyncType s = classifySyncCorrelate(allDibits.data() + i, 25, &e);
        if (e < minErr)
            minErr = e;
        if (s != SyncType::None && e <= 2) {
            if (syncPositions.empty() || i - syncPositions.back() > 24)
                syncPositions.push_back(i);
        }
    }
    std::printf("dibits=%zu minSyncErr=%d bursts=%zu syncs=%zu\n", allDibits.size(), minErr,
                bursts.size(), syncPositions.size());
    {
        // DMR sync codewords recur every 144 symbols (12 CACH + 132 burst). If the
        // mean spacing differs, the assumed samples/symbol (4800 baud) is wrong.
        long long sum = 0;
        size_t n = 0;
        long long mn = 1 << 30, mx = 0;
        for (size_t k = 1; k < syncPositions.size(); ++k) {
            const long long d = static_cast<long long>(syncPositions[k] - syncPositions[k - 1]);
            if (d > 48 && d < 400) {
                sum += d;
                ++n;
                mn = std::min(mn, d);
                mx = std::max(mx, d);
            }
        }
        if (n > 0)
            std::printf("sync spacing: mean=%.2f min=%lld max=%lld (nominal 144)\n",
                        static_cast<double>(sum) / static_cast<double>(n), mn, mx);
    }

    // Level histogram of the aligned demod output.
    {
        long hist[4] = {0, 0, 0, 0};
        for (uint8_t d : allDibits)
            hist[d & 3]++;
        std::printf("aligned level histogram: d0=%ld d1=%ld d2=%ld d3=%ld\n", hist[0], hist[1],
                    hist[2], hist[3]);
    }

    // NCO shift + narrowband decimation BEFORE the discriminator (TETRA/DSD
    // style). Discriminating at the full 240 kHz sample rate leaves the full
    // noise bandwidth in the phase estimate, which swamps the inner +/-1 levels.
    {
        const double ncoHz = 40000.0;
        const int dec = 5; // 240k -> 48k (sps = 10)
        const double dphi = 6.283185307179586 * ncoHz / static_cast<double>(fs);
        std::vector<double> re, im;
        {
            double ph = 0.0;
            double accI = 0.0, accQ = 0.0;
            int cnt = 0;
            for (size_t i = 0; i < samples; ++i) {
                const double I = static_cast<double>(iq[2 * i]) - 127.5;
                const double Q = static_cast<double>(iq[2 * i + 1]) - 127.5;
                const double c = std::cos(ph), s = std::sin(ph);
                accI += I * c + Q * s;
                accQ += -I * s + Q * c;
                ph += dphi;
                if (ph > 6.283185307179586)
                    ph -= 6.283185307179586;
                if (++cnt == dec) {
                    re.push_back(accI / dec);
                    im.push_back(accQ / dec);
                    accI = accQ = 0.0;
                    cnt = 0;
                }
            }
        }
        std::vector<double> disc(re.size() > 0 ? re.size() - 1 : 0);
        double dce = 0.0;
        for (size_t i = 1; i < re.size(); ++i) {
            const double cross = re[i - 1] * im[i] - im[i - 1] * re[i];
            const double dot = re[i] * re[i - 1] + im[i] * im[i - 1];
            const double v = std::atan2(cross, dot);
            dce += (v - dce) * 0.01;
            disc[i - 1] = v - dce;
        }
        std::vector<uint8_t> dd;
        const int sps2 = 10;
        double mean = 0.0, var = 0.0;
        static const int kMapD[4] = {3, 2, 0, 1};
        for (size_t i = 0; i + sps2 < disc.size(); i += sps2) {
            double sum = 0.0;
            for (int k = 0; k < sps2; ++k)
                sum += disc[i + static_cast<size_t>(k)];
            mean += (sum - mean) * 0.02;
            const double dl = sum - mean;
            var += (dl * dl - var) * 0.02;
            const double thr = 0.9 * std::sqrt(var);
            int level;
            if (sum >= mean + thr) level = 3;
            else if (sum >= mean) level = 2;
            else if (sum >= mean - thr) level = 1;
            else level = 0;
            dd.push_back(static_cast<uint8_t>(kMapD[level]));
        }
        int clean = 0;
        for (size_t i = 0; i + 24 <= dd.size(); ++i) {
            int e = 25;
            classifySyncCorrelate(dd.data() + i, 25, &e);
            if (e <= 2)
                ++clean;
        }
        DmrPhy pn;
        std::vector<DMRBurst> bn;
        pn.feed(dd.data(), dd.size(), bn);
        std::printf("NCO+decim: dibits=%zu cleanSyncs=%d bursts=%zu\n", dd.size(), clean,
                    bn.size());
    }

    // DSD-FME-style DMR demod probe: FM discriminator @ fs -> decimate to 48k ->
    // F4EXB root-raised-cosine alpha=0.7 matched filter -> integrate the centre
    // 5 of 10 samples/symbol -> slice. This mirrors the reference decoder so a
    // failure here means the capture itself, not our RRC/MF choices.
    {
        static const float dmrcoeffs[61] = {
            0.0301506278f, 0.0269200615f, 0.0159662432f, -0.0013114705f, -0.0216605133f,
            -0.0404938748f, -0.0528141756f, -0.0543747957f, -0.0428325003f, -0.0186176083f,
            0.0147202645f, 0.0508418571f, 0.0816392577f, 0.0988113688f, 0.0957187780f,
            0.0691512084f, 0.0206194642f, -0.0431564563f, -0.1107569268f, -0.1675773224f,
            -0.1981519842f, -0.1889130786f, -0.1308939560f, -0.0218608492f, 0.1325685970f,
            0.3190962499f, 0.5182530574f, 0.7070497652f, 0.8623526878f, 0.9644213921f,
            1.0000000000f, 0.9644213921f, 0.8623526878f, 0.7070497652f, 0.5182530574f,
            0.3190962499f, 0.1325685970f, -0.0218608492f, -0.1308939560f, -0.1889130786f,
            -0.1981519842f, -0.1675773224f, -0.1107569268f, -0.0431564563f, 0.0206194642f,
            0.0691512084f, 0.0957187780f, 0.0988113688f, 0.0816392577f, 0.0508418571f,
            0.0147202645f, -0.0186176083f, -0.0428325003f, -0.0543747957f, -0.0528141756f,
            -0.0404938748f, -0.0216605133f, -0.0013114705f, 0.0159662432f, 0.0269200615f,
            0.0301506278f};

        std::vector<double> disc(samples > 0 ? samples - 1 : 0);
        double dce = 0.0;
        for (size_t i = 1; i < samples; ++i) {
            const double i0 = static_cast<double>(iq[2 * (i - 1)]) - 127.5;
            const double q0 = static_cast<double>(iq[2 * (i - 1) + 1]) - 127.5;
            const double i1 = static_cast<double>(iq[2 * i]) - 127.5;
            const double q1 = static_cast<double>(iq[2 * i + 1]) - 127.5;
            const double v = std::atan2(q0 * i1 - i0 * q1, i0 * i1 + q0 * q1);
            dce += (v - dce) * 0.001;
            disc[i - 1] = v - dce;
        }
        const unsigned dec = (fs >= 48000) ? (fs / 48000) : 1;
        std::vector<double> d48;
        for (size_t i = 0; i + dec <= disc.size(); i += dec) {
            double s = 0.0;
            for (unsigned k = 0; k < dec; ++k)
                s += disc[i + k];
            d48.push_back(s / dec);
        }
        std::vector<double> f(d48.size(), 0.0);
        for (size_t i = 0; i < d48.size(); ++i) {
            double s = 0.0;
            for (int k = 0; k < 61; ++k) {
                const long idx = static_cast<long>(i) - k + 30;
                if (idx >= 0 && idx < static_cast<long>(d48.size()))
                    s += dmrcoeffs[k] * d48[static_cast<size_t>(idx)];
            }
            f[i] = s;
        }
        std::vector<uint8_t> dd;
        const int sps2 = 10, center = 5;
        // DSD uses boundaries at 5/8 of the max/min envelope (center, umid, lmid).
        double gmax = 0.0, gmin = 0.0;
        bool init = false;
        static const int kMapD[4] = {3, 2, 0, 1};
        for (size_t i = 0; i + sps2 < f.size(); i += sps2) {
            double sum = 0.0;
            for (int k = center - 2; k <= center + 2; ++k)
                sum += f[i + static_cast<size_t>(k)];
            if (!init) {
                gmax = sum;
                gmin = sum;
                init = true;
            }
            if (sum > gmax) gmax = sum;
            else gmax += (sum - gmax) * 0.02;
            if (sum < gmin) gmin = sum;
            else gmin += (sum - gmin) * 0.02;
            const double c = (gmax + gmin) * 0.5;
            const double umid = ((gmax - c) * 5.0 / 8.0) + c;
            const double lmid = ((gmin - c) * 5.0 / 8.0) + c;
            int level;
            if (sum > c)
                level = (sum > umid) ? 3 : 2;
            else
                level = (sum < lmid) ? 0 : 1;
            dd.push_back(static_cast<uint8_t>(kMapD[level]));
        }
        DmrPhy pdsd;
        std::vector<DMRBurst> bdsd;
        pdsd.feed(dd.data(), dd.size(), bdsd);
        int dataCount = 0;
        int clean = 0;
        for (size_t i = 0; i + 24 <= dd.size(); ++i) {
            int e = 25;
            classifySyncCorrelate(dd.data() + i, 25, &e);
            if (e <= 2)
                ++clean;
        }
        for (const auto& b : bdsd) {
            if (b.sync != SyncType::BsData && b.sync != SyncType::MsData)
                continue;
            ++dataCount;
            if (dataCount <= 6) {
                std::array<int, 4> perm{0, 1, 2, 3};
                std::sort(perm.begin(), perm.end());
                do {
                    DMRBurst mapped = b;
                    for (auto& d : mapped.dibits)
                        d = static_cast<uint8_t>(perm[d & 3]);
                    decodeLc(mapped, "DSD");
                } while (std::next_permutation(perm.begin(), perm.end()));
            }
        }
        std::printf("DSD-style: dibits=%zu cleanSyncs=%d bursts=%zu dataBursts=%d\n", dd.size(),
                    clean, bdsd.size(), dataCount);
    }

    // Diagnostics: rectangular-integrator phase sweep (FskDemod) over the whole
    // capture. If some phase yields many clean syncs, the signal is decodable and
    // the issue is the RRC matched filter / phase search, not the RF.
    {
        int bestPh = -1;
        int bestClean = -1;
        int bestMinErr = 25;
        for (int ph = 0; ph < static_cast<int>(fs / 4800); ++ph) {
            FskDemodConfig fc;
            fc.sample_rate = fs;
            fc.symbol_rate = 4800;
            FskDemod fsk(fc);
            fsk.setPhase(ph);
            std::vector<uint8_t> dd;
            fsk.process(iq.data(), samples, dd);
            int clean = 0;
            int mn = 25;
            for (size_t i = 0; i + 24 <= dd.size(); ++i) {
                int e = 25;
                classifySyncCorrelate(dd.data() + i, 25, &e);
                if (e < mn)
                    mn = e;
                if (e <= 2)
                    ++clean;
            }
            if (clean > bestClean) {
                bestClean = clean;
                bestPh = ph;
                bestMinErr = mn;
            }
        }
        std::printf("FskDemod sweep: bestPhase=%d cleanSyncs=%d minErr=%d\n", bestPh, bestClean,
                    bestMinErr);

        // Decode LC with the rectangular demod at the best phase, trying all
        // level->dibit permutations. The user's own radio ID starts with 461.
        if (bestPh >= 0) {
            FskDemodConfig fc;
            fc.sample_rate = fs;
            fc.symbol_rate = 4800;
            FskDemod fsk(fc);
            fsk.setPhase(bestPh);
            std::vector<uint8_t> dd;
            fsk.process(iq.data(), samples, dd);
            DmrPhy p;
            std::vector<DMRBurst> bs;
            p.feed(dd.data(), dd.size(), bs);
            int dataCount = 0;
            int permsTried = 0;
            for (const auto& b : bs) {
                if (b.sync != SyncType::BsData && b.sync != SyncType::MsData)
                    continue;
                ++dataCount;
                if (dataCount > 8)
                    continue;
                std::array<int, 4> perm{0, 1, 2, 3};
                std::sort(perm.begin(), perm.end());
                do {
                    DMRBurst mapped = b;
                    for (auto& d : mapped.dibits)
                        d = static_cast<uint8_t>(perm[d & 3]);
                    decodeLc(mapped, "FSKDATA");
                    ++permsTried;
                } while (std::next_permutation(perm.begin(), perm.end()));
            }
            std::printf("FskDemod bestPhase=%d bursts=%zu dataBursts=%d perms=%d\n", bestPh,
                        bs.size(), dataCount, permsTried);

            // Dump the first sync neighbourhood as a dibit string so it can be
            // eyeballed against the known DMR codewords.
            size_t sp = 0;
            int me = 25;
            for (size_t i = 0; i + 24 <= dd.size(); ++i) {
                int e = 25;
                classifySyncCorrelate(dd.data() + i, 25, &e);
                if (e < me) {
                    me = e;
                    sp = i;
                }
                if (e == 0)
                    break;
            }
            std::printf("first sync pos=%zu err=%d firstDibits=", sp, me);
            for (size_t i = 0; i < 40 && i < dd.size(); ++i)
                std::printf("%d", dd[i] & 3);
            std::printf("\n");
            if (sp >= 54) {
                std::printf("burst132=");
                for (long i = static_cast<long>(sp) - 54; i < static_cast<long>(sp) - 54 + 132; ++i) {
                    if (i < 0 || static_cast<size_t>(i) >= dd.size())
                        break;
                    std::printf("%c", ((dd[static_cast<size_t>(i)] & 3) >> 1) & 1 ? '3' : '1');
                }
                std::printf("\n");
            }
        }
    }

    int dataCount = 0;
    for (const auto& b : bursts) {
        if (b.sync != SyncType::BsData && b.sync != SyncType::MsData)
            continue;
        ++dataCount;
        // Try all 24 dibit permutations; the user's known truth is
        // source ~= 461xxxx and target == 16777215 (0xFFFFFF).
        std::array<int, 4> perm{0, 1, 2, 3};
        std::sort(perm.begin(), perm.end());
        do {
            DMRBurst mapped = b;
            for (auto& d : mapped.dibits)
                d = static_cast<uint8_t>(perm[d & 3]);
            decodeLc(mapped, "DATA");
        } while (std::next_permutation(perm.begin(), perm.end()));
    }
    std::printf("data bursts=%d\n", dataCount);
    return 0;
}
