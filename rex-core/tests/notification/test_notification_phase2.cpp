#include <gtest/gtest.h>
#include "modules/Notification/renderer/INotificationRenderer.hpp"
#include "modules/Notification/renderer/NotificationRenderer.hpp"
#include "modules/Notification/animation/NotificationAnimation.hpp"
#include "modules/Notification/sound/NotificationSound.hpp"
#include "modules/Notification/services/NotificationManager.hpp"
#include "modules/Notification/services/NotificationService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"
#include <memory>

using namespace rex::notifications;
using namespace rex::modules;

// ── NotificationRenderer ───────────────────────────────────────────────────

TEST(NotificationRendererTest, ShowAddsCard) {
    NotificationRenderer renderer;
    Notification n;
    n.id = "test1";
    n.title = "Hello";
    EXPECT_NO_THROW(renderer.show(n));
}

TEST(NotificationRendererTest, UpdateExistingCard) {
    NotificationRenderer renderer;
    Notification n;
    n.id = "id1";
    n.progress = 0;
    renderer.show(n);

    n.progress = 50;
    EXPECT_NO_THROW(renderer.update(n));
}

TEST(NotificationRendererTest, DismissWithCallback) {
    NotificationRenderer renderer;
    Notification n;
    n.id = "id1";
    renderer.show(n);

    bool dismissed = false;
    renderer.dismiss("id1", [&]() { dismissed = true; });
    EXPECT_TRUE(dismissed);
}

TEST(NotificationRendererTest, DismissNonExistentNoCrash) {
    NotificationRenderer renderer;
    EXPECT_NO_THROW(renderer.dismiss("nonexistent", nullptr));
}

TEST(NotificationRendererTest, DismissNonExistentWithCallback) {
    NotificationRenderer renderer;
    bool called = false;
    renderer.dismiss("nonexistent", [&]() { called = true; });
    EXPECT_FALSE(called);
}

TEST(NotificationRendererTest, ClearAllRemovesCards) {
    NotificationRenderer renderer;
    Notification n1; n1.id = "a"; renderer.show(n1);
    Notification n2; n2.id = "b"; renderer.show(n2);
    EXPECT_NO_THROW(renderer.clearAll());
}

TEST(NotificationRendererTest, SetPositionStoresValues) {
    NotificationRenderer renderer;
    EXPECT_NO_THROW(renderer.setPosition(2, 50, 100));
}

TEST(NotificationRendererTest, TickAnimatesAlpha) {
    NotificationRenderer renderer;
    Notification n;
    n.id = "anim";
    renderer.show(n);
    renderer.tick(0.25f);
    // Should not crash; visual check would require rendering
    EXPECT_NO_THROW(renderer.tick(0.25f));
}

TEST(NotificationRendererTest, MultipleTicksCompleteAlpha) {
    NotificationRenderer renderer;
    Notification n;
    n.id = "fade";
    renderer.show(n);
    for (int i = 0; i < 10; ++i) {
        renderer.tick(0.1f);
    }
    EXPECT_NO_THROW(renderer.tick(0.1f));
}

TEST(NotificationRendererTest, ShowMultipleDistinctIds) {
    NotificationRenderer renderer;
    for (int i = 0; i < 10; ++i) {
        Notification n;
        n.id = "card_" + std::to_string(i);
        EXPECT_NO_THROW(renderer.show(n));
    }
}

// ── NotificationAnimation ──────────────────────────────────────────────────

TEST(NotificationAnimationTest, StartAnimation) {
    NotificationAnimation anim;
    EXPECT_NO_THROW(anim.start("a", AnimationType::Fade, 1000.0f));
}

TEST(NotificationAnimationTest, TickIncreasesProgress) {
    NotificationAnimation anim;
    anim.start("a", AnimationType::Fade, 100.0f);
    anim.tick(50.0f);
    float prog = anim.getProgress("a");
    EXPECT_GT(prog, 0.0f);
    EXPECT_LT(prog, 1.0f);
}

TEST(NotificationAnimationTest, TickCompletesAnimation) {
    NotificationAnimation anim;
    anim.start("a", AnimationType::Fade, 100.0f);
    anim.tick(200.0f);
    float prog = anim.getProgress("a");
    EXPECT_FLOAT_EQ(prog, 1.0f);
}

TEST(NotificationAnimationTest, CallbackFiredOnCompletion) {
    NotificationAnimation anim;
    bool fired = false;
    anim.start("a", AnimationType::Slide, 100.0f, [&]() { fired = true; });
    anim.tick(200.0f);
    EXPECT_TRUE(fired);
}

TEST(NotificationAnimationTest, CallbackFiredOnlyOnce) {
    NotificationAnimation anim;
    int count = 0;
    anim.start("a", AnimationType::Fade, 50.0f, [&]() { ++count; });
    anim.tick(100.0f);
    anim.tick(100.0f);
    EXPECT_EQ(count, 1);
}

TEST(NotificationAnimationTest, CancelRemovesAnimation) {
    NotificationAnimation anim;
    anim.start("a", AnimationType::Fade, 1000.0f);
    anim.cancel("a");
    EXPECT_FLOAT_EQ(anim.getProgress("a"), 1.0f);
}

TEST(NotificationAnimationTest, MultipleAnimationsIndependent) {
    NotificationAnimation anim;
    anim.start("a", AnimationType::Fade, 100.0f);
    anim.start("b", AnimationType::Slide, 200.0f);
    anim.tick(50.0f);
    float progA = anim.getProgress("a");
    float progB = anim.getProgress("b");
    EXPECT_GT(progA, progB);
}

TEST(NotificationAnimationTest, GetProgressUnknownReturnsOne) {
    NotificationAnimation anim;
    EXPECT_FLOAT_EQ(anim.getProgress("unknown"), 1.0f);
}

TEST(NotificationAnimationTest, StartOverwritesExisting) {
    NotificationAnimation anim;
    anim.start("a", AnimationType::Fade, 1000.0f);
    anim.tick(500.0f);
    anim.start("a", AnimationType::Scale, 50.0f);
    anim.tick(25.0f);
    EXPECT_FLOAT_EQ(anim.getProgress("a"), 0.5f);
}

// ── NotificationSound ──────────────────────────────────────────────────────

TEST(NotificationSoundTest, PlayForNotificationNoCrash) {
    NotificationSound sound;
    Notification n;
    EXPECT_NO_THROW(sound.playForNotification(n));
}

TEST(NotificationSoundTest, PlaySilentNotificationNoCrash) {
    NotificationSound sound;
    Notification n;
    n.silent = true;
    EXPECT_NO_THROW(sound.playForNotification(n));
}

TEST(NotificationSoundTest, PlayWhenMutedNoCrash) {
    NotificationSound sound;
    sound.setMuted(true);
    Notification n;
    EXPECT_NO_THROW(sound.playForNotification(n));
}

TEST(NotificationSoundTest, SetSoundFileNoCrash) {
    NotificationSound sound;
    EXPECT_NO_THROW(sound.setSoundFile(NotificationType::Error, "error.wav"));
    EXPECT_NO_THROW(sound.setSoundFile(NotificationType::Warning, "warning.wav"));
}

TEST(NotificationSoundTest, MuteToggle) {
    NotificationSound sound;
    sound.setMuted(true);
    // No crash when muted
    EXPECT_NO_THROW(sound.playForNotification(Notification{}));
    sound.setMuted(false);
    EXPECT_NO_THROW(sound.playForNotification(Notification{}));
}

// ── NotificationManager + Renderer Integration ─────────────────────────────

class NotificationManagerRendererTest : public ::testing::Test {
protected:
    struct RendererSpy : public INotificationRenderer {
        int showCount = 0;
        int updateCount = 0;
        int dismissCount = 0;
        int clearAllCount = 0;
        int tickCount = 0;
        std::string lastDismissed;
        std::function<void()> storedCallback;

        void show(const Notification&) override { ++showCount; }
        void update(const Notification&) override { ++updateCount; }
        void dismiss(const std::string& id, std::function<void()> cb) override {
            ++dismissCount;
            lastDismissed = id;
            storedCallback = cb;
        }
        void clearAll() override { ++clearAllCount; }
        void setPosition(int, int, int) override {}
        void tick(float) override { ++tickCount; }
    };

    void SetUp() override {
        svc_ = std::make_unique<NotificationService>(bus_);
        mgr_ = &svc_->manager();

        auto spy = std::make_unique<RendererSpy>();
        spy_ = spy.get();
        mgr_->setRenderer(std::move(spy));
    }

    EventBus bus_;
    std::unique_ptr<NotificationService> svc_;
    NotificationManager* mgr_;
    RendererSpy* spy_;
};

TEST_F(NotificationManagerRendererTest, ShowCallsRendererShow) {
    Notification n; n.title = "t"; n.message = "m";
    svc_->show(n);
    EXPECT_EQ(spy_->showCount, 1);
}

TEST_F(NotificationManagerRendererTest, UpdateCallsRendererUpdate) {
    std::string id = svc_->show(Notification{});
    spy_->showCount = 0; // reset
    Notification updated; updated.progress = 42;
    svc_->update(id, updated);
    EXPECT_EQ(spy_->updateCount, 1);
}

TEST_F(NotificationManagerRendererTest, DismissCallsRendererDismiss) {
    std::string id = svc_->show(Notification{});
    svc_->dismiss(id);
    EXPECT_EQ(spy_->dismissCount, 1);
    EXPECT_EQ(spy_->lastDismissed, id);
}

TEST_F(NotificationManagerRendererTest, DismissAllCallsClearAll) {
    svc_->show(Notification{});
    svc_->show(Notification{});
    svc_->dismissAll();
    EXPECT_EQ(spy_->clearAllCount, 1);
}

TEST_F(NotificationManagerRendererTest, UpdateTickCallsRendererTick) {
    svc_->update(0.016f);
    EXPECT_EQ(spy_->tickCount, 1);
}

TEST_F(NotificationManagerRendererTest, DuplicateIncrementsCallsRenderUpdate) {
    Notification n; n.moduleId = "m"; n.title = "T"; n.message = "M";
    std::string id1 = svc_->show(n);
    int updateBefore = spy_->updateCount;
    std::string id2 = svc_->show(n);
    EXPECT_EQ(id1, id2);
    EXPECT_GT(spy_->updateCount, updateBefore);
}

TEST_F(NotificationManagerRendererTest, DismissCallbackMovesToHistory) {
    std::string id = svc_->show(Notification{});
    svc_->dismiss(id);
    ASSERT_TRUE(spy_->storedCallback);
    spy_->storedCallback();
    EXPECT_EQ(svc_->getHistory().size(), 1);
}

// ── NotificationService setRenderer ────────────────────────────────────────

TEST(NotificationServiceSetRendererTest, SetRendererNoCrash) {
    EventBus bus;
    NotificationService svc(bus);
    EXPECT_NO_THROW(svc.setRenderer(std::make_unique<NotificationRenderer>()));
}

TEST(NotificationServiceSetRendererTest, SetRendererThenShowNoCrash) {
    EventBus bus;
    NotificationService svc(bus);
    svc.setRenderer(std::make_unique<NotificationRenderer>());
    EXPECT_NO_THROW(svc.show(Notification{}));
}
