#include "bmrf/common/sha256.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace bmrf {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32u - bits));
}

class Sha256Context {
public:
    void update(const uint8_t* data, size_t length)
    {
        m_length += length;
        while (length > 0) {
            const size_t space = 64u - m_bufferLength;
            const size_t take = (length < space) ? length : space;
            std::memcpy(m_buffer + m_bufferLength, data, take);
            m_bufferLength += take;
            data += take;
            length -= take;
            if (m_bufferLength == 64u) {
                process(m_buffer);
                m_bufferLength = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish()
    {
        const uint64_t bitLength = m_length * 8u;
        const uint8_t one = 0x80;
        update(&one, 1u);
        const uint8_t zero = 0x00;
        while (m_bufferLength != 56u)
            update(&zero, 1u);

        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = static_cast<uint8_t>(bitLength >> (56 - 8 * i));
        update(lengthBytes, 8u);

        std::array<uint8_t, 32> digest{};
        for (int i = 0; i < 8; ++i) {
            digest[4 * i + 0] = static_cast<uint8_t>(m_state[i] >> 24);
            digest[4 * i + 1] = static_cast<uint8_t>(m_state[i] >> 16);
            digest[4 * i + 2] = static_cast<uint8_t>(m_state[i] >> 8);
            digest[4 * i + 3] = static_cast<uint8_t>(m_state[i]);
        }
        return digest;
    }

private:
    void process(const uint8_t* block)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[4 * i + 0]) << 24) |
                   (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(block[4 * i + 2]) << 8) |
                   (static_cast<uint32_t>(block[4 * i + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<uint32_t, 8> m_state{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    uint64_t m_length = 0;
    uint8_t m_buffer[64]{};
    size_t m_bufferLength = 0;
};

} // namespace

std::vector<uint8_t> sha256(const uint8_t* data, size_t length)
{
    Sha256Context context;
    if (data != nullptr && length > 0)
        context.update(data, length);
    const auto digest = context.finish();
    return std::vector<uint8_t>(digest.begin(), digest.end());
}

std::string sha256Hex(const uint8_t* data, size_t length)
{
    const auto digest = sha256(data, length);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2u);
    for (uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

} // namespace bmrf
