#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bmrf::rf {

// Raw IQ source abstraction. Samples are interleaved unsigned 8-bit I/Q
// ("cu8", as produced by RTL-SDR), centred on 127.
class ISampleSource {
public:
    virtual ~ISampleSource() = default;

    virtual bool open(const std::string& spec, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Reads up to `maxSamples` complex samples. Returns samples read,
    // 0 on end-of-stream / no data, or -1 on error.
    virtual int read(uint8_t* iq, size_t maxSamples) = 0;

    virtual uint32_t sampleRate() const = 0;
    virtual const char* name() const = 0;
};

std::unique_ptr<ISampleSource> createNullSampleSource();

} // namespace bmrf::rf
