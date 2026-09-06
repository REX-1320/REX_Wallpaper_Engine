#include "ipc/ipc_server.h"
#include <cstdio>

static constexpr DWORD PIPE_BUFFER_SIZE = 4096;
static constexpr DWORD PIPE_TIMEOUT_MS = 5000;

IpcServer::IpcServer() = default;
IpcServer::~IpcServer() { Stop(); }

bool IpcServer::Start(const std::string& pipeName) {
    m_pipeName = "\\\\.\\pipe\\" + pipeName;
    m_running = true;

    m_listenerThread = std::thread(&IpcServer::ListenLoop, this);
    printf("[REX] IPC server started on %s\n", m_pipeName.c_str());
    return true;
}

void IpcServer::Stop() {
    m_running = false;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pipe = m_pipe;
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        // Wake a thread blocked in ConnectNamedPipe: a dummy self-connect
        // makes it return (TRUE or ERROR_PIPE_CONNECTED).
        HANDLE wake = CreateFileA(m_pipeName.c_str(), GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) {
            CloseHandle(wake);
        }
        // Wake a thread blocked in ReadFile: disconnecting forces the
        // pending read to complete with an error. Never CloseHandle here —
        // the listener owns all closes (avoids double-close / handle reuse).
        DisconnectNamedPipe(pipe);
    }

    if (m_listenerThread.joinable()) {
        if (m_listenerThread.get_id() == std::this_thread::get_id()) {
            // Stop() invoked from inside the message handler: joining
            // ourselves would deadlock (resource_deadlock). Detach; the
            // thread exits on its own now that m_running is false.
            m_listenerThread.detach();
        } else {
            m_listenerThread.join();
        }
    }
    m_connected = false;
}

void IpcServer::ListenLoop() {
    while (m_running) {
        HANDLE pipe = CreateNamedPipeA(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE,
            PIPE_TIMEOUT_MS,
            nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            printf("[REX] IPC: Failed to create named pipe (error %lu)\n", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pipe = pipe;
        }

        if (!ConnectNamedPipe(pipe, nullptr)) {
            if (GetLastError() != ERROR_PIPE_CONNECTED) {
                // Includes the Stop() dummy-connect wake: just re-check
                // m_running at the top of the loop.
                CloseHandle(pipe);
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_pipe == pipe) m_pipe = INVALID_HANDLE_VALUE;
                continue;
            }
        }

        if (!m_running) {
            // Woken only to shut down (Stop's dummy client connected us).
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pipe == pipe) m_pipe = INVALID_HANDLE_VALUE;
            break;
        }

        m_connected = true;
        printf("[REX] IPC: Shell connected\n");

        // Copy the handler under lock: SetMessageHandler may race us.
        MessageHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            handler = m_handler;
        }

        std::vector<uint8_t> buffer(PIPE_BUFFER_SIZE);
        while (m_running) {
            DWORD bytesRead = 0;
            if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                    printf("[REX] IPC: Shell disconnected\n");
                }
                break;
            }

            if (bytesRead > 0 && handler) {
                buffer.resize(bytesRead);
                handler(buffer);
                buffer.resize(PIPE_BUFFER_SIZE);
            }
        }

        m_connected = false;
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pipe == pipe) m_pipe = INVALID_HANDLE_VALUE;
        }
    }
}

bool IpcServer::Send(const std::vector<uint8_t>& data) {
    if (!m_connected) return false;

    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pipe = m_pipe;
    }
    if (pipe == INVALID_HANDLE_VALUE || data.empty()) return false;

    DWORD bytesWritten = 0;
    return WriteFile(pipe, data.data(),
                     static_cast<DWORD>(data.size()), &bytesWritten, nullptr) != FALSE;
}
