#include "shared/Logger.hpp"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <filesystem>
#include <iostream>

namespace rex::shared {

Logger::Logger() = default;
Logger::~Logger() { Shutdown(); }

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::Initialize(const std::string& logFilePath, LogLevel level) {
    if (m_initialized) return;

    try {
        // Create console sink (stdout is owned by the runtime frame protocol
        // when running as a persistent runtime, so it must be skippable).
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        consoleSink->set_level(m_consoleEnabled ? spdlog::level::trace : spdlog::level::off);

        // Create file sink: bounded growth (P2). Rotate at 2 MB, keep 2
        // spares (mirrors the Electron app log). Fresh file per process
        // start is preserved by removing any stale log first — each run
        // still starts readable from the top, but long soaks can no longer
        // grow logs indefinitely.
        std::error_code ec;
        std::filesystem::remove(logFilePath, ec);
        auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logFilePath, 2 * 1024 * 1024, 2);
        fileSink->set_level(spdlog::level::trace);

        // Create logger with multiple sinks
        std::vector<spdlog::sink_ptr> sinks = {consoleSink, fileSink};
        m_logger = std::make_shared<spdlog::logger>("rex", sinks.begin(), sinks.end());

        // Set pattern
        m_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%s:%#] %v");

        // Set level
        m_logger->set_level(static_cast<spdlog::level::level_enum>(level));

        // Flush after every message. Without this the file sink's 4 KB
        // CRT buffer only hits disk when it fills (~9 s of runtime logging),
        // which makes a healthy process look frozen — logs must be a live
        // instrument for diagnosing hangs.
        m_logger->flush_on(spdlog::level::trace);

        m_initialized = true;

        Info("Logger", "Logger initialized (file: " + logFilePath + ")");
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize logger: " << e.what() << std::endl;
    }
}

void Logger::Shutdown() {
    if (!m_initialized) return;

    Info("Logger", "Logger shutting down");

    if (m_logger) {
        m_logger->flush();
        m_logger.reset();
    }

    m_initialized = false;
    spdlog::drop_all();
}

void Logger::SetLevel(LogLevel level) {
    if (!m_logger) return;
    m_logger->set_level(static_cast<spdlog::level::level_enum>(level));
}

void Logger::SetConsoleOutput(bool enabled) {
    m_consoleEnabled = enabled;
    if (!m_logger) return;
    // Toggle the console sink level for post-init changes.
    for (const auto& sink : m_logger->sinks()) {
        if (dynamic_cast<spdlog::sinks::stdout_color_sink_mt*>(sink.get())) {
            sink->set_level(enabled ? spdlog::level::trace : spdlog::level::off);
        }
    }
}

void Logger::SetFileOutput(bool enabled) {
    // Handled by sinks
}

void Logger::Trace(const std::string& module, const std::string& message) {
    Log(LogLevel::Trace, module, message);
}

void Logger::Debug(const std::string& module, const std::string& message) {
    Log(LogLevel::Debug, module, message);
}

void Logger::Info(const std::string& module, const std::string& message) {
    Log(LogLevel::Info, module, message);
}

void Logger::Warn(const std::string& module, const std::string& message) {
    Log(LogLevel::Warn, module, message);
}

void Logger::Error(const std::string& module, const std::string& message) {
    Log(LogLevel::Error, module, message);
}

void Logger::Fatal(const std::string& module, const std::string& message) {
    Log(LogLevel::Fatal, module, message);
}

void Logger::Log(LogLevel level, const std::string& module, const std::string& message) {
    if (!m_initialized || !m_logger) return;

    std::string fullMessage = "[" + module + "] " + message;

    switch (level) {
        case LogLevel::Trace:   m_logger->trace(fullMessage); break;
        case LogLevel::Debug:   m_logger->debug(fullMessage); break;
        case LogLevel::Info:    m_logger->info(fullMessage); break;
        case LogLevel::Warn:    m_logger->warn(fullMessage); break;
        case LogLevel::Error:   m_logger->error(fullMessage); break;
        case LogLevel::Fatal:   m_logger->critical(fullMessage); break;
        default: break;
    }

    for (const auto& callback : m_callbacks) {
        callback(fullMessage);
    }
}

void Logger::RegisterCallback(LogCallback callback) {
    m_callbacks.push_back(callback);
}

void Logger::UnregisterCallback(LogCallback callback) {
    // Simplified – in production, use proper token-based registration
    // For Phase 0, we'll just clear all callbacks
    m_callbacks.clear();
}

} // namespace rex::shared
