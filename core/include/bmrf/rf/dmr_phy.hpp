#pragma once

#include "bmrf/rf/burst.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bmrf::rf {

// DMR synchroniser / burst extractor: consumes a stream of dibits (0..3), finds
// the 48-bit sync codeword, and emits complete 132-dibit DMRBursts with the
// CACH (timeslot) decoded when available.
class DmrPhy {
public:
    void reset();

    // Append dibits and collect any complete bursts.
    void feed(const uint8_t* dibits, size_t count, std::vector<DMRBurst>& out);

private:
    void fillBurst(size_t burstStart, SyncType sync, DMRBurst& burst) const;

    std::vector<uint8_t> m_buffer;
    size_t m_scan = 0;
    bool m_locked = false;
    size_t m_nextStart = 0;
    int m_parity = 1; // alternating timeslot when CACH does not decode
    int m_syncMisses = 0; // consecutive slots without a valid sync while locked
};

} // namespace bmrf::rf
