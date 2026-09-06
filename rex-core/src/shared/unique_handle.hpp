#pragma once
#include <windows.h>
#include <utility>

namespace rex::shared {

// RAII wrapper for Windows HANDLE objects (CloseHandle on destruction).
// Usage:
//   unique_handle h(CreateEvent(...));
//   if (h) { ... }
//   // automatically closed at scope exit
class unique_handle {
public:
    unique_handle() noexcept = default;
    explicit unique_handle(HANDLE value) noexcept : m_value(value) {}
    ~unique_handle() { close(); }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept : m_value(other.release()) {}
    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            close();
            m_value = other.release();
        }
        return *this;
    }

    HANDLE get() const noexcept { return m_value; }
    HANDLE release() noexcept { HANDLE tmp = m_value; m_value = nullptr; return tmp; }

    void reset(HANDLE value = nullptr) noexcept {
        close();
        m_value = value;
    }

    explicit operator bool() const noexcept { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }

private:
    void close() noexcept {
        if (m_value && m_value != INVALID_HANDLE_VALUE) {
            CloseHandle(m_value);
            m_value = nullptr;
        }
    }

    HANDLE m_value = nullptr;
};

} // namespace rex::shared
