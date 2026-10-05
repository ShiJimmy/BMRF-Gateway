#include "bmrf/brandmeister/homebrew.hpp"

namespace bmrf::brandmeister {

int DMRData::slot() const
{
    return (options & static_cast<uint8_t>(OptionsMask::SlotTs2)) != 0 ? 2 : 1;
}

bool DMRData::isPrivate() const
{
    return (options & static_cast<uint8_t>(OptionsMask::Private)) != 0;
}

bool DMRData::dataSync() const
{
    return (options & static_cast<uint8_t>(OptionsMask::DataSync)) != 0;
}

bool DMRData::voiceSync() const
{
    return (options & static_cast<uint8_t>(OptionsMask::DataSync)) == 0 &&
           (options & static_cast<uint8_t>(OptionsMask::VoiceSync)) != 0;
}

uint8_t DMRData::voiceFrameIndex() const
{
    return static_cast<uint8_t>(options & static_cast<uint8_t>(OptionsMask::LowNibble));
}

uint8_t DMRData::dataType() const
{
    return static_cast<uint8_t>(options & static_cast<uint8_t>(OptionsMask::LowNibble));
}

} // namespace bmrf::brandmeister
