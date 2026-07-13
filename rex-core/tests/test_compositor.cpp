#include <gtest/gtest.h>
#include "../src/compositor.h"

TEST(CompositorTest, SetClearColor) {
    Compositor compositor;
    compositor.SetClearColor(1.0f, 0.0f, 0.0f);
    compositor.SetClearColor(0.0f, 0.5f, 0.8f);
}
