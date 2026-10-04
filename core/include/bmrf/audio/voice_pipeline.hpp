#pragma once

#include "bmrf/audio/audio_output.hpp"
#include "bmrf/audio/jitter_buffer.hpp"
#include "bmrf/audio/voice.hpp"
#include "bmrf/dmr/frame.hpp"
#include "bmrf/vocoder/ambe_decoder.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bmrf {

struct VoicePipelineConfig {
    size_t queueCapacity = 32;
};

// Downlink voice pipeline (design doc §7 / §8):
//   DMRFrame -> JitterBuffer -> IAmbeDecoder -> IAudioOutput
// Runs its own worker thread so network jitter never blocks the audio sink.
class VoicePipeline {
public:
    using DecoderFactory = std::function<std::unique_ptr<IAmbeDecoder>()>;

    // Creates one independent AMBE decoder per DMR timeslot (1 and 2) so that
    // the two interleaved slots' codec state does not mix (which sounds like
    // noise).
    VoicePipeline(DecoderFactory factory,
                  std::unique_ptr<IAudioOutput> output,
                  VoicePipelineConfig config = {});

    // Single-decoder convenience constructor (tests / single-slot use).
    VoicePipeline(std::unique_ptr<IAmbeDecoder> decoder,
                  std::unique_ptr<IAudioOutput> output,
                  VoicePipelineConfig config = {});
    ~VoicePipeline();

    void start();
    void stop();

    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled.load(); }

    // While an RF uplink is active, suppress downlink (network) audio so the
    // local RF transmission is heard on its own. RF PCM pushed via pushPcm is
    // unaffected. Set by the RF receiver.
    void setRfActive(bool active) { m_rfActive.store(active); }
    bool rfActive() const { return m_rfActive.load(); }

    // Called from the network thread. No-op when disabled or not running.
    void push(const DMRFrame& frame);

    // Push already-decoded PCM (8 kHz mono S16LE) straight to the audio output.
    // Used by the DSDcc RF path, which decodes voice itself. Thread-safe.
    void pushPcm(const int16_t* pcm, size_t count);

    void reset();

    const char* decoderName() const;
    std::string outputName() const;
    int sampleRate() const;

    bool playing() const;
    uint64_t decodedBursts() const { return m_decoded.load(); }
    uint64_t droppedBursts() const { return m_dropped.load(); }
    size_t queueSize() const { return m_buffer.size(); }
    // Latency from a downlink burst's arrival timestamp to local audio output.
    uint64_t lastLatencyMs() const { return m_last_latency_ms.load(); }

private:
    void run();
    int writeOutput(const int16_t* pcm, size_t count);

    VoicePipelineConfig m_config;
    // Index by timeslot (0 = unknown/default, 1, 2).
    std::array<std::unique_ptr<IAmbeDecoder>, 3> m_decoders;
    bool m_singleDecoder = false;
    std::unique_ptr<IAudioOutput> m_output;
    std::mutex m_outputMutex;
    JitterBuffer m_buffer;
    std::thread m_thread;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_rfActive{false};
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_last_activity_ms{0};
    std::atomic<uint64_t> m_last_latency_ms{0};
    std::atomic<uint64_t> m_decoded{0};
    std::atomic<uint64_t> m_dropped{0};

    std::vector<int16_t> m_pcm;
};

} // namespace bmrf
