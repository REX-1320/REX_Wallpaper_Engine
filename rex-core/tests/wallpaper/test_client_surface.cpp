// ═══════════════════════════════════════════════════════════════════════════
// Client frame-surface lifecycle — R13 regression tests
// ═══════════════════════════════════════════════════════════════════════════
//
// A dead client used to yield NULL/stale handles serialized as valid
// (DuplicateHandle BOOL returns were discarded), the client process was
// never revalidated, and SHUTDOWN/EOF leaked the process handle because
// CloseClientSurface never touched pid/process.

#include <gtest/gtest.h>

#include <windows.h>

#include "wallpaper/runtime/ClientSurface.hpp"

using namespace rex::wallpaper;

// ─── Liveness gate ──────────────────────────────────────────────────────────

TEST(ClientSurfaceLivenessTest, NullProcessIsNotAlive) {
    EXPECT_FALSE(IsProcessAlive(nullptr));
}

TEST(ClientSurfaceLivenessTest, RunningProcessIsAlive) {
    HANDLE self = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
        GetCurrentProcessId());
    ASSERT_NE(self, nullptr);
    EXPECT_TRUE(IsProcessAlive(self));
    CloseHandle(self);
}

TEST(ClientSurfaceLivenessTest, AliveWithClientBindingRights) {
    // Regression: SET_CLIENT_PID opens the host with PROCESS_DUP_HANDLE |
    // PROCESS_QUERY_LIMITED_INFORMATION (no SYNCHRONIZE), so the liveness
    // check must work with exactly those rights — a wait-based check fails
    // with WAIT_FAILED here and wrongly reports every client as dead.
    HANDLE bound = OpenProcess(
        PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        GetCurrentProcessId());
    ASSERT_NE(bound, nullptr);
    EXPECT_TRUE(IsProcessAlive(bound));
    CloseHandle(bound);
}

TEST(ClientSurfaceLivenessTest, ExitedProcessIsNotAlive) {    // An exited client leaves a signaled handle: duplicating into it yields
    // stale handles, so the runtime must detach instead of serializing them.
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"cmd.exe /c exit 0";
    ASSERT_TRUE(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);
    ASSERT_EQ(WaitForSingleObject(pi.hProcess, 10000), WAIT_OBJECT_0);
    EXPECT_FALSE(IsProcessAlive(pi.hProcess));
    CloseHandle(pi.hProcess);
}

// ─── Duplication-result validation ──────────────────────────────────────────

TEST(ClientSurfaceValidationTest, NullFenceFails) {
    // Pseudo-handles are never closed — safe non-null stand-ins.
    HANDLE slots[2] = { GetCurrentProcess(), GetCurrentProcess() };
    EXPECT_FALSE(ValidateDuplicatedSurface(nullptr, slots, 2));
}

TEST(ClientSurfaceValidationTest, NullSlotFails) {
    HANDLE slots[2] = { GetCurrentProcess(), nullptr };
    EXPECT_FALSE(ValidateDuplicatedSurface(GetCurrentProcess(), slots, 2));
}

TEST(ClientSurfaceValidationTest, CompleteSurfacePasses) {
    HANDLE slots[2] = { GetCurrentProcess(), GetCurrentProcess() };
    EXPECT_TRUE(ValidateDuplicatedSurface(GetCurrentProcess(), slots, 2));
}

// ─── Detach ─────────────────────────────────────────────────────────────────

TEST(ClientSurfaceDetachTest, DetachReleasesProcessAndResetsPid) {
    ClientSurfaceState s;
    s.process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                            FALSE, GetCurrentProcessId());
    ASSERT_NE(s.process, nullptr);
    HANDLE raw = s.process;
    s.pid = 1234;

    DetachClient(s);

    EXPECT_EQ(s.process, nullptr);
    EXPECT_EQ(s.pid, DWORD{ 0 });
    // The handle is really closed, not just forgotten.
    EXPECT_EQ(WaitForSingleObject(raw, 0), WAIT_FAILED);
}

TEST(ClientSurfaceDetachTest, DetachClosesOwnedHandles) {
    ClientSurfaceState s;
    s.fence = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s.slots[0] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(s.fence, nullptr);
    ASSERT_NE(s.slots[0], nullptr);
    s.slotCount = 1;
    s.duplicated = true;

    DetachClient(s);

    EXPECT_EQ(s.fence, nullptr);
    EXPECT_EQ(s.slots[0], nullptr);
    EXPECT_EQ(s.slotCount, UINT{ 0 });
    EXPECT_EQ(s.generation, uint64_t{ 0 });
    EXPECT_FALSE(s.duplicated);
}

TEST(ClientSurfaceDetachTest, DetachDoesNotCloseUnownedHandles) {
    // Self-preview path: handles are process-local, we don't own them.
    ClientSurfaceState s;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(ev, nullptr);
    s.fence = ev;
    s.duplicated = false;

    DetachClient(s);

    EXPECT_EQ(s.fence, nullptr);
    EXPECT_TRUE(SetEvent(ev));  // still valid — not closed behind our back
    CloseHandle(ev);
}
