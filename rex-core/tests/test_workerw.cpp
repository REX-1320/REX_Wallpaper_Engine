#include <gtest/gtest.h>
#include "../src/workerw.h"

TEST(WorkerWTest, InitializeAndShutdown) {
    WorkerWHost host;
    bool result = host.Initialize();
    host.Shutdown();
    // No crash on double shutdown
    host.Shutdown();
}

TEST(WorkerWTest, GetRenderWindowReturnsNullBeforeInit) {
    WorkerWHost host;
    EXPECT_EQ(host.GetRenderTargetWindow(), nullptr);
}
