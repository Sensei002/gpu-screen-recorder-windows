#include "common/log.h"
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_level = level;
}

LogLevel Logger::level() const {
    return m_level;
}

void Logger::set_console_output(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_console_output = enabled;
}

void Logger::set_file_output(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }
    if (!path.empty()) {
        m_file = fopen(path.c_str(), "a");
    }
}

void Logger::set_callback(std::function<void(const std::string&, LogLevel)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = std::move(callback);
}

void Logger::log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (static_cast<int>(level) > static_cast<int>(m_level)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vlog(level, file, line, fmt, args);
    va_end(args);
}

void Logger::vlog(LogLevel level, const char* file, int line, const char* fmt, va_list args) {
    if (static_cast<int>(level) > static_cast<int>(m_level)) {
        return;
    }

    char message[4096];
    vsnprintf(message, sizeof(message), fmt, args);

    std::lock_guard<std::mutex> lock(m_mutex);

    std::string timestamp = format_timestamp();
    std::string level_str = level_to_string(level);

    // Extract filename from path
    const char* filename = file;
    const char* last_slash = strrchr(file, '/');
    if (last_slash) filename = last_slash + 1;
#ifdef _WIN32
    const char* last_backslash = strrchr(file, '\\');
    if (last_backslash) filename = last_backslash + 1;
#endif

    std::string formatted = std::string("[") + timestamp + "][" + level_str + "][" +
                            filename + ":" + std::to_string(line) + "] " + message;

    if (m_console_output) {
        #ifdef _WIN32
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (level == LogLevel::Error) {
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        } else if (level == LogLevel::Warning) {
            SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        } else if (level == LogLevel::Debug || level == LogLevel::Trace) {
            SetConsoleTextAttribute(hConsole, FOREGROUND_INTENSITY);
        }
        printf("%s\n", formatted.c_str());
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        #else
        printf("%s\n", formatted.c_str());
        #endif
        fflush(stdout);
    }

    if (m_file) {
        fprintf(m_file, "%s\n", formatted.c_str());
        fflush(m_file);
    }

    if (m_callback) {
        m_callback(message, level);
    }
}

std::string Logger::format_timestamp() const {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm timeinfo;
    #ifdef _WIN32
    localtime_s(&timeinfo, &now_time_t);
    #else
    localtime_r(&now_time_t, &timeinfo);
    #endif

    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return std::string(buf) + "." + std::to_string(now_ms.count());
}

std::string Logger::level_to_string(LogLevel level) const {
    switch (level) {
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Warning: return "WARN ";
        case LogLevel::Info:    return "INFO ";
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Trace:   return "TRACE";
        default:                return "?????";
    }
}
