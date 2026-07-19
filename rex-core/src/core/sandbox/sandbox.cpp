#include "core/sandbox/sandbox.h"
#include <cstdio>

Sandbox::Sandbox() = default;
Sandbox::~Sandbox() { Terminate(); }

bool Sandbox::Initialize(uint64_t memoryLimitMb) {
    m_job = CreateJobObjectW(nullptr, nullptr);
    if (!m_job) {
        printf("[REX] Sandbox: Failed to create job object (error %lu)\n", GetLastError());
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY |
        JOB_OBJECT_LIMIT_JOB_MEMORY |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memoryLimitMb * 1024 * 1024);
    limits.JobMemoryLimit = static_cast<SIZE_T>(memoryLimitMb * 1024 * 1024);

    if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        printf("[REX] Sandbox: Failed to set job limits (error %lu)\n", GetLastError());
        return false;
    }

    printf("[REX] Sandbox initialized with %llu MB memory limit\n", memoryLimitMb);
    return true;
}

bool Sandbox::SpawnProcess(const std::string& executablePath) {
    if (!m_job) return false;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        executablePath.c_str(),
        nullptr, nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi)) {
        printf("[REX] Sandbox: Failed to spawn process (error %lu)\n", GetLastError());
        return false;
    }

    if (!AssignProcessToJobObject(m_job, pi.hProcess)) {
        printf("[REX] Sandbox: Failed to assign process to job (error %lu)\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return false;
    }

    ResumeThread(pi.hThread);

    m_process = pi.hProcess;
    m_thread = pi.hThread;

    printf("[REX] Sandbox: Process %lu spawned and assigned to job\n", pi.dwProcessId);
    return true;
}

bool Sandbox::IsProcessAlive() const {
    if (!m_process) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_process, &exitCode)) return false;
    return exitCode == STILL_ACTIVE;
}

void Sandbox::Terminate() {
    if (m_process) {
        TerminateProcess(m_process, 1);
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_thread) {
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_job) {
        CloseHandle(m_job);
        m_job = nullptr;
    }
}

DWORD Sandbox::GetProcessId() const {
    if (!m_process) return 0;
    return ::GetProcessId(m_process);
}
