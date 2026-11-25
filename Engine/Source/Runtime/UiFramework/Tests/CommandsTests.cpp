#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application.h>
#include <UiFramework/Button.h>

namespace rocks::ui {

TEST(UiFramework_CommandsBubbleBeforeGlobalAndIsolatePlayerModifiers) {
    SystemAllocator allocator;
    CommandSet set(allocator);
    BindingManager bindings(allocator);
    constexpr Guid id{1, 2};
    const KeyChord controlK{.key = input::Key::K, .control = true};
    REQUIRE(set.Add(id, "명령", "", {controlK, {input::Key::F4}}));
    auto local = SharedPtr<CommandList>::TryMake(allocator, allocator, set, bindings);
    auto global = SharedPtr<CommandList>::TryMake(allocator, allocator, set, bindings);
    auto root = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto leaf = SharedPtr<Button>::TryMake(allocator, allocator);
    REQUIRE(local && global && root && leaf);
    int localCalls = 0;
    int globalCalls = 0;
    bool enabled = true;
    REQUIRE(local.Value()->Map(
        id, {.execute = Delegate<void()>::CreateLambda([&]() noexcept { ++localCalls; }),
             .canExecute = Delegate<bool()>::CreateLambda([&]() noexcept { return enabled; })}));
    REQUIRE(global.Value()->Map(
        id, {.execute = Delegate<void()>::CreateLambda([&]() noexcept { ++globalCalls; })}));
    REQUIRE(root.Value()->TryAddChild(leaf.Value()));
    Application app(allocator);
    app.SetRoot(root.Value());
    REQUIRE(app.SetCommands({}, global.Value()));
    REQUIRE(app.SetCommands(leaf.Value(), local.Value()));
    REQUIRE(app.SetFocus(5, leaf.Value()));
    REQUIRE(app.RouteKey(5, input::InputEvent::KeyDown(input::Key::LeftControl)));
    REQUIRE(!app.RouteKey(0, input::InputEvent::KeyDown(input::Key::K)).Value());
    REQUIRE(app.RouteKey(5, input::InputEvent::KeyDown(input::Key::K)).Value());
    CHECK(localCalls == 1 && globalCalls == 0);
    enabled = false;
    REQUIRE(app.RouteKey(5, input::InputEvent::KeyDown(input::Key::K)).Value());
    CHECK(globalCalls == 1);
    REQUIRE(
        !app.RouteKeyWithin(5, input::InputEvent::KeyDown(input::Key::K), root.Value()).Value());
    CHECK(globalCalls == 1);
    REQUIRE(bindings.Override(id, {{input::Key::F8}, {}}));
    Array<uint8> bytes(allocator);
    REQUIRE(bindings.Save(bytes));
    REQUIRE(bindings.RestoreDefault(id));
    REQUIRE(bindings.Load(bytes.View()));
    CHECK(!bindings.Load({bytes.Data(), bytes.Size() - 1}));
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::F8)).Value());
    CHECK(globalCalls == 2);
    REQUIRE(!app.RouteKey(0, input::InputEvent::KeyDown(input::Key::F4)).Value());
}

} // namespace rocks::ui
