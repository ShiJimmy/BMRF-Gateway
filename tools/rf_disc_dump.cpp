// rf_disc_dump - export the FM discriminator (baseband audio) of a cu8 capture
// as a 48 kHz WAV, so an external decoder (DSD-FME / OpenEar) can be used as a
// reference on exactly the same signal.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

void writeWav(const std::string& path, const std::vector<int16_t>& pcm, uint32_t rate)
{
    std::ofstream out(path, std::ios::binary);
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: rf_disc_dump <file.cu8> [inRate=240000] [decim=5]\n");
        return 2;
    }
    const std::string path = argv[1];
    const uint32_t inRate = argc >= 3 ? static_cast<uint32_t>(std::atoi(argv[2])) : 240000u;
    const int decim = argc >= 4 ? std::atoi(argv[3]) : 5;

    const std::vector<uint8_t> iq = readFile(path);
    if (iq.size() < 4) {
        std::printf("cannot read %s\n", path.c_str());
        return 1;
    }
    const size_t samples = iq.size() / 2;
    const uint32_t outRate = inRate / static_cast<uint32_t>(decim);
    std::printf("samples=%zu -> %u Hz\n", samples, outRate);

    std::vector<int16_t> pcm;
    pcm.reserve(samples / static_cast<size_t>(decim) + 1);

    double dc = 0.0;
    double acc = 0.0;
    int count = 0;
    const double scale = 4000.0;
    for (size_t n = 1; n < samples; ++n) {
        const double i0 = static_cast<double>(iq[2 * (n - 1)]) - 127.5;
        const double q0 = static_cast<double>(iq[2 * (n - 1) + 1]) - 127.5;
        const double i1 = static_cast<double>(iq[2 * n]) - 127.5;
        const double q1 = static_cast<double>(iq[2 * n + 1]) - 127.5;
        const double value = std::atan2(q0 * i1 - i0 * q1, i0 * i1 + q0 * q1);
        dc += (value - dc) * 0.001;
        acc += value - dc;
        if (++count >= decim) {
            double v = acc / decim * scale;
            v = std::max(-32767.0, std::min(32767.0, v));
            pcm.push_back(static_cast<int16_t>(std::lround(v)));
            acc = 0.0;
            count = 0;
        }
    }

    const std::string out = "disc_48k.wav";
    writeWav(out, pcm, outRate);
    std::printf("wrote %s (%zu samples)\n", out.c_str(), pcm.size());
    return 0;
}
