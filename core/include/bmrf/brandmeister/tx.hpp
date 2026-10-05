#pragma once

#include "bmrf/brandmeister/homebrew.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bmrf::brandmeister {

// Repeater configuration advertised to the master in the "RPTC" packet.
struct RepeaterConfig {
    std::string callsign;
    uint32_t id = 0;
    uint32_t rx_freq = 0; // Hz
    uint32_t tx_freq = 0; // Hz
    uint8_t tx_power = 0; // dBm [0,99]
    uint8_t color_code = 1;
    double latitude = 0.0;
    double longitude = 0.0;
    uint16_t height = 0; // meters
    std::string location;
    std::string description;
    std::string url;
    std::string software_id = "BMRF-Gateway/0.1";
    std::string package_id = "android";
};

// Homebrew TX / encoder. Mirrors BrandMeister/go-brandmeister (homebrew/client.go)
// and MMDVMHost (DMRNetwork::write).
// Wire dialect of the Homebrew protocol. The native MMDVM/MMDVMHost dialect
// (raw 4-byte IDs, "RPTACK"/"RPTPING"/"MSTPONG") is what MMDVMHost and DroidStar
// use and is the only one BrandMeister authorises for transmission; the ASCII
// dialect (8 hex chars, "MSTACK"/"MSTPING"/"RPTPONG") is the go-brandmeister
// variant.
enum class HomebrewDialect : uint8_t {
    Native,
    Ascii,
};

class HomebrewEncoder {
public:
    static std::vector<uint8_t> buildLogin(uint32_t repeater_id,
                                           HomebrewDialect dialect = HomebrewDialect::Native);
    static std::vector<uint8_t> buildKey(uint32_t repeater_id,
                                         const uint8_t* salt, size_t saltLen,
                                         const std::string& password,
                                         HomebrewDialect dialect = HomebrewDialect::Native);
    static std::vector<uint8_t> buildConfig(const RepeaterConfig& config,
                                            HomebrewDialect dialect = HomebrewDialect::Native);
    static std::vector<uint8_t> buildDmrData(const DMRData& data);
    static std::vector<uint8_t> buildPing(uint32_t repeater_id,
                                          HomebrewDialect dialect = HomebrewDialect::Native);
    static std::vector<uint8_t> buildClose(uint32_t repeater_id,
                                           HomebrewDialect dialect = HomebrewDialect::Native);

    // "%08x" of a 24/32-bit repeater id, as used by the ASCII Homebrew dialect.
    static std::string hexId(uint32_t repeater_id);
};

} // namespace bmrf::brandmeister
