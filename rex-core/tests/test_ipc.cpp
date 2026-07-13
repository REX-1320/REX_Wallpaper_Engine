#include <gtest/gtest.h>
#include "../src/ipc_server.h"
#include <thread>
#include <chrono>

TEST(IpcTest, StartStop) {
    IpcServer server;
    EXPECT_TRUE(server.Start("REX/test/v1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.Stop();
    EXPECT_FALSE(server.IsConnected());
}

TEST(IpcTest, NotConnectedInitially) {
    IpcServer server;
    EXPECT_FALSE(server.IsConnected());
}

TEST(IpcTest, DoubleStopNoCrash) {
    IpcServer server;
    server.Start("REX/test/v3");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.Stop();
    server.Stop();
}
