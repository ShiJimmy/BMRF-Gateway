#include "bmrf/rf/dmr_sync.hpp"

#include <cstring>

namespace bmrf::rf {

namespace {

// Reference patterns from DSD / DSD-FME (dsd.h).
constexpr char kBsDataSync[] = "313333111331131131331131";
constexpr char kBsVoiceSync[] = "131111333113313313113313";
constexpr char kMsDataSync[] = "311131133313133331131113";
constexpr char kMsVoiceSync[] = "133313311131311113313331";

} // namespace

const char* toString(SyncType type)
{
    switch (type) {
    case SyncType::BsVoice: return "BS_VOICE";
    case SyncType::BsData:  return "BS_DATA";
    case SyncType::MsVoice: return "MS_VOICE";
    case SyncType::MsData:  return "MS_DATA";
    default:                return "NONE";
    }
}

SyncType classifySync(const uint8_t dibits[24])
{
    return classifySyncCorrelate(dibits, 0, nullptr);
}

SyncType classifySyncCorrelate(const uint8_t dibits[24], int maxErrors, int* errors)
{
    // Only the MSB of each dibit participates (matches DSD's convention).
    uint8_t bits[24];
    for (int i = 0; i < 24; ++i)
        bits[i] = (dibits[i] >> 1) & 1;

    const char* patterns[4] = {kBsVoiceSync, kBsDataSync, kMsVoiceSync, kMsDataSync};
    const SyncType types[4] = {SyncType::BsVoice, SyncType::BsData, SyncType::MsVoice,
                               SyncType::MsData};

    int bestErrors = 25;
    int bestIndex = -1;
    for (int p = 0; p < 4; ++p) {
        int diff = 0;
        for (int i = 0; i < 24; ++i) {
            const uint8_t expected = patterns[p][i] == '3' ? 1 : 0;
            if (bits[i] != expected)
                ++diff;
        }
        if (diff < bestErrors) {
            bestErrors = diff;
            bestIndex = p;
        }
    }

    if (errors != nullptr)
        *errors = bestErrors;

    if (bestIndex >= 0 && bestErrors <= maxErrors)
        return types[bestIndex];
    return SyncType::None;
}

} // namespace bmrf::rf
