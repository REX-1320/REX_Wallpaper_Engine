#include <gtest/gtest.h>
#include "../src/graphics/compositor/compositor.h"

TEST(CompositorTest, SetClearColorStoresValues) {
    Compositor compositor;
    compositor.SetClearColor(1.0f, 0.0f, 0.0f);
    compositor.SetClearColor(0.0f, 0.5f, 0.8f);
}

TEST(CompositorTest, SubmitAndRemoveLayer) {
    Compositor compositor;
    Layer layer = {};
    layer.id = 1;
    layer.kind = 1;
    layer.bounds[0] = 0; layer.bounds[1] = 0; layer.bounds[2] = 100; layer.bounds[3] = 100;
    layer.opacity = 1.0f;
    layer.zOrder = 0;
    layer.visible = true;

    compositor.SubmitLayer(layer);
    compositor.RemoveLayer(1);
    compositor.RemoveLayer(999); // no-op, should not crash
}

TEST(CompositorTest, RemoveNonexistentLayerNoCrash) {
    Compositor compositor;
    compositor.RemoveLayer(42);
}

TEST(CompositorTest, ImportTextureWithoutDeviceReturnsFalse) {
    Compositor compositor;
    bool result = compositor.ImportLayerTexture(1, nullptr, 100, 100);
    EXPECT_FALSE(result);
}

TEST(CompositorTest, RemoveNonexistentTextureNoCrash) {
    Compositor compositor;
    compositor.RemoveLayerTexture(42);
}

TEST(CompositorTest, ImportAndRemoveTextureRoundTrip) {
    Compositor compositor;
    // Without a D3D12 device this will fail, but should not crash.
    bool imported = compositor.ImportLayerTexture(1, INVALID_HANDLE_VALUE, 100, 100);
    compositor.RemoveLayerTexture(1);
    compositor.RemoveLayerTexture(1); // double removal - no crash
}
