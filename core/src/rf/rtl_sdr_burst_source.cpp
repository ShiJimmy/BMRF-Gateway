#include "bmrf/rf/rtl_sdr_burst_source.hpp"

#if defined(BMRF_HAVE_RTLSDR)

#include "bmrf/common/log.hpp"
#include "bmrf/rf/dmr_phy.hpp"
#include "bmrf/rf/rtl_sdr.hpp"

// Real DMR is RRC pulse-shaped; the block-aligned demod now carries its own RRC
// matched filter (the rectangular integrator suffered inner-symbol ISI).
#include "bmrf/rf/aligned_demod.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace bmrf::rf {

namespace {

std::atomic<bool> g_captureOnly{false};

constexpr size_t kReadSamples = 32768;
// The app sets the SDR centre 100 kHz below the DMR carrier, so the signal sits
// at +100 kHz in baseband; the demod removes it before decimation.
constexpr double kBasebandOffsetHz = 100000.0;
// Channel-empty squelch threshold (IQ RMS). Noise floor is ~7, a local DMR
// signal is well above 20.
constexpr double kSquelchRms = 20.0;

class RtlSdrBurstSource : public IDmrBurstSource {
public:
    bool open(const std::string&, std::string&) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }

    bool read(DMRBurst& out) override
    {
        if (!m_readLogged) {
            m_readLogged = true;
            logInfo("rtlsdr", std::string("burst source read: open=") +
                                  (m_open ? "1" : "0") + " rtl=" +
                                  (rtlSdrIsOpen() ? "1" : "0"));
        }
        if (!m_open || !rtlSdrIsOpen())
            return false;

        if (!m_bursts.empty())
            return pop(out);

        ensureConfig();
        if (!m_demod)
            return false;

        const int samples = rtlSdrRead(m_iq.data(), kReadSamples);
        if (samples <= 0)
            return false;

        // Capture-only: the recording already happened inside rtlSdrRead.
        if (g_captureOnly.load())
            return false;

        // Squelch: when the channel is empty, still feed the demodulator so its
        // symbol-timing state stays continuous across the gap (dropping blocks
        // would shift the timing); just force the frame synchroniser to
        // re-acquire after a sustained quiet period so noise does not produce
        // false bursts.
        {
            const size_t bytes = static_cast<size_t>(samples) * 2;
            double acc = 0.0;
            for (size_t i = 0; i < bytes; ++i) {
                const double v = static_cast<double>(m_iq[i]) - 127.5;
                acc += v * v;
            }
            m_rms = std::sqrt(acc / static_cast<double>(bytes));
        }

        m_dibits.clear();
        m_demod->process(m_iq.data(), static_cast<size_t>(samples), m_dibits);
        if (!m_dibits.empty())
            m_phy.feed(m_dibits.data(), m_dibits.size(), m_bursts);

        if (m_rms < kSquelchRms) {
            if (++m_quietBlocks == 8)
                m_phy.reset();
        } else {
            m_quietBlocks = 0;
        }

        if (!m_logged && !m_bursts.empty()) {
            m_logged = true;
            logInfo("rtlsdr", "first burst after " + std::to_string(m_rate) + " Hz demod");
        }

        if (m_bursts.empty())
            return false;
        return pop(out);
    }

    // Raw IQ read for the DSDcc decode path (bypasses the built-in demod).
    size_t readIq(uint8_t* iq, size_t maxSamples) override
    {
        if (!m_open || !rtlSdrIsOpen())
            return 0;
        const int n = rtlSdrRead(iq, maxSamples);
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    const char* name() const override { return "rtl-sdr/liquid"; }

private:
    bool pop(DMRBurst& out)
    {
        out = m_bursts.front();
        m_bursts.erase(m_bursts.begin());
        return true;
    }

    void ensureConfig()
    {
        const RtlSdrStatus status = rtlSdrStatus();
        const uint32_t rate = status.sample_rate_hz >= 100000 ? status.sample_rate_hz : 240000;
        if (m_demod == nullptr || rate != m_rate) {
            m_rate = rate;
            AlignedDemodConfig config;
            config.sample_rate = m_rate;
            config.symbol_rate = 4800;
            m_demod.reset(new AlignedDmrDemod(config));
            m_phy.reset();
            m_quietBlocks = 0;
            m_logged = false;
        }
        (void)kBasebandOffsetHz;
    }

    bool m_open = false;
    bool m_logged = false;
    bool m_readLogged = false;
    uint32_t m_rate = 0;
    double m_rms = 0.0;
    int m_quietBlocks = 0;

    std::unique_ptr<AlignedDmrDemod> m_demod;

    DmrPhy m_phy;
    std::vector<uint8_t> m_iq = std::vector<uint8_t>(kReadSamples * 2);
    std::vector<uint8_t> m_dibits;
    std::vector<DMRBurst> m_bursts;
};

} // namespace

std::unique_ptr<IDmrBurstSource> createRtlSdrBurstSource()
{
    return std::make_unique<RtlSdrBurstSource>();
}

std::unique_ptr<IDmrBurstSource> createDefaultRfBurstSource()
{
    return createRtlSdrBurstSource();
}

void rtlSdrBurstSourceSetCaptureOnly(bool captureOnly)
{
    g_captureOnly.store(captureOnly);
}

} // namespace bmrf::rf

#else

namespace bmrf::rf {

std::unique_ptr<IDmrBurstSource> createRtlSdrBurstSource()
{
    return createNullBurstSource();
}

std::unique_ptr<IDmrBurstSource> createDefaultRfBurstSource()
{
    return createNullBurstSource();
}

void rtlSdrBurstSourceSetCaptureOnly(bool)
{
}

} // namespace bmrf::rf

#endif
