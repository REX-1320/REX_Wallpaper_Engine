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

// ─── Unit 14 (audit 2026-10-17 §5.8 #3/#5/#6/#7/#9): pure failure-path and
// bookkeeping helpers. The GPU paths need a real device, so their logic is
// factored into these CPU-testable seams that production calls verbatim.

// DXGI_STATUS_OCCLUDED is a success code (0x087A0001) but must NOT be
// treated as a normally presented frame. Spelled numerically so the test
// does not depend on the SDK defining the status macro.
static constexpr HRESULT kDxgiStatusOccluded = static_cast<HRESULT>(0x087A0001L);

TEST(CompositorPresentTest, ClassifiesEveryPresentOutcome) {
    EXPECT_EQ(Compositor::ClassifyPresent(S_OK), Compositor::PresentOutcome::Ok);
    EXPECT_EQ(Compositor::ClassifyPresent(kDxgiStatusOccluded),
              Compositor::PresentOutcome::Occluded);
    EXPECT_EQ(Compositor::ClassifyPresent(DXGI_ERROR_DEVICE_REMOVED),
              Compositor::PresentOutcome::DeviceLost);
    EXPECT_EQ(Compositor::ClassifyPresent(DXGI_ERROR_DEVICE_RESET),
              Compositor::PresentOutcome::DeviceLost);
    EXPECT_EQ(Compositor::ClassifyPresent(DXGI_ERROR_DEVICE_HUNG),
              Compositor::PresentOutcome::DeviceLost);
    EXPECT_EQ(Compositor::ClassifyPresent(E_FAIL), Compositor::PresentOutcome::Failed);
    EXPECT_EQ(Compositor::ClassifyPresent(DXGI_ERROR_INVALID_CALL),
              Compositor::PresentOutcome::Failed);
}

TEST(CompositorGpuWaitTest, ClassifiesWaitOutcomes) {
    using W = Compositor::GpuWaitOutcome;
    EXPECT_EQ(Compositor::ClassifyGpuWait(WAIT_OBJECT_0, S_OK), W::Completed);
    EXPECT_EQ(Compositor::ClassifyGpuWait(WAIT_TIMEOUT, S_OK), W::TimedOut);
    EXPECT_EQ(Compositor::ClassifyGpuWait(WAIT_FAILED, S_OK), W::WaitFailed);
    EXPECT_EQ(Compositor::ClassifyGpuWait(WAIT_TIMEOUT, DXGI_ERROR_DEVICE_REMOVED),
              W::DeviceLost);
    EXPECT_EQ(Compositor::ClassifyGpuWait(WAIT_FAILED, DXGI_ERROR_DEVICE_RESET),
              W::DeviceLost);
    // A fence that completed is a completed wait regardless of the device
    // state afterwards.
    EXPECT_EQ(Compositor::ClassifyGpuWait(WAIT_OBJECT_0, DXGI_ERROR_DEVICE_REMOVED),
              W::Completed);
}

TEST(CompositorSrvFormatTest, TypelessResourcesGetCanonicalTypedViews) {
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS),
              DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_B8G8R8A8_TYPELESS),
              DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_R10G10B10A2_TYPELESS),
              DXGI_FORMAT_R10G10B10A2_UNORM);
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_R16G16B16A16_TYPELESS),
              DXGI_FORMAT_R16G16B16A16_FLOAT);
}

TEST(CompositorSrvFormatTest, TypedFormatsPassThroughAndUnknownStaysUnknown) {
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_R8G8B8A8_UNORM),
              DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_B8G8R8A8_UNORM),
              DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_NV12),
              DXGI_FORMAT_NV12);
    EXPECT_EQ(Compositor::SrvFormatForResourceFormat(DXGI_FORMAT_UNKNOWN),
              DXGI_FORMAT_UNKNOWN);
}

TEST(CompositorSrvSlotTest, FreeListIsPreferredOverGrowth) {
    std::vector<uint32_t> freeSlots;
    uint32_t next = 0;
    uint32_t slot = 0;

    ASSERT_TRUE(Compositor::TakeSrvSlot(freeSlots, next, 4, slot));
    EXPECT_EQ(slot, 0u);
    EXPECT_EQ(next, 1u);

    freeSlots.push_back(7);
    ASSERT_TRUE(Compositor::TakeSrvSlot(freeSlots, next, 4, slot));
    EXPECT_EQ(slot, 7u);
    EXPECT_TRUE(freeSlots.empty());
    EXPECT_EQ(next, 1u); // free list hit must not advance the watermark
}

TEST(CompositorSrvSlotTest, ExhaustionFailsAndRecycledSlotsAreHandedBackOut) {
    std::vector<uint32_t> freeSlots;
    uint32_t next = 0;
    uint32_t slot = 0;

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(Compositor::TakeSrvSlot(freeSlots, next, 3, slot));
    }
    EXPECT_FALSE(Compositor::TakeSrvSlot(freeSlots, next, 3, slot));

    Compositor::ReturnSrvSlot(freeSlots, 1);
    ASSERT_TRUE(Compositor::TakeSrvSlot(freeSlots, next, 3, slot));
    EXPECT_EQ(slot, 1u);
    EXPECT_FALSE(Compositor::TakeSrvSlot(freeSlots, next, 3, slot));
}

TEST(CompositorSrvSlotTest, NegativesAreIgnoredAndDoubleReturnsAreDeduped) {
    std::vector<uint32_t> freeSlots;
    Compositor::ReturnSrvSlot(freeSlots, -1);
    EXPECT_TRUE(freeSlots.empty());

    // A slot returned twice would be handed to two textures and alias their
    // SRVs - the recycle helper must defend against it.
    Compositor::ReturnSrvSlot(freeSlots, 5);
    Compositor::ReturnSrvSlot(freeSlots, 5);
    ASSERT_EQ(freeSlots.size(), 1u);
    EXPECT_EQ(freeSlots.back(), 5u);
}

TEST(CompositorBackBufferStateTest, StartsPresentAndTracksTheRenderCycle) {
    BackBufferStateTracker tracker;
    for (int i = 0; i < BackBufferStateTracker::COUNT; ++i) {
        EXPECT_EQ(tracker.Before(i), D3D12_RESOURCE_STATE_PRESENT);
    }

    tracker.OnRenderBegin(1);
    EXPECT_EQ(tracker.Before(1), D3D12_RESOURCE_STATE_RENDER_TARGET);
    EXPECT_EQ(tracker.Before(0), D3D12_RESOURCE_STATE_PRESENT);
    EXPECT_EQ(tracker.Before(2), D3D12_RESOURCE_STATE_PRESENT);

    tracker.OnRenderEnd(1);
    EXPECT_EQ(tracker.Before(1), D3D12_RESOURCE_STATE_PRESENT);
}

TEST(CompositorBackBufferStateTest, ResetRestoresPresentForEveryBuffer) {
    BackBufferStateTracker tracker;
    tracker.OnRenderBegin(0);
    tracker.OnRenderBegin(2);
    tracker.Reset();
    for (int i = 0; i < BackBufferStateTracker::COUNT; ++i) {
        EXPECT_EQ(tracker.Before(i), D3D12_RESOURCE_STATE_PRESENT);
    }
}
