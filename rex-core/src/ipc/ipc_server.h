#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

class IpcServer {
public:
    using MessageHandler = std::function<void(const std::vector<uint8_t>&)>;

    IpcServer();
    ~IpcServer();

    bool Start(const std::string& pipeName);
    void Stop();
    bool Send(const std::vector<uint8_t>& data);
    void SetMessageHandler(MessageHandler handler) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_handler = std::move(handler);
    }

    bool IsConnected() const { return m_connected; }

private:
    void ListenLoop();
    bool ConnectToClient();

    // The listener thread is the SOLE owner that closes pipe handles.
    // Stop() never closes: it disconnects (waking blocked ReadFile) plus a
    // dummy self-connect (waking blocked ConnectNamedPipe), then joins.
    // Send() snapshots the handle under the mutex and never closes.
    std::string m_pipeName;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::thread m_listenerThread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_connected{ false };
    std::mutex m_mutex;
    MessageHandler m_handler;
};
