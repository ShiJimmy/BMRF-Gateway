// Replay a layered capture written by bmrf::captureWrite (design doc §21/§22).
//
// Reads a text capture file ("<stage> <ts> <hex-or-text>" per line) and drives
// the BM parser offline so the same RF/BM traffic can be re-examined without
// re-receiving it.

#include "bmrf/brandmeister/rx.hpp"
#include "bmrf/common/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> fromHex(const std::string& hex)
{
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

void dumpDmrd(const std::vector<uint8_t>& bytes)
{
    using namespace bmrf::brandmeister;
    const Packet packet = HomebrewParser::parse(bytes.data(), bytes.size());
    if (packet.type != PacketType::DmrData || !packet.dmr_data) {
        std::cout << "    (not DMRD)\n";
        return;
    }
    const DMRData& d = *packet.dmr_data;
    const bmrf::DMRFrame frame = HomebrewParser::toFrame(d, 0);
    std::cout << "    src=" << d.source_id << " dst=" << d.destination_id
              << " slot=" << d.slot() << " opt=0x" << std::hex
              << static_cast<int>(d.options) << std::dec
              << " dt=" << bmrf::toString(frame.data_type)
              << " seq=" << static_cast<int>(frame.sequence)
              << " ct=" << bmrf::toString(frame.call_type) << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <capture.txt>\n";
        return 1;
    }

    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }

    size_t total = 0;
    size_t dmrd = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string stage;
        uint64_t ts = 0;
        std::string payload;
        if (!(ss >> stage >> ts))
            continue;
        ++total;

        if (stage == "bm_rx" || stage == "bm_tx") {
            ss >> payload;
            const std::vector<uint8_t> bytes = fromHex(payload);
            std::cout << "[" << stage << "] ts=" << ts << " len=" << bytes.size() << "\n";
            dumpDmrd(bytes);
            ++dmrd;
        } else {
            std::getline(ss, payload);
            std::cout << "[" << stage << "] ts=" << ts << payload << "\n";
        }
    }

    std::cout << "\nrecords=" << total << " dmrd=" << dmrd << "\n";
    return 0;
}
