#pragma once

#include <cstddef>
#include <cstdint>

namespace bmrf {

// ---------------------------------------------------------------------------
// Call classification used by the unified DMR model.
// ---------------------------------------------------------------------------
enum class CallType : uint8_t {
    Unknown = 0,
    Group,
    Private,
    Data,
};

enum class CallState : uint8_t {
    Idle = 0,
    CallStart,
    Active,
    CallEnd,
};

// Where a call / frame originated.
enum class CallSource : uint8_t {
    Unknown = 0,
    Rf,
    BrandMeister,
};

// DMR Full Link Control Opcodes (ETSI TS 102 361-1).
enum class Flco : uint8_t {
    GroupVoice = 0x00,
    UnitToUnitVoice = 0x03,
    TalkerAliasHeader = 0x04,
    GpsInfo = 0x06,
};

// DMR data type. Values 0x00..0x11 match the on-wire / Homebrew data type from
// go-dmr (dmr/type.go) and MMDVM. Values >= 0x80 are BMRF pseudo types used by
// the unified model and never appear on the wire.
enum class DataType : uint8_t {
    PIHeader = 0x00,
    VoiceHeader = 0x01,
    TerminatorLC = 0x02,
    CSBK = 0x03,
    MBCHeader = 0x04,
    MBCContinuation = 0x05,
    DataHeader = 0x06,
    Rate12Data = 0x07,
    Rate34Data = 0x08,
    Idle = 0x09,
    VoiceFrameA = 0x0A,
    VoiceFrameB = 0x0B,
    VoiceFrameC = 0x0C,
    VoiceFrameD = 0x0D,
    VoiceFrameE = 0x0E,
    VoiceFrameF = 0x0F,
    EmbeddedData = 0x11,

    // BMRF pseudo types
    VoiceSync = 0x80,
    Voice = 0x81,
};

constexpr size_t kDmrPayloadBytes = 33; // one DMR burst payload
constexpr size_t kAmbeBytes = 33;       // AMBE payload carried in a voice burst
constexpr size_t kDmrIdBytes = 3;       // 24-bit DMR ID

// Number of voice frames in a DMR superframe (Homebrew voice frame index range).
constexpr int kVoiceFramesPerSuperframe = 6;

inline const char* toString(CallType v)
{
    switch (v) {
    case CallType::Group:   return "Group";
    case CallType::Private: return "Private";
    case CallType::Data:    return "Data";
    default:                return "Unknown";
    }
}

inline const char* toString(CallState v)
{
    switch (v) {
    case CallState::Idle:      return "Idle";
    case CallState::CallStart: return "CallStart";
    case CallState::Active:    return "Active";
    case CallState::CallEnd:   return "CallEnd";
    default:                   return "Unknown";
    }
}

inline const char* toString(CallSource v)
{
    switch (v) {
    case CallSource::Rf:           return "Rf";
    case CallSource::BrandMeister: return "BrandMeister";
    default:                       return "Unknown";
    }
}

inline const char* toString(DataType v)
{
    switch (v) {
    case DataType::PIHeader:       return "PIHeader";
    case DataType::VoiceHeader:    return "VoiceHeader";
    case DataType::TerminatorLC:   return "TerminatorLC";
    case DataType::CSBK:           return "CSBK";
    case DataType::MBCHeader:      return "MBCHeader";
    case DataType::MBCContinuation:return "MBCContinuation";
    case DataType::DataHeader:     return "DataHeader";
    case DataType::Rate12Data:     return "Rate12Data";
    case DataType::Rate34Data:     return "Rate34Data";
    case DataType::Idle:           return "Idle";
    case DataType::VoiceFrameA:    return "VoiceFrameA";
    case DataType::VoiceFrameB:    return "VoiceFrameB";
    case DataType::VoiceFrameC:    return "VoiceFrameC";
    case DataType::VoiceFrameD:    return "VoiceFrameD";
    case DataType::VoiceFrameE:    return "VoiceFrameE";
    case DataType::VoiceFrameF:    return "VoiceFrameF";
    case DataType::EmbeddedData:   return "EmbeddedData";
    case DataType::VoiceSync:      return "VoiceSync";
    case DataType::Voice:          return "Voice";
    default:                       return "Unknown";
    }
}

// True for any data type that carries DMR voice.
inline bool isVoiceFrame(DataType v)
{
    switch (v) {
    case DataType::VoiceFrameA:
    case DataType::VoiceFrameB:
    case DataType::VoiceFrameC:
    case DataType::VoiceFrameD:
    case DataType::VoiceFrameE:
    case DataType::VoiceFrameF:
    case DataType::VoiceSync:
    case DataType::Voice:
        return true;
    default:
        return false;
    }
}

} // namespace bmrf
