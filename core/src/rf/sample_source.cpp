#include "bmrf/rf/sample_source.hpp"

namespace bmrf::rf {

namespace {

class NullSampleSource : public ISampleSource {
public:
    bool open(const std::string&, std::string&) override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    int read(uint8_t*, size_t) override { return 0; } // end of stream
    uint32_t sampleRate() const override { return 0; }
    const char* name() const override { return "null"; }

private:
    bool m_open = false;
};

} // namespace

std::unique_ptr<ISampleSource> createNullSampleSource()
{
    return std::make_unique<NullSampleSource>();
}

} // namespace bmrf::rf
