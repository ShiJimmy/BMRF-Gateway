#include "bmrf/brandmeister/tx.hpp"

#include "bmrf/common/sha256.hpp"

#include <cstdio>
#include <cstring>

namespace bmrf::brandmeister {

namespace {

void appendString(std::vector<uint8_t>& out, const std::string& value)
{
    out.insert(out.end(), value.begin(), value.end());
}

std::string padRight(const std::string& value, size_t width)
{
    if (value.size() >= width)
        return value.substr(0, width);
    return value + std::string(width - value.size(), ' ');
}

std::string formatNumber(unsigned long long value, int width)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%0*llu", width, value);
    return std::string(buffer);
}

std::string formatFloat(double value, int width)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%-*f", width, value);
    std::string text(buffer);
    if (static_cast<int>(text.size()) > width)
        text = text.substr(0, static_cast<size_t>(width));
    return text;
}

} // namespace

std::string HomebrewEncoder::hexId(uint32_t repeater_id)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%08x", repeater_id);
    return std::string(buffer);
}

std::vector<uint8_t> HomebrewEncoder::buildLogin(uint32_t repeater_id)
{
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterLogin);
    appendString(out, hexId(repeater_id));
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildKey(uint32_t repeater_id,
                                               const std::array<uint8_t, 8>& nonce,
                                               const std::string& password)
{
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterKey);
    appendString(out, hexId(repeater_id));

    std::vector<uint8_t> material(nonce.begin(), nonce.end());
    material.insert(material.end(), password.begin(), password.end());
    appendString(out, sha256Hex(material.data(), material.size()));
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildConfig(const RepeaterConfig& config)
{
    // MMDVMHost RPTC layout (matches DroidStar/MMDVMHost, which BrandMeister
    // accepts for transmit): "RPTC" + 4 raw id bytes + callsign(8) + rx(9) +
    // tx(9) + power(2) + cc(2) + lat(8) + lon(9) + height(3) + location(20) +
    // description(19) + slots(1) + url(124) + software(40) + package(40) = 302.
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterConfig);
    out.push_back(static_cast<uint8_t>((config.id >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((config.id >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((config.id >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(config.id & 0xFF));
    appendString(out, padRight(config.callsign, 8));
    appendString(out, formatNumber(config.rx_freq, 9));
    appendString(out, formatNumber(config.tx_freq, 9));
    appendString(out, formatNumber(config.tx_power, 2));
    appendString(out, formatNumber(config.color_code, 2));
    appendString(out, formatFloat(config.latitude, 8));
    appendString(out, formatFloat(config.longitude, 9));
    appendString(out, formatNumber(config.height, 3));
    appendString(out, padRight(config.location, 20));
    appendString(out, padRight(config.description, 19));
    out.push_back('4'); // slots: 4 = simplex / DMO
    appendString(out, padRight(config.url, 124));
    appendString(out, padRight(config.software_id, 40));
    appendString(out, padRight(config.package_id, 40));
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildDmrData(const DMRData& data)
{
    std::vector<uint8_t> buffer(kWirePacketLength, 0x00);
    buffer[0] = 'D';
    buffer[1] = 'M';
    buffer[2] = 'R';
    buffer[3] = 'D';

    buffer[4] = data.sequence;

    buffer[5] = static_cast<uint8_t>((data.source_id >> 16) & 0xFF);
    buffer[6] = static_cast<uint8_t>((data.source_id >> 8) & 0xFF);
    buffer[7] = static_cast<uint8_t>(data.source_id & 0xFF);

    buffer[8] = static_cast<uint8_t>((data.destination_id >> 16) & 0xFF);
    buffer[9] = static_cast<uint8_t>((data.destination_id >> 8) & 0xFF);
    buffer[10] = static_cast<uint8_t>(data.destination_id & 0xFF);

    buffer[11] = static_cast<uint8_t>((data.repeater_id >> 24) & 0xFF);
    buffer[12] = static_cast<uint8_t>((data.repeater_id >> 16) & 0xFF);
    buffer[13] = static_cast<uint8_t>((data.repeater_id >> 8) & 0xFF);
    buffer[14] = static_cast<uint8_t>(data.repeater_id & 0xFF);

    buffer[15] = data.options;

    buffer[16] = static_cast<uint8_t>((data.stream_id >> 24) & 0xFF);
    buffer[17] = static_cast<uint8_t>((data.stream_id >> 16) & 0xFF);
    buffer[18] = static_cast<uint8_t>((data.stream_id >> 8) & 0xFF);
    buffer[19] = static_cast<uint8_t>(data.stream_id & 0xFF);

    std::memcpy(buffer.data() + 20, data.data.data(), kDmrPayloadBytes);

    buffer[53] = data.ber;
    buffer[54] = data.rssi;

    return buffer;
}

std::vector<uint8_t> HomebrewEncoder::buildPing(uint32_t repeater_id)
{
    std::vector<uint8_t> out;
    appendString(out, sig::kMasterPing);
    appendString(out, hexId(repeater_id));
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildClose(uint32_t repeater_id)
{
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterClose);
    appendString(out, hexId(repeater_id));
    return out;
}

} // namespace bmrf::brandmeister
