#pragma once

#include "bmrf/rf/burst.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace bmrf::rf {

// A source of already-extracted DMR bursts. This is the seam where the user's
// own DMR PHY/decoder (or the future built-in 4FSK chain) plugs in.
class IDmrBurstSource {
public:
    virtual ~IDmrBurstSource() = default;

    virtual bool open(const std::string& spec, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Returns true and fills `burst`, false on EOF / error / timeout.
    virtual bool read(DMRBurst& burst) = 0;

    // Reads up to `maxSamples` raw cu8 IQ samples (for the DSDcc decode path).
    // Returns the number of samples read, 0 if none are available.
    virtual size_t readIq(uint8_t* iq, size_t maxSamples)
    {
        (void)iq;
        (void)maxSamples;
        return 0;
    }

    virtual const char* name() const = 0;
};

std::unique_ptr<IDmrBurstSource> createNullBurstSource();

// Replays a binary burst capture (design doc §21). Record layout, little endian:
//   uint32 timeslot, int32 color_code, float rssi, float snr, float ber,
//   uint8 syncType, uint8 hasCach, uint8 cach[24], uint8 dibits[132]  (178 bytes)
std::unique_ptr<IDmrBurstSource> createFileBurstSource(const std::string& path);

} // namespace bmrf::rf
