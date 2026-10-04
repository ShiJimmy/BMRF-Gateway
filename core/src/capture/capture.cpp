#include "bmrf/capture/capture.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace bmrf {

namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
// Lock-free "is a capture open" flag so hot paths (RF/network threads) never
// contend on the capture mutex when capture is disabled.
std::atomic<bool> g_active{false};
uint64_t g_writes = 0; // guarded by g_mutex

std::string toHex(const uint8_t* data, size_t len)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

} // namespace

bool captureStart(const std::string& path, std::string& error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_active.store(false);
    g_writes = 0;
    g_file = std::fopen(path.c_str(), "wb");
    if (g_file == nullptr) {
        error = "cannot open capture file " + path;
        return false;
    }
    g_active.store(true);
    return true;
}

void captureStop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_active.store(false);
}

bool captureActive()
{
    return g_active.load(std::memory_order_relaxed);
}

void captureWrite(const char* stage, uint64_t timestamp_ns,
                  const uint8_t* data, size_t len)
{
    if (!g_active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == nullptr)
        return;
    const std::string hex = toHex(data, len);
    std::fprintf(g_file, "%s %llu %s\n", stage,
                 static_cast<unsigned long long>(timestamp_ns), hex.c_str());
    if ((++g_writes % 256) == 0)
        std::fflush(g_file); // bound data loss without stalling every write
}

void captureWriteText(const char* stage, uint64_t timestamp_ns,
                      const std::string& text)
{
    if (!g_active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file == nullptr)
        return;
    std::fprintf(g_file, "%s %llu %s\n", stage,
                 static_cast<unsigned long long>(timestamp_ns), text.c_str());
    if ((++g_writes % 256) == 0)
        std::fflush(g_file);
}

} // namespace bmrf
