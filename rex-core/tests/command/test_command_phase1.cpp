#include <gtest/gtest.h>
#include "modules/Command/models/CommandGroup.hpp"
#include "modules/Command/shortcut/ShortcutManager.hpp"
#include "modules/Command/services/CommandService.hpp"
#include "modules/Command/history/CommandHistory.hpp"
#include "modules/EventBus.hpp"

using namespace rex::command;
using namespace rex::modules;

// ── CommandGroup ───────────────────────────────────────────────────────────

TEST(CommandGroupTest, DefaultConstruction) {
    CommandGroup g;
    EXPECT_TRUE(g.id.empty());
    EXPECT_EQ(g.order, 0);
    EXPECT_TRUE(g.children.empty());
}

TEST(CommandGroupTest, CustomGroup) {
    CommandGroup g;
    g.id = "wallpaper";
    g.name = "Wallpaper";
    g.order = 1;
    g.children = {"wp.next", "wp.prev"};
    EXPECT_EQ(g.id, "wallpaper");
    EXPECT_EQ(g.children.size(), 2);
}

// ── ShortcutManager ────────────────────────────────────────────────────────

class ShortcutManagerTest : public ::testing::Test {
protected:
    EventBus bus_;
    ShortcutManager mgr_{bus_};
};

TEST_F(ShortcutManagerTest, InitiallyNoActiveShortcut) {
    Shortcut s = mgr_.getActiveShortcut("nonexistent");
    EXPECT_FALSE(s.isValid());
}

TEST_F(ShortcutManagerTest, RegisterShortcut) {
    Shortcut s; s.keyCode = 65;
    mgr_.registerShortcut("cmd.a", s);
    Shortcut active = mgr_.getActiveShortcut("cmd.a");
    EXPECT_EQ(active.keyCode, 65);
}

TEST_F(ShortcutManagerTest, CustomShortcutOverridesDefault) {
    Shortcut def; def.keyCode = 65;
    Shortcut custom; custom.keyCode = 66;
    mgr_.registerShortcut("cmd", def);
    mgr_.setCustomShortcut("cmd", custom);
    EXPECT_EQ(mgr_.getActiveShortcut("cmd").keyCode, 66);
}

TEST_F(ShortcutManagerTest, UnregisterRemovesShortcut) {
    Shortcut s; s.keyCode = 65;
    mgr_.registerShortcut("cmd", s);
    ASSERT_TRUE(mgr_.getActiveShortcut("cmd").isValid());
    mgr_.unregisterShortcut("cmd");
    EXPECT_FALSE(mgr_.getActiveShortcut("cmd").isValid());
}

TEST_F(ShortcutManagerTest, GetCommandsForShortcut) {
    Shortcut s; s.keyCode = 65;
    mgr_.registerShortcut("cmd1", s);
    mgr_.registerShortcut("cmd2", s);
    auto cmds = mgr_.getCommandsForShortcut(s);
    EXPECT_EQ(cmds.size(), 2);
}

TEST_F(ShortcutManagerTest, GetCommandsForUnregisteredShortcut) {
    Shortcut s; s.keyCode = 99;
    auto cmds = mgr_.getCommandsForShortcut(s);
    EXPECT_TRUE(cmds.empty());
}

TEST_F(ShortcutManagerTest, OnKeyPressedExecutesCallback) {
    Shortcut s; s.keyCode = 65;
    mgr_.registerShortcut("cmd.a", s);
    std::string executed;
    mgr_.setExecutionCallback([&](const std::string& id) { executed = id; });
    std::string result = mgr_.onKeyPressed(s);
    EXPECT_EQ(result, "cmd.a");
    EXPECT_EQ(executed, "cmd.a");
}

TEST_F(ShortcutManagerTest, OnKeyPressedNoMatch) {
    Shortcut s; s.keyCode = 65;
    Shortcut pressed; pressed.keyCode = 66;
    mgr_.registerShortcut("cmd.a", s);
    std::string result = mgr_.onKeyPressed(pressed);
    EXPECT_TRUE(result.empty());
}

TEST_F(ShortcutManagerTest, CustomShortcutChangesReverseMap) {
    Shortcut def; def.keyCode = 65;
    Shortcut custom; custom.keyCode = 66;
    mgr_.registerShortcut("cmd", def);
    EXPECT_FALSE(mgr_.getCommandsForShortcut(def).empty());
    EXPECT_TRUE(mgr_.getCommandsForShortcut(custom).empty());
    mgr_.setCustomShortcut("cmd", custom);
    EXPECT_TRUE(mgr_.getCommandsForShortcut(def).empty());
    EXPECT_FALSE(mgr_.getCommandsForShortcut(custom).empty());
}

TEST_F(ShortcutManagerTest, UnregisterUpdatesReverseMap) {
    Shortcut s; s.keyCode = 65;
    mgr_.registerShortcut("cmd", s);
    ASSERT_FALSE(mgr_.getCommandsForShortcut(s).empty());
    mgr_.unregisterShortcut("cmd");
    EXPECT_TRUE(mgr_.getCommandsForShortcut(s).empty());
}

// ── CommandService: Phase 1 additions ──────────────────────────────────────

class CommandServicePhase1Test : public ::testing::Test {
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

    void registerUndoableCommand(const std::string& id) {
        CommandDefinition def;
        def.id = id;
        def.execute = [](CommandContext&) -> CommandResult { return {true}; };
        def.undo = [](CommandContext&) {};
        service_->registerCommand(def);
    }

    void registerSimpleCommand(const std::string& id) {
        CommandDefinition def;
        def.id = id;
        def.execute = [](CommandContext&) -> CommandResult { return {true}; };
        service_->registerCommand(def);
    }

    EventBus bus_;
    std::shared_ptr<CommandService> service_;
};

TEST_F(CommandServicePhase1Test, RegisterCommandRegistersShortcut) {
    CommandDefinition def;
    def.id = "cmd";
    def.defaultShortcut.keyCode = 65;
    def.execute = [](CommandContext&) -> CommandResult { return {true}; };
    service_->registerCommand(def);
    Shortcut s = service_->getShortcut("cmd");
    EXPECT_EQ(s.keyCode, 65);
}

TEST_F(CommandServicePhase1Test, SetCustomShortcutDelegates) {
    registerSimpleCommand("cmd");
    Shortcut custom; custom.keyCode = 99;
    service_->setCustomShortcut("cmd", custom);
    EXPECT_EQ(service_->getShortcut("cmd").keyCode, 99);
}

TEST_F(CommandServicePhase1Test, GetConflictingShortcuts) {
    CommandDefinition a, b;
    a.id = "cmd_a"; a.defaultShortcut.keyCode = 65; a.execute = [](CommandContext&) -> CommandResult { return {true}; };
    b.id = "cmd_b"; b.defaultShortcut.keyCode = 65; b.execute = [](CommandContext&) -> CommandResult { return {true}; };
    service_->registerCommand(a);
    service_->registerCommand(b);
    Shortcut s; s.keyCode = 65;
    auto conflicts = service_->getConflictingCommands(s);
    EXPECT_EQ(conflicts.size(), 2);
}

TEST_F(CommandServicePhase1Test, RegisterGroup) {
    CommandGroup g;
    g.id = "edit";
    g.name = "Edit";
    EXPECT_TRUE(service_->registerGroup(g));
    EXPECT_FALSE(service_->registerGroup(g));
}

TEST_F(CommandServicePhase1Test, GetGroup) {
    CommandGroup g;
    g.id = "view";
    service_->registerGroup(g);
    const auto* retrieved = service_->getGroup("view");
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->id, "view");
}

TEST_F(CommandServicePhase1Test, GetGroupNonExistent) {
    EXPECT_EQ(service_->getGroup("nonexistent"), nullptr);
}

TEST_F(CommandServicePhase1Test, GetAllGroups) {
    CommandGroup a, b;
    a.id = "a"; b.id = "b";
    service_->registerGroup(a);
    service_->registerGroup(b);
    EXPECT_EQ(service_->getAllGroups().size(), 2);
}

TEST_F(CommandServicePhase1Test, RedoReexecutesCommand) {
    int execCount = 0;
    CommandDefinition def;
    def.id = "redo_test";
    def.execute = [&](CommandContext&) -> CommandResult {
        ++execCount;
        return {true};
    };
    def.undo = [](CommandContext&) {};
    service_->registerCommand(def);

    service_->execute("redo_test");
    EXPECT_EQ(execCount, 1);
    ASSERT_TRUE(service_->canUndo());

    service_->undo();
    ASSERT_TRUE(service_->canRedo());

    service_->redo();
    EXPECT_EQ(execCount, 2);
}

TEST_F(CommandServicePhase1Test, RedoWithoutHistoryDoesNothing) {
    EXPECT_FALSE(service_->canRedo());
    EXPECT_NO_THROW(service_->redo());
}

TEST_F(CommandServicePhase1Test, UndoRedoChainMaintainsStacks) {
    registerUndoableCommand("cmd1");
    registerUndoableCommand("cmd2");

    service_->execute("cmd1");
    service_->execute("cmd2");
    EXPECT_TRUE(service_->canUndo());
    EXPECT_FALSE(service_->canRedo());

    service_->undo();
    EXPECT_TRUE(service_->canRedo());
    EXPECT_TRUE(service_->canUndo());

    service_->redo();
    EXPECT_TRUE(service_->canUndo());
    EXPECT_FALSE(service_->canRedo());
}

// ── CommandHistory: Phase 1 additions ──────────────────────────────────────

TEST(CommandHistoryPhase1Test, EntryHasContextSnapshot) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "test";
    def.undo = [](CommandContext&) {};
    CommandContext ctx;
    ctx.source = "shortcut";
    hist.push("test", def, ctx);
    hist.undo();

    ASSERT_TRUE(hist.canRedo());
    auto entry = hist.popRedoEntry();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->commandId, "test");
    EXPECT_EQ(entry->contextSnapshot.source, "shortcut");
}

TEST(CommandHistoryPhase1Test, PopRedoEntryRemovesFromRedo) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "t";
    def.undo = [](CommandContext&) {};
    hist.push("t", def, CommandContext{});
    hist.undo();
    ASSERT_TRUE(hist.canRedo());
    hist.popRedoEntry();
    EXPECT_FALSE(hist.canRedo());
}

TEST(CommandHistoryPhase1Test, PopRedoEntryEmptyReturnsNullopt) {
    CommandHistory hist;
    auto entry = hist.popRedoEntry();
    EXPECT_FALSE(entry.has_value());
}

TEST(CommandHistoryPhase1Test, ContextSnapshotIsIndependentCopy) {
    CommandHistory hist;
    CommandDefinition def;
    def.id = "t";
    def.undo = [](CommandContext&) {};
    CommandContext ctx;
    ctx.source = "original";
    hist.push("t", def, ctx);
    ctx.source = "modified";
    hist.undo();
    auto entry = hist.popRedoEntry();
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->contextSnapshot.source, "original");
}
