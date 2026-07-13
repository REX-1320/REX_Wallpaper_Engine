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

TEST(IpcTest, PingPong) {
    IpcServer server;
    std::vector<uint8_t> received;
    server.SetMessageHandler([&](const std::vector<uint8_t>& data) {
        received = data;
    });

    server.Start("REX/test/v2");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server.Stop();
}
