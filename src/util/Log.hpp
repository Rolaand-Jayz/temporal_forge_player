// Log.hpp — minimal leveled logger using std::format (C++23).
#pragma once
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace temporal_forge {

enum class LogLevel : uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

class Logger {
public:
    // instance: process-wide singleton accessor (Meyers singleton).
    //
    // Called by: logAtLevel (this file), and main.cpp at startup to set the
    //            log level before playback begins.
    // Notes:     Also sets stdout/stderr line-buffered on first construction so
    //            log lines survive a SIGTERM/kill (e.g. timeout(1)) instead of
    //            sitting in a 4KiB pipe buffer.
    static Logger& instance();

    // setLevel / level: configure and read the minimum severity to emit.
    //                   Called by main.cpp (sets Debug when TFORGE_VK_VALIDATE etc.).
    void setLevel(LogLevel level) { level_ = level; }
    [[nodiscard]] LogLevel level() const { return level_; }

    // write: core entry — formats and writes a single log line.
    //
    // Called by: logAtLevel (this file), the wrapper layer that every call site
    //            goes through (TFORGE_LOG_* macros and the logInfo/logWarn/...
    //            back-compat helpers).
    // Calls:     std::format, levelTag, fileName, std::fwrite under mutex_.
    // Notes:     Drops the line silently if level < level_ (threshold check).
    //            file/line are passed explicitly by the wrapper macros to avoid
    //            the variadic + defaulted-source_location deduction problem.
    //            (Renamed from emit() to avoid the Qt moc `emit` macro collision.)
    //            Thread-safe via mutex_; WARN/ERROR go to stderr, the rest to stdout.
    void write(LogLevel level, const char* file, int line,
               std::string_view body) {
        if (static_cast<uint8_t>(level) < static_cast<uint8_t>(level_)) return;
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char ts[24];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

        std::string header = std::format("[{}] {} {}:{}: ",
            ts, levelTag(level), fileName(file), line);
        std::lock_guard lock(mutex_);
        std::FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
        std::fwrite(header.data(), 1, header.size(), out);
        std::fwrite(body.data(), 1, body.size(), out);
        std::fputc('\n', out);
    }

private:
    Logger() = default;

    // levelTag: maps a LogLevel to its fixed-width tag string (TRACE/DEBUG/...).
    //           Called by: write (this file).
    static const char* levelTag(LogLevel l) {
        switch (l) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Error: return "ERROR";
        }
        return "?    ";
    }

    // fileName: strips a __FILE__ path down to its base name for compact logs.
    //           Called by: write (this file).
    static std::string_view fileName(std::string_view path) {
        if (auto pos = path.rfind('/'); pos != std::string_view::npos)
            return path.substr(pos + 1);
        return path;
    }

    std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
};

inline Logger& log() { return Logger::instance(); }

// logAtLevel: the variadic wrapper that renders a std::format string and forwards
//             it to Logger::write.
//
// Called by: the TFORGE_LOG_* macros (every log call site) and the back-compat
//            logInfo/logWarn/... helpers.
// Calls:     std::vformat, std::make_format_args, Logger::instance().write.
// Notes:     file/line come from the call-site macros. Args are taken by
//            const lvalue reference so both lvalues and rvalues bind, and so
//            std::make_format_args (which in C++23 requires lvalue args for
//            lifetime safety) is satisfied.
template <typename... Args>
void logAtLevel(LogLevel level, const char* file, int line,
                std::format_string<Args...> fmt, const Args&... args) {
    std::string body = std::vformat(fmt.get(),
        std::make_format_args(args...));
    Logger::instance().write(level, file, line, body);
}

#define TFORGE_LOG_INFO(...)  ::temporal_forge::logAtLevel( \
    ::temporal_forge::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define TFORGE_LOG_WARN(...)  ::temporal_forge::logAtLevel( \
    ::temporal_forge::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define TFORGE_LOG_ERROR(...) ::temporal_forge::logAtLevel( \
    ::temporal_forge::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
#define TFORGE_LOG_DEBUG(...) ::temporal_forge::logAtLevel( \
    ::temporal_forge::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)

// Back-compat names used in existing call sites.
template <typename... Args>
void logInfo(std::format_string<Args...> fmt, const Args&... args) {
    logAtLevel(LogLevel::Info, "?", 0, fmt, args...);
}
template <typename... Args>
void logWarn(std::format_string<Args...> fmt, const Args&... args) {
    logAtLevel(LogLevel::Warn, "?", 0, fmt, args...);
}
template <typename... Args>
void logError(std::format_string<Args...> fmt, const Args&... args) {
    logAtLevel(LogLevel::Error, "?", 0, fmt, args...);
}
template <typename... Args>
void logDebug(std::format_string<Args...> fmt, const Args&... args) {
    logAtLevel(LogLevel::Debug, "?", 0, fmt, args...);
}

} // namespace temporal_forge
