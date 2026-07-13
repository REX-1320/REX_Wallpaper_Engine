#include <gtest/gtest.h>
#include "../src/sandbox.h"

TEST(SandboxTest, InitializeAndTerminate) {
    Sandbox sandbox;
    EXPECT_TRUE(sandbox.Initialize(50));
    sandbox.Terminate();
}

TEST(SandboxTest, ProcessExceedsMemoryLimitIsKilled) {
    Sandbox sandbox;
    ASSERT_TRUE(sandbox.Initialize(50));

    std::string hostPath = "rex-widget-host.exe";
    ASSERT_TRUE(sandbox.SpawnProcess(hostPath + " --alloc-memory-mb 100"));

    // Give it time to allocate and be killed
    std::this_thread::sleep_for(std::chrono::seconds(2));
    EXPECT_FALSE(sandbox.IsProcessAlive());
}
