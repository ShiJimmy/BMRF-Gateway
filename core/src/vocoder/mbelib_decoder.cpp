// mbelib-backed AMBE+2 decoder for DMR.
//
// Compiled only when configured with -DBMRF_USE_MBELIB=ON (the Android build
// enables it by default). The DMR bit handling follows the proven DroidStar /
// DSD-FME path:
//
//   1. Reassemble the 27 AMBE bytes from the 33-byte Homebrew DMR payload
//      (the payload interleaves 6 bytes of embedded signalling at positions
//      14..19 and splits AMBE byte 13 across two nibbles).
//   2. For each 9-byte (72-bit) AMBE frame, scatter the bits into the
//      mbelib ambe_fr[4][24] container using the DMR AMBE interleave schedule
//      (rW/rX/rY/rZ).
//   3. Call mbe_processAmbe3600x2450Frame() to synthesize 160 PCM samples.
//
// 3 frames per burst -> 480 samples (60 ms @ 8 kHz).

#include "bmrf/vocoder/ambe_decoder.hpp"

#include "bmrf/common/log.hpp"
#include "bmrf/common/types.hpp"

#if defined(BMRF_USE_MBELIB)

extern "C" {
#include <mbelib.h>
}

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace bmrf {

namespace {

constexpr const char* kTag = "vocoder/mbelib";

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_errsum{0};

// DMR AMBE interleave schedule (ETSI / DSD dmr_const.h).
const int rW[36] = {
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
    0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
    0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2,
};
const int rX[36] = {
    23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
    17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
    11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4,
};
const int rY[36] = {
    0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
    1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
    1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
};
const int rZ[36] = {
    5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12,
    22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6,
    16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0,
};

// 33-byte Homebrew DMR payload -> 27 AMBE bytes (3 x 9).
void reassembleAmbe(const uint8_t* payload, uint8_t out27[27])
{
    std::memcpy(out27, payload, 14);
    out27[13] = static_cast<uint8_t>((payload[13] & 0xF0) | (payload[19] & 0x0F));
    std::memcpy(out27 + 14, payload + 20, 13);
}

// 9-byte (72-bit) AMBE frame -> mbelib ambe_fr[4][24].
void unpackAmbeFrame(const uint8_t* ambe9, char ambeFr[4][24])
{
    std::memset(ambeFr, 0, 4 * 24);

    const int* w = rW;
    const int* x = rX;
    const int* y = rY;
    const int* z = rZ;

    for (int i = 0; i < 9; ++i) {
        for (int j = 0; j < 8; j += 2) {
            ambeFr[*y][*z] = static_cast<char>(1 & (ambe9[i] >> (7 - (j + 1))));
            ambeFr[*w][*x] = static_cast<char>(1 & (ambe9[i] >> (7 - j)));
            ++w; ++x; ++y; ++z;
        }
    }
}

class MbelibAmbeDecoder : public IAmbeDecoder {
public:
    MbelibAmbeDecoder()
    {
        mbe_initMbeParms(&m_cur, &m_prev, &m_prevEnhanced);
    }

    int decodeBurst(const uint8_t* payload, size_t payloadLen, uint8_t /*voiceIndex*/,
                    int16_t* pcm, size_t maxSamples) override
    {
        if (payload == nullptr || payloadLen < kDmrPayloadBytes || pcm == nullptr)
            return -1;

        uint8_t ambe27[27];
        reassembleAmbe(payload, ambe27);

        int total = 0;
        for (int frame = 0; frame < kAmbeFramesPerBurst; ++frame) {
            char ambeFr[4][24];
            unpackAmbeFrame(ambe27 + frame * 9, ambeFr);

            short out[kAmbeFrameSamples];
            char ambeD[49] = {0};
            int errs = 0;
            int errs2 = 0;
            char errStr[64] = {0};

            static const bool use2400 = std::getenv("BMRF_AMBE_2400") != nullptr;
            if (use2400)
                mbe_processAmbe3600x2400Frame(out, &errs, &errs2, errStr, ambeFr, ambeD,
                                              &m_cur, &m_prev, &m_prevEnhanced, 3);
            else
                mbe_processAmbe3600x2450Frame(out, &errs, &errs2, errStr, ambeFr, ambeD,
                                              &m_cur, &m_prev, &m_prevEnhanced, 3);

            const uint64_t frames = g_frames.fetch_add(1) + 1;
            g_errsum.fetch_add(static_cast<uint64_t>(errs));
            if ((frames % 100) == 0) {
                logInfo(kTag, "ambe ecc avg errs=" +
                                  std::to_string(g_errsum.load() / frames) +
                                  " frames=" + std::to_string(frames));
            }

            const size_t remaining = maxSamples - static_cast<size_t>(total);
            const size_t copy = std::min(remaining, static_cast<size_t>(kAmbeFrameSamples));
            std::memcpy(pcm + total, out, copy * sizeof(int16_t));
            total += static_cast<int>(copy);
        }

        return total;
    }

    int sampleRate() const override { return kAmbeSampleRate; }
    int samplesPerBurst() const override { return kAmbeFramesPerBurst * kAmbeFrameSamples; }
    const char* name() const override { return "mbelib"; }

private:
    mbe_parms m_cur{};
    mbe_parms m_prev{};
    mbe_parms m_prevEnhanced{};
};

} // namespace

std::unique_ptr<IAmbeDecoder> createMbelibDecoder()
{
    logInfo(kTag, "mbelib AMBE+2 decoder active (DMR 2450x1150)");
    return std::make_unique<MbelibAmbeDecoder>();
}

// Offline/diagnostic accessor for the accumulated AMBE Golay error counts.
extern "C" void bmrf_mbelib_stats(unsigned long long* frames, unsigned long long* errs)
{
    if (frames)
        *frames = g_frames.load();
    if (errs)
        *errs = g_errsum.load();
}

} // namespace bmrf

#endif // BMRF_USE_MBELIB
