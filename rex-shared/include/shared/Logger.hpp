#pragma once

#include <string>
#include <memory>
#include <functional>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <fmt/core.h>

namespace rex::shared {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
    Off
};

class Logger {
public:
    Logger();
    ~Logger();

    static Logger& Instance();

    void Initialize(const std::string& logFilePath = "rex.log", LogLevel level = LogLevel::Debug);
    void Shutdown();

    void SetLevel(LogLevel level);
    void SetConsoleOutput(bool enabled);
    void SetFileOutput(bool enabled);

    void Trace(const std::string& module, const std::string& message);
    void Debug(const std::string& module, const std::string& message);
    void Info(const std::string& module, const std::string& message);
    void Warn(const std::string& module, const std::string& message);
    void Error(const std::string& module, const std::string& message);
    void Fatal(const std::string& module, const std::string& message);

    using LogCallback = std::function<void(const std::string&)>;
    void RegisterCallback(LogCallback callback);
    void UnregisterCallback(LogCallback callback);

    bool IsInitialized() const { return m_initialized; }

private:
    void Log(LogLevel level, const std::string& module, const std::string& message);

    std::shared_ptr<spdlog::logger> m_logger;
    bool m_initialized = false;
    std::vector<LogCallback> m_callbacks;
};

} // namespace rex::shared

// ─── Macros ────────────────────────────────────────────────────────────────

#define REX_LOG_TRACE(module, ...) \
    rex::shared::Logger::Instance().Trace(module, fmt::format(__VA_ARGS__))

#define REX_LOG_DEBUG(module, ...) \
    rex::shared::Logger::Instance().Debug(module, fmt::format(__VA_ARGS__))

#define REX_LOG_INFO(module, ...) \
    rex::shared::Logger::Instance().Info(module, fmt::format(__VA_ARGS__))

#define REX_LOG_WARN(module, ...) \
    rex::shared::Logger::Instance().Warn(module, fmt::format(__VA_ARGS__))

#define REX_LOG_ERROR(module, ...) \
    rex::shared::Logger::Instance().Error(module, fmt::format(__VA_ARGS__))

#define REX_LOG_FATAL(module, ...) \
    rex::shared::Logger::Instance().Fatal(module, fmt::format(__VA_ARGS__))
