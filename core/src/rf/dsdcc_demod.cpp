#include "bmrf/rf/dsdcc_demod.hpp"

#if defined(BMRF_USE_DSDCC)

#include "bmrf/common/log.hpp"

// <complex> must precede <liquid.h> so liquid selects std::complex<float>.
#include <complex>
#include <liquid.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "dsd_decoder.h"

namespace bmrf::rf {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
constexpr uint32_t kChannelRate = 48000; // DSDcc requires exactly 48 kS/s
constexpr double kDiscriGain = 32767.0 / 3.14159265358979323846;

// 27 AMBE bytes -> 33-byte Homebrew DMR payload (matches RfDecoder).
void buildHomebrewPayload(const uint8_t ambe27[27], uint8_t payload[33])
{
    std::memset(payload, 0, 33);
    std::memcpy(payload, ambe27, 13);
    payload[13] = static_cast<uint8_t>(ambe27[13] & 0xF0);
    payload[19] = static_cast<uint8_t>(ambe27[13] & 0x0F);
    std::memcpy(payload + 20, ambe27 + 14, 13);
}
} // namespace

struct DsdccDmrDemod::Impl {
    uint32_t sample_rate = 240000;
    double freq_offset_hz = 40000.0;
    unsigned decim = 5;

    nco_crcf nco = nullptr;
    firdecim_crcf dec = nullptr;

    std::vector<liquid_float_complex> carry; // partial decimation group
    std::vector<liquid_float_complex> decimOut;

    int prevI = 0;
    int prevQ = 0;
    bool havePrev = false;

    DSDcc::DSDDecoder decoder;
    int audio_rate = 8000;
    double discri_gain = kDiscriGain * 4.0; // best-measured scale (BMRF_DSD_GAIN overrides)

    // Power squelch: DSDcc false-locks on noise if fed it, producing idle hiss.
    // When the channel power is below the level we feed zeros so DSDcc hits its
    // internal squelch and stops decoding.
    double magsq_avg = 0.0;
    double noise_floor = -1.0;
    double squelch_factor = 8.0;
    int sq_hang = 0; // hold time after the power drops (samples @ 48k)
    bool squelch_open = false;
    double pcm_gain = 4.0; // extra software volume on the DSDcc output
    int last_voice_frames = 0;

    // Raw AMBE frames for the RF -> BrandMeister uplink (design doc §6): DSDcc
    // produces 3 x 9-byte frames per burst; we reassemble the 33-byte Homebrew
    // payload (27 AMBE bytes + embedded signalling) without re-encoding.
    std::array<uint8_t, 27> ambe_accum[2] = {};
    int ambe_count[2] = {0, 0};
    // Completed bursts are queued: a single SDR read (e.g. 32768 samples at
    // 240 kS/s = ~136 ms) can contain several DMR bursts, so a one-deep buffer
    // would silently drop all but the last. Losing 3 of every 4 voice frames is
    // what made BrandMeister registration unreliable.
    struct QueuedPayload {
        std::array<uint8_t, 33> data{};
        int seq = 0;
    };
    std::deque<QueuedPayload> payloadQueue[2];
    int payload_seq[2] = {0, 0}; // Homebrew voice number N, [0..5] per burst
};

DsdccDmrDemod::DsdccDmrDemod(uint32_t sample_rate, double freq_offset_hz)
    : m_impl(new Impl())
{
    Impl& d = *m_impl;
    d.sample_rate = sample_rate;
    d.freq_offset_hz = freq_offset_hz;
    d.decim = (sample_rate >= kChannelRate) ? (sample_rate / kChannelRate) : 1;
    if (d.decim == 0)
        d.decim = 1;

    d.nco = nco_crcf_create(LIQUID_NCO);
    nco_crcf_set_frequency(d.nco,
                           static_cast<float>(kTwoPi * freq_offset_hz / sample_rate));

    // Band-limit to the DMR channel (~6 kHz) before decimating, as SDRangel does
    // (its interpolation filter cut-off is rfBandwidth/2.2). Decimating only to
    // the 48 kHz Nyquist passes ~4x more noise into the FM discriminator, which
    // degrades the symbol decisions (audible distortion).
    {
        const unsigned M = d.decim;
        const unsigned hLen = 2u * M * 8u + 1u;
        std::vector<float> h(hLen);
        const float fc = 6000.0f / static_cast<float>(sample_rate);
        liquid_firdes_kaiser(hLen, fc, 60.0f, 0.0f, h.data());
        double sum = 0.0;
        for (float v : h)
            sum += v;
        for (float& v : h)
            v = static_cast<float>(v / sum); // unity DC gain
        d.dec = firdecim_crcf_create(M, h.data(), hLen);
    }
    d.carry.reserve(d.decim * 4u);

    d.decoder.setQuiet();
    d.decoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeNone, true); // disable all
    d.decoder.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeDMR, true);  // DMR only
    d.decoder.setDataRate(DSDcc::DSDDecoder::DSDRate4800);
    d.decoder.setMbeRate(DSDcc::DSDDecoder::DSDMBERate3600x2450);
    d.decoder.enableMbelib(true);
    const bool noCos = std::getenv("BMRF_DSD_NOCOS") != nullptr;
    d.decoder.enableCosineFiltering(!noCos);
    d.decoder.enableAudioOut(true);
    d.decoder.setUpsampling(1);
    d.decoder.setStereo(false);
    d.decoder.setAudioGain(3.0f); // DSDcc output is quiet by default
    d.audio_rate = 8000 * (d.decoder.upsampling() > 0 ? d.decoder.upsampling() : 1);
    if (const char* g = std::getenv("BMRF_DSD_GAIN"))
        d.discri_gain = kDiscriGain * std::atof(g);
    if (const char* sq = std::getenv("BMRF_DSD_SQ"))
        d.squelch_factor = std::atof(sq);

    logInfo("dsdcc", "DMR decoder: fs=" + std::to_string(sample_rate) + " decim=" +
                         std::to_string(d.decim) + " offset=" + std::to_string(freq_offset_hz) +
                         " audio=" + std::to_string(d.audio_rate));
}

DsdccDmrDemod::~DsdccDmrDemod()
{
    if (m_impl == nullptr)
        return;
    if (m_impl->nco)
        nco_crcf_destroy(m_impl->nco);
    if (m_impl->dec)
        firdecim_crcf_destroy(m_impl->dec);
    delete m_impl;
}

void DsdccDmrDemod::reset()
{
    if (m_impl == nullptr)
        return;
    Impl& d = *m_impl;
    if (d.dec)
        firdecim_crcf_reset(d.dec);
    d.carry.clear();
    d.decimOut.clear();
    d.prevI = d.prevQ = 0;
    d.havePrev = false;
}

int DsdccDmrDemod::audioSampleRate() const
{
    return m_impl ? m_impl->audio_rate : 8000;
}

void DsdccDmrDemod::process(const uint8_t* iq, size_t samples, std::vector<int16_t>& pcm)
{
    Impl& d = *m_impl;
    if (samples == 0)
        return;

    // Convert + NCO-shift and append to the carry buffer so decimation groups
    // are never split across calls.
    const size_t base = d.carry.size();
    d.carry.resize(base + samples);
    for (size_t i = 0; i < samples; ++i) {
        const float re = (static_cast<float>(iq[2 * i]) - 127.5f) / 128.0f;
        const float im = (static_cast<float>(iq[2 * i + 1]) - 127.5f) / 128.0f;
        d.carry[base + i] = liquid_float_complex(re, im);
    }
    nco_crcf_mix_block_down(d.nco, d.carry.data() + base, d.carry.data() + base,
                            static_cast<unsigned>(samples));

    const size_t n = (d.carry.size() / d.decim) * d.decim;
    if (n > 0) {
        const size_t outN = n / d.decim;
        d.decimOut.resize(outN);
        firdecim_crcf_execute_block(d.dec, d.carry.data(), static_cast<unsigned>(outN),
                                    d.decimOut.data());
        d.carry.erase(d.carry.begin(), d.carry.begin() + static_cast<long>(n));

        for (size_t i = 0; i < outN; ++i) {
            const liquid_float_complex c = d.decimOut[i];
            // Relative squelch: track the channel noise floor (gain-independent)
            // and open only when the power is well above it. An absolute
            // threshold fails because the RTL AGC scales the noise too.
            const double magsq = std::norm(c);
            d.magsq_avg += (magsq - d.magsq_avg) * 0.002;
            if (d.noise_floor < 0.0)
                d.noise_floor = d.magsq_avg;
            else if (d.magsq_avg < d.noise_floor)
                d.noise_floor = d.magsq_avg;
            else
                d.noise_floor += (d.magsq_avg - d.noise_floor) * 2e-6;
            // Hang: once the power crosses the threshold, hold the squelch open
            // for a while so the gaps between DMR bursts do not chop the audio
            // (which sounds like loud clicks and destroys intelligibility).
            if (d.magsq_avg > d.noise_floor * d.squelch_factor + 1e-6)
                d.sq_hang = 12000; // 250 ms @ 48k
            else if (d.sq_hang > 0)
                --d.sq_hang;
            const bool open = d.sq_hang > 0;
            d.squelch_open = open;

            const int si = static_cast<int>(std::lround(std::real(c) * 1000.0f));
            const int sq = static_cast<int>(std::lround(std::imag(c) * 1000.0f));
            if (d.havePrev) {
                const double cross = static_cast<double>(d.prevI) * sq -
                                     static_cast<double>(d.prevQ) * si;
                const double dot = static_cast<double>(si) * d.prevI +
                                   static_cast<double>(sq) * d.prevQ;
                const double val = std::atan2(cross, dot);
                int s = static_cast<int>(std::lround(val * d.discri_gain));
                if (s > 32767)
                    s = 32767;
                else if (s < -32768)
                    s = -32768;
                // Feed zeros while squelched so DSDcc's own squelch stops it
                // false-locking on noise (which plays as idle hiss).
                d.decoder.run(open ? static_cast<short>(s) : static_cast<short>(0));

                // Capture raw AMBE frames for the RF -> BM uplink.
                for (int slot = 0; slot < 2; ++slot) {
                    const bool ready =
                        (slot == 0) ? d.decoder.mbeDVReady1() : d.decoder.mbeDVReady2();
                    if (!ready)
                        continue;
                    const unsigned char* f =
                        (slot == 0) ? d.decoder.getMbeDVFrame1() : d.decoder.getMbeDVFrame2();
                    std::memcpy(d.ambe_accum[slot].data() + 9 * d.ambe_count[slot], f, 9);
                    if (++d.ambe_count[slot] == 3) {
                        Impl::QueuedPayload q;
                        buildHomebrewPayload(d.ambe_accum[slot].data(), q.data.data());
                        q.seq = (d.payload_seq[slot] + 1) % 6;
                        d.payload_seq[slot] = q.seq;
                        if (d.payloadQueue[slot].size() < 64)
                            d.payloadQueue[slot].push_back(q);
                        d.ambe_count[slot] = 0;
                    }
                    if (slot == 0)
                        d.decoder.resetMbeDV1();
                    else
                        d.decoder.resetMbeDV2();
                }
            }
            d.prevI = si;
            d.prevQ = sq;
            d.havePrev = true;
        }
    }

    // Drain decoded PCM from both TDMA slots while the (hang-extended) squelch
    // is open, so a whole voice frame is never chopped in half.
    const bool open = d.squelch_open;
    auto append = [&](short* src, int n) {
        const size_t base = pcm.size();
        pcm.resize(base + static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            int v = static_cast<int>(std::lround(src[i] * d.pcm_gain));
            if (v > 32767)
                v = 32767;
            else if (v < -32768)
                v = -32768;
            pcm[base + static_cast<size_t>(i)] = static_cast<int16_t>(v);
        }
    };

    int nb1 = 0;
    short* a1 = d.decoder.getAudio1(nb1);
    double rms1 = 0.0;
    if (a1 && nb1 > 0) {
        double acc = 0.0;
        for (int i = 0; i < nb1; ++i)
            acc += static_cast<double>(a1[i]) * a1[i];
        rms1 = std::sqrt(acc / nb1);
        if (open)
            append(a1, nb1);
    }
    d.decoder.resetAudio1();

    int nb2 = 0;
    short* a2 = d.decoder.getAudio2(nb2);
    if (a2 && nb2 > 0 && open)
        append(a2, nb2);
    d.decoder.resetAudio2();
    d.last_voice_frames = (nb1 + nb2) / 160; // one AMBE frame == 160 samples @ 8k

    {
        static uint64_t diagCount = 0;
        if ((++diagCount % 200) == 0) {
            logInfo("dsdcc", "diag: sync=" + std::to_string(static_cast<int>(d.decoder.getSyncType())) +
                                 " v1=" + std::to_string(d.decoder.getVoice1On() ? 1 : 0) +
                                 " v2=" + std::to_string(d.decoder.getVoice2On() ? 1 : 0) +
                                 " sq=" + std::to_string(open ? 1 : 0) + " a1=" + std::to_string(nb1) +
                                 " rms1=" + std::to_string(static_cast<int>(rms1)) +
                                 " a2=" + std::to_string(nb2) + " magsq=" +
                                 std::to_string(static_cast<int>(d.magsq_avg)) + " nf=" +
                                 std::to_string(static_cast<int>(d.noise_floor)));
        }
    }
}

std::string DsdccDmrDemod::statusText()
{
    if (m_impl == nullptr)
        return {};
    char buf[160] = {0};
    m_impl->decoder.formatStatusText(buf);
    return std::string(buf);
}

int DsdccDmrDemod::inLevel() const
{
    return m_impl ? m_impl->decoder.getInLevel() : 0;
}

int DsdccDmrDemod::symbolSyncQuality() const
{
    return m_impl ? m_impl->decoder.getSymbolSyncQuality() : 0;
}

double DsdccDmrDemod::channelLevelDb() const
{
    if (m_impl == nullptr || m_impl->magsq_avg <= 0.0)
        return 0.0;
    // magsq is IQ power referred to unit full scale; 10*log10 gives dBFS.
    return 10.0 * std::log10(m_impl->magsq_avg);
}

double DsdccDmrDemod::channelSnrDb() const
{
    if (m_impl == nullptr || m_impl->magsq_avg <= 0.0 || m_impl->noise_floor <= 0.0)
        return 0.0;
    const double snr = 10.0 * std::log10(m_impl->magsq_avg / m_impl->noise_floor);
    return snr > 0.0 ? snr : 0.0;
}

int DsdccDmrDemod::timeslot() const
{
    if (m_impl == nullptr)
        return 0;
    if (m_impl->decoder.getVoice1On())
        return 1;
    if (m_impl->decoder.getVoice2On())
        return 2;
    return 0;
}

int DsdccDmrDemod::mbeErrors() const
{
    if (m_impl == nullptr)
        return 0;
    return m_impl->decoder.mbeErrs(timeslot());
}

uint32_t DsdccDmrDemod::sourceId() const
{
    if (m_impl == nullptr)
        return 0;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    return static_cast<uint32_t>(m_impl->decoder.getDMRDecoder().getSlotSource(s));
}

uint32_t DsdccDmrDemod::destinationId() const
{
    if (m_impl == nullptr)
        return 0;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    return static_cast<uint32_t>(m_impl->decoder.getDMRDecoder().getSlotTarget(s));
}

bool DsdccDmrDemod::groupCall() const
{
    if (m_impl == nullptr)
        return false;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    return m_impl->decoder.getDMRDecoder().getSlotGroup(s);
}

uint8_t DsdccDmrDemod::flco() const
{
    if (m_impl == nullptr)
        return 0;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    return m_impl->decoder.getDMRDecoder().getSlotFlco(s);
}

uint8_t DsdccDmrDemod::serviceOptions() const
{
    if (m_impl == nullptr)
        return 0;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    return m_impl->decoder.getDMRDecoder().getSlotServiceOptions(s);
}

bool DsdccDmrDemod::addressesValid() const
{
    if (m_impl == nullptr)
        return false;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    return m_impl->decoder.getDMRDecoder().getSlotAddressesValid(s);
}

uint64_t DsdccDmrDemod::lcDecodeVersion() const
{
    if (m_impl == nullptr)
        return 0;
    auto& dmr = m_impl->decoder.getDMRDecoder();
    return static_cast<uint64_t>(dmr.getSlotLcDecodes(0)) +
           static_cast<uint64_t>(dmr.getSlotLcDecodes(1));
}

int DsdccDmrDemod::colorCode() const
{
    return m_impl ? static_cast<int>(m_impl->decoder.getDMRDecoder().getColorCode()) : -1;
}

int DsdccDmrDemod::voiceFrameIndex() const
{
    if (m_impl == nullptr)
        return 0;
    const unsigned s = (timeslot() == 2) ? 1u : 0u;
    const unsigned idx = m_impl->decoder.getDMRDecoder().getVoiceFrameIndex(s);
    return (idx < 6u) ? static_cast<int>(idx) : 0;
}

int DsdccDmrDemod::lastVoiceFrames() const
{
    return m_impl ? m_impl->last_voice_frames : 0;
}

bool DsdccDmrDemod::takePayload(uint8_t out[33], int* sequence)
{
    if (m_impl == nullptr || out == nullptr)
        return false;
    Impl& d = *m_impl;
    for (int slot = 0; slot < 2; ++slot) {
        if (d.payloadQueue[slot].empty())
            continue;
        const Impl::QueuedPayload& q = d.payloadQueue[slot].front();
        std::memcpy(out, q.data.data(), 33);
        if (sequence != nullptr)
            *sequence = q.seq;
        d.payloadQueue[slot].pop_front();
        return true;
    }
    return false;
}

} // namespace bmrf::rf

#endif // BMRF_USE_DSDCC
