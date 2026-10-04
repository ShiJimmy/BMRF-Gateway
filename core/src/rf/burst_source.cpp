#include "bmrf/rf/burst_source.hpp"

#include <cstdio>
#include <cstring>

namespace bmrf::rf {

namespace {

constexpr size_t kRecordSize = 4 + 4 + 4 + 4 + 4 + 1 + 1 + 24 + kDmrBurstDibits; // 178

class NullBurstSource : public IDmrBurstSource {
public:
    bool open(const std::string&, std::string&) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    bool read(DMRBurst&) override { return false; }
    const char* name() const override { return "null"; }

private:
    bool m_open = false;
};

class FileBurstSource : public IDmrBurstSource {
public:
    explicit FileBurstSource(std::string path) : m_path(std::move(path)) {}

    ~FileBurstSource() override { close(); }

    bool open(const std::string& spec, std::string& error) override
    {
        close();
        const std::string path = spec.empty() ? m_path : spec;
        m_file = std::fopen(path.c_str(), "rb");
        if (m_file == nullptr) {
            error = "cannot open burst capture: " + path;
            return false;
        }
        m_open = true;
        return true;
    }

    void close() override
    {
        if (m_file != nullptr) {
            std::fclose(m_file);
            m_file = nullptr;
        }
        m_open = false;
    }

    bool isOpen() const override { return m_open; }

    bool read(DMRBurst& burst) override
    {
        if (m_file == nullptr)
            return false;

        uint8_t record[kRecordSize];
        if (std::fread(record, 1, kRecordSize, m_file) != kRecordSize)
            return false;

        auto readU32 = [](const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        };
        auto readF32 = [](const uint8_t* p) {
            const uint32_t raw = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            return value;
        };

        burst = DMRBurst();
        burst.timeslot = static_cast<int>(readU32(record + 0));
        burst.color_code = static_cast<int>(readU32(record + 4));
        burst.rssi_dbm = readF32(record + 8);
        burst.snr_db = readF32(record + 12);
        burst.ber_percent = readF32(record + 16);
        burst.has_metrics = true;
        burst.sync = static_cast<SyncType>(record[20]);
        burst.has_cach = record[21] != 0;
        std::memcpy(burst.cach.data(), record + 22, 24);
        std::memcpy(burst.dibits.data(), record + 46, kDmrBurstDibits);
        return true;
    }

    const char* name() const override { return "file"; }

private:
    std::string m_path;
    std::FILE* m_file = nullptr;
    bool m_open = false;
};

} // namespace

std::unique_ptr<IDmrBurstSource> createNullBurstSource()
{
    return std::make_unique<NullBurstSource>();
}

std::unique_ptr<IDmrBurstSource> createFileBurstSource(const std::string& path)
{
    return std::make_unique<FileBurstSource>(path);
}

} // namespace bmrf::rf
