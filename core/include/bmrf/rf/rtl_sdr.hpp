#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bmrf::rf {

struct RtlSdrStatus {
    bool open = false;
    uint32_t center_hz = 0;
    uint32_t sample_rate_hz = 0;
    int gain_tenths_db = 0;
    int ppm = 0;
    uint64_t samples_read = 0;
    float rms = 0.0f;
    // Bytes dropped because the consumer fell behind the async USB reader.
    uint64_t dropped_bytes = 0;
};

// Set the file descriptor from Android's UsbDeviceConnection (must be called
// before rtlSdrOpen). No-op on non-Android builds.
void rtlSdrSetAndroidFd(int fd);

bool rtlSdrOpen(uint32_t centerHz, uint32_t sampleRateHz, int gainTenthsDb, int ppm,
                std::string& error);
void rtlSdrClose();
bool rtlSdrIsOpen();

// Reads up to maxSamples complex samples (interleaved cu8). Updates status.
int rtlSdrRead(uint8_t* iq, size_t maxSamples);

RtlSdrStatus rtlSdrStatus();

// Record raw cu8 IQ to `path` while reading (design doc §21). Only one active.
bool rtlSdrStartRecording(const std::string& path, std::string& error);
void rtlSdrStopRecording();
bool rtlSdrRecording();

} // namespace bmrf::rf
