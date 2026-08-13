#include <gtest/gtest.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <windows.h>
#include <psapi.h>

#include <cstdint>

using namespace rex::wallpaper;

namespace {

uint64_t GetPrivateMemory() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc),
                             sizeof(pmc))) {
        return static_cast<uint64_t>(pmc.PrivateUsage);
    }
    return 0;
}

constexpr const char* kSampleImage =
    "C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg";

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Resource stability
// ═══════════════════════════════════════════════════════════════════════════
//
// These are smoke checks, not leak proofs: they catch gross linear growth
// (leaked textures/devices per cycle), not slow one-time driver caching.
// The first init/shutdown warms up one-time allocations (D3D12 device,
// FFmpeg, shader caches) before the baseline is taken.

TEST(ResourceTest, NoMemoryLeakAfterRepeatedInitShutdown) {
    // Warm-up: one-time driver/runtime allocations happen here.
    {
        auto engine = CreateWallpaperEngine();
        ASSERT_NE(engine, nullptr);
        ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());
        engine->Shutdown();
    }

    const uint64_t before = GetPrivateMemory();
    ASSERT_GT(before, 0u);

    for (int i = 0; i < 3; ++i) {
        auto engine = CreateWallpaperEngine();
        ASSERT_NE(engine, nullptr);
        ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());
        engine->Shutdown();
    }

    const uint64_t after = GetPrivateMemory();
    // A 10 MB allowance covers allocator noise; a per-cycle device leak
    // would blow far past it after just 3 cycles.
    EXPECT_LT(after, before + 10ull * 1024 * 1024)
        << "private memory grew from " << before << " to " << after;
}

TEST(ResourceTest, StableMemoryDuringPlayback) {
    auto engine = CreateWallpaperEngine();
    ASSERT_NE(engine, nullptr);
    ASSERT_TRUE(engine->Initialize(WallpaperConfig{}).IsSuccess());

    SceneInfo scene;
    scene.id = "leak_static";
    scene.sourcePath = kSampleImage;
    scene.type = "static";
    auto loadResult = engine->LoadScene(scene);
    if (loadResult.IsError()) {
        GTEST_SKIP() << "sample image not present: "
                     << loadResult.Error().message;
    }
    ASSERT_TRUE(engine->Play().IsSuccess());

    // Static playback renders a frame per loop; memory must stay flat.
    const uint64_t start = GetPrivateMemory();
    for (int i = 0; i < 20; ++i) {
        Sleep(100);
        const uint64_t current = GetPrivateMemory();
        EXPECT_LT(current, start + 50ull * 1024 * 1024)
            << "memory grew at sample " << i;
    }

    engine->Stop();
    engine->Shutdown();
}
