#pragma once

#include "bmrf/rf/dmr_sync.hpp"

#include <array>
#include <cstdint>

namespace bmrf::rf {

// One DMR burst = 132 dibits (264 bits) in transmission order, CACH excluded.
// Layout (ETSI TS 102 361-1):
//   dibits   0..35  : AMBE frame 1 (72 bits)
//   dibits  36..53  : AMBE frame 2, first half (36 bits)
//   dibits  54..77  : 48-bit sync or embedded signalling
//   dibits  78..95  : AMBE frame 2, second half (36 bits)
//   dibits  96..131 : AMBE frame 3 (72 bits)
constexpr int kDmrBurstDibits = 132;
constexpr int kDmrSyncOffset = 54;
constexpr int kDmrSyncDibits = 24;

struct DMRBurst {
    uint64_t timestamp_ns = 0;
    int timeslot = 0;    // 1 or 2, 0 = unknown
    int color_code = -1; // -1 = unknown
    SyncType sync = SyncType::None;

    bool has_metrics = false;
    float rssi_dbm = 0.0f;
    float snr_db = 0.0f;
    float ber_percent = 0.0f;

    std::array<uint8_t, kDmrBurstDibits> dibits{}; // each 0..3

    // Optional de-interleaved CACH (24 bits) when the source provides it.
    bool has_cach = false;
    std::array<uint8_t, 24> cach{};
};

} // namespace bmrf::rf
