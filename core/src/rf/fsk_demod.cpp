#include "bmrf/rf/fsk_demod.hpp"

#include <cmath>

namespace bmrf::rf {

namespace {
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kDcAlpha = 0.001; // DC blocker time constant (~1000 samples)
}

FskDemod::FskDemod(FskDemodConfig config)
    : m_config(config)
{
    m_samplesPerSymbol = m_config.symbol_rate > 0
                             ? m_config.sample_rate / m_config.symbol_rate
                             : 50;
    if (m_samplesPerSymbol == 0)
        m_samplesPerSymbol = 1;
}

void FskDemod::reset()
{
    m_accumulator = 0.0;
    m_dcEma = 0.0;
    m_symbolSamples = 0;
    m_skipped = 0;
    m_prevI = 0;
    m_prevQ = 0;
    m_havePrev = false;
}

int FskDemod::slice(double phaseSum) const
{
    // Per-symbol phase step for the inner deviation.
    const double d = kTwoPi * m_config.inner_deviation_hz / static_cast<double>(m_config.symbol_rate);
    const double threshold = 2.0 * d;

    int level;
    if (phaseSum >= threshold)
        level = 3;
    else if (phaseSum >= 0.0)
        level = 2;
    else if (phaseSum >= -threshold)
        level = 1;
    else
        level = 0;

    // DMR Gray-coded mapping from the four FSK levels to dibits (calibrated
    // against a real DMR signal: levels -3d,-d,+d,+3d -> 0,2,3,1).
    static const int kLevelToDibit[4] = {0, 2, 3, 1};
    return kLevelToDibit[level];
}

void FskDemod::setPhase(int phase)
{
    m_phase = phase < 0 ? 0 : (static_cast<size_t>(phase) % m_samplesPerSymbol == 0
                                  ? 0
                                  : phase % static_cast<int>(m_samplesPerSymbol));
    m_skipped = 0;
    m_accumulator = 0.0;
    m_symbolSamples = 0;
}

void FskDemod::process(const uint8_t* iq, size_t samples, std::vector<uint8_t>& out)
{
    for (size_t n = 0; n < samples; ++n) {
        const int i = static_cast<int>(iq[2 * n]) - 127;
        const int q = static_cast<int>(iq[2 * n + 1]) - 127;

        if (m_havePrev) {
            // Phase difference via the cross/dot product, then atan2 for a
            // robust per-sample frequency estimate.
            const double cross = static_cast<double>(m_prevI) * q -
                                 static_cast<double>(m_prevQ) * i;
            const double dot = static_cast<double>(i) * m_prevI +
                               static_cast<double>(q) * m_prevQ;
            const double disc = std::atan2(cross, dot);

            // DC blocker: track the slow carrier / RTL-SDR DC-spike offset and
            // remove it so the four FSK levels stay centred on zero.
            m_dcEma += (disc - m_dcEma) * kDcAlpha;

            if (m_skipped < static_cast<size_t>(m_phase)) {
                ++m_skipped;
            } else {
                m_accumulator += disc - m_dcEma;
                ++m_symbolSamples;
            }
        }
        m_prevI = i;
        m_prevQ = q;
        m_havePrev = true;

        if (m_symbolSamples >= m_samplesPerSymbol) {
            out.push_back(static_cast<uint8_t>(slice(m_accumulator)));
            m_accumulator = 0.0;
            m_symbolSamples = 0;
        }
    }
}

} // namespace bmrf::rf
