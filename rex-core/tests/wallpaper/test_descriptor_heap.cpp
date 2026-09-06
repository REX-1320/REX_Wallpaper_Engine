#include <gtest/gtest.h>
#include "graphics/renderer/DescriptorHeapManager.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>

namespace rex::wallpaper {
namespace {

class DescriptorHeapManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&m_device));
        if (FAILED(hr)) {
            GTEST_SKIP() << "D3D12 device unavailable (hr=" << std::hex << hr << ")";
        }
    }

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
};

TEST_F(DescriptorHeapManagerTest, DH01_AllocationReturnsUniqueIndices) {
    DescriptorHeapManager heap;
    ASSERT_TRUE(heap.Initialize(m_device.Get(), 256));

    std::vector<UINT> indices;
    for (int i = 0; i < 256; ++i) {
        auto alloc = heap.Allocate();
        ASSERT_TRUE(alloc.has_value()) << "Failed at index " << i;
        indices.push_back(alloc->index);
    }

    // All indices should be unique
    std::sort(indices.begin(), indices.end());
    auto last = std::unique(indices.begin(), indices.end());
    EXPECT_EQ(last, indices.end());
}

TEST_F(DescriptorHeapManagerTest, DH02_Exhaustion) {
    DescriptorHeapManager heap;
    ASSERT_TRUE(heap.Initialize(m_device.Get(), 4));

    std::vector<DescriptorAllocation> allocs;
    for (int i = 0; i < 4; ++i) {
        auto a = heap.Allocate();
        ASSERT_TRUE(a.has_value());
        allocs.push_back(*a);
    }

    auto exhausted = heap.Allocate();
    EXPECT_FALSE(exhausted.has_value());
}

TEST_F(DescriptorHeapManagerTest, DH03_FreeAndReuse) {
    DescriptorHeapManager heap;
    ASSERT_TRUE(heap.Initialize(m_device.Get(), 8));

    auto a = heap.Allocate();
    auto b = heap.Allocate();
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    heap.Free(*a);

    auto reused = heap.Allocate();
    EXPECT_TRUE(reused.has_value());
    EXPECT_EQ(reused->index, a->index);
}

TEST_F(DescriptorHeapManagerTest, DH04_DoubleFreeProtection) {
    DescriptorHeapManager heap;
    ASSERT_TRUE(heap.Initialize(m_device.Get(), 8));

    auto alloc = heap.Allocate();
    ASSERT_TRUE(alloc.has_value());

    heap.Free(*alloc);
    // Second free should not corrupt the heap
    heap.Free(*alloc);

    // Heap should still work correctly
    auto reused = heap.Allocate();
    EXPECT_TRUE(reused.has_value());
}

TEST_F(DescriptorHeapManagerTest, DH05_ThreadSafety) {
    DescriptorHeapManager heap;
    ASSERT_TRUE(heap.Initialize(m_device.Get(), 128));

    std::mutex mtx;
    std::vector<UINT> allocated;

    auto allocateTask = [&]() {
        for (int i = 0; i < 32; ++i) {
            auto idx = heap.Allocate();
            if (idx.has_value()) {
                std::lock_guard lock(mtx);
                allocated.push_back(idx->index);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(allocateTask);
    }
    for (auto& t : threads) t.join();

    // All allocated indices should be unique
    std::sort(allocated.begin(), allocated.end());
    auto last = std::unique(allocated.begin(), allocated.end());
    size_t uniqueCount = std::distance(allocated.begin(), last);
    EXPECT_EQ(uniqueCount, allocated.size());
}

} // namespace
} // namespace rex::wallpaper
