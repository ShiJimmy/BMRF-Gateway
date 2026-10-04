#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bmrf {

// Audio sink abstraction. On Android this is backed by AAudio; on the desktop
// and in tests it is a null sink.
class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    virtual bool open(int sampleRate, int channels, std::string& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Write `count` interleaved 16-bit frames. Returns frames written or -1.
    virtual int write(const int16_t* samples, size_t count) = 0;

    virtual const char* name() const = 0;
};

std::unique_ptr<IAudioOutput> createAudioOutput();

} // namespace bmrf
