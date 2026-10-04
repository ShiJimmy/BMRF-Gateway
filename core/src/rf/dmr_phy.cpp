#include "bmrf/rf/dmr_phy.hpp"

#include "bmrf/common/log.hpp"
#include "bmrf/common/time.hpp"
#include "bmrf/rf/fec.hpp"

#include <cstring>

namespace bmrf::rf {

namespace {

// CACH de-interleave schedule (ETSI / DSD).
const int kCachInterleave[24] = {
    0, 7, 8, 9, 1, 10, 11, 12, 2, 13, 14, 15, 3, 16, 4, 17, 18, 19, 5, 20, 21, 22, 6, 23,
};

constexpr int kCachDibits = 12;
constexpr size_t kTrimThreshold = 8192;

} // namespace

void DmrPhy::reset()
{
    m_buffer.clear();
    m_scan = 0;
    m_locked = false;
    m_nextStart = 0;
    m_syncMisses = 0;
}

void DmrPhy::fillBurst(size_t burstStart, SyncType sync, DMRBurst& burst) const
{
    burst.timestamp_ns = nowNs();
    burst.sync = sync;
    std::memcpy(burst.dibits.data(), m_buffer.data() + burstStart, kDmrBurstDibits);

    if (burstStart >= static_cast<size_t>(kCachDibits)) {
        const uint8_t* cachDibits = m_buffer.data() + burstStart - kCachDibits;
        uint8_t cach[24] = {0};
        for (int i = 0; i < kCachDibits; ++i) {
            const uint8_t dibit = cachDibits[i] & 0x03;
            cach[kCachInterleave[i * 2]] = (dibit >> 1) & 1;
            cach[kCachInterleave[i * 2 + 1]] = dibit & 1;
        }
        burst.has_cach = true;
        std::memcpy(burst.cach.data(), cach, 24);

        uint8_t tact[7];
        std::memcpy(tact, cach, 7);
        if (fec::hamming7x4Decode(tact))
            burst.timeslot = tact[1] + 1;
    }
}

void DmrPhy::feed(const uint8_t* dibits, size_t count, std::vector<DMRBurst>& out)
{
    m_buffer.insert(m_buffer.end(), dibits, dibits + count);

    // Emit one burst per sync codeword found. A burst is only valid if it
    // actually carries a sync; free-running (emitting slots without a sync)
    // produces garbage frames from noise/interference that play as hiss, so it
    // is not used. Scanning every symbol also tolerates symbol-clock drift and
    // gaps between transmissions.
    for (;;) {
        if (m_scan + kDmrSyncDibits > m_buffer.size())
            break;

        int errors = 25;
        const SyncType sync = classifySyncCorrelate(m_buffer.data() + m_scan, 2, &errors);
        if (sync == SyncType::None) {
            ++m_scan;
            continue;
        }

        const long burstStart = static_cast<long>(m_scan) - kDmrSyncOffset;
        if (burstStart < 0) {
            ++m_scan;
            continue;
        }
        if (static_cast<size_t>(burstStart) + kDmrBurstDibits > m_buffer.size())
            break; // need more data

        DMRBurst burst;
        fillBurst(static_cast<size_t>(burstStart), sync, burst);
        if (burst.timeslot == 0)
            burst.timeslot = m_parity;
        m_parity = (m_parity == 1) ? 2 : 1;
        out.push_back(burst);

        // Continue scanning just after this sync (don't skip a whole slot, so a
        // tightly packed next burst is still found).
        m_scan += kDmrSyncDibits;
    }

    if (m_scan > kTrimThreshold) {
        const size_t cut = m_scan;
        if (cut <= m_buffer.size()) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<long>(cut));
            m_scan -= cut;
        }
    }
}

} // namespace bmrf::rf
