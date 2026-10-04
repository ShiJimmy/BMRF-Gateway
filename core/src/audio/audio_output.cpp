#include "bmrf/audio/audio_output.hpp"

#include "bmrf/common/log.hpp"

#include <algorithm>
#include <cstring>

#if defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif

namespace bmrf {

namespace {

constexpr const char* kTag = "audio/out";

class NullAudioOutput : public IAudioOutput {
public:
    bool open(int, int, std::string&) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    int write(const int16_t*, size_t count) override { return static_cast<int>(count); }
    const char* name() const override { return "null"; }

private:
    bool m_open = false;
};

#if defined(__ANDROID__)

class AAudioOutput : public IAudioOutput {
public:
    ~AAudioOutput() override { close(); }

    bool open(int sampleRate, int channels, std::string& error) override
    {
        close();

        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
            error = "AAudio_createStreamBuilder failed";
            return false;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(builder, channels);
        AAudioStreamBuilder_setSampleRate(builder, sampleRate);

        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &m_stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || m_stream == nullptr) {
            error = std::string("AAudio open failed: ") + AAudio_convertResultToText(result);
            m_stream = nullptr;
            return false;
        }

        AAudioStream_requestStart(m_stream);
        logInfo(kTag, "AAudio stream opened @ " + std::to_string(sampleRate) + " Hz");
        return true;
    }

    void close() override
    {
        if (m_stream != nullptr) {
            AAudioStream_requestStop(m_stream);
            AAudioStream_close(m_stream);
            m_stream = nullptr;
        }
    }

    bool isOpen() const override { return m_stream != nullptr; }

    int write(const int16_t* samples, size_t count) override
    {
        if (m_stream == nullptr)
            return -1;
        const aaudio_result_t written =
            AAudioStream_write(m_stream, samples, static_cast<int32_t>(count), 100000000LL);
        if (written < 0)
            return -1;
        return static_cast<int>(written);
    }

    const char* name() const override { return "aaudio"; }

private:
    AAudioStream* m_stream = nullptr;
};

#endif // __ANDROID__

} // namespace

std::unique_ptr<IAudioOutput> createAudioOutput()
{
#if defined(__ANDROID__)
    return std::make_unique<AAudioOutput>();
#else
    return std::make_unique<NullAudioOutput>();
#endif
}

} // namespace bmrf
