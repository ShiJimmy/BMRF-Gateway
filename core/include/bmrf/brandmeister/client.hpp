#pragma once

#include "bmrf/brandmeister/homebrew.hpp"
#include "bmrf/brandmeister/network_state.hpp"
#include "bmrf/brandmeister/rx.hpp"
#include "bmrf/brandmeister/tx.hpp"
#include "bmrf/brandmeister/transport.hpp"
#include "bmrf/dmr/event.hpp"
#include "bmrf/dmr/frame.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bmrf {
class CallManager;
class EventBus;
class VoicePipeline;
}

namespace bmrf::brandmeister {

struct ClientConfig {
    std::string master_host = "brandmeister.network";
    uint16_t master_port = kDefaultPort;
    uint32_t repeater_id = 0;
    std::string password;
    RepeaterConfig repeater;
    bool enable_tx = false; // RF -> BM uplink (future stage)
};

// BrandMeister Homebrew client with the network state machine of design doc §14.
// The owner drives it by calling clock(nowMs) periodically (e.g. every 50 ms).
class HomebrewClient {
public:
    using FrameCallback = std::function<void(const DMRFrame&)>;

    HomebrewClient(std::unique_ptr<ITransport> transport, ClientConfig config);
    ~HomebrewClient();

    void setCallManager(CallManager* manager);
    void setEventBus(EventBus* bus);
    void setVoicePipeline(VoicePipeline* pipeline);
    void setAudioEnabled(bool enabled) { m_audioEnabled.store(enabled); }
    bool audioEnabled() const { return m_audioEnabled.load(); }
    void setFrameCallback(FrameCallback cb);

    // Restrict local playback of downlink voice to these destination IDs
    // (talkgroups). Empty = play every call. Does not affect the call list.
    void setAudioTalkgroups(std::vector<uint32_t> talkgroups);

    void start(uint64_t now_ms);
    void stop();
    void clock(uint64_t now_ms);

    NetworkState state() const { return m_state; }
    const std::string& masterHost() const { return m_config.master_host; }
    uint16_t masterPort() const { return m_config.master_port; }
    // Resolved master endpoint actually connected to ("ip:port"); empty if unknown.
    std::string masterPeer() const
    {
        return m_transport ? m_transport->peerAddress() : std::string();
    }
    uint32_t repeaterId() const { return m_config.repeater_id; }
    uint32_t latencyMs() const { return m_latency_ms; }
    uint64_t lastRxMs() const { return m_last_rx_ms; }
    bool loggedIn() const { return m_auth == AuthState::Done; }

    // Forward an RF frame to BrandMeister (RF -> BM uplink). Returns false when
    // the transport is not open or the frame cannot be represented.
    bool sendFrame(const DMRFrame& frame);

    // Inject a received datagram. Used by tests and packet replay (design doc §21).
    void inject(const uint8_t* data, size_t len, uint64_t now_ms);

    // Send a raw datagram on the session socket (diagnostics / packet replay).
    bool sendRawPacket(const uint8_t* data, size_t len)
    {
        return writeRaw(std::vector<uint8_t>(data, data + len));
    }

private:
    enum class AuthState {
        None,
        SentLogin,
        SentKey,
        Done,
        Failed,
    };

    void handlePacket(const Packet& packet, uint64_t now_ms);
    void handleDmrData(const DMRData& data, uint64_t now_ms);
    void sendLogin();
    void sendKey();
    void sendConfig();
    void sendLcHeader(const DMRFrame& frame, int slotIndex);
    void sendTerminator(int slotIndex);
    void sendPing(uint64_t now_ms);
    void beginReconnect(uint64_t now_ms, const char* reason);
    void transitionTo(NetworkState state);
    void publishNetworkEvent(DMREventType, const std::string& message, uint64_t now_ms);
    bool writeRaw(const std::vector<uint8_t>& bytes);

    std::unique_ptr<ITransport> m_transport;
    ClientConfig m_config;
    CallManager* m_callManager = nullptr;
    EventBus* m_bus = nullptr;
    VoicePipeline* m_voicePipeline = nullptr;
    std::atomic<bool> m_audioEnabled{false};
    FrameCallback m_frameCallback;

    mutable std::mutex m_audioFilterMutex;
    std::vector<uint32_t> m_audioTalkgroups;
    bool audioAllowed(uint32_t destination_id) const;

    AuthState m_auth = AuthState::None;
    NetworkState m_state = NetworkState::Disconnected;

    std::array<uint8_t, 8> m_nonce{};
    bool m_hasNonce = false;
    // Wire dialect: Native (MMDVMHost/DroidStar) is the one BrandMeister
    // authorises for transmission. The ASCII variant remains available in
    // HomebrewEncoder for reference/tests.
    HomebrewDialect m_dialect = HomebrewDialect::Native;

    uint64_t m_last_rx_ms = 0;
    uint64_t m_last_ping_ms = 0;
    uint64_t m_ping_sent_ms = 0;
    uint64_t m_reconnect_at_ms = 0;
    uint32_t m_latency_ms = 0;

    // RF -> BM uplink state (per timeslot).
    uint8_t m_txSequence[2] = {0, 0};
    uint32_t m_txStreamId[2] = {0, 0};
    uint32_t m_nextStreamId = 1;
    bool m_slotActive[2] = {false, false};
    uint64_t m_slotLastFrameMs[2] = {0, 0};
    DMRFrame m_slotLastFrame[2];
    uint8_t m_slotFrames[2] = {0, 0}; // voice frames seen in the current call
    // Link Control of the call currently in progress on each slot (used to detect
    // a talkgroup/caller change mid-stream and start a new BM call).
    uint32_t m_callSrc[2] = {0, 0};
    uint32_t m_callDst[2] = {0, 0};
    uint8_t m_callFlco[2] = {0, 0};
    // After a mid-call talkgroup switch the new Voice LC Header is held until
    // this time so BrandMeister closes the old session first.
    uint64_t m_switchHoldUntilMs[2] = {0, 0};
    // Link-control stability vote: a new BM call only starts once the decoded
    // (source, destination, FLCO) has been stable for several consecutive frames,
    // so the synthesised Voice LC Header uses a clean LC.
    uint32_t m_pendSrc[2] = {0, 0};
    uint32_t m_pendDst[2] = {0, 0};
    uint8_t m_pendFlco[2] = {0, 0};
    uint8_t m_pendCount[2] = {0, 0};
    uint64_t m_slotLastHeaderMs[2] = {0, 0}; // last Voice LC Header sent per slot
};

} // namespace bmrf::brandmeister
