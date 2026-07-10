#pragma once

#include <string>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <vector>
#include <functional>

// Log levels
enum class LogLevel {
    Error = 0,
    Warning = 1,
    Info = 2,
    Debug = 3,
    Trace = 4,
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    LogLevel level() const;

    void set_console_output(bool enabled);
    void set_file_output(const std::string& path);
    void set_callback(std::function<void(const std::string&, LogLevel)> callback);

    void log(LogLevel level, const char* file, int line, const char* fmt, ...);
    void vlog(LogLevel level, const char* file, int line, const char* fmt, va_list args);

private:
    Logger() = default;
    std::string format_timestamp() const;
    std::string level_to_string(LogLevel level) const;

    LogLevel m_level = LogLevel::Info;
    bool m_console_output = true;
    FILE* m_file = nullptr;
    std::function<void(const std::string&, LogLevel)> m_callback;
    std::mutex m_mutex;
};

// Convenience macros
#define LOG_ERROR(...)   Logger::instance().log(LogLevel::Error,   __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)    Logger::instance().log(LogLevel::Warning, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)    Logger::instance().log(LogLevel::Info,    __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...)   Logger::instance().log(LogLevel::Debug,   __FILE__, __LINE__, __VA_ARGS__)
#define LOG_TRACE(...)   Logger::instance().log(LogLevel::Trace,   __FILE__, __LINE__, __VA_ARGS__)
