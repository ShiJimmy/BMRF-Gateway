#include "bmrf/common/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace bmrf {

namespace {

#if defined(__ANDROID__)
int androidPriority(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return ANDROID_LOG_VERBOSE;
    case LogLevel::Debug: return ANDROID_LOG_DEBUG;
    case LogLevel::Info:  return ANDROID_LOG_INFO;
    case LogLevel::Warn:  return ANDROID_LOG_WARN;
    case LogLevel::Error: return ANDROID_LOG_ERROR;
    default:              return ANDROID_LOG_DEFAULT;
    }
}
#endif

} // namespace

namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;

void timestamp(char* buffer, size_t size, int& millis)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tmValue{};
#if defined(_WIN32)
    localtime_s(&tmValue, &seconds);
#else
    localtime_r(&seconds, &tmValue);
#endif
    std::strftime(buffer, size, "%H:%M:%S", &tmValue);
}
} // namespace

void setLogLevel(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }
LogLevel logLevel() { return g_level.load(std::memory_order_relaxed); }

const char* toString(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    default:              return "OFF";
    }
}

void logMessage(LogLevel level, const std::string& tag, const std::string& message)
{
    if (level == LogLevel::Off || level < logLevel())
        return;

    char time[32];
    int millis = 0;
    timestamp(time, sizeof(time), millis);

    std::lock_guard<std::mutex> lock(g_mutex);
#if defined(__ANDROID__)
    (void)time;
    (void)millis;
    __android_log_print(androidPriority(level), ("BMRF/" + tag).c_str(), "%s", message.c_str());
#else
    std::fprintf(stderr, "[%s.%03d] [%-5s] %s: %s\n", time, millis,
                 toString(level), tag.c_str(), message.c_str());
    std::fflush(stderr);
#endif
}

} // namespace bmrf
