#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/Element.h>

using namespace rocks;
using namespace rocks::ui;

TEST(UiCore_TreeRejectsCyclesAndDetachesChildren) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<Element>::TryMake(allocator, allocator);
    auto childResult = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(rootResult && childResult);
    auto root = rootResult.Value();
    auto child = childResult.Value();
    REQUIRE(root->TryAddChild(child));
    CHECK(child->Parent() == root.Get());
    CHECK(!child->TryAddChild(root));
    CHECK(!root->TryAddChild(child));
    CHECK(root->RemoveChild(*child));
    CHECK(child->Parent() == nullptr);
    CHECK(root->ChildCount() == 0);
}

TEST(UiCore_HitTestUsesTopmostChildAndVisibility) {
    SystemAllocator allocator;
    auto rootResult = SharedPtr<Element>::TryMake(allocator, allocator);
    auto backResult = SharedPtr<Element>::TryMake(allocator, allocator);
    auto frontResult = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(rootResult && backResult && frontResult);
    auto root = rootResult.Value();
    auto back = backResult.Value();
    auto front = frontResult.Value();
    Geometry geometry{{100, 100}, {}, {}};
    root->SetGeometry(geometry);
    back->SetGeometry(geometry);
    front->SetGeometry(geometry);
    REQUIRE(root->TryAddChild(back));
    REQUIRE(root->TryAddChild(front));
    CHECK(root->HitTest({10, 10}) == front.Get());
    front->SetVisibility(Visibility::Hidden);
    CHECK(root->HitTest({10, 10}) == back.Get());
    root->SetVisibility(Visibility::HitTestInvisible);
    CHECK(root->HitTest({10, 10}) == nullptr);
}
