#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bmrf::rf {

// DMR digital-voice demodulator backed by the vendored DSDcc library (the same
// decoder used by SDRangel). DSDcc expects FM-discriminated S16LE audio at
// exactly 48 kS/s, so this class front-ends the RTL-SDR IQ:
//
//   cu8 IQ -> NCO (remove the DC avoidance offset) -> Kaiser decimation to
//   48 kS/s -> FM discriminator -> DSDcc (symbol timing, FEC, AMBE, and DMR
//   frame/LC parsing).
//
// Verified against the reference: on a real Tier-2 transmission it decodes the
// source/target IDs and clear voice. When the embedded LC fails to decode the
// addressing comes out as all-ones (0xFFFFFF); callers must treat that as
// invalid (see RfReceiver).
class DsdccDmrDemod {
public:
    explicit DsdccDmrDemod(uint32_t sample_rate = 240000, double freq_offset_hz = 40000.0);
    ~DsdccDmrDemod();

    DsdccDmrDemod(const DsdccDmrDemod&) = delete;
    DsdccDmrDemod& operator=(const DsdccDmrDemod&) = delete;

    // Demodulate a block of raw cu8 IQ, appending 8 kHz S16LE PCM to `pcm`.
    void process(const uint8_t* iq, size_t samples, std::vector<int16_t>& pcm);
    void reset();

    int audioSampleRate() const;

    // Last decoded DMR identity / status (human-readable, e.g. the DSDcc frame
    // subtype string). Empty until a frame has been decoded.
    std::string statusText();
    int inLevel() const;
    int symbolSyncQuality() const;
    // mbelib error-corrected bit count of the last decoded AMBE frame (BER proxy).
    int mbeErrors() const;
    // Coarse RF measurements from the IQ channel power: level in dB relative to
    // full scale and channel SNR in dB over the tracked noise floor.
    double channelLevelDb() const;
    double channelSnrDb() const;

    // Unified DMR model bridging (design doc §4.1): decoded addressing and call
    // classification from DSDcc's DMR decoder.
    uint32_t sourceId() const;
    uint32_t destinationId() const;
    int timeslot() const;      // 1 or 2, 0 unknown
    int colorCode() const;     // -1 unknown
    bool groupCall() const;
    // Full Link Control opcode and LC Service Options from the embedded LC.
    uint8_t flco() const;
    uint8_t serviceOptions() const;
    // True once the embedded-signalling Link Control for the current slot has
    // passed its FEC (the addressing/FLCO can be trusted).
    bool addressesValid() const;
    // Monotonic count of decoded (FEC-clean) embedded Link Controls. Advances
    // about once per DMR superframe; used to detect when the addressing has been
    // refreshed after a new transmission starts.
    uint64_t lcDecodeVersion() const;
    // Voice superframe position [0..5] for the Homebrew voice frame index.
    int voiceFrameIndex() const;
    // Number of AMBE voice frames DSDcc produced during the last process() call.
    int lastVoiceFrames() const;

    // Take one reassembled 33-byte Homebrew voice payload (27 AMBE bytes +
    // embedded signalling) for the RF -> BrandMeister uplink. Returns false when
    // none is buffered. Clears the slot's buffer on success. `sequence` (when
    // non-null) receives the per-burst voice sequence [0..5] (Homebrew options
    // low nibble / MMDVM voice number N).
    bool takePayload(uint8_t out[33], int* sequence = nullptr);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace bmrf::rf
