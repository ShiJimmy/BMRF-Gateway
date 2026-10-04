#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bmrf {

// DMR AMBE+2 parameters (per ETSI TS 102 361 / mbelib conventions).
constexpr int kAmbeSampleRate = 8000;    // 8 kHz mono
constexpr int kAmbeFrameSamples = 160;   // 20 ms
constexpr int kAmbeFramesPerBurst = 3;   // one DMR voice burst carries 3 frames

// Pluggable AMBE decoder (design doc §16). The decoder owns the DMR-specific
// bit deinterleaving and AMBE unpacking; the pipeline only hands it a raw
// 33-byte DMR voice payload.
//
// The default implementation is a null decoder (silence). Enable real voice by
// building with -DBMRF_USE_MBELIB=ON and providing mbelib.
class IAmbeDecoder {
public:
    virtual ~IAmbeDecoder() = default;

    // Decode one 33-byte DMR voice payload into 16-bit mono PCM.
    // Returns the number of samples written, 0 for none, or -1 on error.
    virtual int decodeBurst(const uint8_t* payload, size_t payloadLen,
                            uint8_t voiceIndex, int16_t* pcm,
                            size_t maxSamples) = 0;

    virtual int sampleRate() const = 0;
    virtual int samplesPerBurst() const = 0;
    virtual const char* name() const = 0;
};

// Returns an mbelib-backed decoder when built with BMRF_USE_MBELIB, otherwise a
// silent null decoder.
std::unique_ptr<IAmbeDecoder> createAmbeDecoder();

#if defined(BMRF_USE_MBELIB)
// Defined in mbelib_decoder.cpp when built with -DBMRF_USE_MBELIB=ON.
std::unique_ptr<IAmbeDecoder> createMbelibDecoder();
#endif

} // namespace bmrf
