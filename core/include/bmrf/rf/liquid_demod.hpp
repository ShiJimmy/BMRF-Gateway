#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmrf::rf {

// DMR 4FSK demodulator built on liquid-dsp, modelled on the verified TETRA
// receive chain (engine/dqpsk_demod.c): complex baseband processing with a
// decimating anti-alias filter, feedforward AGC, an FM discriminator and RRC
// symbol-timing recovery (symsync_crcf) instead of a per-block integer phase
// search. The continuous timing-tracking loop is what keeps the inner +/-1
// levels (AMBE / LC payload) intact across a whole transmission.
//
// Chain:
//   cu8 IQ -> complex -> NCO (remove the DC-spike offset)
//     -> Kaiser decimation to sps samples/symbol
//     -> feedforward AGC (normalize before detection)
//     -> FM discriminator -> DC blocker
//     -> symsync_crcf (RRC matched filter + fractional timing loop)
//     -> 2/3-envelope adaptive 4-level slicer -> dibit
class LiquidDmrDemod {
public:
    explicit LiquidDmrDemod(uint32_t sample_rate = 240000, uint32_t symbol_rate = 4800,
                            double freq_offset_hz = 0.0, int samples_per_symbol = 2,
                            std::array<int, 4> level_to_dibit = {3, 2, 0, 1});
    ~LiquidDmrDemod();

    LiquidDmrDemod(const LiquidDmrDemod&) = delete;
    LiquidDmrDemod& operator=(const LiquidDmrDemod&) = delete;

    // Demodulate a block of raw cu8 IQ (interleaved I,Q). Partial decimation
    // input is carried across calls so no samples are ever dropped.
    void process(const uint8_t* iq, size_t samples, std::vector<uint8_t>& dibits);
    void reset();

    // Retune the analysis frequency relative to the SDR centre (Hz).
    void setFreqOffset(double hz);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace bmrf::rf
