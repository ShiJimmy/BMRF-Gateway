#include "bmrf/rf/liquid_demod.hpp"

#if defined(BMRF_HAVE_LIQUID)

#include "bmrf/common/log.hpp"

// <complex> must precede <liquid.h> so that liquid selects std::complex<float>
// for its complex type in C++ translation units.
#include <complex>
#include <liquid.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace bmrf::rf {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kPi = kTwoPi / 2.0;
constexpr double kDcAlpha = 0.001;    // residual carrier / DC-blocker coefficient
constexpr double kEnvAlpha = 0.001;   // envelope tracker decay
constexpr float kRolloff = 0.2f;      // DMR RRC beta
} // namespace

// DMR 4FSK receiver modelled on the verified TETRA chain, with a Gardner
// symbol-timing PLL in place of liquid's symsync (whose internal rate loop does
// not track here). The real RTL-SDR sample clock is not exactly the requested
// rate (240 kHz -> ~234 kHz), i.e. the true symbol period is ~4.885 samples at
// 24 kHz instead of 5, so a fixed-rate demod drifts continuously. The PLL
// tracks both the phase and the rate.
struct LiquidDmrDemod::Impl {
    uint32_t sample_rate = 240000;
    uint32_t symbol_rate = 4800;
    int sps = 5;
    unsigned factor = 10;
    unsigned channel_rate = 24000;

    int level_to_dibit[4] = {3, 2, 0, 1};

    nco_crcf nco = nullptr;
    firdecim_crcf decim = nullptr;
    freqdem dem = nullptr;

    // Feedforward AGC
    static constexpr unsigned kAgcWindow = 8;
    float agc_pwr[kAgcWindow] = {0};
    unsigned agc_idx = 0;
    float agc_sum = 0.0f;

    // DC blocker
    double dc = 0.0;
    bool have_dc = false;

    // RRC matched filter (real, at the channel rate).
    firfilt_rrrf rrc = nullptr;

    // Gardner timing PLL state.
    double T = 5.0;       // nominal symbol period [channel samples]
    double pos = 0.0;     // fractional read position within ybuf
    double freq_err = 0.0; // accumulated rate error [samples/symbol]
    double kp = 0.10;
    double ki = 0.0100;
    bool have_prev = false;
    bool initialized = false;

    // RRC-filtered signal history (carried across process() calls).
    std::vector<float> ybuf;

    // Envelope-adaptive 4-level slicer.
    double max_val = 1e-3;
    double min_val = -1e-3;

    // Partial decimation input carry and scratch.
    std::vector<liquid_float_complex> pending;
    std::vector<liquid_float_complex> decim_buf;
    std::vector<float> rrc_in;
    std::vector<float> rrc_out;

    double interp(double x) const
    {
        if (x <= 0.0)
            return ybuf.empty() ? 0.0 : static_cast<double>(ybuf[0]);
        const size_t i = static_cast<size_t>(x);
        if (i + 1 >= ybuf.size())
            return ybuf.empty() ? 0.0 : static_cast<double>(ybuf.back());
        const double f = x - static_cast<double>(i);
        return static_cast<double>(ybuf[i]) +
               (static_cast<double>(ybuf[i + 1]) - static_cast<double>(ybuf[i])) * f;
    }

    void sliceInto(double v, std::vector<uint8_t>& out)
    {
        if (v > max_val)
            max_val = v;
        else
            max_val += (v - max_val) * kEnvAlpha;
        if (v < min_val)
            min_val = v;
        else
            min_val += (v - min_val) * kEnvAlpha;

        const double center = (max_val + min_val) * 0.5;
        const double half = (max_val - min_val) * 0.5;
        const double thr = half * 2.0 / 3.0;

        int level;
        if (v >= center + thr)
            level = 3;
        else if (v >= center)
            level = 2;
        else if (v >= center - thr)
            level = 1;
        else
            level = 0;

        out.push_back(static_cast<uint8_t>(level_to_dibit[level]));
    }
};

namespace {

// Root-raised-cosine taps for the discriminator-domain matched filter.
std::vector<float> makeRrc(unsigned sps, double beta)
{
    const int half = 8;
    const size_t taps = 2 * static_cast<size_t>(half) * sps + 1;
    std::vector<float> h(taps, 0.0f);
    double sum = 0.0;
    for (size_t n = 0; n < taps; ++n) {
        const double t = (static_cast<double>(n) - static_cast<double>(taps / 2)) /
                         static_cast<double>(sps);
        double v;
        if (std::fabs(t) < 1e-9)
            v = 1.0 - beta + 4.0 * beta / kPi;
        else
            v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) /
                (kPi * t * (1 - std::pow(4 * beta * t, 2)));
        h[n] = static_cast<float>(v);
        sum += v;
    }
    for (auto& v : h)
        v = static_cast<float>(v / sum);
    return h;
}

} // namespace

LiquidDmrDemod::LiquidDmrDemod(uint32_t sample_rate, uint32_t symbol_rate,
                               double freq_offset_hz, int samples_per_symbol,
                               std::array<int, 4> level_to_dibit)
    : m_impl(new Impl())
{
    Impl& d = *m_impl;
    for (int i = 0; i < 4; ++i)
        d.level_to_dibit[i] = level_to_dibit[static_cast<size_t>(i)];
    d.sample_rate = sample_rate;
    d.symbol_rate = symbol_rate;
    d.sps = samples_per_symbol > 0 ? samples_per_symbol : 5;
    d.channel_rate = symbol_rate * static_cast<uint32_t>(d.sps);
    d.factor = (d.channel_rate != 0) ? (sample_rate / d.channel_rate) : 1;
    if (d.factor == 0)
        d.factor = 1;

    d.decim = firdecim_crcf_create_kaiser(d.factor, 12u, 60.0f);

    d.nco = nco_crcf_create(LIQUID_NCO);
    const double omega = kTwoPi * freq_offset_hz / static_cast<double>(sample_rate);
    nco_crcf_set_frequency(d.nco, static_cast<float>(omega));

    d.dem = freqdem_create(1.0f);

    std::vector<float> rrcTaps = makeRrc(static_cast<unsigned>(d.sps), kRolloff);
    d.rrc = firfilt_rrrf_create(rrcTaps.data(), static_cast<unsigned>(rrcTaps.size()));

    d.T = static_cast<double>(d.sps);
    d.pos = d.T;                 // keep T samples of look-back available
    d.ybuf.assign(d.sps, 0.0f);  // pre-roll so interp(pos - T) is valid

    const char* kp = std::getenv("BMRF_DEMOD_KP");
    const char* ki = std::getenv("BMRF_DEMOD_KI");
    if (kp)
        d.kp = std::atof(kp);
    if (ki)
        d.ki = std::atof(ki);

    logInfo("liquid", "DMR demod: fs=" + std::to_string(sample_rate) + " decim=" +
                          std::to_string(d.factor) + " sps=" + std::to_string(d.sps) +
                          " channel=" + std::to_string(d.channel_rate) + " offset=" +
                          std::to_string(freq_offset_hz));
}

LiquidDmrDemod::~LiquidDmrDemod()
{
    if (m_impl == nullptr)
        return;
    if (m_impl->decim)
        firdecim_crcf_destroy(m_impl->decim);
    if (m_impl->dem)
        freqdem_destroy(m_impl->dem);
    if (m_impl->rrc)
        firfilt_rrrf_destroy(m_impl->rrc);
    if (m_impl->nco)
        nco_crcf_destroy(m_impl->nco);
    delete m_impl;
}

void LiquidDmrDemod::reset()
{
    if (m_impl == nullptr)
        return;
    Impl& d = *m_impl;
    d.dc = 0.0;
    d.have_dc = false;
    d.agc_sum = 0.0f;
    d.agc_idx = 0;
    for (unsigned i = 0; i < Impl::kAgcWindow; ++i)
        d.agc_pwr[i] = 0.0f;
    d.max_val = 1e-3;
    d.min_val = -1e-3;
    d.T = static_cast<double>(d.sps);
    d.pos = d.T;
    d.freq_err = 0.0;
    d.have_prev = false;
    d.initialized = false;
    d.ybuf.assign(d.sps, 0.0f);
    if (d.decim)
        firdecim_crcf_reset(d.decim);
    if (d.dem)
        freqdem_reset(d.dem);
    if (d.rrc)
        firfilt_rrrf_reset(d.rrc);
    d.pending.clear();
}

void LiquidDmrDemod::setFreqOffset(double hz)
{
    if (m_impl == nullptr || m_impl->nco == nullptr)
        return;
    const double omega = kTwoPi * hz / static_cast<double>(m_impl->sample_rate);
    nco_crcf_set_frequency(m_impl->nco, static_cast<float>(omega));
    reset();
}

void LiquidDmrDemod::process(const uint8_t* iq, size_t samples, std::vector<uint8_t>& dibits)
{
    Impl& d = *m_impl;
    if (samples == 0 || d.decim == nullptr || d.rrc == nullptr)
        return;

    const size_t base = d.pending.size();
    d.pending.resize(base + samples);
    for (size_t i = 0; i < samples; ++i) {
        const float re = (static_cast<float>(iq[2 * i]) - 127.5f) / 128.0f;
        const float im = (static_cast<float>(iq[2 * i + 1]) - 127.5f) / 128.0f;
        d.pending[base + i] = liquid_float_complex(re, im);
    }
    nco_crcf_mix_block_down(d.nco, d.pending.data() + base, d.pending.data() + base,
                            static_cast<unsigned>(samples));

    const size_t n = (d.pending.size() / d.factor) * d.factor;
    if (n == 0)
        return;
    const size_t outN = n / d.factor;

    d.decim_buf.resize(outN);
    firdecim_crcf_execute_block(d.decim, d.pending.data(), static_cast<unsigned>(outN),
                                d.decim_buf.data());
    d.pending.erase(d.pending.begin(), d.pending.begin() + static_cast<long>(n));

    // AGC -> FM discriminator -> DC blocker -> RRC matched filter.
    d.rrc_in.assign(outN, 0.0f);
    for (size_t i = 0; i < outN; ++i) {
        const liquid_float_complex s = d.decim_buf[i];
        const float pwr = std::norm(s);
        d.agc_sum -= d.agc_pwr[d.agc_idx];
        d.agc_pwr[d.agc_idx] = pwr;
        d.agc_sum += pwr;
        d.agc_idx = (d.agc_idx + 1) % Impl::kAgcWindow;
        const float avg = d.agc_sum / static_cast<float>(Impl::kAgcWindow);
        const float gain = (avg > 1e-20f) ? (1.0f / std::sqrt(avg)) : 1.0f;

        float f = 0.0f;
        freqdem_demodulate(d.dem, s * gain, &f);
        if (!d.have_dc) {
            d.dc = f;
            d.have_dc = true;
        }
        d.dc += (f - d.dc) * kDcAlpha;
        d.rrc_in[i] = f - static_cast<float>(d.dc);
    }

    d.rrc_out.resize(outN);
    firfilt_rrrf_execute_block(d.rrc, d.rrc_in.data(), static_cast<unsigned>(outN),
                               d.rrc_out.data());
    d.ybuf.insert(d.ybuf.end(), d.rrc_out.begin(), d.rrc_out.end());

    // Seed the timing phase from a DMR sync codeword. The Gardner TED has a
    // stable (wrong) equilibrium at the inter-symbol midpoint, so the loop must
    // start on the eye opening. The sync patterns are the strongest feature.
    if (!d.initialized && d.ybuf.size() > 64) {
        static const char* kSync[4] = {
            "131111333113313313113313", "313333111331131331331131",
            "133313311131311113313331", "311131133313133331131113"};
        const int sub = 16;
        int bestErrors = 25;
        double bestPhase = 0.0;
        for (int f = 0; f < sub; ++f) {
            const double phi = static_cast<double>(f) * d.T / static_cast<double>(sub);
            const size_t maxSyms = static_cast<size_t>((d.ybuf.size() - phi - 1.0) / d.T);
            if (maxSyms < 24)
                continue;
            std::vector<uint8_t> bits(maxSyms);
            for (size_t j = 0; j < maxSyms; ++j)
                bits[j] = (d.interp(phi + static_cast<double>(j) * d.T) < 0.0) ? 1u : 0u;
            for (size_t pos = 0; pos + 24 <= maxSyms; ++pos) {
                for (int p = 0; p < 4; ++p) {
                    int diff = 0;
                    for (int i = 0; i < 24; ++i)
                        if (static_cast<int>(bits[pos + i]) != (kSync[p][i] == '3' ? 1 : 0))
                            ++diff;
                    if (diff < bestErrors) {
                        bestErrors = diff;
                        bestPhase = phi;
                    }
                }
            }
        }
        if (bestErrors <= 4) {
            d.pos = bestPhase + d.T; // keep T samples of look-back
            d.freq_err = 0.0;
            d.initialized = true;
        }
    }

    // Gardner timing PLL: one symbol per loop iteration.
    while (d.pos + 1.0 < static_cast<double>(d.ybuf.size())) {
        const double s1 = d.interp(d.pos);
        const double s0 = d.interp(d.pos - d.T);
        const double sh = d.interp(d.pos - 0.5 * d.T);

        double e = (s1 - s0) * sh;
        const double lim = 1.0;
        if (e > lim)
            e = lim;
        else if (e < -lim)
            e = -lim;

        d.freq_err += d.ki * e;
        if (d.freq_err < -0.75 * d.T)
            d.freq_err = -0.75 * d.T;
        else if (d.freq_err > 0.75 * d.T)
            d.freq_err = 0.75 * d.T;

        double step = d.T + d.freq_err + d.kp * e;
        if (step < 0.25 * d.T)
            step = 0.25 * d.T;
        else if (step > 1.75 * d.T)
            step = 1.75 * d.T;

        d.sliceInto(s1, dibits);
        d.pos += step;
    }

    // Drop fully consumed history, keeping at least T samples of look-back.
    const double keepBack = d.T + 2.0;
    if (d.pos > keepBack) {
        const size_t drop = static_cast<size_t>(d.pos - keepBack);
        if (drop > 0 && drop <= d.ybuf.size()) {
            d.ybuf.erase(d.ybuf.begin(), d.ybuf.begin() + static_cast<long>(drop));
            d.pos -= static_cast<double>(drop);
        }
    }
}

} // namespace bmrf::rf

#endif // BMRF_HAVE_LIQUID
