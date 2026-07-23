#include <gtest/gtest.h>
#include "modules/Command/models/CommandDefinition.hpp"
#include "modules/Command/models/CommandInstance.hpp"
#include "modules/Command/models/CommandContext.hpp"
#include "modules/Command/models/CommandResult.hpp"
#include "modules/Command/models/ExecutionMode.hpp"
#include "modules/Command/models/Shortcut.hpp"
#include "modules/Command/interfaces/ICommandService.hpp"
#include "modules/Command/interfaces/ICommandProvider.hpp"
#include "modules/Command/services/CommandRegistry.hpp"
#include "modules/Command/services/CommandValidator.hpp"
#include "modules/Command/services/CommandExecutor.hpp"
#include "modules/Command/services/CommandDispatcher.hpp"
#include "modules/Command/services/CommandService.hpp"
#include "modules/Command/history/CommandHistory.hpp"
#include "modules/Command/events/CommandEvents.hpp"
#include "modules/Command/shortcut/ShortcutManager.hpp"
#include "modules/EventBus.hpp"

using namespace rex::command;
using namespace rex::modules;

// ── ExecutionMode ──────────────────────────────────────────────────────────

TEST(ExecutionModeTest, ValuesAreDistinct) {
    EXPECT_NE(static_cast<int>(ExecutionMode::Immediate),
              static_cast<int>(ExecutionMode::Background));
    EXPECT_NE(static_cast<int>(ExecutionMode::Immediate),
              static_cast<int>(ExecutionMode::Deferred));
    EXPECT_NE(static_cast<int>(ExecutionMode::Background),
              static_cast<int>(ExecutionMode::Deferred));
}

// ── Shortcut ───────────────────────────────────────────────────────────────

TEST(ShortcutTest, DefaultIsInvalid) {
    Shortcut s;
    EXPECT_FALSE(s.isValid());
}

TEST(ShortcutTest, WithKeyCodeIsValid) {
    Shortcut s;
    s.keyCode = 65;
    EXPECT_TRUE(s.isValid());
}

TEST(ShortcutTest, Equality) {
    Shortcut a, b;
    a.modifiers = 1; a.keyCode = 65;
    b.modifiers = 1; b.keyCode = 65;
    EXPECT_TRUE(a == b);
}

TEST(ShortcutTest, Inequality) {
    Shortcut a, b;
    a.modifiers = 1; a.keyCode = 65;
    b.modifiers = 2; b.keyCode = 65;
    EXPECT_FALSE(a == b);
}

// ── CommandContext ─────────────────────────────────────────────────────────

TEST(CommandContextTest, DefaultConstruction) {
    CommandContext ctx;
    EXPECT_TRUE(ctx.source.empty());
    EXPECT_EQ(ctx.cancelToken, nullptr);
    EXPECT_EQ(ctx.service, nullptr);
    EXPECT_EQ(ctx.bus, nullptr);
}

TEST(CommandContextTest, ParametersAny) {
    CommandContext ctx;
    ctx.parameters = std::string("hello");
    EXPECT_EQ(std::any_cast<std::string>(ctx.parameters), "hello");
}

// ── CommandResult ──────────────────────────────────────────────────────────

TEST(CommandResultTest, DefaultIsSuccess) {
    CommandResult r;
    EXPECT_TRUE(r.success);
    EXPECT_TRUE(r.errorMessage.empty());
}

TEST(CommandResultTest, FailureResult) {
    CommandResult r{false, "error"};
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.errorMessage, "error");
}

// ── CommandDefinition ──────────────────────────────────────────────────────

TEST(CommandDefinitionTest, DefaultConstruction) {
    CommandDefinition def;
    EXPECT_TRUE(def.id.empty());
    EXPECT_EQ(def.mode, ExecutionMode::Immediate);
    EXPECT_FALSE(def.supportsCancellation);
    EXPECT_FALSE(def.canExecute);
    EXPECT_FALSE(def.execute);
    EXPECT_FALSE(def.undo);
}

TEST(CommandDefinitionTest, CustomConstruction) {
    CommandDefinition def;
    def.id = "test.hello";
    def.name = "Say Hello";
    def.category = "Test";
    def.mode = ExecutionMode::Background;
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    ASSERT_TRUE(def.execute);
    CommandContext ctx;
    auto result = def.execute(ctx);
    EXPECT_TRUE(result.success);
}

// ── CommandInstance ─────────────────────────────────────────────────────────

TEST(CommandInstanceTest, DefaultConstruction) {
    CommandInstance inst;
    EXPECT_TRUE(inst.id.empty());
    EXPECT_EQ(inst.definition, nullptr);
    EXPECT_TRUE(inst.enabled);
    EXPECT_TRUE(inst.visible);
}

// ── CommandRegistry ────────────────────────────────────────────────────────

class CommandRegistryTest : public ::testing::Test {
protected:
    CommandRegistry registry_;

    CommandDefinition makeDef(const std::string& id) {
        CommandDefinition def;
        def.id = id;
        def.name = id;
        def.execute = [](CommandContext&) -> CommandResult { return {true}; };
        return def;
    }
};

TEST_F(CommandRegistryTest, InitiallyEmpty) {
    EXPECT_TRUE(registry_.getAllInstances().empty());
}

TEST_F(CommandRegistryTest, AddDefinition) {
    EXPECT_TRUE(registry_.addDefinition(makeDef("cmd1")));
    EXPECT_FALSE(registry_.addDefinition(makeDef("cmd1")));
}

TEST_F(CommandRegistryTest, GetDefinitionReturnsCorrect) {
    registry_.addDefinition(makeDef("cmd.test"));
    auto* def = registry_.getDefinition("cmd.test");
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->id, "cmd.test");
}

TEST_F(CommandRegistryTest, GetDefinitionNonExistent) {
    EXPECT_EQ(registry_.getDefinition("nonexistent"), nullptr);
}

TEST_F(CommandRegistryTest, RemoveDefinition) {
    registry_.addDefinition(makeDef("cmd1"));
    EXPECT_TRUE(registry_.removeDefinition("cmd1"));
    EXPECT_EQ(registry_.getDefinition("cmd1"), nullptr);
}

TEST_F(CommandRegistryTest, GetAllInstancesAfterAdd) {
    registry_.addDefinition(makeDef("a"));
    registry_.addDefinition(makeDef("b"));
    EXPECT_EQ(registry_.getAllInstances().size(), 2);
}

TEST_F(CommandRegistryTest, SetInstanceEnabled) {
    registry_.addDefinition(makeDef("cmd"));
    EXPECT_TRUE(registry_.setInstanceEnabled("cmd", false));
    auto* inst = registry_.getInstance("cmd");
    ASSERT_NE(inst, nullptr);
    EXPECT_FALSE(inst->enabled);
}

TEST_F(CommandRegistryTest, SetInstanceVisible) {
    registry_.addDefinition(makeDef("cmd"));
    EXPECT_TRUE(registry_.setInstanceVisible("cmd", false));
    auto* inst = registry_.getInstance("cmd");
    ASSERT_NE(inst, nullptr);
    EXPECT_FALSE(inst->visible);
}

TEST_F(CommandRegistryTest, SetStateNonExistent) {
    EXPECT_FALSE(registry_.setInstanceEnabled("nope", false));
    EXPECT_FALSE(registry_.setInstanceVisible("nope", false));
}

TEST_F(CommandRegistryTest, InstancePointsToDefinition) {
    registry_.addDefinition(makeDef("ref"));
    auto* inst = registry_.getInstance("ref");
    ASSERT_NE(inst, nullptr);
    ASSERT_NE(inst->definition, nullptr);
    EXPECT_EQ(inst->definition->id, "ref");
}

// ── CommandValidator ───────────────────────────────────────────────────────

TEST(CommandValidatorTest, NonExistentReturnsFalse) {
    CommandRegistry reg;
    CommandValidator val(reg);
    EXPECT_FALSE(val.canExecute("nonexistent"));
}

TEST(CommandValidatorTest, NoConditionReturnsTrue) {
    CommandRegistry reg;
    CommandDefinition def;
    def.id = "cmd";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    reg.addDefinition(def);
    CommandValidator val(reg);
    EXPECT_TRUE(val.canExecute("cmd"));
}

TEST(CommandValidatorTest, ConditionBlocksExecution) {
    CommandRegistry reg;
    CommandDefinition def;
    def.id = "blocked";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    def.canExecute = []() { return false; };
    reg.addDefinition(def);
    CommandValidator val(reg);
    EXPECT_FALSE(val.canExecute("blocked"));
}

TEST(CommandValidatorTest, ConditionAllowsExecution) {
    CommandRegistry reg;
    CommandDefinition def;
    def.id = "allowed";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    def.canExecute = []() { return true; };
    reg.addDefinition(def);
    CommandValidator val(reg);
    EXPECT_TRUE(val.canExecute("allowed"));
}

// ── CommandExecutor ────────────────────────────────────────────────────────

TEST(CommandExecutorTest, ExecuteImmediate) {
    CommandExecutor exec;
    CommandDefinition def;
    def.id = "imm";
    def.mode = ExecutionMode::Immediate;
    bool executed = false;
    def.execute = [&](CommandContext&) -> CommandResult {
        executed = true;
        return {true};
    };
    CommandContext ctx;
    auto result = exec.execute(def, ctx);
    EXPECT_TRUE(executed);
    EXPECT_TRUE(result.success);
}

TEST(CommandExecutorTest, ExecuteImmediateReturnsFailure) {
    CommandExecutor exec;
    CommandDefinition def;
    def.id = "fail";
    def.mode = ExecutionMode::Immediate;
    def.execute = [](CommandContext&) -> CommandResult {
        return {false, "failed"};
    };
    CommandContext ctx;
    auto result = exec.execute(def, ctx);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, "failed");
}

TEST(CommandExecutorTest, ExecuteBackground) {
    CommandExecutor exec;
    CommandDefinition def;
    def.id = "bg";
    def.mode = ExecutionMode::Background;
    bool executed = false;
    def.execute = [&](CommandContext&) -> CommandResult {
        executed = true;
        return {true};
    };
    CommandContext ctx;
    auto result = exec.execute(def, ctx);
    EXPECT_TRUE(result.success);
    // Give worker time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(executed);
}

TEST(CommandExecutorTest, ExecuteDeferredReturnsFailure) {
    CommandExecutor exec;
    CommandDefinition def;
    def.id = "def";
    def.mode = ExecutionMode::Deferred;
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    CommandContext ctx;
    auto result = exec.execute(def, ctx);
    EXPECT_FALSE(result.success);
}

TEST(CommandExecutorTest, CancelNoActiveExecution) {
    CommandExecutor exec;
    EXPECT_NO_THROW(exec.cancel("nonexistent"));
}

TEST(CommandExecutorTest, ExecuteBackgroundAsyncCallbackCalled) {
    CommandExecutor exec;
    CommandDefinition def;
    def.id = "async_cb";
    def.mode = ExecutionMode::Background;
    bool callbackCalled = false;
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    CommandContext ctx;
    exec.execute(def, ctx, [&](CommandResult) { callbackCalled = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(callbackCalled);
}

// ── CommandHistory ─────────────────────────────────────────────────────────

TEST(CommandHistoryTest, InitiallyCannotUndoRedo) {
    CommandHistory hist;
    EXPECT_FALSE(hist.canUndo());
    EXPECT_FALSE(hist.canRedo());
}

TEST(CommandHistoryTest, PushEnablesUndo) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "undoable";
    def.undo = [](CommandContext&) {};
    CommandContext ctx;
    hist.push("undoable", def, ctx);
    EXPECT_TRUE(hist.canUndo());
}

TEST(CommandHistoryTest, PushWithoutUndoNoEffect) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "no_undo";
    CommandContext ctx;
    hist.push("no_undo", def, ctx);
    EXPECT_FALSE(hist.canUndo());
}

TEST(CommandHistoryTest, UndoCallsUndoAction) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "test_undo";
    bool undone = false;
    def.undo = [&](CommandContext&) { undone = true; };
    CommandContext ctx;
    hist.push("test_undo", def, ctx);
    hist.undo();
    EXPECT_TRUE(undone);
}

TEST(CommandHistoryTest, UndoPopulatesRedoStack) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "test";
    def.undo = [](CommandContext&) {};
    CommandContext ctx;
    hist.push("test", def, ctx);
    hist.undo();
    EXPECT_TRUE(hist.canRedo());
}

TEST(CommandHistoryTest, UndoEmptyDoesNothing) {
    CommandHistory hist;
    EXPECT_NO_THROW(hist.undo());
    EXPECT_FALSE(hist.canUndo());
}

TEST(CommandHistoryTest, RedoEmptyDoesNothing) {
    CommandHistory hist;
    EXPECT_NO_THROW(hist.redo());
    EXPECT_FALSE(hist.canRedo());
}

TEST(CommandHistoryTest, ClearEmptiesStacks) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "c";
    def.undo = [](CommandContext&) {};
    CommandContext ctx;
    hist.push("c", def, ctx);
    ASSERT_TRUE(hist.canUndo());
    hist.undo();
    ASSERT_FALSE(hist.canUndo());
    ASSERT_TRUE(hist.canRedo());
    hist.clear();
    EXPECT_FALSE(hist.canUndo());
    EXPECT_FALSE(hist.canRedo());
}

TEST(CommandHistoryTest, NewPushClearsRedoStack) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "x";
    def.undo = [](CommandContext&) {};
    CommandContext ctx;
    hist.push("x", def, ctx);
    hist.undo();
    ASSERT_TRUE(hist.canRedo());
    hist.push("y", def, ctx);
    EXPECT_FALSE(hist.canRedo());
}

// ── CommandService ─────────────────────────────────────────────────────────

class CommandServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto registry = std::make_unique<CommandRegistry>();
        auto validator = std::make_unique<CommandValidator>(*registry);
        auto executor = std::make_unique<CommandExecutor>();
        auto history = std::make_unique<CommandHistory>();
        auto dispatcher = std::make_unique<CommandDispatcher>(*registry, *validator,
                                                              *executor, *history, bus_);
        auto shortcutMgr = std::make_unique<ShortcutManager>(bus_);

        service_ = std::make_shared<CommandService>(
            std::move(registry), std::move(validator), std::move(executor),
            std::move(dispatcher), std::move(history), std::move(shortcutMgr));
    }

    void registerHello() {
        CommandDefinition def;
        def.id = "hello";
        def.name = "Hello";
        def.execute = [](CommandContext&) -> CommandResult { return {true}; };
        service_->registerCommand(def);
    }

    EventBus bus_;
    std::shared_ptr<CommandService> service_;
};

TEST_F(CommandServiceTest, RegisterCommand) {
    EXPECT_TRUE(service_->registerCommand(CommandDefinition{}));
}

TEST_F(CommandServiceTest, RegisterDuplicateFails) {
    CommandDefinition def;
    def.id = "dup";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    EXPECT_TRUE(service_->registerCommand(def));
    EXPECT_FALSE(service_->registerCommand(def));
}

TEST_F(CommandServiceTest, UnregisterCommand) {
    CommandDefinition def;
    def.id = "del";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    service_->registerCommand(def);
    EXPECT_TRUE(service_->unregisterCommand("del"));
    EXPECT_FALSE(service_->unregisterCommand("del"));
}

TEST_F(CommandServiceTest, ExecuteNonExistent) {
    auto result = service_->execute("nonexistent");
    EXPECT_FALSE(result.success);
}

TEST_F(CommandServiceTest, ExecuteExisting) {
    registerHello();
    auto result = service_->execute("hello");
    EXPECT_TRUE(result.success);
}

TEST_F(CommandServiceTest, IsEnabled) {
    registerHello();
    EXPECT_TRUE(service_->isEnabled("hello"));
}

TEST_F(CommandServiceTest, IsEnabledNonExistent) {
    EXPECT_FALSE(service_->isEnabled("nope"));
}

TEST_F(CommandServiceTest, GetAllCommands) {
    registerHello();
    auto all = service_->getAllCommands();
    EXPECT_EQ(all.size(), 1);
    EXPECT_EQ(all[0].id, "hello");
}

TEST_F(CommandServiceTest, UndoRedoChain) {
    CommandDefinition def;
    def.id = "undoable";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    def.undo = [](CommandContext&) {};
    service_->registerCommand(def);
    service_->execute("undoable");
    EXPECT_TRUE(service_->canUndo());
    EXPECT_FALSE(service_->canRedo());
    service_->undo();
    EXPECT_TRUE(service_->canRedo());
    EXPECT_FALSE(service_->canUndo());
    service_->redo();
    EXPECT_TRUE(service_->canUndo());
}

TEST_F(CommandServiceTest, ClearHistory) {
    CommandDefinition def;
    def.id = "c";
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    def.undo = [](CommandContext&) {};
    service_->registerCommand(def);
    service_->execute("c");
    ASSERT_TRUE(service_->canUndo());
    service_->clearHistory();
    EXPECT_FALSE(service_->canUndo());
}

TEST_F(CommandServiceTest, SetAndGetShortcut) {
    registerHello();
    Shortcut s;
    s.modifiers = 1; s.keyCode = 'H';
    service_->setCustomShortcut("hello", s);
    auto retrieved = service_->getShortcut("hello");
    EXPECT_EQ(retrieved.modifiers, s.modifiers);
    EXPECT_EQ(retrieved.keyCode, s.keyCode);
}

TEST_F(CommandServiceTest, GetDefaultShortcut) {
    CommandDefinition def;
    def.id = "with_shortcut";
    def.defaultShortcut.keyCode = 65;
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    service_->registerCommand(def);
    auto retrieved = service_->getShortcut("with_shortcut");
    EXPECT_EQ(retrieved.keyCode, 65);
}

TEST_F(CommandServiceTest, GetShortcutNonExistent) {
    Shortcut s = service_->getShortcut("nonexistent");
    EXPECT_FALSE(s.isValid());
}

TEST_F(CommandServiceTest, ConflictingCommands) {
    CommandDefinition a, b;
    a.id = "cmd_a"; a.defaultShortcut.keyCode = 65; a.execute = [](CommandContext&) -> CommandResult { return {true}; };
    b.id = "cmd_b"; b.defaultShortcut.keyCode = 65; b.execute = [](CommandContext&) -> CommandResult { return {true}; };
    service_->registerCommand(a);
    service_->registerCommand(b);
    Shortcut s; s.keyCode = 65;
    auto conflicts = service_->getConflictingCommands(s);
    EXPECT_EQ(conflicts.size(), 2);
}

// ── CommandEvents constants ────────────────────────────────────────────────

TEST(CommandEventsTest, ConstantsDefined) {
    EXPECT_STREQ(CommandEvents::Executed, "command_executed");
    EXPECT_STREQ(CommandEvents::Failed, "command_failed");
    EXPECT_STREQ(CommandEvents::CannotExecute, "command_cannot_execute");
    EXPECT_STREQ(CommandEvents::Registered, "command_registered");
    EXPECT_STREQ(CommandEvents::Unregistered, "command_unregistered");
    EXPECT_STREQ(CommandEvents::Undone, "command_undone");
    EXPECT_STREQ(CommandEvents::Redone, "command_redone");
    EXPECT_STREQ(CommandEvents::Cancelled, "command_cancelled");
    EXPECT_STREQ(CommandEvents::ShortcutChanged, "shortcut_changed");
    EXPECT_STREQ(CommandEvents::HistoryCleared, "command_history_cleared");
}
