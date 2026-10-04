#include "test_framework.hpp"

#include "bmrf/audio/audio_output.hpp"
#include "bmrf/audio/jitter_buffer.hpp"
#include "bmrf/audio/voice_pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace bmrf;

namespace {

class FakeDecoder : public IAmbeDecoder {
public:
    std::atomic<int> calls{0};

    int decodeBurst(const uint8_t*, size_t, uint8_t, int16_t* pcm, size_t maxSamples) override
    {
        calls.fetch_add(1);
        const size_t count = std::min(maxSamples, static_cast<size_t>(samplesPerBurst()));
        for (size_t i = 0; i < count; ++i)
            pcm[i] = 0;
        return static_cast<int>(count);
    }
    int sampleRate() const override { return 8000; }
    int samplesPerBurst() const override { return 480; }
    const char* name() const override { return "fake"; }
};

class FakeOutput : public IAudioOutput {
public:
    std::atomic<int> writes{0};

    bool open(int, int, std::string&) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    int write(const int16_t*, size_t count) override
    {
        writes.fetch_add(1);
        return static_cast<int>(count);
    }
    const char* name() const override { return "fake"; }

private:
    bool m_open = false;
};

DMRFrame voiceFrame(uint32_t src, uint32_t dst, uint32_t sequence)
{
    DMRFrame frame;
    frame.has_voice = true;
    frame.data_type = DataType::Voice;
    frame.sequence = sequence;
    frame.source_id = src;
    frame.destination_id = dst;
    frame.timeslot = 2;
    return frame;
}

} // namespace

BMRF_TEST(jitter_buffer_bounds_and_drops)
{
    JitterBuffer buffer(2);
    VoiceBurst a;
    a.sequence = 1;
    VoiceBurst b;
    b.sequence = 2;
    VoiceBurst c;
    c.sequence = 3;

    BMRF_CHECK(buffer.push(a));
    BMRF_CHECK(buffer.push(b));
    BMRF_CHECK(!buffer.push(c)); // oldest dropped
    BMRF_CHECK_EQ(buffer.size(), size_t(2));

    VoiceBurst out;
    BMRF_CHECK(buffer.tryPop(out));
    BMRF_CHECK_EQ(out.sequence, 2ULL);
    BMRF_CHECK(buffer.tryPop(out));
    BMRF_CHECK_EQ(out.sequence, 3ULL);
    BMRF_CHECK(!buffer.tryPop(out));
}

BMRF_TEST(voice_pipeline_decodes_bursts)
{
    auto decoder = std::make_unique<FakeDecoder>();
    auto* decoderPtr = decoder.get();
    auto output = std::make_unique<FakeOutput>();
    auto* outputPtr = output.get();

    VoicePipeline pipeline(std::move(decoder), std::move(output));
    pipeline.setEnabled(true);
    pipeline.start();

    for (uint32_t i = 0; i < 3; ++i)
        pipeline.push(voiceFrame(100, 91, i));

    for (int i = 0; i < 200 && decoderPtr->calls.load() < 3; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    BMRF_CHECK(decoderPtr->calls.load() >= 3);
    BMRF_CHECK(outputPtr->writes.load() >= 3);

    pipeline.stop();
}

BMRF_TEST(voice_pipeline_disabled_drops)
{
    auto decoder = std::make_unique<FakeDecoder>();
    auto* decoderPtr = decoder.get();

    VoicePipeline pipeline(std::move(decoder), std::make_unique<FakeOutput>());
    pipeline.setEnabled(false);
    pipeline.start();

    pipeline.push(voiceFrame(1, 2, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    BMRF_CHECK_EQ(decoderPtr->calls.load(), 0);
    pipeline.stop();
}

#if defined(BMRF_USE_MBELIB)
BMRF_TEST(mbelib_decoder_smoke)
{
    auto decoder = createAmbeDecoder();
    BMRF_CHECK(decoder != nullptr);
    BMRF_CHECK_EQ(std::string(decoder->name()), std::string("mbelib"));
    BMRF_CHECK_EQ(decoder->sampleRate(), kAmbeSampleRate);
    BMRF_CHECK_EQ(decoder->samplesPerBurst(), kAmbeFramesPerBurst * kAmbeFrameSamples);

    std::array<uint8_t, kDmrPayloadBytes> payload{};
    int16_t pcm[480] = {0};
    const int samples = decoder->decodeBurst(payload.data(), payload.size(), 0, pcm, 480);
    BMRF_CHECK_EQ(samples, kAmbeFramesPerBurst * kAmbeFrameSamples);
}
#endif
