// bmrf_frame_dump - decode a raw BrandMeister Homebrew datagram and print both
// the wire-level DMRD fields and the normalized DMRFrame (design doc §21).

#include "bmrf/brandmeister/rx.hpp"
#include "bmrf/common/log.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace bmrf;
using namespace bmrf::brandmeister;

namespace {

int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool parseHex(const std::string& text, std::vector<uint8_t>& out)
{
    std::string cleaned;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            cleaned.push_back(c);
    }
    if (cleaned.size() % 2 != 0)
        return false;
    out.clear();
    out.reserve(cleaned.size() / 2);
    for (size_t i = 0; i < cleaned.size(); i += 2) {
        const int hi = hexNibble(cleaned[i]);
        const int lo = hexNibble(cleaned[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

void dump(const uint8_t* data, size_t len)
{
    const Packet packet = HomebrewParser::parse(data, len);
    std::printf("packet type : %s\n", toString(packet.type));
    std::printf("length      : %zu\n", len);

    if (packet.type == PacketType::DmrData && packet.dmr_data) {
        const DMRData& d = *packet.dmr_data;
        std::printf("  sequence  : %u\n", d.sequence);
        std::printf("  source    : %u (0x%06X)\n", d.source_id, d.source_id);
        std::printf("  dest      : %u (0x%06X)\n", d.destination_id, d.destination_id);
        std::printf("  repeater  : %u\n", d.repeater_id);
        std::printf("  stream    : 0x%08X\n", d.stream_id);
        std::printf("  slot      : %d\n", d.slot());
        std::printf("  call type : %s\n", d.isPrivate() ? "Private" : "Group");
        std::printf("  data sync : %s\n", d.dataSync() ? "yes" : "no");
        std::printf("  voice sync: %s\n", d.voiceSync() ? "yes" : "no");
        std::printf("  vframe idx: %u\n", d.voiceFrameIndex());
        if (d.has_metrics)
            std::printf("  ber / rssi: %u / %u\n", d.ber, d.rssi);

        const DMRFrame frame = HomebrewParser::toFrame(d, 0);
        std::printf("-- unified DMRFrame --\n");
        std::printf("  data type : %s\n", toString(frame.data_type));
        std::printf("  source    : %u\n", frame.source_id);
        std::printf("  dest      : %u\n", frame.destination_id);
        std::printf("  call type : %s\n", toString(frame.call_type));
        std::printf("  timeslot  : %d\n", frame.timeslot);
        std::printf("  voice     : %s\n", frame.has_voice ? "yes" : "no");
        if (frame.voice_lc) {
            const VoiceLC& lc = *frame.voice_lc;
            std::printf("  voice LC  : flco=0x%02X protect=%d fset=0x%02X svc=0x%02X\n",
                        lc.flco, lc.protect ? 1 : 0, lc.feature_set_id, lc.service_options);
        }
    } else if (packet.type == PacketType::MasterAck && packet.has_nonce) {
        std::printf("  nonce     : ");
        for (uint8_t byte : packet.nonce)
            std::printf("%02X", byte);
        std::printf("\n");
    }
}

void usage()
{
    std::printf("usage: bmrf_frame_dump --hex <hex bytes>\n");
    std::printf("       bmrf_frame_dump --file <path>\n");
    std::printf("       bmrf_frame_dump -v --hex <hex bytes>\n");
}

} // namespace

int main(int argc, char** argv)
{
    std::string hexText;
    std::string filePath;
    bool useFile = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--hex" && i + 1 < argc) {
            hexText = argv[++i];
        } else if (arg == "--file" && i + 1 < argc) {
            filePath = argv[++i];
            useFile = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }

    if (verbose)
        setLogLevel(LogLevel::Debug);

    if (useFile) {
        std::ifstream input(filePath);
        if (!input) {
            std::fprintf(stderr, "cannot open %s\n", filePath.c_str());
            return 1;
        }
        std::stringstream buffer;
        buffer << input.rdbuf();
        hexText = buffer.str();
    }

    if (hexText.empty()) {
        usage();
        return 2;
    }

    std::vector<uint8_t> bytes;
    if (!parseHex(hexText, bytes)) {
        std::fprintf(stderr, "invalid hex input\n");
        return 1;
    }

    dump(bytes.data(), bytes.size());
    return 0;
}
