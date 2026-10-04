#include "bmrf/rf/rf_receiver.hpp"

#include "bmrf/audio/voice_pipeline.hpp"
#include "bmrf/capture/capture.hpp"
#include "bmrf/common/log.hpp"
#include "bmrf/common/time.hpp"
#include "bmrf/dmr/call_manager.hpp"
#include "bmrf/dmr/event_bus.hpp"
#include "bmrf/rf/rf_decoder.hpp"
#include "bmrf/rf/rtl_sdr.hpp"

#include <chrono>
#include <cstring>
#include <string>

namespace bmrf::rf {

namespace {
// Keep downlink muted for this long after the last RF voice burst.
constexpr uint64_t kRfDuckHoldMs = 500;
} // namespace

RfReceiver::RfReceiver(std::unique_ptr<IDmrBurstSource> source)
    : m_source(std::move(source))
{
}

RfReceiver::~RfReceiver()
{
    stop();
    close();
}

bool RfReceiver::open(const std::string& spec, std::string& error)
{
    if (!m_source)
        return false;
    return m_source->open(spec, error);
}

void RfReceiver::close()
{
    if (m_source)
        m_source->close();
}

void RfReceiver::start()
{
    // Always restart cleanly: a previously started thread may have exited (for
    // example while the SDR was closed), which would leave m_running set and
    // make a plain exchange(true) a no-op forever.
    if (m_thread.joinable()) {
        m_running.store(false);
        m_thread.join();
    }
    m_running.store(true);
    m_thread = std::thread([this] { run(); });
}

void RfReceiver::stop()
{
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
    m_lastRfActivityMs = 0;
    if (m_pipeline)
        m_pipeline->setRfActive(false);
}

const char* RfReceiver::sourceName() const
{
    return m_source ? m_source->name() : "none";
}

void RfReceiver::run()
{
    logInfo("rf", "receiver thread started");
    uint32_t sinceLog = 0;
    uint64_t lastDecoded = 0;
    while (m_running.load()) {
#if defined(BMRF_USE_DSDCC)
        if (m_dsdccEnabled.load() && m_source) {
            const RtlSdrStatus st = rtlSdrStatus();
            const uint32_t rate = st.sample_rate_hz >= 100000 ? st.sample_rate_hz : 240000u;
            if (!m_dsdcc || rate != m_dsdRate) {
                // The app tunes the SDR centre below the carrier to dodge the
                // RTL-SDR DC spike; the DSDcc front end removes that offset.
                const double offset = rate <= 300000 ? 40000.0 : 100000.0;
                m_dsdcc = std::make_unique<DsdccDmrDemod>(rate, offset);
                m_dsdRate = rate;
                m_iqBuf.assign(32768u * 2u, 0);
            }
            const size_t n = m_source->readIq(m_iqBuf.data(), 32768u);
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            m_dsdPcm.clear();
            m_dsdcc->process(m_iqBuf.data(), n, m_dsdPcm);
            m_levelDb.store(static_cast<float>(m_dsdcc->channelLevelDb()));
            m_snrDb.store(static_cast<float>(m_dsdcc->channelSnrDb()));

            // Duck downlink (network) audio while an RF uplink is being received,
            // releasing it after a short hold so brief inter-burst gaps do not
            // flap the mute. RF PCM itself is still played.
            if (m_pipeline) {
                if (!m_dsdPcm.empty()) {
                    m_lastRfActivityMs = nowMs();
                    m_pipeline->setRfActive(true);
                } else if (m_lastRfActivityMs != 0 &&
                           (nowMs() - m_lastRfActivityMs) > kRfDuckHoldMs) {
                    m_lastRfActivityMs = 0;
                    m_pipeline->setRfActive(false);
                }
            }
            if (!m_dsdPcm.empty() && m_audioEnabled.load() && m_pipeline)
                m_pipeline->pushPcm(m_dsdPcm.data(), m_dsdPcm.size());

            // Bridge the decoded RF voice into the unified DMR model so RF calls
            // reach the CallManager / EventBus / UI and the BM uplink (design doc
            // §4.1 / §6 / §10). One 33-byte Homebrew payload == one DMR burst.
            uint8_t payload[33];
            int seq = 0;
            while (m_dsdcc->takePayload(payload, &seq)) {
                DMRFrame frame;
                frame.source = DMRFrame::Source::Rf;
                frame.timestamp_ns = nowNs();
                frame.timeslot = m_dsdcc->timeslot();
                frame.color_code = m_dsdcc->colorCode();
                frame.source_id = m_dsdcc->sourceId();
                frame.destination_id = m_dsdcc->destinationId();

                // Skip bursts whose Link Control has not been recovered yet (the
                // first burst or two carry no addressing). Forwarding them would
                // create a bogus src=0/dst=0 call and a useless Voice LC header.
                if (frame.source_id == 0 || frame.destination_id == 0)
                    continue;

                frame.call_type =
                    m_dsdcc->groupCall() ? CallType::Group : CallType::Private;

                // Recover the Link Control from the RF embedded signalling so the
                // unified model carries FLCO / service options / Voice LC.
                VoiceLC lc;
                lc.flco = m_dsdcc->flco();
                lc.protect = !m_dsdcc->groupCall();
                lc.service_options = m_dsdcc->serviceOptions();
                lc.source_id = frame.source_id;
                lc.destination_id = frame.destination_id;
                frame.voice_lc = lc;
                frame.flco = lc.flco;
                frame.service_options = lc.service_options;

                frame.has_voice = true;
                frame.data_type = DataType::Voice;
                frame.sequence = static_cast<uint32_t>(seq);
                std::memcpy(frame.ambe.data(), payload, frame.ambe.size());

                // BER proxy: share of the corrected AMBE bits in the last decoded
                // frame (mbelib ECC count over the 72-bit 3600x2450 word).
                {
                    float ber = static_cast<float>(m_dsdcc->mbeErrors()) * (100.0f / 72.0f);
                    if (ber > 100.0f)
                        ber = 100.0f;
                    frame.ber_percent = ber;
                }
                // RF local measurements. RSSI is relative to full scale (dBFS);
                // without SDR gain calibration it is not an absolute dBm value.
                frame.rssi_dbm = static_cast<float>(m_dsdcc->channelLevelDb());
                frame.snr_db = static_cast<float>(m_dsdcc->channelSnrDb());

                if (captureActive()) {
                    static const char* hexd = "0123456789abcdef";
                    std::string ambeHex;
                    ambeHex.reserve(frame.ambe.size() * 2);
                    for (uint8_t b : frame.ambe) {
                        ambeHex.push_back(hexd[(b >> 4) & 0xF]);
                        ambeHex.push_back(hexd[b & 0xF]);
                    }
                    captureWriteText(
                        "rf_frame", frame.timestamp_ns,
                        "slot=" + std::to_string(frame.timeslot) +
                            " src=" + std::to_string(frame.source_id) +
                            " dst=" + std::to_string(frame.destination_id) +
                            " ct=" + std::to_string(static_cast<int>(frame.call_type)) +
                            " dt=" + std::to_string(static_cast<int>(frame.data_type)) +
                            " seq=" + std::to_string(frame.sequence) +
                            " flco=" + std::to_string(frame.flco) +
                            " so=" + std::to_string(frame.service_options) +
                            " ber=" + std::to_string(frame.ber_percent.value_or(0.0f)) +
                            " ambe=" + ambeHex);
                }

                if (m_callManager)
                    m_callManager->onFrame(frame);
                if (m_callback)
                    m_callback(frame);

                // Strict uplink gate: only forward to BrandMeister once the RF
                // embedded Link Control has passed FEC, so the synthesised Voice
                // LC Header is built from a trustworthy LC. Track the success rate.
                const bool lcOk = m_dsdcc->addressesValid();
                static uint64_t lcTotal = 0;
                static uint64_t lcValid = 0;
                ++lcTotal;
                if (lcOk)
                    ++lcValid;
                if ((lcTotal % 200) == 0) {
                    logInfo("rf", "LC valid=" + std::to_string(lcValid) + "/" +
                                      std::to_string(lcTotal) + " (" +
                                      std::to_string(lcTotal ? (lcValid * 100 / lcTotal) : 0) +
                                      "%)");
                }
                if (lcOk && m_forwardEnabled.load() && m_uplink)
                    m_uplink(frame);

                static uint64_t dsdFrames = 0;
                if ((++dsdFrames % 40) == 0) {
                    const CallManagerStats cm = m_callManager ? m_callManager->stats()
                                                              : CallManagerStats{};
                    logInfo("rf", "rf-frame seq=" + std::to_string(seq) +
                                      " slot=" + std::to_string(frame.timeslot) +
                                      " src=" + std::to_string(frame.source_id) +
                                      " dst=" + std::to_string(frame.destination_id) +
                                      " frames=" + std::to_string(cm.total_frames) +
                                      " lost=" + std::to_string(cm.total_lost));
                }
            }

            static uint64_t dsdBlocks = 0;
            if ((++dsdBlocks % 300) == 0)
                logInfo("rf", "dsdcc: " + m_dsdcc->statusText());
            continue;
        }
#endif
        DMRBurst burst;
        if (!m_source || !m_source->read(burst)) {
            // Never exit on a transient closed/absent source: the SDR may be
            // (re)opened later, so keep polling until stop() is requested.
            if (++sinceLog >= 400) {
                sinceLog = 0;
                const bmrf::rf::RtlSdrStatus st = bmrf::rf::rtlSdrStatus();
                logInfo("rf", "alive: decoded=" + std::to_string(m_decoded.load()) +
                                  " rms=" + std::to_string(st.rms) + " samples=" +
                                  std::to_string(st.samples_read));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (burst.sync == SyncType::None)
            burst.sync = classifySync(burst.dibits.data() + kDmrSyncOffset);
        if (burst.timestamp_ns == 0)
            burst.timestamp_ns = nowNs();

        DMRFrame frame;
        if (!RfDecoder::decode(burst, frame))
            continue;

        m_decoded.fetch_add(1);
        if (m_callManager)
            m_callManager->onFrame(frame);

        const int slotFilter = m_timeslot.load();
        const bool slotMatch = (slotFilter == 0) || (frame.timeslot == slotFilter);

        if (slotMatch && frame.has_voice && m_audioEnabled.load() && m_pipeline)
            m_pipeline->push(frame);

        if (slotMatch && m_forwardEnabled.load() && m_uplink) {
            if (m_uplink(frame))
                m_forwarded.fetch_add(1);
        }

        if (m_callback)
            m_callback(frame);

        if (m_decoded.load() != lastDecoded) {
            lastDecoded = m_decoded.load();
            sinceLog = 0;
            logInfo("rf", "decoded=" + std::to_string(lastDecoded) + " bursts rms=" +
                              std::to_string(bmrf::rf::rtlSdrStatus().rms));
        }
    }
    logInfo("rf", "receiver thread stopped");
}

} // namespace bmrf::rf
