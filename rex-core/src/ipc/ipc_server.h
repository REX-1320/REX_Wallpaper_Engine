#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>

class IpcServer {
public:
    using MessageHandler = std::function<void(const std::vector<uint8_t>&)>;

    IpcServer();
    ~IpcServer();

    bool Start(const std::string& pipeName);
    void Stop();
    bool Send(const std::vector<uint8_t>& data);
    void SetMessageHandler(MessageHandler handler) { m_handler = std::move(handler); }

    bool IsConnected() const { return m_connected; }

private:
    void ListenLoop();
    bool ConnectToClient();

    std::string m_pipeName;
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    std::thread m_listenerThread;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_connected{ false };
    MessageHandler m_handler;
};
