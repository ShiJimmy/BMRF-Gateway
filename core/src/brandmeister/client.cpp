#include "bmrf/brandmeister/client.hpp"

#include "bmrf/capture/capture.hpp"
#include "bmrf/common/log.hpp"
#include "bmrf/dmr/lc_encode.hpp"
#include "bmrf/common/time.hpp"
#include "bmrf/audio/voice_pipeline.hpp"
#include "bmrf/brandmeister/uplink.hpp"
#include "bmrf/dmr/call_manager.hpp"
#include "bmrf/dmr/event_bus.hpp"

#include <cstring>
#include <string>

namespace bmrf::brandmeister {

namespace {

constexpr const char* kTag = "BM";
constexpr uint64_t kReconnectDelayMs = 3000;
// Delay between the old call's Terminator and the new call's Voice LC Header
// when the radio switches talkgroup mid-stream (see sendFrame).
constexpr uint64_t kSwitchHoldMs = 200;

std::string hexBytes(const uint8_t* data, size_t len)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

} // namespace

HomebrewClient::HomebrewClient(std::unique_ptr<ITransport> transport, ClientConfig config)
    : m_transport(std::move(transport)), m_config(std::move(config))
{
}

HomebrewClient::~HomebrewClient()
{
    stop();
}

void HomebrewClient::setCallManager(CallManager* manager)
{
    m_callManager = manager;
}

void HomebrewClient::setEventBus(EventBus* bus)
{
    m_bus = bus;
}

void HomebrewClient::setAudioTalkgroups(std::vector<uint32_t> talkgroups)
{
    std::lock_guard<std::mutex> lock(m_audioFilterMutex);
    m_audioTalkgroups = std::move(talkgroups);
}

bool HomebrewClient::audioAllowed(uint32_t destination_id) const
{
    std::lock_guard<std::mutex> lock(m_audioFilterMutex);
    if (m_audioTalkgroups.empty())
        return true;
    for (uint32_t tg : m_audioTalkgroups) {
        if (tg == destination_id)
            return true;
    }
    return false;
}

void HomebrewClient::setVoicePipeline(VoicePipeline* pipeline)
{
    m_voicePipeline = pipeline;
}

void HomebrewClient::setFrameCallback(FrameCallback callback)
{
    m_frameCallback = std::move(callback);
}

void HomebrewClient::start(uint64_t now_ms)
{
    if (!m_transport)
        return;

    std::string error;
    if (!m_transport->open(m_config.master_host, m_config.master_port, error)) {
        publishNetworkEvent(DMREventType::NetworkDisconnected, "open failed: " + error, now_ms);
        transitionTo(NetworkState::Reconnecting);
        m_reconnect_at_ms = now_ms + kReconnectDelayMs;
        return;
    }

    m_last_rx_ms = now_ms;
    m_last_ping_ms = now_ms;
    m_auth = AuthState::None;
    m_hasNonce = false;
    sendLogin();
}

void HomebrewClient::stop()
{
    // Tell the master we are leaving so it drops the session immediately
    // instead of waiting for its keepalive timeout (MMDVMHost sends RPTCL too).
    if (m_transport && m_transport->isOpen())
        writeRaw(HomebrewEncoder::buildClose(m_config.repeater_id, m_dialect));
    if (m_transport)
        m_transport->close();
    m_auth = AuthState::None;
    m_hasNonce = false;
    transitionTo(NetworkState::Disconnected);
}

void HomebrewClient::sendLogin()
{
    writeRaw(HomebrewEncoder::buildLogin(m_config.repeater_id, m_dialect));
    m_auth = AuthState::SentLogin;
    transitionTo(NetworkState::Connecting);
    logInfo(kTag, "sent RPTL repeater=" + std::to_string(m_config.repeater_id));
}

void HomebrewClient::sendKey()
{
    const size_t saltLen = (m_dialect == HomebrewDialect::Ascii) ? 8U : 4U;
    writeRaw(HomebrewEncoder::buildKey(m_config.repeater_id, m_nonce.data(), saltLen,
                                       m_config.password, m_dialect));
    m_auth = AuthState::SentKey;
    logDebug(kTag, "sent RPTK (password)");
}

void HomebrewClient::sendConfig()
{
    RepeaterConfig config = m_config.repeater;
    if (config.id == 0)
        config.id = m_config.repeater_id;
    writeRaw(HomebrewEncoder::buildConfig(config, m_dialect));
    logDebug(kTag, "sent RPTC (configuration)");
}

void HomebrewClient::sendPing(uint64_t now_ms)
{
    writeRaw(HomebrewEncoder::buildPing(m_config.repeater_id, m_dialect));
    m_last_ping_ms = now_ms;
    m_ping_sent_ms = now_ms;
    logTrace(kTag, "sent MSTPING");
}

bool HomebrewClient::writeRaw(const std::vector<uint8_t>& bytes)
{
    if (!m_transport || !m_transport->isOpen())
        return false;
    if (bytes.size() != 55)
        logInfo("BM/tx", hexBytes(bytes.data(), bytes.size()));
    return m_transport->send(bytes.data(), bytes.size());
}

void HomebrewClient::transitionTo(NetworkState state)
{
    if (m_state == state)
        return;
    const NetworkState previous = m_state;
    m_state = state;
    logInfo(kTag, std::string("state ") + toString(previous) + " -> " + toString(state));
}

void HomebrewClient::publishNetworkEvent(DMREventType type, const std::string& message,
                                         uint64_t now_ms)
{
    if (m_bus == nullptr)
        return;
    DMREvent event;
    event.type = type;
    event.message = message;
    event.timestamp_ns = msToNs(now_ms);
    m_bus->publish(event);
}

void HomebrewClient::sendLcHeader(const DMRFrame& frame, int slotIndex)
{
    const uint8_t flco = frame.flco != 0
                             ? frame.flco
                             : (frame.call_type == CallType::Private ? 0x03U : 0x00U);
    const uint8_t colorCode = frame.color_code >= 0
                                  ? static_cast<uint8_t>(frame.color_code)
                                  : 1U;

    uint8_t payload[33];
    if (!encodeDmrLcPayload(flco, 0x00U, frame.service_options, frame.destination_id,
                            frame.source_id, colorCode, 1U, payload))
        return;

    DMRFrame header = frame;
    header.data_type = DataType::VoiceHeader;
    header.sequence = 0;
    std::memcpy(header.ambe.data(), payload, header.ambe.size());

    const DMRData data = toDmrData(header, m_config.repeater_id,
                                   m_txStreamId[slotIndex], 0);
    const std::vector<uint8_t> bytes = HomebrewEncoder::buildDmrData(data);
    writeRaw(bytes);
    m_slotLastHeaderMs[slotIndex] = nowMs();
    logInfo("BM/tx", "Voice LC header src=" + std::to_string(frame.source_id) +
                         " dst=" + std::to_string(frame.destination_id) + " " +
                         hexBytes(bytes.data(), bytes.size()));
}

bool HomebrewClient::sendFrame(const DMRFrame& frame)
{
    // Only forward once the Homebrew session is fully active (logged in and
    // configured); never before the network is ready (design doc §14).
    if (m_state != NetworkState::Active) {
        static uint64_t dropCount = 0;
        if ((++dropCount % 50) == 1)
            logWarn(kTag, "uplink dropped: network state=" + std::string(toString(m_state)));
        return false;
    }
    if (!m_transport || !m_transport->isOpen())
        return false;

    const int slotIndex = (frame.timeslot == 2) ? 1 : 0;

    // Track the pending Link Control (source/destination/FLCO) and count how many
    // consecutive frames carry it, so a noisy LC does not start/restart a call.
    uint8_t& pc = m_pendCount[slotIndex];
    if (frame.source_id == m_pendSrc[slotIndex] &&
        frame.destination_id == m_pendDst[slotIndex] &&
        frame.flco == m_pendFlco[slotIndex]) {
        if (pc < 250)
            ++pc;
    } else {
        m_pendSrc[slotIndex] = frame.source_id;
        m_pendDst[slotIndex] = frame.destination_id;
        m_pendFlco[slotIndex] = frame.flco;
        pc = 1;
    }

    // A call is in progress but the Link Control changed (the radio switched
    // talkgroup/caller). Do not append the new transmission to the old call:
    // drop the first frames until the new LC is confirmed, then terminate the old
    // call so BrandMeister stops routing to the old target; the code below emits
    // a fresh Voice LC Header for the new one. Without this the first post-switch
    // transmission kept the old target.
    if (m_slotActive[slotIndex] &&
        (frame.source_id != m_callSrc[slotIndex] ||
         frame.destination_id != m_callDst[slotIndex] ||
         frame.flco != m_callFlco[slotIndex])) {
        if (pc < 3)
            return false; // LC settling; don't leak into the old call
        sendTerminator(slotIndex);
        m_slotActive[slotIndex] = false;
        m_txStreamId[slotIndex] = 0;
        // BrandMeister merges a Voice LC Header that arrives in the same instant
        // as the Terminator into the old talkgroup's session. Hold the new call
        // back for a short beat so the master closes the old one first.
        m_switchHoldUntilMs[slotIndex] = nowMs() + kSwitchHoldMs;
        return false;
    }

    if (m_switchHoldUntilMs[slotIndex] != 0 && nowMs() < m_switchHoldUntilMs[slotIndex])
        return false; // let the master process the terminator before the header

    if (!m_slotActive[slotIndex] && pc < 3)
        return false; // brief LC settle before starting the BM call

    bool newCall = false;
    if (!m_slotActive[slotIndex] || m_txStreamId[slotIndex] == 0 ||
        frame.data_type == DataType::VoiceHeader) {
        // New call on this timeslot: pick a fresh random stream id (LCG) and
        // restart the DMRD/voice numbering, exactly like DroidStar (header seq 0,
        // first voice frame seq 1, ...), and remember the LC this call belongs to.
        newCall = true;
        m_nextStreamId = m_nextStreamId * 1664525u + 1013904223u;
        m_txStreamId[slotIndex] = m_nextStreamId | 1u;
        m_slotFrames[slotIndex] = 0;
        m_txSequence[slotIndex] = 0;
        m_callSrc[slotIndex] = frame.source_id;
        m_callDst[slotIndex] = frame.destination_id;
        m_callFlco[slotIndex] = frame.flco;
    }

    // BrandMeister expects a Voice LC Header (data-sync burst) once at the start
    // of a call so it can classify/route it (as MMDVMHost/DroidStar do). The RF
    // path only produces voice frames, so synthesise the header once per call.
    if (newCall && frame.data_type != DataType::VoiceHeader && frame.has_voice) {
        sendLcHeader(frame, slotIndex);
        m_txSequence[slotIndex] = 1; // header carried seq 0
    }
    if (m_slotFrames[slotIndex] < 255)
        ++m_slotFrames[slotIndex];

    // Voice frames carry the DMR embedded signalling (embedded LC slice + EMB);
    // n == 0 becomes a Voice Sync burst. Without it BrandMeister does not accept
    // the call's voice stream.
    DMRFrame out = frame;
    if (frame.data_type == DataType::Voice || frame.data_type == DataType::VoiceSync) {
        const int n = static_cast<int>(frame.sequence % 6U);
        const uint8_t flco = frame.flco != 0
                                 ? frame.flco
                                 : (frame.call_type == CallType::Private ? 0x03U : 0x00U);
        const uint8_t cc = frame.color_code >= 0
                               ? static_cast<uint8_t>(frame.color_code)
                               : 1U;
        uint8_t lc[9];
        lc[0] = flco;
        lc[1] = 0U;
        lc[2] = frame.service_options;
        lc[3] = static_cast<uint8_t>((frame.destination_id >> 16) & 0xFFU);
        lc[4] = static_cast<uint8_t>((frame.destination_id >> 8) & 0xFFU);
        lc[5] = static_cast<uint8_t>(frame.destination_id & 0xFFU);
        lc[6] = static_cast<uint8_t>((frame.source_id >> 16) & 0xFFU);
        lc[7] = static_cast<uint8_t>((frame.source_id >> 8) & 0xFFU);
        lc[8] = static_cast<uint8_t>(frame.source_id & 0xFFU);

        applyVoiceSignalling(out.ambe.data(), lc, n, cc);
        if (n == 0)
            out.data_type = DataType::VoiceSync;
    }

    const DMRData data = toDmrData(out, m_config.repeater_id, m_txStreamId[slotIndex],
                                   m_txSequence[slotIndex]);
    m_txSequence[slotIndex]++;
    const std::vector<uint8_t> bytes = HomebrewEncoder::buildDmrData(data);
    {
        static uint64_t txCount = 0;
        if ((++txCount % 20) == 1)
            logInfo("BM/tx", "UPLINK " + hexBytes(bytes.data(), bytes.size()));
    }
    if (captureActive())
        captureWrite("bm_tx", nowMs() * 1000000ULL, bytes.data(), bytes.size());
    const bool ok = writeRaw(bytes);
    m_slotActive[slotIndex] = true;
    m_slotLastFrameMs[slotIndex] = nowMs();
    m_slotLastFrame[slotIndex] = frame;
    return ok;
}

void HomebrewClient::sendTerminator(int slotIndex)
{
    if (m_state != NetworkState::Active || !m_transport || !m_transport->isOpen())
        return;

    const DMRFrame& f = m_slotLastFrame[slotIndex];
    if (f.source_id == 0 && f.destination_id == 0)
        return;

    const uint8_t flco = f.flco != 0
                             ? f.flco
                             : (f.call_type == CallType::Private ? 0x03U : 0x00U);
    const uint8_t colorCode = f.color_code >= 0
                                  ? static_cast<uint8_t>(f.color_code)
                                  : 1U;

    uint8_t payload[33];
    if (!encodeDmrLcPayload(flco, 0x00U, f.service_options, f.destination_id,
                            f.source_id, colorCode, 2U, payload))
        return;

    DMRFrame term = f;
    term.data_type = DataType::TerminatorLC;
    term.sequence = 0;
    std::memcpy(term.ambe.data(), payload, term.ambe.size());

    const DMRData data = toDmrData(term, m_config.repeater_id,
                                   m_txStreamId[slotIndex], 0);
    const std::vector<uint8_t> bytes = HomebrewEncoder::buildDmrData(data);
    writeRaw(bytes);
    logInfo("BM/tx", "Voice terminator src=" + std::to_string(f.source_id) +
                         " dst=" + std::to_string(f.destination_id));
}

void HomebrewClient::inject(const uint8_t* data, size_t len, uint64_t now_ms)
{
    if (captureActive())
        captureWrite("bm_rx", now_ms * 1000000ULL, data, len);
    if (len != 55) {
        logInfo("BM/rx", hexBytes(data, len));
    } else {
        static uint64_t dmrdCount = 0;
        if ((++dmrdCount % 100) == 1)
            logDebug("BM/rx", "DMRD downlink #" + std::to_string(dmrdCount));
    }
    const Packet packet = HomebrewParser::parse(data, len);
    m_last_rx_ms = now_ms;
    handlePacket(packet, now_ms);
}

void HomebrewClient::handlePacket(const Packet& packet, uint64_t now_ms)
{
    switch (packet.type) {
    case PacketType::DmrData:
        if (packet.dmr_data)
            handleDmrData(*packet.dmr_data, now_ms);
        break;

    case PacketType::MasterAck:
        if (m_auth == AuthState::SentLogin) {
            if (packet.has_nonce) {
                m_nonce = packet.nonce;
                m_hasNonce = true;
                sendKey();
            } else {
                logWarn(kTag, "MSTACK without nonce during login");
            }
        } else if (m_auth == AuthState::SentKey) {
            m_auth = AuthState::Done;
            transitionTo(NetworkState::Registered);
            sendConfig();
        } else if (m_auth == AuthState::Done) {
            // Configuration accepted; start the keepalive and mark the session
            // active.
            sendPing(now_ms);
            transitionTo(NetworkState::Active);
            publishNetworkEvent(DMREventType::NetworkConnected, "configuration accepted", now_ms);
        }
        break;

    case PacketType::MasterNak:
        if (m_auth == AuthState::SentLogin)
            logError(kTag, "master refused login (RPTL)");
        else if (m_auth == AuthState::SentKey)
            logError(kTag, "master refused password (RPTK)");
        beginReconnect(now_ms, "master NAK");
        break;

    case PacketType::MasterClose:
        beginReconnect(now_ms, "master closed connection (MSTCL)");
        break;

    case PacketType::RepeaterPong:
        if (m_ping_sent_ms > 0 && now_ms >= m_ping_sent_ms)
            m_latency_ms = static_cast<uint32_t>(now_ms - m_ping_sent_ms);
        if (m_state != NetworkState::Active) {
            transitionTo(NetworkState::Active);
            publishNetworkEvent(DMREventType::NetworkConnected, "BM session active", now_ms);
        }
        publishNetworkEvent(DMREventType::NetworkKeepalive,
                            "pong latency=" + std::to_string(m_latency_ms) + "ms", now_ms);
        break;

    case PacketType::MasterPing:
        sendPing(now_ms);
        break;

    case PacketType::Unknown:
    default:
        logDebug(kTag, "unhandled packet len=" + std::to_string(packet.raw.size()));
        break;
    }
}

void HomebrewClient::handleDmrData(const DMRData& data, uint64_t now_ms)
{
    if (m_auth == AuthState::Done && m_state != NetworkState::Active) {
        transitionTo(NetworkState::Active);
        publishNetworkEvent(DMREventType::NetworkConnected, "BM session active", now_ms);
    }

    const DMRFrame frame = HomebrewParser::toFrame(data, msToNs(now_ms));
    if (m_callManager)
        m_callManager->onFrame(frame);
    if (m_voicePipeline && m_audioEnabled.load() && frame.has_voice &&
        audioAllowed(frame.destination_id))
        m_voicePipeline->push(frame);
    if (m_frameCallback)
        m_frameCallback(frame);
}

void HomebrewClient::clock(uint64_t now_ms)
{
    if (!m_transport)
        return;

    if (m_transport->isOpen()) {
        uint8_t buffer[2048];
        for (int guard = 0; guard < 64; ++guard) {
            const int received = m_transport->recv(buffer, sizeof(buffer));
            if (received <= 0)
                break;
            inject(buffer, static_cast<size_t>(received), now_ms);
        }
    }

    if (m_state == NetworkState::Reconnecting && now_ms >= m_reconnect_at_ms)
        start(now_ms);

    if (m_state == NetworkState::Disconnected)
        return;

    if (m_auth != AuthState::None && now_ms > m_last_rx_ms &&
        (now_ms - m_last_rx_ms) > kNetworkTimeoutMs) {
        beginReconnect(now_ms, "network timeout");
        return;
    }

    // Close an outgoing RF call that has gone quiet (frame gap > 1.5 s) by
    // sending the Voice Terminator, completing the call framing for BM.
    if (m_state == NetworkState::Active) {
        for (int s = 0; s < 2; ++s) {
            if (m_slotActive[s] && now_ms > m_slotLastFrameMs[s] &&
                (now_ms - m_slotLastFrameMs[s]) > 1500) {
                sendTerminator(s);
                m_slotActive[s] = false;
                m_txStreamId[s] = 0;
                m_pendCount[s] = 0;
                m_pendSrc[s] = 0;
                m_pendDst[s] = 0;
                m_pendFlco[s] = 0;
                m_callSrc[s] = 0;
                m_callDst[s] = 0;
                m_callFlco[s] = 0;
            }
        }
    }

    if (m_auth == AuthState::Done && now_ms >= m_last_ping_ms &&
        (now_ms - m_last_ping_ms) >= kKeepAliveIntervalMs) {
        sendPing(now_ms);
    }
}

void HomebrewClient::beginReconnect(uint64_t now_ms, const char* reason)
{
    logWarn(kTag, std::string("reconnecting: ") + reason);
    publishNetworkEvent(DMREventType::NetworkDisconnected, reason, now_ms);
    transitionTo(NetworkState::Reconnecting);
    m_auth = AuthState::None;
    m_hasNonce = false;
    if (m_transport)
        m_transport->close();
    m_reconnect_at_ms = now_ms + kReconnectDelayMs;
}

} // namespace bmrf::brandmeister
