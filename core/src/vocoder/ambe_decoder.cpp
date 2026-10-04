#include "bmrf/vocoder/ambe_decoder.hpp"

#include "bmrf/common/log.hpp"

#include <algorithm>
#include <cstring>

namespace bmrf {

namespace {

// Silent fallback decoder. Keeps the audio pipeline timing intact so the UI can
// show call/audio activity even when no real AMBE decoder is available.
class NullAmbeDecoder : public IAmbeDecoder {
public:
    int decodeBurst(const uint8_t*, size_t, uint8_t, int16_t* pcm, size_t maxSamples) override
    {
        const size_t count = std::min(maxSamples, static_cast<size_t>(samplesPerBurst()));
        std::memset(pcm, 0, count * sizeof(int16_t));
        return static_cast<int>(count);
    }
    int sampleRate() const override { return kAmbeSampleRate; }
    int samplesPerBurst() const override { return kAmbeFramesPerBurst * kAmbeFrameSamples; }
    const char* name() const override { return "null"; }
};

} // namespace

std::unique_ptr<IAmbeDecoder> createAmbeDecoder()
{
#if defined(BMRF_USE_MBELIB)
    if (auto decoder = createMbelibDecoder())
        return decoder;
    logWarn("vocoder", "mbelib decoder unavailable, falling back to null decoder");
#endif
    return std::make_unique<NullAmbeDecoder>();
}

} // namespace bmrf
