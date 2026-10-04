#pragma once

#include <string>

namespace bmrf {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Off,
};

void setLogLevel(LogLevel level);
LogLevel logLevel();

const char* toString(LogLevel level);

void logMessage(LogLevel level, const std::string& tag, const std::string& message);

inline void logTrace(const std::string& tag, const std::string& m) { logMessage(LogLevel::Trace, tag, m); }
inline void logDebug(const std::string& tag, const std::string& m) { logMessage(LogLevel::Debug, tag, m); }
inline void logInfo(const std::string& tag, const std::string& m)  { logMessage(LogLevel::Info,  tag, m); }
inline void logWarn(const std::string& tag, const std::string& m)  { logMessage(LogLevel::Warn,  tag, m); }
inline void logError(const std::string& tag, const std::string& m) { logMessage(LogLevel::Error, tag, m); }

} // namespace bmrf
