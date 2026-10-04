#include "bmrf/rf/rtl_sdr.hpp"

#include "bmrf/common/log.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(BMRF_HAVE_RTLSDR)
#include <rtl-sdr.h>
#include <thread>
#endif

#if defined(BMRF_HAVE_RTLSDR) && defined(__ANDROID__)
extern "C" void rtlsdr_set_android_fd(int fd);
#endif

namespace bmrf::rf {

namespace {

constexpr const char* kTag = "rtlsdr";

std::mutex g_mutex;
bool g_open = false;
RtlSdrStatus g_status;

#if defined(BMRF_HAVE_RTLSDR)
bool g_loggedFirstRead = false;
#endif

std::mutex g_recordMutex;
std::FILE* g_record = nullptr;

#if defined(BMRF_HAVE_RTLSDR)
rtlsdr_dev_t* g_dev = nullptr;

// Asynchronous capture: a dedicated thread drives rtlsdr_read_async (draining
// the USB endpoints continuously) and pushes IQ into a ring. The receiver
// thread pops from the ring. Keeping the demod off the reader thread is the
// TETRA capture architecture; synchronous reads wedge/drop samples when the
// demod is slower than real time.
//
// When the consumer falls behind, the ring DROPS THE OLDEST data so the
// consumer always reads near-live samples. Keeping the newest data matters:
// retaining the oldest instead (dropping new) leaves the consumer permanently
// one ring-length behind, which showed up as ~10 s of latency.
constexpr size_t kRingCapacity = 1u * 1024u * 1024u; // bytes (~2 s at 240 kS/s)
std::mutex g_ringMutex;
std::vector<uint8_t> g_ring;
size_t g_ringHead = 0; // write index (monotonic bytes)
size_t g_ringTail = 0; // read index (monotonic bytes)
std::atomic<uint64_t> g_dropped{0};
std::thread g_reader;
std::atomic<bool> g_streaming{false};
std::atomic<bool> g_readerRunning{false};

void ringPush(const uint8_t* src, size_t len)
{
    std::lock_guard<std::mutex> lock(g_ringMutex);
    if (len > kRingCapacity) {
        // Never let a single oversized chunk overflow the ring.
        src += len - kRingCapacity;
        g_dropped.fetch_add(len - kRingCapacity, std::memory_order_relaxed);
        len = kRingCapacity;
    }
    const size_t used = g_ringHead - g_ringTail;
    if (used + len > kRingCapacity) {
        const size_t drop = used + len - kRingCapacity;
        g_ringTail += drop; // discard oldest
        g_dropped.fetch_add(drop, std::memory_order_relaxed);
    }
    const size_t pos = g_ringHead % kRingCapacity;
    const size_t first = (kRingCapacity - pos < len) ? (kRingCapacity - pos) : len;
    std::memcpy(g_ring.data() + pos, src, first);
    if (len > first)
        std::memcpy(g_ring.data(), src + first, len - first);
    g_ringHead += len;
}

size_t ringPop(uint8_t* dst, size_t maxLen)
{
    std::lock_guard<std::mutex> lock(g_ringMutex);
    const size_t used = g_ringHead - g_ringTail;
    const size_t n = (used < maxLen) ? used : maxLen;
    if (n == 0)
        return 0;
    const size_t pos = g_ringTail % kRingCapacity;
    const size_t first = (kRingCapacity - pos < n) ? (kRingCapacity - pos) : n;
    std::memcpy(dst, g_ring.data() + pos, first);
    if (n > first)
        std::memcpy(dst + first, g_ring.data(), n - first);
    g_ringTail += n;
    return n;
}

void asyncCallback(unsigned char* buf, uint32_t len, void*)
{
    {
        std::lock_guard<std::mutex> lock(g_recordMutex);
        if (g_record != nullptr)
            std::fwrite(buf, 1, len, g_record);
    }
    ringPush(buf, len);
}

// Single-shot async reader (as in the TETRA capture). If it ever returns, the
// thread exits and rtlSdrRead falls back to the synchronous reader; we do NOT
// restart it from here (calling rtlsdr_read_async while one is still active
// returns -2 and would spin).
void readerLoop()
{
    g_readerRunning.store(true);
    const int r = rtlsdr_read_async(g_dev, asyncCallback, nullptr, 0, 0);
    g_readerRunning.store(false);
    if (g_streaming.load(std::memory_order_relaxed))
        logWarn(kTag, "read_async returned (" + std::to_string(r) + "), reader stopped");
    g_streaming.store(false);
}

void startAsyncLocked()
{
    if (g_reader.joinable()) {
        if (g_readerRunning.load())
            return;         // already running
        g_reader.join();    // reap a finished reader so we can restart
    }
    if (g_dev == nullptr)
        return;
    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        g_ring.assign(kRingCapacity, 0);
        g_ringHead = 0;
        g_ringTail = 0;
    }
    g_dropped.store(0);
    g_streaming.store(true);
    g_reader = std::thread(readerLoop);
    logInfo(kTag, "async capture started (ring=" + std::to_string(kRingCapacity) + " bytes)");
}

void stopAsyncLocked()
{
    if (!g_reader.joinable())
        return;
    g_streaming.store(false);
    if (g_dev != nullptr)
        rtlsdr_cancel_async(g_dev);
    g_reader.join();
    g_readerRunning.store(false);
}
#endif

} // namespace

void rtlSdrSetAndroidFd(int fd)
{
#if defined(BMRF_HAVE_RTLSDR) && defined(__ANDROID__)
    rtlsdr_set_android_fd(fd);
    logInfo(kTag, "android usb fd=" + std::to_string(fd));
#else
    (void)fd;
#endif
}

bool rtlSdrOpen(uint32_t centerHz, uint32_t sampleRateHz, int gainTenthsDb, int ppm,
                std::string& error)
{
#if defined(BMRF_HAVE_RTLSDR)
    std::lock_guard<std::mutex> lock(g_mutex);

    // Never let an empty/invalid UI field turn into set_sample_rate(0).
    if (sampleRateHz < 100000) {
        sampleRateHz = (g_status.sample_rate_hz >= 100000) ? g_status.sample_rate_hz : 240000;
    }
    if (gainTenthsDb < 0)
        gainTenthsDb = 0;

    if (g_open && g_dev != nullptr) {
        // Already streaming: stop the reader, retune, restart.
        stopAsyncLocked();
        rtlsdr_set_sample_rate(g_dev, sampleRateHz);
        if (rtlsdr_get_sample_rate(g_dev) == 0) {
            // The resampler rejects (300 kHz, 900 kHz]; fall back to a valid rate.
            sampleRateHz = 240000;
            rtlsdr_set_sample_rate(g_dev, sampleRateHz);
        }
        rtlsdr_set_center_freq(g_dev, centerHz);
        if (gainTenthsDb == 0) {
            rtlsdr_set_tuner_gain_mode(g_dev, 0);
        } else {
            rtlsdr_set_tuner_gain_mode(g_dev, 1);
            rtlsdr_set_tuner_gain(g_dev, gainTenthsDb);
        }
        rtlsdr_set_freq_correction(g_dev, ppm);
        rtlsdr_reset_buffer(g_dev);

        g_status.center_hz = centerHz;
        g_status.sample_rate_hz = rtlsdr_get_sample_rate(g_dev);
        g_status.gain_tenths_db = gainTenthsDb;
        g_status.ppm = ppm;
        startAsyncLocked();
        logInfo(kTag, "retuned: " + std::to_string(centerHz) + " Hz, rate=" +
                          std::to_string(g_status.sample_rate_hz) + ", gain=" +
                          std::to_string(gainTenthsDb) + ", ppm=" + std::to_string(ppm));
        return true;
    }

    if (rtlsdr_open(&g_dev, 0) < 0 || g_dev == nullptr) {
        error = "rtlsdr_open failed";
        logError(kTag, error);
        return false;
    }

    rtlsdr_set_sample_rate(g_dev, sampleRateHz);
    if (rtlsdr_get_sample_rate(g_dev) == 0) {
        // The resampler rejects (300 kHz, 900 kHz]; fall back to a valid rate so
        // the demodulator's assumed rate always matches the hardware.
        sampleRateHz = 240000;
        rtlsdr_set_sample_rate(g_dev, sampleRateHz);
    }
    rtlsdr_set_center_freq(g_dev, centerHz);

    if (gainTenthsDb == 0) {
        rtlsdr_set_tuner_gain_mode(g_dev, 0); // automatic gain
    } else {
        rtlsdr_set_tuner_gain_mode(g_dev, 1);
        rtlsdr_set_tuner_gain(g_dev, gainTenthsDb);
    }
    rtlsdr_set_freq_correction(g_dev, ppm);
    rtlsdr_reset_buffer(g_dev);

    g_open = true;
    g_status = RtlSdrStatus();
    g_status.open = true;
    g_status.center_hz = centerHz;
    g_status.sample_rate_hz = rtlsdr_get_sample_rate(g_dev);
    g_status.gain_tenths_db = gainTenthsDb;
    g_status.ppm = ppm;
    startAsyncLocked();

    logInfo(kTag, "opened: " + std::to_string(g_status.center_hz) + " Hz, " +
                      std::to_string(g_status.sample_rate_hz) + " Hz, gain=" +
                      std::to_string(gainTenthsDb) + ", ppm=" + std::to_string(ppm));
    return true;
#else
    (void)centerHz;
    (void)sampleRateHz;
    (void)gainTenthsDb;
    (void)ppm;
    error = "RTL-SDR support not compiled";
    return false;
#endif
}

void rtlSdrClose()
{
#if defined(BMRF_HAVE_RTLSDR)
    std::lock_guard<std::mutex> lock(g_mutex);
    stopAsyncLocked();
    if (g_dev != nullptr) {
        rtlsdr_close(g_dev);
        g_dev = nullptr;
    }
#endif
    {
        std::lock_guard<std::mutex> lock(g_recordMutex);
        if (g_record != nullptr) {
            std::fclose(g_record);
            g_record = nullptr;
        }
    }
    g_open = false;
    g_status = RtlSdrStatus();
}

bool rtlSdrIsOpen()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_open;
}

int rtlSdrRead(uint8_t* iq, size_t maxSamples)
{
#if defined(BMRF_HAVE_RTLSDR)
    if (!g_open || g_dev == nullptr)
        return -1;

    int bytesRead = 0;
#if defined(__ANDROID__)
    // Prefer the async ring while the reader is running; otherwise fall back to
    // the synchronous reader below.
    if (g_readerRunning.load()) {
        const size_t want = maxSamples * 2;
        const size_t n = ringPop(iq, want);
        if (n == 0)
            return 0;
        bytesRead = static_cast<int>(n);
    }
#endif
    if (bytesRead == 0) {
        const int requested = static_cast<int>(maxSamples * 2);
        const int result = rtlsdr_read_sync(g_dev, iq, requested, &bytesRead);
        if (result < 0 || bytesRead <= 0) {
            // The synchronous bulk reader can wedge when the RTL2832 FIFO
            // overflows. Reset the USB endpoint so streaming recovers.
            rtlsdr_reset_buffer(g_dev);
            static std::atomic<int> g_stalls{0};
            const int n = g_stalls.fetch_add(1) + 1;
            if (n <= 5 || n % 200 == 0)
                logWarn(kTag, "read stalled (" + std::to_string(n) + "), resetting buffer");
            return -1;
        }
    }

    // The async callback already recorded the samples; only record here for the
    // synchronous fallback path.
    {
        std::lock_guard<std::mutex> lock(g_recordMutex);
        if (g_record != nullptr && bytesRead > 0 && !g_readerRunning.load())
            std::fwrite(iq, 1, static_cast<size_t>(bytesRead), g_record);
    }

    const size_t samples = static_cast<size_t>(bytesRead) / 2;
    double acc = 0.0;
    for (int i = 0; i < bytesRead; ++i) {
        const double v = static_cast<double>(iq[i]) - 127.5;
        acc += v * v;
    }
    const float rms = bytesRead > 0 ? static_cast<float>(std::sqrt(acc / bytesRead)) : 0.0f;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_status.samples_read += samples;
    g_status.rms = rms;
    g_status.dropped_bytes = g_dropped.load(std::memory_order_relaxed);
    if (!g_loggedFirstRead && samples > 0) {
        g_loggedFirstRead = true;
        logInfo(kTag, "first read: " + std::to_string(samples) + " samples, rms=" +
                          std::to_string(rms));
    }
    return static_cast<int>(samples);
#else
    (void)iq;
    (void)maxSamples;
    return -1;
#endif
}

RtlSdrStatus rtlSdrStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
#if defined(BMRF_HAVE_RTLSDR)
    g_status.dropped_bytes = g_dropped.load(std::memory_order_relaxed);
#endif
    return g_status;
}

bool rtlSdrStartRecording(const std::string& path, std::string& error)
{
    std::lock_guard<std::mutex> lock(g_recordMutex);
    if (g_record != nullptr) {
        std::fclose(g_record);
        g_record = nullptr;
    }
    g_record = std::fopen(path.c_str(), "wb");
    if (g_record == nullptr) {
        error = "cannot open " + path;
        logError(kTag, error);
        return false;
    }
    logInfo(kTag, "recording IQ to " + path);
    return true;
}

void rtlSdrStopRecording()
{
    std::lock_guard<std::mutex> lock(g_recordMutex);
    if (g_record != nullptr) {
        std::fclose(g_record);
        g_record = nullptr;
        logInfo(kTag, "stopped IQ recording");
    }
}

bool rtlSdrRecording()
{
    std::lock_guard<std::mutex> lock(g_recordMutex);
    return g_record != nullptr;
}

} // namespace bmrf::rf
