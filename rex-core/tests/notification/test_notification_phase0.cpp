#include <gtest/gtest.h>
#include <memory>
#include <chrono>
#include <thread>

#include "modules/Notification/models/NotificationType.hpp"
#include "modules/Notification/models/NotificationPriority.hpp"
#include "modules/Notification/models/NotificationAction.hpp"
#include "modules/Notification/models/NotificationState.hpp"
#include "modules/Notification/models/Notification.hpp"
#include "modules/Notification/services/NotificationQueue.hpp"
#include "modules/Notification/services/NotificationManager.hpp"
#include "modules/Notification/services/NotificationService.hpp"
#include "modules/Notification/renderer/INotificationRenderer.hpp"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include "modules/EventBus.hpp"
#include "modules/ModuleEvents.hpp"

using namespace rex::notifications;
using namespace rex::modules;

// ── Model Tests ──────────────────────────────────────────────────────────

TEST(NotificationTypeTest, EnumHasValues) {
    EXPECT_EQ(static_cast<int>(NotificationType::Information), 0);
    EXPECT_EQ(static_cast<int>(NotificationType::Success), 1);
    EXPECT_EQ(static_cast<int>(NotificationType::Warning), 2);
    EXPECT_EQ(static_cast<int>(NotificationType::Error), 3);
    EXPECT_EQ(static_cast<int>(NotificationType::Progress), 4);
    EXPECT_EQ(static_cast<int>(NotificationType::Download), 5);
    EXPECT_EQ(static_cast<int>(NotificationType::Update), 6);
    EXPECT_EQ(static_cast<int>(NotificationType::Reminder), 7);
    EXPECT_EQ(static_cast<int>(NotificationType::System), 8);
    EXPECT_EQ(static_cast<int>(NotificationType::Custom), 9);
}

TEST(NotificationPriorityTest, Values) {
    EXPECT_EQ(static_cast<int>(NotificationPriority::Critical), 0);
    EXPECT_EQ(static_cast<int>(NotificationPriority::High), 1);
    EXPECT_EQ(static_cast<int>(NotificationPriority::Normal), 2);
    EXPECT_EQ(static_cast<int>(NotificationPriority::Low), 3);
    EXPECT_EQ(static_cast<int>(NotificationPriority::Background), 4);
}

TEST(NotificationStateTest, Values) {
    EXPECT_NE(static_cast<int>(NotificationState::Pending),
              static_cast<int>(NotificationState::Queued));
    EXPECT_NE(static_cast<int>(NotificationState::Displaying),
              static_cast<int>(NotificationState::Dismissed));
}

TEST(NotificationActionTest, Defaults) {
    NotificationAction action;
    EXPECT_TRUE(action.id.empty());
    EXPECT_TRUE(action.label.empty());
    EXPECT_TRUE(action.enabled);
}

TEST(NotificationTest, DefaultConstruction) {
    Notification n;
    EXPECT_TRUE(n.id.empty());
    EXPECT_EQ(n.type, NotificationType::Information);
    EXPECT_EQ(n.priority, NotificationPriority::Normal);
    EXPECT_EQ(n.duration.count(), 5000);
    EXPECT_EQ(n.progress, -1);
    EXPECT_TRUE(n.closable);
    EXPECT_FALSE(n.persistent);
    EXPECT_FALSE(n.silent);
    EXPECT_EQ(n.state, NotificationState::Pending);
}

TEST(NotificationTest, CustomConstruction) {
    Notification n;
    n.moduleId = "test.module";
    n.title = "Test Title";
    n.message = "Test Message";
    n.type = NotificationType::Warning;
    n.priority = NotificationPriority::High;
    n.duration = std::chrono::milliseconds(3000);
    n.closable = true;
    n.persistent = false;

    EXPECT_EQ(n.moduleId, "test.module");
    EXPECT_EQ(n.title, "Test Title");
    EXPECT_EQ(n.message, "Test Message");
    EXPECT_EQ(n.type, NotificationType::Warning);
    EXPECT_EQ(n.priority, NotificationPriority::High);
    EXPECT_EQ(n.duration.count(), 3000);
}

TEST(NotificationTest, ProgressValues) {
    Notification n;
    EXPECT_EQ(n.progress, -1);

    n.progress = 0;
    EXPECT_EQ(n.progress, 0);

    n.progress = 50;
    EXPECT_EQ(n.progress, 50);

    n.progress = 100;
    EXPECT_EQ(n.progress, 100);

    n.progressStatus = "Downloading...";
    EXPECT_EQ(n.progressStatus, "Downloading...");
}

TEST(NotificationTest, GroupKey) {
    Notification a, b;
    a.groupKey = "update_1";
    b.groupKey = "update_1";
    EXPECT_EQ(a.groupKey, b.groupKey);

    b.groupKey = "update_2";
    EXPECT_NE(a.groupKey, b.groupKey);
}

// ── Notification Queue Tests ─────────────────────────────────────────────

class NotificationQueueTest : public ::testing::Test {
protected:
    NotificationQueue queue_;
};

TEST_F(NotificationQueueTest, InitiallyEmpty) {
    EXPECT_EQ(queue_.size(), 0);
    EXPECT_EQ(queue_.peek(), nullptr);
}

TEST_F(NotificationQueueTest, EnqueueAndDequeue) {
    Notification n;
    n.id = "test1";
    n.title = "Test";

    queue_.enqueue(n);
    EXPECT_EQ(queue_.size(), 1);

    auto result = queue_.dequeue();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, "test1");
    EXPECT_EQ(queue_.size(), 0);
}

TEST_F(NotificationQueueTest, DequeueEmptyReturnsNullopt) {
    auto result = queue_.dequeue();
    EXPECT_FALSE(result.has_value());
}

TEST_F(NotificationQueueTest, PeekReturnsFront) {
    Notification a, b;
    a.id = "first";
    b.id = "second";

    queue_.enqueue(a);
    queue_.enqueue(b);

    const Notification* front = queue_.peek();
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->id, "first");
}

TEST_F(NotificationQueueTest, RemoveExisting) {
    Notification n;
    n.id = "remove_me";
    queue_.enqueue(n);
    EXPECT_EQ(queue_.size(), 1);

    EXPECT_TRUE(queue_.remove("remove_me"));
    EXPECT_EQ(queue_.size(), 0);
}

TEST_F(NotificationQueueTest, RemoveNonExisting) {
    EXPECT_FALSE(queue_.remove("nonexistent"));
}

TEST_F(NotificationQueueTest, ClearEmptiesQueue) {
    for (int i = 0; i < 10; ++i) {
        Notification n;
        n.id = "n_" + std::to_string(i);
        queue_.enqueue(n);
    }
    EXPECT_EQ(queue_.size(), 10);

    queue_.clear();
    EXPECT_EQ(queue_.size(), 0);
    EXPECT_EQ(queue_.peek(), nullptr);
}

TEST_F(NotificationQueueTest, PriorityOrdering) {
    Notification low, normal, high, critical;
    low.id = "low";
    low.priority = NotificationPriority::Low;
    normal.id = "normal";
    normal.priority = NotificationPriority::Normal;
    high.id = "high";
    high.priority = NotificationPriority::High;
    critical.id = "critical";
    critical.priority = NotificationPriority::Critical;

    queue_.enqueue(low);
    queue_.enqueue(high);
    queue_.enqueue(normal);
    queue_.enqueue(critical);

    EXPECT_EQ(queue_.dequeue()->id, "critical");
    EXPECT_EQ(queue_.dequeue()->id, "high");
    EXPECT_EQ(queue_.dequeue()->id, "normal");
    EXPECT_EQ(queue_.dequeue()->id, "low");
}

TEST_F(NotificationQueueTest, PrioritizeBringsToFront) {
    Notification a, b, c;
    a.id = "a";
    b.id = "b";
    c.id = "c";

    queue_.enqueue(a);
    queue_.enqueue(b);
    queue_.enqueue(c);

    queue_.prioritize("c");
    EXPECT_EQ(queue_.dequeue()->id, "c");
}

// ── Notification Manager Tests ───────────────────────────────────────────

class TestNotificationService : public INotificationService {
public:
    std::string show(const Notification& n) override {
        lastShown = n;
        return n.id;
    }
    bool update(const std::string& id, const Notification&) override {
        updatedIds.push_back(id);
        return true;
    }
    void dismiss(const std::string& id) override { dismissedIds.push_back(id); }
    void dismissAll() override { dismissedAll_ = true; }
    void invokeAction(const std::string&, const std::string&) override {}
    std::vector<Notification> getHistory(const std::string&, NotificationType) override { return {}; }
    void clearHistory() override {}
    std::vector<Notification> getDisplayed() override { return {}; }
    std::optional<Notification> getById(const std::string&) override { return std::nullopt; }

    Notification lastShown;
    std::vector<std::string> updatedIds;
    std::vector<std::string> dismissedIds;
    bool dismissedAll_ = false;
};

class NotificationManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<NotificationManager>(bus_, service_);
    }

    EventBus bus_;
    TestNotificationService service_;
    std::unique_ptr<NotificationManager> manager_;
};

TEST_F(NotificationManagerTest, ShowReturnsId) {
    Notification n;
    n.title = "Test";
    n.message = "Hello";

    std::string id = manager_->showNotification(n);
    EXPECT_FALSE(id.empty());
    EXPECT_TRUE(id.find("notif_") == 0);
}

TEST_F(NotificationManagerTest, ShowMakesNotificationDisplayed) {
    Notification n;
    n.title = "Test";

    std::string id = manager_->showNotification(n);
    auto displayed = manager_->getDisplayedNotifications();
    ASSERT_EQ(displayed.size(), 1);
    EXPECT_EQ(displayed[0].id, id);
    EXPECT_EQ(displayed[0].state, NotificationState::Displaying);
}

TEST_F(NotificationManagerTest, MaxVisibleEnforced) {
    manager_->setMaxVisible(2);

    for (int i = 0; i < 5; ++i) {
        Notification n;
        n.id = "n" + std::to_string(i);
        n.title = "Test " + std::to_string(i);
        manager_->showNotification(n);
    }

    auto displayed = manager_->getDisplayedNotifications();
    EXPECT_EQ(displayed.size(), 2);
}

TEST_F(NotificationManagerTest, DismissRemovesFromActive) {
    std::string id = manager_->showNotification(Notification{});
    ASSERT_EQ(manager_->getDisplayedNotifications().size(), 1);

    EXPECT_TRUE(manager_->dismissNotification(id));
    EXPECT_EQ(manager_->getDisplayedNotifications().size(), 0);
}

TEST_F(NotificationManagerTest, DismissNonExistingReturnsFalse) {
    EXPECT_FALSE(manager_->dismissNotification("nonexistent"));
}

TEST_F(NotificationManagerTest, DismissAllClearsEverything) {
    for (int i = 0; i < 3; ++i) {
        Notification n;
        n.id = "n" + std::to_string(i);
        manager_->showNotification(n);
    }

    manager_->dismissAll();
    EXPECT_EQ(manager_->getDisplayedNotifications().size(), 0);
}

TEST_F(NotificationManagerTest, UpdateExistingNotification) {
    std::string id = manager_->showNotification(Notification{});
    ASSERT_TRUE(manager_->getNotification(id).has_value());

    Notification updated;
    updated.title = "Updated Title";
    updated.message = "Updated Message";
    updated.progress = 75;

    EXPECT_TRUE(manager_->updateNotification(id, updated));

    auto result = manager_->getNotification(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->title, "Updated Title");
    EXPECT_EQ(result->message, "Updated Message");
    EXPECT_EQ(result->progress, 75);
}

TEST_F(NotificationManagerTest, UpdateNonExistingReturnsFalse) {
    EXPECT_FALSE(manager_->updateNotification("nonexistent", Notification{}));
}

TEST_F(NotificationManagerTest, GetByIdReturnsCorrect) {
    Notification n;
    n.title = "FindMe";

    std::string id = manager_->showNotification(n);
    auto result = manager_->getNotification(id);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->title, "FindMe");
}

TEST_F(NotificationManagerTest, GetByIdNonExisting) {
    auto result = manager_->getNotification("nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST_F(NotificationManagerTest, DismissPromotesFromQueue) {
    manager_->setMaxVisible(1);

    Notification first;
    first.title = "first";
    std::string firstId = manager_->showNotification(first);

    Notification second;
    second.title = "second";
    std::string secondId = manager_->showNotification(second);

    auto displayed = manager_->getDisplayedNotifications();
    ASSERT_EQ(displayed.size(), 1);
    EXPECT_EQ(displayed[0].id, firstId);

    manager_->dismissNotification(firstId);

    displayed = manager_->getDisplayedNotifications();
    ASSERT_EQ(displayed.size(), 1);
    EXPECT_EQ(displayed[0].id, secondId);
}

TEST_F(NotificationManagerTest, AutoDismissAfterDuration) {
    Notification n;
    n.title = "Timed";
    n.duration = std::chrono::milliseconds(1);

    std::string id = manager_->showNotification(n);
    ASSERT_EQ(manager_->getDisplayedNotifications().size(), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    manager_->update(0.01f);

    EXPECT_EQ(manager_->getDisplayedNotifications().size(), 0);
}

TEST_F(NotificationManagerTest, PersistentNotificationNotAutoDismissed) {
    Notification n;
    n.title = "Persistent";
    n.duration = std::chrono::milliseconds(1);
    n.persistent = true;

    std::string id = manager_->showNotification(n);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    manager_->update(0.01f);

    EXPECT_EQ(manager_->getDisplayedNotifications().size(), 1);
}

TEST_F(NotificationManagerTest, ZeroDurationIsPersistent) {
    Notification n;
    n.title = "Zero Duration";
    n.duration = std::chrono::milliseconds(0);

    std::string id = manager_->showNotification(n);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    manager_->update(0.01f);

    EXPECT_EQ(manager_->getDisplayedNotifications().size(), 1);
}

TEST_F(NotificationManagerTest, ConfigChangeAffectsMaxVisible) {
    EXPECT_EQ(manager_->maxVisible(), 5);
    manager_->setMaxVisible(10);
    EXPECT_EQ(manager_->maxVisible(), 10);
}

TEST_F(NotificationManagerTest, ConfigChangeAffectsDefaultDuration) {
    EXPECT_EQ(manager_->defaultDuration().count(), 5000);
    manager_->setDefaultDuration(std::chrono::milliseconds(3000));
    EXPECT_EQ(manager_->defaultDuration().count(), 3000);
}

// ── Event Bus Integration Tests ──────────────────────────────────────────

TEST_F(NotificationManagerTest, ShowPublishesCreatedAndShownEvents) {
    std::vector<ModuleEventType> received;
    bus_.subscribe([&](const ModuleEvent& e) {
        received.push_back(e.type);
    });

    manager_->showNotification(Notification{});

    ASSERT_GE(received.size(), 2);
    EXPECT_EQ(received[0], ModuleEventType::NotificationCreated);
    EXPECT_EQ(received[1], ModuleEventType::NotificationShown);
}

TEST_F(NotificationManagerTest, DismissPublishesDismissedEvent) {
    std::vector<ModuleEventType> received;
    bus_.subscribe([&](const ModuleEvent& e) {
        received.push_back(e.type);
    });

    std::string id = manager_->showNotification(Notification{});
    manager_->dismissNotification(id);

    bool hasDismissed = std::find(received.begin(), received.end(),
                                   ModuleEventType::NotificationDismissed) != received.end();
    EXPECT_TRUE(hasDismissed);
}

TEST_F(NotificationManagerTest, UpdatePublishesUpdatedEvent) {
    std::vector<ModuleEventType> received;
    bus_.subscribe([&](const ModuleEvent& e) {
        received.push_back(e.type);
    });

    std::string id = manager_->showNotification(Notification{});
    manager_->updateNotification(id, Notification{});

    bool hasUpdated = std::find(received.begin(), received.end(),
                                 ModuleEventType::NotificationUpdated) != received.end();
    EXPECT_TRUE(hasUpdated);
}

TEST_F(NotificationManagerTest, EventCarriesNotificationId) {
    std::string publishedId;
    bus_.subscribe([&](const ModuleEvent& e) {
        if (e.type == ModuleEventType::NotificationCreated) {
            publishedId = e.error;
        }
    });

    Notification n;
    n.title = "EventTest";
    std::string id = manager_->showNotification(n);

    EXPECT_EQ(publishedId, id);
}

// ── INotificationRenderer Interface Tests ────────────────────────────────

class StubRenderer : public INotificationRenderer {
public:
    void show(const Notification&) override {}
    void update(const Notification&) override {}
    void dismiss(const std::string&, std::function<void()> cb) override { if (cb) cb(); }
    void clearAll() override {}
    void setPosition(int, int, int) override {}
    void tick(float) override {}
};

TEST(INotificationRendererTest, StubRendererCanShow) {
    StubRenderer renderer;
    Notification n;
    EXPECT_NO_THROW(renderer.show(n));
}

TEST(INotificationRendererTest, StubRendererCanDismissWithCallback) {
    StubRenderer renderer;
    bool called = false;
    renderer.dismiss("test", [&]() { called = true; });
    EXPECT_TRUE(called);
}

// ── Notification Service Tests ───────────────────────────────────────────

class NotificationServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<NotificationService>(bus_);
    }

    EventBus bus_;
    std::unique_ptr<NotificationService> service_;
};

TEST_F(NotificationServiceTest, ShowReturnsId) {
    Notification n;
    n.title = "Service Test";
    std::string id = service_->show(n);
    EXPECT_FALSE(id.empty());
}

TEST_F(NotificationServiceTest, GetDisplayedReturnsNotifications) {
    service_->show(Notification{});
    service_->show(Notification{});

    auto displayed = service_->getDisplayed();
    EXPECT_EQ(displayed.size(), 2);
}

TEST_F(NotificationServiceTest, DismissRemovesFromDisplay) {
    std::string id = service_->show(Notification{});
    EXPECT_EQ(service_->getDisplayed().size(), 1);

    service_->dismiss(id);
    EXPECT_EQ(service_->getDisplayed().size(), 0);
}

TEST_F(NotificationServiceTest, DismissAllClearsAll) {
    for (int i = 0; i < 5; ++i) {
        service_->show(Notification{});
    }
    EXPECT_EQ(service_->getDisplayed().size(), 5);

    service_->dismissAll();
    EXPECT_EQ(service_->getDisplayed().size(), 0);
}

TEST_F(NotificationServiceTest, GetByIdReturnsCorrect) {
    Notification n;
    n.title = "ById";
    std::string id = service_->show(n);

    auto result = service_->getById(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->title, "ById");
}

TEST_F(NotificationServiceTest, UpdateExisting) {
    std::string id = service_->show(Notification{});

    Notification updated;
    updated.progress = 50;
    EXPECT_TRUE(service_->update(id, updated));

    auto result = service_->getById(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->progress, 50);
}

TEST_F(NotificationServiceTest, UpdateNonExistingReturnsFalse) {
    EXPECT_FALSE(service_->update("nonexistent", Notification{}));
}

TEST_F(NotificationServiceTest, UpdateTick) {
    service_->show(Notification{});
    EXPECT_EQ(service_->getDisplayed().size(), 1);

    service_->update(0.1f);
    EXPECT_EQ(service_->getDisplayed().size(), 1);
}
