#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmrf::rf {

struct AlignedDemodConfig {
    uint32_t sample_rate = 240000;
    uint32_t symbol_rate = 4800;
    double inner_deviation_hz = 648.0;
    int max_sync_errors = 2; // accept a phase whose sync distance is <= this
    // RRC matched-filter excess bandwidth. ETSI DMR is 0.2, but real transmitter
    // + receiver IF filtering widens the pulse; DSD-FME uses alpha = 0.7 for DMR
    // (F4EXB filter). Too narrow a filter causes ISI on the inner +/-1 levels.
    double rolloff = 0.2;
    // Sub-sample phase resolution: the symbol phase is searched over
    // m_sps * fractional_steps phases and interpolated between samples. Real
    // signals have a fractional symbol timing offset and drift; integer-only
    // phases corrupt the inner +/-1 levels (AMBE/LC) while the outer sync
    // symbols still correlate.
    int fractional_steps = 8;
};

// Block-based DMR 4FSK demodulator with per-block symbol-phase re-locking.
//
// The RTL-SDR / radio clocks drift slowly, so a single fixed symbol phase loses
// lock within a burst or two. Each block is re-aligned to whichever sampling
// phase best matches a DMR sync codeword; when no sync is present the previous
// phase is reused (drift over one block is negligible).
class AlignedDmrDemod {
public:
    explicit AlignedDmrDemod(AlignedDemodConfig config = {});

    // Demodulate one IQ block, appending aligned dibits (0..3) to `dibits`.
    // Returns the symbol phase used for this block.
    int processBlock(const uint8_t* iq, size_t samples, std::vector<uint8_t>& dibits);

    // Interface-compatible alias for the burst source (same as processBlock).
    void process(const uint8_t* iq, size_t samples, std::vector<uint8_t>& dibits)
    {
        processBlock(iq, samples, dibits);
    }

    int phase() const { return m_phase; }
    bool locked() const { return m_locked; }
    void reset();

    // Fractional symbol phase in [0, m_sps) used for the last block.
    double fractionalPhase() const { return static_cast<double>(m_phase) + m_frac; }

    // Adaptive 4-level decision thresholds (tracked from the signal envelope).
    double envelopeMax() const { return m_max; }
    double envelopeMin() const { return m_min; }

private:
    AlignedDemodConfig m_config;
    size_t m_sps;
    double m_threshold;
    double m_dcEma = 0.0;
    int m_phase = 0;
    double m_frac = 0.0;
    bool m_locked = false;
    int m_prevI = 0;
    int m_prevQ = 0;
    bool m_havePrev = false;
    double m_max = 1e-3;
    double m_min = -1e-3;
    double m_symMean = 0.0;
    double m_symVar = 0.0;
    std::vector<float> m_disc;
    std::vector<float> m_discHistory; // pre-filter samples carried across blocks
    std::vector<float> m_rrc;
};

} // namespace bmrf::rf
