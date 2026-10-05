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
    // MMDVMHost/DroidStar format latitude/longitude with "%08f"/"%09f" (zero
    // padded) and then truncate to the field width; a space-padded field makes
    // BrandMeister answer MSTNAK.
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%0*f", width, value);
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

namespace {

void appendRawId(std::vector<uint8_t>& out, uint32_t id)
{
    out.push_back(static_cast<uint8_t>((id >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((id >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(id & 0xFF));
}

} // namespace

std::vector<uint8_t> HomebrewEncoder::buildLogin(uint32_t repeater_id, HomebrewDialect dialect)
{
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterLogin);
    if (dialect == HomebrewDialect::Ascii)
        appendString(out, hexId(repeater_id));
    else
        appendRawId(out, repeater_id);
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildKey(uint32_t repeater_id,
                                               const uint8_t* salt, size_t saltLen,
                                               const std::string& password,
                                               HomebrewDialect dialect)
{
    std::vector<uint8_t> material(salt, salt + saltLen);
    material.insert(material.end(), password.begin(), password.end());

    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterKey);
    if (dialect == HomebrewDialect::Ascii) {
        appendString(out, hexId(repeater_id));
        appendString(out, sha256Hex(material.data(), material.size()));
    } else {
        appendRawId(out, repeater_id);
        const std::vector<uint8_t> digest = sha256(material.data(), material.size());
        out.insert(out.end(), digest.begin(), digest.end()); // 32 raw bytes
    }
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildConfig(const RepeaterConfig& config,
                                                  HomebrewDialect dialect)
{
    // MMDVMHost RPTC layout (matches DroidStar/MMDVMHost, which BrandMeister
    // accepts for transmit): "RPTC" + 4 raw id bytes + callsign(8) + rx(9) +
    // tx(9) + power(2) + cc(2) + lat(8) + lon(9) + height(3) + location(20) +
    // description(19) + slots(1) + url(124) + software(40) + package(40) = 302.
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterConfig);
    // The ASCII/Homebrew dialect (which our RPTL/RPTK use) advertises the ID as
    // 8 hex characters after the callsign and has no slots byte; the
    // MMDVM/raw dialect uses 4 raw id bytes before the callsign and a slots byte.
    // Selectable for diagnostics.
    const bool asciiId = (dialect == HomebrewDialect::Ascii);
    if (asciiId) {
        appendString(out, padRight(config.callsign, 8));
        appendString(out, hexId(config.id));
    } else {
        out.push_back(static_cast<uint8_t>((config.id >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((config.id >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((config.id >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(config.id & 0xFF));
        appendString(out, padRight(config.callsign, 8));
    }
    appendString(out, formatNumber(config.rx_freq, 9));
    appendString(out, formatNumber(config.tx_freq, 9));
    appendString(out, formatNumber(config.tx_power, 2));
    appendString(out, formatNumber(config.color_code, 2));
    appendString(out, formatFloat(config.latitude, 8));
    appendString(out, formatFloat(config.longitude, 9));
    appendString(out, formatNumber(config.height, 3));
    appendString(out, padRight(config.location, 20));
    if (asciiId) {
        appendString(out, padRight(config.description, 20));
    } else {
        appendString(out, padRight(config.description, 19));
        out.push_back('4'); // slots: 4 = simplex/hotspot (matches DroidStar)
    }
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

std::vector<uint8_t> HomebrewEncoder::buildPing(uint32_t repeater_id, HomebrewDialect dialect)
{
    std::vector<uint8_t> out;
    if (dialect == HomebrewDialect::Ascii) {
        appendString(out, sig::kMasterPing);
        appendString(out, hexId(repeater_id));
    } else {
        appendString(out, sig::kRepeaterPingNative);
        appendRawId(out, repeater_id);
    }
    return out;
}

std::vector<uint8_t> HomebrewEncoder::buildClose(uint32_t repeater_id, HomebrewDialect dialect)
{
    std::vector<uint8_t> out;
    appendString(out, sig::kRepeaterClose);
    if (dialect == HomebrewDialect::Ascii)
        appendString(out, hexId(repeater_id));
    else
        appendRawId(out, repeater_id);
    return out;
}

} // namespace bmrf::brandmeister
