#include <gtest/gtest.h>
#include "modules/ModuleRegistry.hpp"
#include "modules/LifecycleCoordinator.hpp"
#include "modules/EventBus.hpp"
#include "modules/ServiceContainer.hpp"
#include "modules/ModuleState.hpp"
#include "modules/interfaces/IModule.hpp"

using namespace rex::modules;

class FakeModule : public IModule {
public:
    explicit FakeModule(std::string id, bool failStart = false)
        : id_(std::move(id)), failStart_(failStart) {
        meta_.id = id_;
        meta_.category = "test";
    }

    bool initialize(IModuleContext*) override {
        initCount_++;
        return true;
    }

    bool start() override {
        if (failStart_) return false;
        startCount_++;
        return true;
    }

    void stop() override { stopCount_++; }
    void shutdown() override {}

    const ModuleMetadata& getMetadata() const override { return meta_; }
    ModuleState getState() const override { return ModuleState::Unknown; }
    ModuleHealth getHealth() const override { return ModuleHealth{}; }

    int initCount_ = 0;
    int startCount_ = 0;
    int stopCount_ = 0;

private:
    ModuleMetadata meta_;
    std::string id_;
    bool failStart_;
};

TEST(LifecycleCoordinatorTest, StartSingleModule) {
    ModuleRegistry registry;
    EventBus bus;
    ServiceContainer container;
    LifecycleCoordinator coord(registry, bus, container);

    auto mod = std::make_unique<FakeModule>("test");
    registry.registerModule(std::move(mod));
    registry.setModuleState("test", ModuleState::Initialized);

    coord.startModule("test");
    EXPECT_EQ(registry.getModuleState("test"), ModuleState::Running);

    auto* raw = registry.getModule("test");
    ASSERT_NE(raw, nullptr);
    auto* fake = static_cast<FakeModule*>(raw);
    EXPECT_EQ(fake->startCount_, 1);
}

TEST(LifecycleCoordinatorTest, StopSingleModule) {
    ModuleRegistry registry;
    EventBus bus;
    ServiceContainer container;
    LifecycleCoordinator coord(registry, bus, container);

    auto mod = std::make_unique<FakeModule>("test");
    registry.registerModule(std::move(mod));
    registry.setModuleState("test", ModuleState::Running);

    coord.stopModule("test");
    EXPECT_EQ(registry.getModuleState("test"), ModuleState::Stopped);
}

TEST(LifecycleCoordinatorTest, StartFailure) {
    ModuleRegistry registry;
    EventBus bus;
    ServiceContainer container;
    LifecycleCoordinator coord(registry, bus, container);

    auto mod = std::make_unique<FakeModule>("test", true);
    registry.registerModule(std::move(mod));
    registry.setModuleState("test", ModuleState::Initialized);

    coord.startModule("test");
    EXPECT_EQ(registry.getModuleState("test"), ModuleState::Failed);
}

TEST(LifecycleCoordinatorTest, RestartModule) {
    ModuleRegistry registry;
    EventBus bus;
    ServiceContainer container;
    LifecycleCoordinator coord(registry, bus, container);

    auto mod = std::make_unique<FakeModule>("test");
    registry.registerModule(std::move(mod));
    registry.setModuleState("test", ModuleState::Running);

    coord.restartModule("test");
    EXPECT_EQ(registry.getModuleState("test"), ModuleState::Running);

    auto* raw = registry.getModule("test");
    ASSERT_NE(raw, nullptr);
    auto* fake = static_cast<FakeModule*>(raw);
    EXPECT_EQ(fake->stopCount_, 1);
    EXPECT_EQ(fake->startCount_, 1);
}

TEST(LifecycleCoordinatorTest, InitializeAllRespectsOrder) {
    ModuleRegistry registry;
    EventBus bus;
    ServiceContainer container;
    LifecycleCoordinator coord(registry, bus, container);

    auto modA = std::make_unique<FakeModule>("A");
    auto modB = std::make_unique<FakeModule>("B");
    registry.registerModule(std::move(modA));
    registry.registerModule(std::move(modB));

    std::vector<std::string> order = {"A", "B"};
    coord.initializeAll(order);

    EXPECT_EQ(registry.getModuleState("A"), ModuleState::Initialized);
    EXPECT_EQ(registry.getModuleState("B"), ModuleState::Initialized);
}
