#include "ipc_server.h"
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
    m_connected = false;
    if (m_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    if (m_listenerThread.joinable()) {
        m_listenerThread.join();
    }
}

void IpcServer::ListenLoop() {
    while (m_running) {
        m_pipe = CreateNamedPipeA(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE,
            PIPE_TIMEOUT_MS,
            nullptr
        );

        if (m_pipe == INVALID_HANDLE_VALUE) {
            printf("[REX] IPC: Failed to create named pipe (error %lu)\n", GetLastError());
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (!ConnectNamedPipe(m_pipe, nullptr)) {
            if (GetLastError() != ERROR_PIPE_CONNECTED) {
                CloseHandle(m_pipe);
                m_pipe = INVALID_HANDLE_VALUE;
                continue;
            }
        }

        m_connected = true;
        printf("[REX] IPC: Shell connected\n");

        std::vector<uint8_t> buffer(PIPE_BUFFER_SIZE);
        while (m_running) {
            DWORD bytesRead = 0;
            if (!ReadFile(m_pipe, buffer.data(), buffer.size(), &bytesRead, nullptr)) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                    printf("[REX] IPC: Shell disconnected\n");
                }
                break;
            }

            if (bytesRead > 0 && m_handler) {
                buffer.resize(bytesRead);
                m_handler(buffer);
                buffer.resize(PIPE_BUFFER_SIZE);
            }
        }

        m_connected = false;
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
}

bool IpcServer::Send(const std::vector<uint8_t>& data) {
    if (!m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    return WriteFile(m_pipe, data.data(), data.size(), &bytesWritten, nullptr) != FALSE;
}
