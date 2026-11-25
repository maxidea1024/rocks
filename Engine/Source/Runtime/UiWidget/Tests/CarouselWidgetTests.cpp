#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/CarouselNavBarWidget.generated.h>
#include <UiWidget/CarouselWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TEST(UiWidget_CarouselReleasesHiddenPageAndFollowsNavigationButton) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 32});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRoot = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRoot);
    auto treeBorrow = objects.ResolveForWrite<WidgetTree>(access, treeRoot.Value().Handle());
    REQUIRE(treeBorrow);
    auto* tree = treeBorrow.Value().Get().Value();
    auto pagesId = tree->ConstructWidget(objects, GeneratedType<CarouselWidget>::Id(), "Pages");
    auto navId =
        tree->ConstructWidget(objects, GeneratedType<CarouselNavBarWidget>::Id(), "Navigation");
    auto first = tree->ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "First");
    auto second = tree->ConstructWidget(objects, GeneratedType<ButtonWidget>::Id(), "Second");
    REQUIRE(pagesId && navId && first && second);
    auto pagesBorrow = objects.ResolveForWrite<CarouselWidget>(access, pagesId.Value().Handle());
    auto navBorrow = objects.ResolveForWrite<CarouselNavBarWidget>(access, navId.Value().Handle());
    REQUIRE(pagesBorrow && navBorrow);
    auto* pages = pagesBorrow.Value().Get().Value();
    auto* nav = navBorrow.Value().Get().Value();
    REQUIRE(pages->SetMoveSpeed(0));
    REQUIRE(pages->SetCacheChildren(false));
    REQUIRE(pages->AddChild(objects, first.Value().Handle()));
    REQUIRE(pages->AddChild(objects, second.Value().Handle()));
    REQUIRE(nav->SetLinkedCarousel(objects, pagesId.Value().Handle()));
    auto view = pages->TakeElement(objects);
    auto bar = nav->TakeElement(objects);
    REQUIRE(view && bar);
    CHECK(view.Value()->ChildAt(0)->ChildCount() == 1);
    CHECK(view.Value()->ChildAt(1)->ChildCount() == 0);
    Application app(allocator);
    app.SetRoot(bar.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({80, 20}, {}), 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {30, 8}, 0}));
    REQUIRE(app.RoutePointer({PointerEventType::Up, {30, 8}, 0}));
    CHECK(pages->ActiveIndex() == 1);
    CHECK(view.Value()->ChildAt(0)->ChildCount() == 0);
    CHECK(view.Value()->ChildAt(1)->ChildCount() == 1);
    REQUIRE(pages->NextPage());
    CHECK(pages->ActiveIndex() == 0);
    REQUIRE(pages->PreviousPage());
    CHECK(pages->ActiveIndex() == 1);

    app.SetRoot(view.Value());
    REQUIRE(pages->BeginAutoScrolling(app, 1.e-12f));
    REQUIRE(app.UpdatePaintCache(Geometry::Root({80, 20}, {}), 1));
    CHECK(pages->ActiveIndex() == 0);
    pages->EndAutoScrolling();
    REQUIRE(pages->RemoveChild(objects, first.Value().Handle()));
    CHECK(pages->ActiveIndex() == 0);
    CHECK(bar.Value()->ChildAt(0)->ChildCount() == 1);
    auto stable = app.UpdatePaintCache(Geometry::Root({80, 20}, {}), 1);
    REQUIRE(stable);
    const auto revision = stable.Value()->Revision();
    REQUIRE(app.UpdatePaintCache(Geometry::Root({80, 20}, {}), 1));
    CHECK(stable.Value()->Revision() == revision && stable.Value()->LastUpdate().paintCalls == 0);
    nav->ReleaseElements();
    pages->ReleaseElements();
}

} // namespace rocks::ui
