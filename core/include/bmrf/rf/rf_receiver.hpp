#pragma once

#include "bmrf/dmr/frame.hpp"
#include "bmrf/rf/burst_source.hpp"

#if defined(BMRF_USE_DSDCC)
#include "bmrf/rf/dsdcc_demod.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bmrf {
class CallManager;
class EventBus;
class VoicePipeline;
}

namespace bmrf::rf {

// Drives the RF receive chain: burst source -> RfDecoder -> CallManager /
// VoicePipeline / EventBus, on its own thread.
class RfReceiver {
public:
    using FrameCallback = std::function<void(const DMRFrame&)>;

    explicit RfReceiver(std::unique_ptr<IDmrBurstSource> source);
    ~RfReceiver();

    void setCallManager(CallManager* manager) { m_callManager = manager; }
    void setEventBus(EventBus* bus) { m_bus = bus; }
    void setVoicePipeline(VoicePipeline* pipeline) { m_pipeline = pipeline; }
    void setFrameCallback(FrameCallback callback) { m_callback = std::move(callback); }

    // Two independent toggles:
    //  - audio: play decoded RF voice locally (uplink audio monitoring)
    //  - forward: send decoded RF frames to BrandMeister (RF -> BM uplink)
    void setAudioEnabled(bool enabled) { m_audioEnabled.store(enabled); }
    void setForwardEnabled(bool enabled) { m_forwardEnabled.store(enabled); }
    bool audioEnabled() const { return m_audioEnabled.load(); }
    bool forwardEnabled() const { return m_forwardEnabled.load(); }

    // Restrict audio/uplink to one DMR timeslot (0 = both/any). Prevents the
    // idle timeslot's noise from being played alongside the active one.
    void setTimeslot(int slot) { m_timeslot.store(slot); }
    int timeslot() const { return m_timeslot.load(); }

    // Uplink sink (typically HomebrewClient::sendFrame).
    void setUplink(std::function<bool(const DMRFrame&)> uplink) { m_uplink = std::move(uplink); }

    // When the build includes DSDcc, the receiver can decode DMR voice with it
    // instead of the built-in demodulator (IQ -> DSDcc -> PCM -> VoicePipeline).
    // Enabled by default when available.
    void setDsdccEnabled(bool enabled) { m_dsdccEnabled.store(enabled); }
    bool dsdccEnabled() const { return m_dsdccEnabled.load(); }

    bool open(const std::string& spec, std::string& error);
    void close();
    void start();
    void stop();

    bool running() const { return m_running.load(); }
    uint64_t decodedVoiceFrames() const { return m_decoded.load(); }
    uint64_t forwardedFrames() const { return m_forwarded.load(); }
    double rfLevelDb() const { return m_levelDb.load(); }
    double rfSnrDb() const { return m_snrDb.load(); }
    const char* sourceName() const;

private:
    void run();

    std::unique_ptr<IDmrBurstSource> m_source;
    CallManager* m_callManager = nullptr;
    EventBus* m_bus = nullptr;
    VoicePipeline* m_pipeline = nullptr;
    FrameCallback m_callback;

    std::function<bool(const DMRFrame&)> m_uplink;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_dsdccEnabled{true};
#if defined(BMRF_USE_DSDCC)
    std::unique_ptr<DsdccDmrDemod> m_dsdcc;
    uint32_t m_dsdRate = 0;
    std::vector<uint8_t> m_iqBuf;
    std::vector<int16_t> m_dsdPcm;
#endif
    std::atomic<bool> m_audioEnabled{false};
    std::atomic<bool> m_forwardEnabled{false};
    std::atomic<int> m_timeslot{0};
    std::atomic<uint64_t> m_decoded{0};
    std::atomic<uint64_t> m_forwarded{0};
    std::atomic<float> m_levelDb{0.0f};
    std::atomic<float> m_snrDb{0.0f};
    uint64_t m_lastRfActivityMs = 0;
};

} // namespace bmrf::rf
