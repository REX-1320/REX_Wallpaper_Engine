#include <gtest/gtest.h>
#include "../src/workerw.h"

TEST(WorkerWTest, Initialize) {
    WorkerWHost host;
    bool result = host.Initialize();
    host.Shutdown();
}

TEST(WorkerWTest, FindWorkerW) {
    WorkerWHost host;
    HWND hwnd = nullptr;
    EXPECT_NO_THROW({
        if (host.Initialize()) {
            hwnd = host.GetRenderTargetWindow();
            host.Shutdown();
        }
    });
}
