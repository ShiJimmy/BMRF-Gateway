#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bmrf {

// Minimal, self-contained SHA-256 (FIPS 180-4). Used for the Homebrew "RPTK"
// password hash: sha256(nonce || password), hex encoded.
std::vector<uint8_t> sha256(const uint8_t* data, size_t len);

std::string sha256Hex(const uint8_t* data, size_t len);

inline std::vector<uint8_t> sha256(const std::vector<uint8_t>& data)
{
    return sha256(data.data(), data.size());
}

inline std::vector<uint8_t> sha256(const std::string& data)
{
    return sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

} // namespace bmrf
