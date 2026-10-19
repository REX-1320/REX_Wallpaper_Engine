// Pool slot leasing (Phase D prerequisite).
//
// The decode thread must not overwrite a pool slot the render thread is still
// reading. Fences give GPU ORDERING but not LIFETIME, so allocation must
// respect a consumed watermark published by the render thread.
//
// PickSlotOldestSafe is pure integer bookkeeping, so it is tested directly
// with no device.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using UINT = unsigned int;

namespace {

// Calls the production allocator directly (audit 2026-10-17, §5.2 #15): the
// previous mirror copy could not detect drift — these tests would keep
// passing while the real semantics changed underneath them.
UINT PickSlotOldestSafe(const int64_t* slotFrameIndex, UINT slots,
                        int64_t consumed, int64_t margin, bool haveConsumed) {
    return rex::wallpaper::FfmpegVideoDecoder::PickSlotOldestSafe(
        slotFrameIndex, slots, consumed, margin, haveConsumed);
}

constexpr UINT kSlots = 8;
constexpr int64_t kMargin = 3;

}  // namespace

TEST(PoolSlotLease, PicksOldestWhenNothingConsumedYet) {
    // Before the render thread publishes a watermark there is nothing to be
    // safe about, so allocate the stalest slot (never blind round-robin).
    int64_t slots[kSlots] = {50, 51, 52, 53, 54, 55, 56, 57};
    EXPECT_EQ(PickSlotOldestSafe(slots, kSlots, -1, kMargin, false), 0u);
    std::swap(slots[0], slots[4]);
    EXPECT_EQ(PickSlotOldestSafe(slots, kSlots, -1, kMargin, false), 4u);
}

TEST(PoolSlotLease, AvoidsSlotsNearTheConsumedWatermark) {
    // Renderer has consumed frame 100. Slots holding 90..96 are inside the
    // 3-frame reuse margin; only the older ones are eligible.
    int64_t slots[kSlots] = {95, 96, 97, 90, 91, 92, 93, 99};
    const UINT slot = PickSlotOldestSafe(slots, kSlots, 100, kMargin, true);
    EXPECT_EQ(slot, 3u);          // holds frame 90, the oldest
    EXPECT_LE(slots[slot], 100 - kMargin);
}

TEST(PoolSlotLease, NeverReusesTheFrameJustConsumed) {
    int64_t slots[kSlots] = {0, 1, 2, 3, 4, 5, 6, 7};
    const int64_t consumed = 7;
    for (int i = 0; i < kSlots; ++i) {
        const UINT slot = PickSlotOldestSafe(slots, kSlots, consumed, kMargin, true);
        bool anySafe = false;
        for (int j = 0; j < kSlots; ++j) {
            if (slots[j] <= consumed - kMargin) { anySafe = true; break; }
        }
        if (anySafe) {
            EXPECT_LE(slots[slot], consumed - kMargin)
                << "a safely-reusable slot existed but was not chosen";
        }
        slots[slot] = 8 + i;   // publish a fresh frame into that slot
    }
}

TEST(PoolSlotLease, SustainedDecodeAheadDegradesGracefully) {
    // Decoder publishes 2 frames per step, renderer consumes 1. The pool WILL
    // saturate, so the margin cannot always be honoured. The invariant that
    // must hold in the saturated regime: consume the STRICT OLDEST slot, never
    // a fresher one, so the pipeline keeps making forward progress.
    int64_t slots[kSlots] = {};
    for (int i = 0; i < kSlots; ++i) slots[i] = i;

    int64_t nextFrame = kSlots;
    int64_t consumed = 0;
    for (int step = 0; step < 200; ++step) {
        for (int k = 0; k < 2; ++k) {
            const UINT slot = PickSlotOldestSafe(slots, kSlots, consumed,
                                                 kMargin, true);
            int64_t oldest = slots[0];
            bool anySafe = false;
            for (int j = 0; j < kSlots; ++j) {
                oldest = std::min(oldest, slots[j]);
                if (slots[j] <= consumed - kMargin) anySafe = true;
            }
            if (anySafe) {
                EXPECT_LE(slots[slot], consumed - kMargin)
                    << "step " << step << ": safe slot existed, not used";
            } else {
                EXPECT_EQ(slots[slot], oldest)
                    << "step " << step << ": saturated, must take strict oldest";
            }
            slots[slot] = nextFrame++;
        }
        consumed = step;
    }
}
TEST(PoolSlotLease, MarginOfZeroIsStillSafeAgainstTheConsumedFrame) {
    // Even with no margin the slot holding the just-consumed frame must not be
    // chosen while any older slot exists.
    int64_t slots[kSlots] = {10, 11, 12, 13, 14, 15, 16, 17};
    const UINT slot = PickSlotOldestSafe(slots, kSlots, 17, 0, true);
    EXPECT_EQ(slot, 0u);
}
