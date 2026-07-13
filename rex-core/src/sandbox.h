#pragma once
#include <windows.h>
#include <string>

class Sandbox {
public:
    Sandbox();
    ~Sandbox();

    bool Initialize(uint64_t memoryLimitMb);
    bool SpawnProcess(const std::string& executablePath);
    bool IsProcessAlive() const;
    void Terminate();
    DWORD GetProcessId() const;

private:
    HANDLE m_job = nullptr;
    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
};
