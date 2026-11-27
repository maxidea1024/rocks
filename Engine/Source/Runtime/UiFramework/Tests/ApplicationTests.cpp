#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>

using namespace rocks;
using namespace rocks::ui;

namespace ui_application_tests {

class TestElement final : public Element {
    ROCKS_UI_ELEMENT(TestElement, Element)

public:
    explicit TestElement(Allocator& allocator) noexcept : Element(allocator) {
    }

    PointerReply OnPreviewPointerDown(const PointerEvent&) noexcept override {
        ++previewCalls;
        return {.handled = consumePreview};
    }

    PointerReply OnPointerEvent(const PointerEvent&) noexcept override {
        ++calls;
        if (removeOnEvent != nullptr) {
            (void)RemoveChild(*removeOnEvent);
        }

        if (removeSelf && Parent() != nullptr) {
            (void)Parent()->RemoveChild(*this);
        }

        return {.handled = consume};
    }

    int calls = 0;
    int previewCalls = 0;
    bool consume = false;
    bool consumePreview = false;
    bool removeSelf = false;
    Element* removeOnEvent = nullptr;
};

TEST(UiFramework_BubblesUntilHandled) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<TestElement>::TryMake(allocator, allocator);
    auto childResult = SharedPtr<TestElement>::TryMake(allocator, allocator);
    REQUIRE(rootResult && childResult);
    auto root = rootResult.Value();
    auto child = childResult.Value();
    Geometry geometry{{40, 40}, {}, {}};
    root->SetGeometry(geometry);
    child->SetGeometry(geometry);
    REQUIRE(root->TryAddChild(child));
    Application app(allocator);
    app.SetRoot(root);
    auto first = app.RoutePointer({PointerEventType::Down, {5, 5}});
    REQUIRE(first);
    CHECK(!first.Value() && child->calls == 1 && root->calls == 1);
    child->consume = true;
    auto second = app.RoutePointer({PointerEventType::Up, {5, 5}});
    REQUIRE(second);
    CHECK(second.Value() && child->calls == 2 && root->calls == 1);
}

TEST(UiFramework_PreviewCanStopPointerDownBeforeBubble) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<TestElement>::TryMake(allocator, allocator);
    auto childResult = SharedPtr<TestElement>::TryMake(allocator, allocator);
    REQUIRE(rootResult && childResult);
    auto root = rootResult.Value();
    auto child = childResult.Value();
    root->SetGeometry({{40, 40}, {}, {}});
    child->SetGeometry({{40, 40}, {}, {}});
    root->consumePreview = true;
    REQUIRE(root->TryAddChild(child));

    Application app(allocator);
    app.SetRoot(root);
    auto result = app.RoutePointer({PointerEventType::Down, {5, 5}});
    REQUIRE(result);
    CHECK(result.Value());
    CHECK(root->previewCalls == 1 && child->previewCalls == 0);
    CHECK(root->calls == 0 && child->calls == 0);
}

TEST(UiFramework_RoutingKeepsPathAliveWhenTargetRemovesItself) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<TestElement>::TryMake(allocator, allocator);
    auto childResult = SharedPtr<TestElement>::TryMake(allocator, allocator);
    REQUIRE(rootResult && childResult);
    auto root = rootResult.Value();
    auto child = childResult.Value();
    root->SetGeometry({{40, 40}, {}, {}});
    child->SetGeometry({{40, 40}, {}, {}});
    child->removeSelf = true;
    REQUIRE(root->TryAddChild(child));
    Application app(allocator);
    app.SetRoot(root);
    auto result = app.RoutePointer({PointerEventType::Down, {5, 5}});
    REQUIRE(result);
    CHECK(root->ChildCount() == 0 && child->calls == 1 && root->calls == 1);
}

} // namespace ui_application_tests
