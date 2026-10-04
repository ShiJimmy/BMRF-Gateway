#include "bmrf/rf/aligned_demod.hpp"

#include "bmrf/rf/dmr_sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bmrf::rf {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kPi = kTwoPi / 2.0;
constexpr double kDcAlpha = 0.01;
// ETSI DMR Gray-coded level -> dibit mapping: +3->01, +1->00, -1->10, -3->11.
// level -> dibit mapping (default ETSI {3,2,0,1}); overridable for calibration
// with BMRF_LEVEL_MAP=<4 digits>, e.g. "2013".
const int* levelMap()
{
    static int map[4] = {3, 2, 0, 1};
    static const bool init = [] {
        if (const char* e = std::getenv("BMRF_LEVEL_MAP")) {
            if (std::strlen(e) >= 4) {
                for (int i = 0; i < 4; ++i)
                    map[i] = e[i] - '0';
            }
        }
        return true;
    }();
    (void)init;
    return map;
}

int levelToDibit(double phaseSum, double threshold)
{
    int level;
    if (phaseSum >= threshold)
        level = 3;
    else if (phaseSum >= 0.0)
        level = 2;
    else if (phaseSum >= -threshold)
        level = 1;
    else
        level = 0;
    return levelMap()[level];
}

} // namespace

AlignedDmrDemod::AlignedDmrDemod(AlignedDemodConfig config)
    : m_config(config)
{
    m_sps = std::max<size_t>(1, m_config.sample_rate / m_config.symbol_rate);
    m_threshold = 2.0 * kTwoPi * m_config.inner_deviation_hz /
                  static_cast<double>(m_config.symbol_rate);

    // Root-raised-cosine matched filter. Without it the rectangular symbol
    // integration suffers ISI and corrupts the payload, even though the sync
    // (outer symbols) still correlates.
    double beta = m_config.rolloff;
    if (const char* e = std::getenv("BMRF_DEMOD_ROLLOFF"))
        beta = std::atof(e);
    const int half = 8;
    const size_t taps = 2 * static_cast<size_t>(half) * m_sps + 1;
    m_rrc.assign(taps, 0.0f);
    double sum = 0.0;
    for (size_t n = 0; n < taps; ++n) {
        const double t = (static_cast<double>(n) - static_cast<double>(taps / 2)) /
                         static_cast<double>(m_sps);
        double v;
        if (std::fabs(t) < 1e-9)
            v = 1.0 - beta + 4.0 * beta / kPi;
        else
            v = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) /
                (kPi * t * (1 - std::pow(4 * beta * t, 2)));
        m_rrc[n] = static_cast<float>(v);
        sum += v;
    }
    for (auto& v : m_rrc)
        v = static_cast<float>(v / sum);
}

void AlignedDmrDemod::reset()
{
    m_dcEma = 0.0;
    m_phase = 0;
    m_frac = 0.0;
    m_locked = false;
    m_havePrev = false;
    m_max = 1e-3;
    m_min = -1e-3;
    m_symMean = 0.0;
    m_symVar = 0.0;
    m_disc.clear();
    m_discHistory.clear();
}

int AlignedDmrDemod::processBlock(const uint8_t* iq, size_t samples,
                                  std::vector<uint8_t>& dibits)
{
    if (samples < 2 || m_sps == 0)
        return m_phase;

    // Per-sample FM discriminator with a continuous DC blocker. disc[0] is the
    // boundary sample carried over from the previous block, so the stream is
    // contiguous across calls.
    m_disc.assign(samples, 0.0f);
    for (size_t n = 0; n < samples; ++n) {
        const int i = static_cast<int>(iq[2 * n]) - 127;
        const int q = static_cast<int>(iq[2 * n + 1]) - 127;
        if (m_havePrev) {
            const double cross = static_cast<double>(m_prevI) * q -
                                 static_cast<double>(m_prevQ) * i;
            const double dot = static_cast<double>(i) * m_prevI +
                               static_cast<double>(q) * m_prevQ;
            const double value = std::atan2(cross, dot);
            m_dcEma += (value - m_dcEma) * kDcAlpha;
            m_disc[n] = static_cast<float>(value - m_dcEma);
        }
        m_prevI = i;
        m_prevQ = q;
        m_havePrev = true;
    }

    // RRC matched filter (per-sample). The filtered signal carries the symbol
    // value at the symbol instant, so we sample instead of rect-integrating.
    const size_t discLen = m_disc.size();
    if (discLen < m_sps * 8)
        return m_phase;

    {
        const size_t taps = m_rrc.size();
        const long half = static_cast<long>(taps / 2);
        const size_t histLen = taps - 1;

        // Carry the previous block's tail so the FIR has continuous history and
        // the block boundaries do not corrupt the symbol values.
        std::vector<float> hist = m_discHistory;
        if (hist.size() < histLen)
            hist.insert(hist.begin(), histLen - hist.size(), 0.0f);

        std::vector<float> ext;
        ext.reserve(hist.size() + discLen);
        ext.insert(ext.end(), hist.begin(), hist.end());
        ext.insert(ext.end(), m_disc.begin(), m_disc.end());

        std::vector<float> filtered(discLen, 0.0f);
        for (size_t i = 0; i < discLen; ++i) {
            double acc = 0.0;
            const long base = static_cast<long>(hist.size() + i);
            for (size_t j = 0; j < taps; ++j) {
                const long idx = base - half + static_cast<long>(j);
                if (idx >= 0 && static_cast<size_t>(idx) < ext.size())
                    acc += static_cast<double>(ext[static_cast<size_t>(idx)]) * m_rrc[j];
            }
            filtered[i] = static_cast<float>(acc);
        }

        const size_t keep = std::min(histLen, ext.size());
        m_discHistory.assign(ext.end() - static_cast<long>(keep), ext.end());
        m_disc.swap(filtered);
    }

    // Per-block symbol-phase re-lock via sync correlation. Search integer
    // phases first (cheap, and exact for synthetic/integer-timed signals); if
    // none gives a clean sync, refine at sub-sample resolution because real
    // signals carry a fractional symbol-timing offset. The symbol value is
    // linearly interpolated between the two nearest RRC-matched samples.
    const int sub = m_config.fractional_steps > 0 ? m_config.fractional_steps : 1;
    int bestErrors = 25;
    size_t bestPhase = static_cast<size_t>(m_phase % m_sps);
    double bestFrac = m_frac;
    double bestCorr = -1.0;
    std::vector<uint8_t> probe;
    std::vector<double> vals;

    // DMR sync codewords (dibit-MSB strings, matching dmr_sync.cpp).
    static const char* kSync[4] = {
        "131111333113313313113313", "313333111331131331331131",
        "133313311131311113313331", "311131133313133331131113"};

    // Score a candidate phase by (a) minimum sync Hamming distance and (b) the
    // soft correlation of the received symbols with the matched sync codeword.
    // The Hamming distance only sees symbol signs, which stay correct over a
    // range of sampling instants; the soft correlation is maximal only at the
    // true eye opening, which the 4-level payload needs. A noise-dominated
    // variance/energy metric cannot distinguish this (the noise floor inflates
    // it), so the data-aided correlation is used instead.
    auto evaluate = [&](size_t ip, double frac, int& errors, double& corr) {
        probe.clear();
        vals.clear();
        const size_t cap = discLen / m_sps + 1;
        probe.reserve(cap);
        vals.reserve(cap);
        for (size_t s = ip; s + 1 < discLen; s += m_sps) {
            const double v = m_disc[s] + (m_disc[s + 1] - m_disc[s]) * frac;
            vals.push_back(v);
            probe.push_back(static_cast<uint8_t>(levelToDibit(v, 0.0)));
        }
        errors = 25;
        corr = 0.0;
        size_t bestPos = 0;
        int bestPat = -1;
        for (size_t pos = 0; pos + 24 <= probe.size(); ++pos) {
            for (int p = 0; p < 4; ++p) {
                int diff = 0;
                for (int i = 0; i < 24; ++i) {
                    const uint8_t bit = (probe[pos + i] >> 1) & 1;
                    const uint8_t exp = (kSync[p][i] == '3') ? 1 : 0;
                    if (bit != exp)
                        ++diff;
                }
                if (diff < errors) {
                    errors = diff;
                    bestPos = pos;
                    bestPat = p;
                }
            }
        }
        if (bestPat >= 0) {
            double c = 0.0;
            for (int i = 0; i < 24; ++i) {
                // '3' (dibit MSB 1) corresponds to a negative symbol value.
                const double sign = (kSync[bestPat][i] == '3') ? -1.0 : 1.0;
                c += vals[bestPos + static_cast<size_t>(i)] * sign;
            }
            corr = std::fabs(c);
        }
    };

    auto consider = [&](size_t ip, double frac) {
        int e = 25;
        double c = 0.0;
        evaluate(ip, frac, e, c);
        if (e < bestErrors || (e == bestErrors && c > bestCorr)) {
            bestErrors = e;
            bestPhase = ip;
            bestFrac = frac;
            bestCorr = c;
        }
    };

    // Integer phase pass (all phases; the eye-energy tie-break below picks the
    // true peak among equal-sync-error phases).
    for (size_t ip = 0; ip < m_sps; ++ip)
        consider(ip, 0.0);
    // Sub-sample refinement only when integer phases are not exact, and only
    // around the best integer phase (a full 2-D sweep is far too slow for a
    // real-time receiver).
    if (bestErrors > 0) {
        const size_t center = bestPhase;
        for (int d = -1; d <= 1; ++d) {
            const size_t ip = (center + m_sps + static_cast<size_t>(d)) % m_sps;
            for (int f = 1; f < sub; ++f)
                consider(ip, static_cast<double>(f) / static_cast<double>(sub));
        }
    }

    if (bestErrors <= m_config.max_sync_errors) {
        m_phase = static_cast<int>(bestPhase);
        m_frac = bestFrac;
        m_locked = true;
    }

    // Demodulate at the chosen fractional phase with adaptive thresholds.
    const size_t phase = static_cast<size_t>(m_phase % m_sps);
    double dbgSum = 0.0, dbgSumsq = 0.0;
    size_t dbgN = 0;
    int dbgHist[16] = {0};
    double dbgVals[120];
    size_t dbgValsN = 0;
    for (size_t s = phase; s + 1 < discLen; s += m_sps) {
        const double sum = m_disc[s] + (m_disc[s + 1] - m_disc[s]) * m_frac;
        dbgSum += sum;
        dbgSumsq += sum * sum;
        ++dbgN;
        if (dbgValsN < 120)
            dbgVals[dbgValsN++] = sum;
        {
            const int bin = static_cast<int>((sum + 0.8) / 0.1);
            if (bin >= 0 && bin < 16)
                dbgHist[bin]++;
        }

        // Decision thresholds from the running mean/variance of the sampled
        // symbols. A max/min envelope is corrupted by a single outlier transient
        // (which then makes the inner/outer boundary far too wide and pushes
        // every symbol into the inner levels); the variance is robust.
        m_symMean += (sum - m_symMean) * 0.02;
        const double dev = sum - m_symMean;
        m_symVar += (dev * dev - m_symVar) * 0.02;
        const double thr = 0.9 * std::sqrt(m_symVar);

        if (m_symVar > 0.0 && thr < m_max)
            m_max = thr; // expose the effective threshold for diagnostics

        int level;
        if (sum >= m_symMean + thr)
            level = 3;
        else if (sum >= m_symMean)
            level = 2;
        else if (sum >= m_symMean - thr)
            level = 1;
        else
            level = 0;
        dibits.push_back(static_cast<uint8_t>(levelMap()[level]));
    }

    static const bool dbg = std::getenv("BMRF_ALIGNED_DEBUG") != nullptr;
    if (dbg) {
        const double m = dbgN ? dbgSum / static_cast<double>(dbgN) : 0.0;
        const double vv = dbgN ? dbgSumsq / static_cast<double>(dbgN) - m * m : 0.0;
        const double dchz = m_dcEma * static_cast<double>(m_config.sample_rate) / kTwoPi;
        std::fprintf(stderr,
                     "[aligned] phase=%zu frac=%.3f bestErr=%d energy=%.5f sym_mean=%.4f "
                     "sym_std=%.4f dc=%.0fHz env=[%.4f %.4f]\n",
                     bestPhase, bestFrac, bestErrors, bestCorr, m, std::sqrt(vv), dchz, m_min,
                     m_max);

        static const bool dump = std::getenv("BMRF_ALIGNED_HIST") != nullptr;
        if (dump && bestErrors <= 2) {
            std::fprintf(stderr, "[hist]");
            for (int b = 0; b < 16; ++b)
                std::fprintf(stderr, " %d", dbgHist[b]);
            std::fprintf(stderr, "  (bins -0.8..0.8 width 0.1)\n");

            std::fprintf(stderr, "[vals]");
            for (size_t i = 0; i < dbgValsN; ++i)
                std::fprintf(stderr, " %.0f", dbgVals[i] * 100.0);
            std::fprintf(stderr, "\n");
        }
    }

    return m_phase;
}

} // namespace bmrf::rf
