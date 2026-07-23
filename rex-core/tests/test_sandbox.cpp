#include <gtest/gtest.h>
#include "../src/core/sandbox/sandbox.h"

TEST(SandboxTest, InitializeAndTerminate) {
    Sandbox sandbox;
    EXPECT_TRUE(sandbox.Initialize(50));
    sandbox.Terminate();
}
