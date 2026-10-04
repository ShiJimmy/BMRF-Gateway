#include "bmrf/audio/voice_pipeline.hpp"

#include "bmrf/common/log.hpp"
#include "bmrf/common/time.hpp"

#include <cmath>
#include <string>

namespace bmrf {

namespace {
constexpr const char* kTag = "audio";
}

VoicePipeline::VoicePipeline(DecoderFactory factory,
                             std::unique_ptr<IAudioOutput> output,
                             VoicePipelineConfig config)
    : m_config(config),
      m_output(std::move(output)),
      m_buffer(config.queueCapacity)
{
    // Independent decoder per timeslot so the two interleaved slots' AMBE state
    // never mixes.
    for (int slot = 0; slot < 3; ++slot)
        m_decoders[static_cast<size_t>(slot)] = factory();

    const size_t samples =
        m_decoders[0] ? static_cast<size_t>(m_decoders[0]->samplesPerBurst())
                      : static_cast<size_t>(kAmbeFramesPerBurst * kAmbeFrameSamples);
    m_pcm.resize(samples);
}

VoicePipeline::VoicePipeline(std::unique_ptr<IAmbeDecoder> decoder,
                             std::unique_ptr<IAudioOutput> output,
                             VoicePipelineConfig config)
    : m_config(config),
      m_output(std::move(output)),
      m_buffer(config.queueCapacity),
      m_singleDecoder(true)
{
    m_decoders[0] = std::move(decoder);
    const size_t samples =
        m_decoders[0] ? static_cast<size_t>(m_decoders[0]->samplesPerBurst())
                      : static_cast<size_t>(kAmbeFramesPerBurst * kAmbeFrameSamples);
    m_pcm.resize(samples);
}

VoicePipeline::~VoicePipeline()
{
    stop();
}

void VoicePipeline::start()
{
    if (m_running.exchange(true))
        return;

    m_buffer.clear();

    std::string error;
    if (m_output && !m_output->open(sampleRate(), 1, error))
        logWarn(kTag, "audio output open failed: " + error);

    m_thread = std::thread([this] { run(); });
}

void VoicePipeline::stop()
{
    const bool wasRunning = m_running.exchange(false);
    if (wasRunning)
        m_buffer.close();
    if (m_thread.joinable())
        m_thread.join();
    if (m_output)
        m_output->close();
    m_buffer.clear(); // reset the closed flag for a possible restart
}

void VoicePipeline::setEnabled(bool enabled)
{
    m_enabled.store(enabled);
    if (!enabled)
        m_buffer.clear();
}

void VoicePipeline::push(const DMRFrame& frame)
{
    if (!m_enabled.load() || !m_running.load() || !frame.has_voice)
        return;
    if (m_rfActive.load())
        return; // ducked while an RF uplink is active

    VoiceBurst burst;
    burst.payload = frame.ambe;
    burst.voiceIndex = static_cast<uint8_t>(frame.sequence & 0x0F);
    burst.sequence = frame.sequence;
    burst.timestampNs = frame.timestamp_ns;
    burst.timeslot = frame.timeslot;
    burst.voiceSync = (frame.data_type == DataType::VoiceSync);

    if (!m_buffer.push(burst))
        m_dropped.fetch_add(1);
}

void VoicePipeline::pushPcm(const int16_t* pcm, size_t count)
{
    if (!m_enabled.load() || !m_running.load() || pcm == nullptr || count == 0)
        return;

    if (writeOutput(pcm, count) > 0) {
        m_last_activity_ms.store(nowMs());
        const uint64_t decoded = m_decoded.fetch_add(1) + 1;
        if (decoded <= 3 || (decoded % 50) == 0) {
            double acc = 0.0;
            for (size_t i = 0; i < count; ++i) {
                const double v = pcm[i];
                acc += v * v;
            }
            logInfo(kTag, "dsdcc pcm blocks=" + std::to_string(decoded) + " samples=" +
                              std::to_string(count) + " pcmRms=" +
                              std::to_string(count ? std::sqrt(acc / count) : 0.0));
        }
    }
}

int VoicePipeline::writeOutput(const int16_t* pcm, size_t count)
{
    std::lock_guard<std::mutex> lock(m_outputMutex);
    if (m_output && m_output->isOpen())
        return m_output->write(pcm, count);
    return -1;
}

void VoicePipeline::reset()
{
    m_buffer.clear();
}

const char* VoicePipeline::decoderName() const
{
    return m_decoders[0] ? m_decoders[0]->name() : "none";
}

std::string VoicePipeline::outputName() const
{
    return m_output ? m_output->name() : "none";
}

int VoicePipeline::sampleRate() const
{
    return m_decoders[0] ? m_decoders[0]->sampleRate() : kAmbeSampleRate;
}

bool VoicePipeline::playing() const
{
    const uint64_t last = m_last_activity_ms.load();
    return m_enabled.load() && last != 0 && (nowMs() - last) < 500;
}

void VoicePipeline::run()
{
    VoiceBurst burst;
    while (m_running.load()) {
        if (!m_buffer.waitPop(burst, 200))
            continue;
        if (!m_running.load())
            break;
        if (m_rfActive.load())
            continue; // ducked while an RF uplink is active

        IAmbeDecoder* decoder = m_decoders[0].get();
        if (!m_singleDecoder && (burst.timeslot == 1 || burst.timeslot == 2)) {
            IAmbeDecoder* slotDecoder = m_decoders[static_cast<size_t>(burst.timeslot)].get();
            if (slotDecoder != nullptr)
                decoder = slotDecoder;
        }
        if (decoder == nullptr)
            continue;

        const int samples = decoder->decodeBurst(
            burst.payload.data(), burst.payload.size(), burst.voiceIndex,
            m_pcm.data(), m_pcm.size());

        if (samples > 0)
            writeOutput(m_pcm.data(), static_cast<size_t>(samples));

        // Staged timing (§24): latency from the burst's arrival to local audio.
        if (burst.timestampNs != 0) {
            const uint64_t now = nowNs();
            if (now > burst.timestampNs)
                m_last_latency_ms.store((now - burst.timestampNs) / 1000000ULL);
        }

        if (samples >= 0) {
            const uint64_t decoded = m_decoded.fetch_add(1) + 1;
            m_last_activity_ms.store(nowMs());

            if (decoded <= 3 || (decoded % 20) == 0) {
                double acc = 0.0;
                for (int i = 0; i < samples; ++i) {
                    const double v = m_pcm[static_cast<size_t>(i)];
                    acc += v * v;
                }
                const double pcmRms =
                    samples > 0 ? std::sqrt(acc / samples) : 0.0;
                logInfo(kTag, "decoded " + std::to_string(decoded) + " bursts, pcmRms=" +
                                  std::to_string(pcmRms));
            }
        }
    }
}

} // namespace bmrf
