#pragma once

#include <string>
#include <variant>
#include <vector>

namespace rex::shared {

// ─── Error Codes ──────────────────────────────────────────────────────────

enum class ErrorCode {
    Success = 0,
    UnknownError,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    OutOfMemory,
    Timeout,
    NotInitialized,
    AlreadyInitialized,
    InvalidState,
    FileNotFound,
    FileReadError,
    FileWriteError,
    InvalidFormat,
    UnsupportedFormat,
    HardwareNotSupported,
    DeviceLost,
    DriverError,
};

struct ErrorInfo {
    ErrorCode code = ErrorCode::Success;
    std::string message;
    std::string file;
    int line = 0;
};

// ─── Result ─────────────────────────────────────────────────────────────

template<typename T>
class Result {
public:
    Result() : m_hasValue(false), m_error{ErrorCode::Success, "Success", "", 0} {}

    Result(T value) : m_hasValue(true), m_value(std::move(value)) {}

    Result(ErrorCode code, const std::string& message, const std::string& file = "", int line = 0)
        : m_hasValue(false), m_error{code, message, file, line} {}

    bool IsSuccess() const { return m_hasValue; }
    bool IsError() const { return !m_hasValue; }

    T& Value() { return m_value; }
    const T& Value() const { return m_value; }

    ErrorInfo Error() const { return m_error; }

    std::string ToString() const {
        if (m_hasValue) {
            return "Success";
        }
        return m_error.message + " (code: " + std::to_string(static_cast<int>(m_error.code)) + ")";
    }

private:
    bool m_hasValue;
    T m_value;
    ErrorInfo m_error;
};

// ─── Void Result ─────────────────────────────────────────────────────────

template<>
class Result<void> {
public:
    Result() : m_success(true) {}

    Result(ErrorCode code, const std::string& message, const std::string& file = "", int line = 0)
        : m_success(false), m_error{code, message, file, line} {}

    bool IsSuccess() const { return m_success; }
    bool IsError() const { return !m_success; }

    ErrorInfo Error() const { return m_error; }

    std::string ToString() const {
        if (m_success) {
            return "Success";
        }
        return m_error.message + " (code: " + std::to_string(static_cast<int>(m_error.code)) + ")";
    }

private:
    bool m_success;
    ErrorInfo m_error;
};

// ─── Helper Macros ──────────────────────────────────────────────────────

#define REX_SUCCESS(value) rex::shared::Result<T>(value)
#define REX_ERROR(code, msg) rex::shared::Result<T>(code, msg, __FILE__, __LINE__)

#define REX_TRY(expr) \
    do { \
        auto result = (expr); \
        if (result.IsError()) { \
            return result; \
        } \
    } while (0)

#define REX_TRY_VAL(expr) \
    [&]() -> auto { \
        auto result = (expr); \
        if (result.IsError()) { \
            return result; \
        } \
        return result.Value(); \
    }()

} // namespace rex::shared
