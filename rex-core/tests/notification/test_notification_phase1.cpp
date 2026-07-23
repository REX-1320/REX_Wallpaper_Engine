#include <gtest/gtest.h>
#include "modules/Notification/models/Notification.hpp"
#include "modules/Notification/history/NotificationHistory.hpp"
#include "modules/Notification/services/NotificationManager.hpp"
#include "modules/Notification/services/NotificationService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"

using namespace rex::notifications;
using namespace rex::modules;

// ── Notification Model: Phase 1 fields ─────────────────────────────────────

TEST(NotificationPhase1Test, DefaultDuplicateCountIsOne) {
    Notification n;
    EXPECT_EQ(n.duplicateCount, 1);
}

TEST(NotificationPhase1Test, DefaultIsGroupedIsFalse) {
    Notification n;
    EXPECT_FALSE(n.isGrouped);
}

// ── ModuleEventType: new enum values ────────────────────────────────────────

TEST(ModuleEventTypePhase1Test, NotificationActionInvokedExists) {
    ModuleEvent ev;
    ev.type = ModuleEventType::NotificationActionInvoked;
    EXPECT_EQ(ev.type, ModuleEventType::NotificationActionInvoked);
}

TEST(ModuleEventTypePhase1Test, HistoryClearedExists) {
    ModuleEvent ev;
    ev.type = ModuleEventType::HistoryCleared;
    EXPECT_EQ(ev.type, ModuleEventType::HistoryCleared);
}

// ── NotificationHistory ────────────────────────────────────────────────────

class NotificationHistoryTest : public ::testing::Test {
protected:
    NotificationHistory history_;
};

TEST_F(NotificationHistoryTest, InitiallyEmpty) {
    auto all = history_.getAll();
    EXPECT_TRUE(all.empty());
}

TEST_F(NotificationHistoryTest, AddIncreasesCount) {
    history_.add(Notification{});
    EXPECT_EQ(history_.getAll().size(), 1);
}

TEST_F(NotificationHistoryTest, GetAllReturnsAllAdded) {
    history_.add(Notification{});
    history_.add(Notification{});
    history_.add(Notification{});
    EXPECT_EQ(history_.getAll().size(), 3);
}

TEST_F(NotificationHistoryTest, GetAllFiltersByModule) {
    Notification n1; n1.moduleId = "module_a";
    Notification n2; n2.moduleId = "module_b";
    Notification n3; n3.moduleId = "module_a";
    history_.add(n1);
    history_.add(n2);
    history_.add(n3);
    EXPECT_EQ(history_.getAll("module_a").size(), 2);
    EXPECT_EQ(history_.getAll("module_b").size(), 1);
    EXPECT_EQ(history_.getAll("nonexistent").size(), 0);
}

TEST_F(NotificationHistoryTest, GetAllFiltersByType) {
    Notification info;   info.type = NotificationType::Information;
    Notification warn;   warn.type = NotificationType::Warning;
    Notification error;  error.type = NotificationType::Error;
    history_.add(info);
    history_.add(warn);
    history_.add(error);
    EXPECT_EQ(history_.getAll("", NotificationType::Information).size(), 1);
    EXPECT_EQ(history_.getAll("", NotificationType::Warning).size(), 1);
    EXPECT_EQ(history_.getAll("", NotificationType::Error).size(), 1);
    EXPECT_EQ(history_.getAll("", NotificationType::Custom).size(), 3);
}

TEST_F(NotificationHistoryTest, GetAllCombinedFilter) {
    Notification n; n.moduleId = "mod"; n.type = NotificationType::Warning;
    history_.add(n);
    EXPECT_EQ(history_.getAll("mod", NotificationType::Warning).size(), 1);
    EXPECT_EQ(history_.getAll("mod", NotificationType::Information).size(), 0);
    EXPECT_EQ(history_.getAll("other", NotificationType::Warning).size(), 0);
}

TEST_F(NotificationHistoryTest, ClearRemovesAll) {
    history_.add(Notification{});
    history_.add(Notification{});
    ASSERT_EQ(history_.getAll().size(), 2);
    history_.clear();
    EXPECT_TRUE(history_.getAll().empty());
}

TEST_F(NotificationHistoryTest, SearchMatchesTitle) {
    Notification n; n.title = "Disk Space Low";
    history_.add(n);
    EXPECT_EQ(history_.search("Disk").size(), 1);
    EXPECT_EQ(history_.search("disk").size(), 1);
    EXPECT_EQ(history_.search("Memory").size(), 0);
}

TEST_F(NotificationHistoryTest, SearchMatchesMessage) {
    Notification n; n.message = "Something went wrong";
    history_.add(n);
    EXPECT_EQ(history_.search("wrong").size(), 1);
    EXPECT_EQ(history_.search("something").size(), 1);
    EXPECT_EQ(history_.search("correct").size(), 0);
}

TEST_F(NotificationHistoryTest, SearchEmptyQueryReturnsAll) {
    history_.add(Notification{});
    history_.add(Notification{});
    EXPECT_EQ(history_.search("").size(), 2);
}

TEST_F(NotificationHistoryTest, MaxSizeEvictsOldest) {
    history_.setMaxSize(3);
    for (int i = 0; i < 5; ++i) {
        Notification n;
        n.title = "Entry " + std::to_string(i);
        history_.add(n);
    }
    auto all = history_.getAll();
    ASSERT_EQ(all.size(), 3);
    EXPECT_EQ(all[0].title, "Entry 2");
    EXPECT_EQ(all[1].title, "Entry 3");
    EXPECT_EQ(all[2].title, "Entry 4");
}

TEST_F(NotificationHistoryTest, ZeroMaxSizeDoesNotEvict) {
    history_.setMaxSize(0);
    for (int i = 0; i < 10; ++i) {
        history_.add(Notification{});
    }
    EXPECT_EQ(history_.getAll().size(), 10);
}

// ── NotificationManager: duplicate grouping ────────────────────────────────

class NotificationManagerPhase1Test : public ::testing::Test {
protected:
    struct TestActionRecorder {
        std::string lastId;
        void reset() { lastId.clear(); }
    };

    void SetUp() override {
        // Use a stub service so manager can construct
        svc_ = std::make_unique<NotificationService>(bus_);
        mgr_ = std::make_unique<NotificationManager>(bus_, *svc_);
        mgr_->setDuplicateWindow(std::chrono::milliseconds(5000));
        recorder_.reset();
    }

    Notification makeNotif(const std::string& moduleId, const std::string& title,
                           const std::string& message, const std::string& groupKey = "") {
        Notification n;
        n.moduleId = moduleId;
        n.title = title;
        n.message = message;
        n.groupKey = groupKey;
        return n;
    }

    EventBus bus_;
    std::unique_ptr<NotificationService> svc_;
    std::unique_ptr<NotificationManager> mgr_;
    TestActionRecorder recorder_;
};

TEST_F(NotificationManagerPhase1Test, DuplicateByTitleAndMessageIncrementsCount) {
    Notification n1 = makeNotif("mod", "Title", "Message");
    std::string id1 = mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod", "Title", "Message");
    std::string id2 = mgr_->showNotification(n2);
    EXPECT_EQ(id1, id2);
    auto opt = mgr_->getNotification(id1);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->duplicateCount, 2);
}

TEST_F(NotificationManagerPhase1Test, DuplicateByGroupKeyIncrementsCount) {
    Notification n1 = makeNotif("mod", "A", "B", "my_group");
    std::string id1 = mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod", "C", "D", "my_group");
    std::string id2 = mgr_->showNotification(n2);
    EXPECT_EQ(id1, id2);
    auto opt = mgr_->getNotification(id1);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->duplicateCount, 2);
}

TEST_F(NotificationManagerPhase1Test, DifferentModuleNotDuplicate) {
    Notification n1 = makeNotif("mod1", "Same", "Same");
    std::string id1 = mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod2", "Same", "Same");
    std::string id2 = mgr_->showNotification(n2);
    EXPECT_NE(id1, id2);
}

TEST_F(NotificationManagerPhase1Test, DifferentTitleNotDuplicate) {
    Notification n1 = makeNotif("mod", "Alpha", "Same");
    mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod", "Beta", "Same");
    std::string id2 = mgr_->showNotification(n2);
    auto opt = mgr_->getNotification(id2);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->duplicateCount, 1);
}

TEST_F(NotificationManagerPhase1Test, DifferentMessageNotDuplicate) {
    Notification n1 = makeNotif("mod", "Same", "Alpha");
    mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod", "Same", "Beta");
    std::string id2 = mgr_->showNotification(n2);
    auto opt = mgr_->getNotification(id2);
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->duplicateCount, 1);
}

TEST_F(NotificationManagerPhase1Test, EmptyTitleAndMessageWithNoGroupKeyCreatesNew) {
    Notification n1;
    n1.moduleId = "mod";
    std::string id1 = mgr_->showNotification(n1);
    Notification n2;
    n2.moduleId = "mod";
    std::string id2 = mgr_->showNotification(n2);
    EXPECT_NE(id1, id2);
}

TEST_F(NotificationManagerPhase1Test, DuplicateOutsideWindowCreatesNew) {
    mgr_->setDuplicateWindow(std::chrono::milliseconds(0));
    Notification n1 = makeNotif("mod", "Title", "Msg");
    std::string id1 = mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod", "Title", "Msg");
    std::string id2 = mgr_->showNotification(n2);
    EXPECT_NE(id1, id2);
}

TEST_F(NotificationManagerPhase1Test, DuplicateTriggersUpdatedEvent) {
    int eventCount = 0;
    bus_.subscribe([&](const ModuleEvent& ev) {
        if (ev.type == ModuleEventType::NotificationUpdated) ++eventCount;
    });
    Notification n1 = makeNotif("mod", "T", "M");
    mgr_->showNotification(n1);
    Notification n2 = makeNotif("mod", "T", "M");
    mgr_->showNotification(n2);
    EXPECT_GE(eventCount, 1);
}

// ── NotificationManager: invokeAction ──────────────────────────────────────

TEST_F(NotificationManagerPhase1Test, InvokeActionCallsCallback) {
    Notification n = makeNotif("mod", "Test", "Msg");
    n.actions.push_back(NotificationAction{});
    n.actions.back().id = "act1";
    n.actions.back().label = "Action 1";
    n.actions.back().callback = [&](const std::string& id) {
        recorder_.lastId = id;
    };
    std::string id = mgr_->showNotification(n);
    mgr_->invokeAction(id, "act1");
    EXPECT_EQ(recorder_.lastId, id);
}

TEST_F(NotificationManagerPhase1Test, InvokeActionPublishesEvent) {
    bool eventFired = false;
    bus_.subscribe([&](const ModuleEvent& ev) {
        if (ev.type == ModuleEventType::NotificationActionInvoked) eventFired = true;
    });
    Notification n = makeNotif("mod", "Test", "Msg");
    n.actions.push_back(NotificationAction{});
    n.actions.back().id = "act1";
    n.actions.back().callback = [](const std::string&) {};
    std::string id = mgr_->showNotification(n);
    mgr_->invokeAction(id, "act1");
    EXPECT_TRUE(eventFired);
}

TEST_F(NotificationManagerPhase1Test, InvokeActionDisabledDoesNotFire) {
    bool called = false;
    Notification n = makeNotif("mod", "Test", "Msg");
    n.actions.push_back(NotificationAction{});
    n.actions.back().id = "act1";
    n.actions.back().enabled = false;
    n.actions.back().callback = [&](const std::string&) { called = true; };
    std::string id = mgr_->showNotification(n);
    mgr_->invokeAction(id, "act1");
    EXPECT_FALSE(called);
}

TEST_F(NotificationManagerPhase1Test, InvokeActionNonExistentIdNoCrash) {
    EXPECT_NO_THROW(mgr_->invokeAction("does_not_exist", "act1"));
}

TEST_F(NotificationManagerPhase1Test, InvokeActionNonExistentActionIdNoCrash) {
    Notification n = makeNotif("mod", "Test", "Msg");
    n.actions.push_back(NotificationAction{});
    n.actions.back().id = "real";
    std::string id = mgr_->showNotification(n);
    EXPECT_NO_THROW(mgr_->invokeAction(id, "fake"));
}

// ── NotificationManager: history ───────────────────────────────────────────

TEST_F(NotificationManagerPhase1Test, DismissAddsToHistory) {
    std::string id = mgr_->showNotification(makeNotif("mod", "T", "M"));
    mgr_->dismissNotification(id);
    auto history = mgr_->getHistory();
    EXPECT_EQ(history.size(), 1);
    EXPECT_EQ(history[0].id, id);
}

TEST_F(NotificationManagerPhase1Test, DismissAllAddsAllToHistory) {
    mgr_->showNotification(makeNotif("mod", "A", "M1"));
    mgr_->showNotification(makeNotif("mod", "B", "M2"));
    mgr_->dismissAll();
    EXPECT_EQ(mgr_->getHistory().size(), 2);
}

TEST_F(NotificationManagerPhase1Test, ClearHistoryEmptiesHistory) {
    std::string id = mgr_->showNotification(makeNotif("mod", "T", "M"));
    mgr_->dismissNotification(id);
    ASSERT_EQ(mgr_->getHistory().size(), 1);
    mgr_->clearHistory();
    EXPECT_TRUE(mgr_->getHistory().empty());
}

TEST_F(NotificationManagerPhase1Test, ClearHistoryPublishesEvent) {
    bool eventFired = false;
    bus_.subscribe([&](const ModuleEvent& ev) {
        if (ev.type == ModuleEventType::HistoryCleared) eventFired = true;
    });
    mgr_->clearHistory();
    EXPECT_TRUE(eventFired);
}

TEST_F(NotificationManagerPhase1Test, GetHistoryFiltersByModule) {
    Notification n1 = makeNotif("mod_a", "T1", "M1");
    Notification n2 = makeNotif("mod_b", "T2", "M2");
    mgr_->dismissNotification(mgr_->showNotification(n1));
    mgr_->dismissNotification(mgr_->showNotification(n2));
    EXPECT_EQ(mgr_->getHistory("mod_a").size(), 1);
    EXPECT_EQ(mgr_->getHistory("mod_b").size(), 1);
    EXPECT_EQ(mgr_->getHistory("mod_c").size(), 0);
}

// ── NotificationService: Phase 1 delegation ────────────────────────────────

class NotificationServicePhase1Test : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<NotificationService>(bus_);
    }

    EventBus bus_;
    std::unique_ptr<NotificationService> service_;
};

TEST_F(NotificationServicePhase1Test, InvokeActionDelegatesToManager) {
    Notification n;
    n.moduleId = "mod";
    n.title = "Test";
    n.actions.push_back(NotificationAction{});
    n.actions.back().id = "act1";
    n.actions.back().callback = [](const std::string&) {};
    std::string id = service_->show(n);
    EXPECT_NO_THROW(service_->invokeAction(id, "act1"));
}

TEST_F(NotificationServicePhase1Test, GetHistoryReturnsEmptyInitially) {
    EXPECT_TRUE(service_->getHistory().empty());
}

TEST_F(NotificationServicePhase1Test, DismissAddsToHistoryViaService) {
    std::string id = service_->show(Notification{});
    service_->dismiss(id);
    EXPECT_EQ(service_->getHistory().size(), 1);
}

TEST_F(NotificationServicePhase1Test, ClearHistoryViaService) {
    std::string id = service_->show(Notification{});
    service_->dismiss(id);
    ASSERT_EQ(service_->getHistory().size(), 1);
    service_->clearHistory();
    EXPECT_TRUE(service_->getHistory().empty());
}
