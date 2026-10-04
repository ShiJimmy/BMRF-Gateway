#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmrf::rf {

struct FskDemodConfig {
    uint32_t sample_rate = 240000; // Hz
    uint32_t symbol_rate = 4800;   // DMR symbols/s
    // Inner symbol deviation (Hz). DMR: outer 1944 Hz, inner 648 Hz.
    double inner_deviation_hz = 648.0;
};

// 4FSK demodulator for DMR: cu8 IQ -> dibits (0..3) via an FM discriminator and
// integrate-and-dump slicing. Symbol timing is derived from the nominal sample
// rate; the DMR synchroniser then aligns to the burst.
//
// NOTE: the level -> dibit mapping and slicing thresholds must be validated
// against a real DMR signal; the synthetic unit test only proves the round trip.
class FskDemod {
public:
    explicit FskDemod(FskDemodConfig config = {});

    void process(const uint8_t* iq, size_t samples, std::vector<uint8_t>& out);
    void reset();

    size_t samplesPerSymbol() const { return m_samplesPerSymbol; }

    // Symbol-timing phase (samples to skip before the first integrate window).
    // Calibrated by correlating against the DMR sync codewords.
    void setPhase(int phase);
    int phase() const { return m_phase; }

private:
    int slice(double phaseSum) const;

    FskDemodConfig m_config;
    size_t m_samplesPerSymbol;

    double m_accumulator = 0.0;
    double m_dcEma = 0.0;
    size_t m_symbolSamples = 0;
    size_t m_skipped = 0;
    int m_phase = 0;
    int m_prevI = 0;
    int m_prevQ = 0;
    bool m_havePrev = false;
};

} // namespace bmrf::rf
