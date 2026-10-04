#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bmrf {

// Process-wide layered capture (design doc §21). Writes one text record per
// line so the same RF call can be replayed without re-receiving it:
//
//   <stage> <timestamp_ns> <hex-or-text>
//
// stages:
//   rf_iq     raw cu8 IQ block                    (hex)
//   rf_frame  a decoded RF DMRFrame               (compact text, see below)
//   bm_tx     a DMRD/datagram sent to BrandMeister(hex)
//   bm_rx     a datagram received from the master (hex)
//
// All entry points are thread-safe and no-ops when no capture is open.
bool captureStart(const std::string& path, std::string& error);
void captureStop();
bool captureActive();

void captureWrite(const char* stage, uint64_t timestamp_ns,
                  const uint8_t* data, size_t len);
void captureWriteText(const char* stage, uint64_t timestamp_ns,
                      const std::string& text);

} // namespace bmrf
