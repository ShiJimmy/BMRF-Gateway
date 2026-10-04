// DMR Full Link Control encoder (Voice LC Header / Terminator payloads).
//
// Ported from MMDVMHost (GPL-2.0+): RS129 (RS(12,9)), BPTC(196,96) with
// Hamming(15,11,3)/Hamming(13,9,3), and the Golay(20,8) slot-type code. The FEC
// tables live in lc_tables.inc.

#include "bmrf/dmr/lc_encode.hpp"

#include "lc_tables.inc"

#include <cstring>

namespace bmrf {

namespace {

constexpr unsigned int kNPAR = 3U;

void byteToBitsBE(uint8_t byte, bool* bits)
{
    bits[0] = (byte & 0x80U) == 0x80U;
    bits[1] = (byte & 0x40U) == 0x40U;
    bits[2] = (byte & 0x20U) == 0x20U;
    bits[3] = (byte & 0x10U) == 0x10U;
    bits[4] = (byte & 0x08U) == 0x08U;
    bits[5] = (byte & 0x04U) == 0x04U;
    bits[6] = (byte & 0x02U) == 0x02U;
    bits[7] = (byte & 0x01U) == 0x01U;
}

void bitsToByteBE(const bool* bits, uint8_t& byte)
{
    byte = bits[0] ? 0x80U : 0x00U;
    byte |= bits[1] ? 0x40U : 0x00U;
    byte |= bits[2] ? 0x20U : 0x00U;
    byte |= bits[3] ? 0x10U : 0x00U;
    byte |= bits[4] ? 0x08U : 0x00U;
    byte |= bits[5] ? 0x04U : 0x00U;
    byte |= bits[6] ? 0x02U : 0x00U;
    byte |= bits[7] ? 0x01U : 0x00U;
}

uint8_t gmult(uint8_t a, uint8_t b)
{
    if (a == 0U || b == 0U)
        return 0U;
    return kExpTable[static_cast<unsigned int>(kLogTable[a]) +
                     static_cast<unsigned int>(kLogTable[b])];
}

// Reed-Solomon (12,9) parity.
void rs129Encode(const uint8_t* msg, unsigned int nbytes, uint8_t parity[4])
{
    for (unsigned int i = 0U; i < kNPAR + 1U; i++)
        parity[i] = 0x00U;

    for (unsigned int i = 0U; i < nbytes; i++) {
        const uint8_t dbyte = msg[i] ^ parity[kNPAR - 1U];
        for (int j = static_cast<int>(kNPAR) - 1; j > 0; j--)
            parity[j] = parity[j - 1] ^ gmult(kPoly[j], dbyte);
        parity[0] = gmult(kPoly[0], dbyte);
    }
}

void encode15113_2(bool* d)
{
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    d[13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    d[14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
}

void encode1393(bool* d)
{
    d[9]  = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    d[10] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
}

// BPTC(196,96) encode of a 12-byte block into a 33-byte burst payload.
void bptcEncode(const uint8_t in[12], uint8_t out[33])
{
    bool deInter[196];
    bool raw[196];
    for (unsigned int i = 0U; i < 196U; i++) {
        deInter[i] = false;
        raw[i] = false;
    }

    bool bData[96];
    byteToBitsBE(in[0],  bData + 0);
    byteToBitsBE(in[1],  bData + 8);
    byteToBitsBE(in[2],  bData + 16);
    byteToBitsBE(in[3],  bData + 24);
    byteToBitsBE(in[4],  bData + 32);
    byteToBitsBE(in[5],  bData + 40);
    byteToBitsBE(in[6],  bData + 48);
    byteToBitsBE(in[7],  bData + 56);
    byteToBitsBE(in[8],  bData + 64);
    byteToBitsBE(in[9],  bData + 72);
    byteToBitsBE(in[10], bData + 80);
    byteToBitsBE(in[11], bData + 88);

    unsigned int pos = 0U;
    for (unsigned int a = 4U; a <= 11U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 16U; a <= 26U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 31U; a <= 41U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 46U; a <= 56U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 61U; a <= 71U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 76U; a <= 86U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 91U; a <= 101U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 106U; a <= 116U; a++, pos++) deInter[a] = bData[pos];
    for (unsigned int a = 121U; a <= 131U; a++, pos++) deInter[a] = bData[pos];

    // Hamming(15,11,3) on the 9 data rows.
    for (unsigned int r = 0U; r < 9U; r++) {
        const unsigned int p = (r * 15U) + 1U;
        encode15113_2(deInter + p);
    }

    // Hamming(13,9,3) on the 15 columns.
    for (unsigned int c = 0U; c < 15U; c++) {
        bool col[13];
        unsigned int p = c + 1U;
        for (unsigned int a = 0U; a < 13U; a++) {
            col[a] = deInter[p];
            p += 15U;
        }
        encode1393(col);
        p = c + 1U;
        for (unsigned int a = 0U; a < 13U; a++) {
            deInter[p] = col[a];
            p += 15U;
        }
    }

    // Interleave.
    for (unsigned int a = 0U; a < 196U; a++)
        raw[(a * 181U) % 196U] = deInter[a];

    // Extract binary.
    bitsToByteBE(raw + 0U,   out[0U]);
    bitsToByteBE(raw + 8U,   out[1U]);
    bitsToByteBE(raw + 16U,  out[2U]);
    bitsToByteBE(raw + 24U,  out[3U]);
    bitsToByteBE(raw + 32U,  out[4U]);
    bitsToByteBE(raw + 40U,  out[5U]);
    bitsToByteBE(raw + 48U,  out[6U]);
    bitsToByteBE(raw + 56U,  out[7U]);
    bitsToByteBE(raw + 64U,  out[8U]);
    bitsToByteBE(raw + 72U,  out[9U]);
    bitsToByteBE(raw + 80U,  out[10U]);
    bitsToByteBE(raw + 88U,  out[11U]);

    uint8_t byte = 0U;
    bitsToByteBE(raw + 96U, byte);
    out[12U] = (out[12U] & 0x3FU) | ((byte >> 0) & 0xC0U);
    out[20U] = (out[20U] & 0xFCU) | ((byte >> 4) & 0x03U);

    bitsToByteBE(raw + 100U, out[21U]);
    bitsToByteBE(raw + 108U, out[22U]);
    bitsToByteBE(raw + 116U, out[23U]);
    bitsToByteBE(raw + 124U, out[24U]);
    bitsToByteBE(raw + 132U, out[25U]);
    bitsToByteBE(raw + 140U, out[26U]);
    bitsToByteBE(raw + 148U, out[27U]);
    bitsToByteBE(raw + 156U, out[28U]);
    bitsToByteBE(raw + 164U, out[29U]);
    bitsToByteBE(raw + 172U, out[30U]);
    bitsToByteBE(raw + 180U, out[31U]);
    bitsToByteBE(raw + 188U, out[32U]);
}

// Golay(20,8) slot type (color code + data type) placed into the burst payload.
void slotTypeEncode(uint8_t colorCode, uint8_t dataType, uint8_t data[33])
{
    uint8_t st[3];
    st[0] = static_cast<uint8_t>(((colorCode << 4) & 0xF0U) | (dataType & 0x0FU));
    st[1] = 0x00U;
    st[2] = 0x00U;

    const unsigned int cksum = kGolay2087Enc[st[0]];
    st[1] = static_cast<uint8_t>(cksum & 0xFFU);
    st[2] = static_cast<uint8_t>(cksum >> 8);

    data[12U] = static_cast<uint8_t>((data[12U] & 0xC0U) | ((st[0] >> 2) & 0x3FU));
    data[13U] = static_cast<uint8_t>((data[13U] & 0x0FU) | ((st[0] << 6) & 0xC0U) |
                                     ((st[1] >> 2) & 0x30U));
    data[19U] = static_cast<uint8_t>((data[19U] & 0xF0U) | ((st[1] >> 2) & 0x0FU));
    data[20U] = static_cast<uint8_t>((data[20U] & 0x03U) | ((st[1] << 6) & 0xC0U) |
                                     ((st[2] >> 2) & 0x3CU));
}

} // namespace

bool encodeDmrLcPayload(uint8_t flco, uint8_t featureSetId, uint8_t serviceOptions,
                        uint32_t destinationId, uint32_t sourceId, uint8_t colorCode,
                        uint8_t dataType, uint8_t out[33])
{
    if (dataType != 1U && dataType != 2U)
        return false;

    uint8_t lc[12];
    std::memset(lc, 0, sizeof(lc));
    lc[0] = static_cast<uint8_t>(flco & 0x3FU); // FLCO (no PF/R bits)
    lc[1] = featureSetId;
    lc[2] = serviceOptions;
    lc[3] = static_cast<uint8_t>((destinationId >> 16) & 0xFFU);
    lc[4] = static_cast<uint8_t>((destinationId >> 8) & 0xFFU);
    lc[5] = static_cast<uint8_t>(destinationId & 0xFFU);
    lc[6] = static_cast<uint8_t>((sourceId >> 16) & 0xFFU);
    lc[7] = static_cast<uint8_t>((sourceId >> 8) & 0xFFU);
    lc[8] = static_cast<uint8_t>(sourceId & 0xFFU);

    uint8_t parity[4];
    rs129Encode(lc, 9U, parity);

    const uint8_t mask = (dataType == 1U) ? 0x96U : 0x99U;
    lc[9]  = static_cast<uint8_t>(parity[2] ^ mask);
    lc[10] = static_cast<uint8_t>(parity[1] ^ mask);
    lc[11] = static_cast<uint8_t>(parity[0] ^ mask);

    std::memset(out, 0, 33);

    // DMR data-sync pattern (MS sourced) lives in payload bytes 13..19. BrandMeister
    // rejects a data-sync burst (Voice LC Header / Terminator) without it.
    static const uint8_t kDataSync[7] = {0x0DU, 0x5DU, 0x7FU, 0x77U, 0xFDU, 0x75U, 0x70U};
    static const uint8_t kSyncMask[7] = {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U};
    for (int i = 0; i < 7; ++i)
        out[13 + i] = static_cast<uint8_t>((out[13 + i] & ~kSyncMask[i]) | kDataSync[i]);

    slotTypeEncode(colorCode, dataType, out);
    bptcEncode(lc, out);
    return true;
}

// ---------------------------------------------------------------------------
// DMR voice-burst embedded signalling (ported from MMDVMHost / DroidStar).
// ---------------------------------------------------------------------------
namespace {

uint8_t fiveBitCrc(const uint8_t lc[9])
{
    uint16_t total = 0U;
    for (int i = 0; i < 9; ++i)
        total = static_cast<uint16_t>(total + lc[i]);
    return static_cast<uint8_t>(total % 31U);
}

void hamming16114(bool* d)
{
    d[11] = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    d[12] = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    d[13] = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    d[14] = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
    d[15] = d[0] ^ d[2] ^ d[5] ^ d[6] ^ d[8] ^ d[9] ^ d[10];
}

void qr1676(uint8_t* data)
{
    const uint32_t value = (data[0] >> 1) & 0x7FU;
    const uint32_t cksum = kQr1676[value];
    data[0] = static_cast<uint8_t>(cksum >> 8);
    data[1] = static_cast<uint8_t>(cksum & 0xFFU);
}

// EMB: colour code + link control status (bytes 13,14,18,19).
void applyEmb(uint8_t payload[33], uint8_t lcss, uint8_t colorCode)
{
    uint8_t emb[2];
    emb[0] = static_cast<uint8_t>((colorCode << 4) & 0xF0U);
    emb[0] |= static_cast<uint8_t>((lcss << 1) & 0x06U);
    emb[1] = 0x00U;

    qr1676(emb);

    payload[13] = static_cast<uint8_t>((payload[13] & 0xF0U) | ((emb[0] >> 4U) & 0x0FU));
    payload[14] = static_cast<uint8_t>((payload[14] & 0x0FU) | ((emb[0] << 4U) & 0xF0U));
    payload[18] = static_cast<uint8_t>((payload[18] & 0xF0U) | ((emb[1] >> 4U) & 0x0FU));
    payload[19] = static_cast<uint8_t>((payload[19] & 0x0FU) | ((emb[1] << 4U) & 0xF0U));
}

// Build the 128-bit embedded LC block for the whole superframe.
void buildEmbeddedRaw(const uint8_t lc[9], bool mRaw[128])
{
    bool mData[72];
    for (int i = 0; i < 9; ++i)
        byteToBitsBE(lc[i], mData + i * 8);

    const uint8_t crc = fiveBitCrc(lc);

    bool data[128];
    for (int i = 0; i < 128; ++i)
        data[i] = false;
    data[106] = (crc & 0x01U) != 0;
    data[90] = (crc & 0x02U) != 0;
    data[74] = (crc & 0x04U) != 0;
    data[58] = (crc & 0x08U) != 0;
    data[42] = (crc & 0x10U) != 0;

    uint32_t b = 0U;
    for (uint32_t a = 0U; a < 11U; a++, b++) data[a] = mData[b];
    for (uint32_t a = 16U; a < 27U; a++, b++) data[a] = mData[b];
    for (uint32_t a = 32U; a < 42U; a++, b++) data[a] = mData[b];
    for (uint32_t a = 48U; a < 58U; a++, b++) data[a] = mData[b];
    for (uint32_t a = 64U; a < 74U; a++, b++) data[a] = mData[b];
    for (uint32_t a = 80U; a < 90U; a++, b++) data[a] = mData[b];
    for (uint32_t a = 96U; a < 106U; a++, b++) data[a] = mData[b];

    for (uint32_t a = 0U; a < 112U; a += 16U)
        hamming16114(data + a);

    for (uint32_t a = 0U; a < 16U; a++)
        data[a + 112U] = data[a + 0U] ^ data[a + 16U] ^ data[a + 32U] ^ data[a + 48U] ^
                         data[a + 64U] ^ data[a + 80U] ^ data[a + 96U];

    b = 0U;
    for (uint32_t a = 0U; a < 128U; a++) {
        mRaw[a] = data[b];
        b += 16U;
        if (b > 127U)
            b -= 127U;
    }
}

// Slice one 32-bit chunk of the embedded LC into the voice payload (bytes 14..18).
uint8_t sliceEmbeddedData(uint8_t payload[33], const bool mRaw[128], int n)
{
    if (n >= 1 && n < 5) {
        const int nn = n - 1;
        bool bits[40];
        for (int i = 0; i < 40; ++i)
            bits[i] = false;
        for (int i = 0; i < 32; ++i)
            bits[4 + i] = mRaw[nn * 32 + i];

        uint8_t bytes[5];
        bitsToByteBE(bits + 0U, bytes[0]);
        bitsToByteBE(bits + 8U, bytes[1]);
        bitsToByteBE(bits + 16U, bytes[2]);
        bitsToByteBE(bits + 24U, bytes[3]);
        bitsToByteBE(bits + 32U, bytes[4]);

        payload[14] = static_cast<uint8_t>((payload[14] & 0xF0U) | (bytes[0] & 0x0FU));
        payload[15] = bytes[1];
        payload[16] = bytes[2];
        payload[17] = bytes[3];
        payload[18] = static_cast<uint8_t>((payload[18] & 0x0FU) | (bytes[4] & 0xF0U));

        switch (nn) {
        case 0: return 1U;
        case 3: return 2U;
        default: return 3U;
        }
    }

    payload[14] &= 0xF0U;
    payload[15] = 0x00U;
    payload[16] = 0x00U;
    payload[17] = 0x00U;
    payload[18] &= 0x0FU;
    return 0U;
}

} // namespace

void applyVoiceSignalling(uint8_t payload[33], const uint8_t lc[9], int n,
                          uint8_t colorCode)
{
    if (n == 0) {
        // Voice Sync burst: MS-sourced audio sync pattern (merged into 13..19).
        static const uint8_t kAudioSync[7] = {0x07U, 0xF7U, 0xD5U, 0xDDU, 0x57U, 0xDFU, 0xD0U};
        static const uint8_t kSyncMask[7] = {0x0FU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xF0U};
        for (int i = 0; i < 7; ++i)
            payload[13 + i] =
                static_cast<uint8_t>((payload[13 + i] & ~kSyncMask[i]) | kAudioSync[i]);
        return;
    }

    bool mRaw[128];
    buildEmbeddedRaw(lc, mRaw);
    const uint8_t lcss = sliceEmbeddedData(payload, mRaw, n);
    applyEmb(payload, lcss, colorCode);
}

} // namespace bmrf
