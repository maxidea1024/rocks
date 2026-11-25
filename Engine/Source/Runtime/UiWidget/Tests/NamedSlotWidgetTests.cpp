#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/NamedSlotWidget.generated.h>
#include <UiWidget/OverlayWidget.generated.h>
#include <UiWidget/ProgressBarWidget.generated.h>
#include <UiWidget/SpinBoxWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

TEST(UiWidget_NamedSlotsReplaceLiveContentAndKeepOwnershipOnFailure) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 45});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto screen = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
    REQUIRE(screen);
    auto foreign = objects.NewObject<UserWidget>(access, GeneratedType<UserWidget>::Id());
    REQUIRE(foreign);
    ObjectHandle oldId, newId;

    {
        auto screenBorrow = objects.ResolveForWrite<UserWidget>(access, screen.Value().Handle());
        REQUIRE(screenBorrow);
        auto& user = *screenBorrow.Value().Get().Value();
        REQUIRE(user.Initialize(objects));
        auto treeBorrow = objects.ResolveForWrite<WidgetTree>(access, user.Tree());
        REQUIRE(treeBorrow);
        auto& tree = *treeBorrow.Value().Get().Value();
        auto root = tree.ConstructWidget(objects, GeneratedType<OverlayWidget>::Id(), "Root");
        auto header = tree.ConstructWidget(objects, GeneratedType<NamedSlotWidget>::Id(), "Header");
        auto footer = tree.ConstructWidget(objects, GeneratedType<NamedSlotWidget>::Id(), "Footer");
        auto detached =
            tree.ConstructWidget(objects, GeneratedType<NamedSlotWidget>::Id(), "Detached");
        REQUIRE(root && header && footer && detached);
        REQUIRE(tree.SetRoot(objects, root.Value().Handle()));
        auto panel = objects.ResolveForWrite<OverlayWidget>(access, root.Value().Handle());
        REQUIRE(panel);
        REQUIRE(panel.Value().Get().Value()->AddChild(objects, header.Value().Handle()));
        REQUIRE(panel.Value().Get().Value()->AddChild(objects, footer.Value().Handle()));
        auto names = user.GetSlotNames(objects);
        REQUIRE(names && names.Value().Size() == 2);
        CHECK(names.Value()[0].View() == "Footer" && names.Value()[1].View() == "Header");
        CHECK(!user.GetContentForSlot(objects, "Detached"));
        REQUIRE(user.GetContentForSlot(objects, "Header").Value().IsNull());

        auto old = tree.ConstructWidget(objects, GeneratedType<ProgressBarWidget>::Id(), "Old");
        auto next = tree.ConstructWidget(objects, GeneratedType<ProgressBarWidget>::Id(), "New");
        auto unshaped =
            tree.ConstructWidget(objects, GeneratedType<SpinBoxWidget>::Id(), "Unshaped");
        REQUIRE(old && next && unshaped);
        oldId = old.Value().Handle();
        newId = next.Value().Handle();
        REQUIRE(user.SetContentForSlot(objects, "Header", oldId));
        auto live = user.TakeElement(objects);
        REQUIRE(live);
        auto named = objects.Resolve<NamedSlotWidget>(access, header.Value().Handle());
        REQUIRE(named);
        auto& slot = *named.Value().Get().Value();
        const auto slotId = slot.SlotAt(0);
        const auto slotElement = slot.CachedElement();
        REQUIRE(slotElement);
        const auto oldElement = slotElement->ChildRefAt(0);
        REQUIRE(oldElement);
        auto failed = user.SetContentForSlot(objects, "Header", unshaped.Value().Handle());
        CHECK(!failed && failed.Error() == ErrorCode::NotReady);
        CHECK(!user.SetContentForSlot(objects, "Header", foreign.Value().Handle()));
        CHECK(!user.SetContentForSlot(objects, "Header", root.Value().Handle()));
        CHECK(!user.SetContentForSlot(objects, "Header", footer.Value().Handle()));
        CHECK(slotElement->ChildAt(0) == oldElement.Get());
        CHECK(user.GetContentForSlot(objects, "Header").Value() == oldId);

        REQUIRE(user.SetContentForSlot(objects, "Header", newId));
        CHECK(slot.SlotAt(0) == slotId);
        auto source = user.FindSlotForContent(objects, newId);
        REQUIRE(source && source.Value().View() == "Header");
        CHECK(!user.ContainsContent(objects, oldId).Value());
        CHECK(slotElement->ChildCount() == 1 && slotElement->ChildAt(0) != oldElement.Get());
        CHECK(oldElement->Parent() == nullptr);
        REQUIRE(user.SetContentForSlot(objects, "Header", newId));
        user.ReleaseElements();
        REQUIRE(user.TakeElement(objects));
        CHECK(user.GetContentForSlot(objects, "Header").Value() == newId);
    }

    REQUIRE(objects.Collect(access));
    CHECK(!objects.Resolve<Widget>(access, oldId));
    CHECK(objects.Resolve<Widget>(access, newId));
    {
        auto owner = objects.ResolveForWrite<UserWidget>(access, screen.Value().Handle());
        REQUIRE(owner);
        auto& user = *owner.Value().Get().Value();
        REQUIRE(user.SetContentForSlot(objects, "Header", {}));
        CHECK(user.GetContentForSlot(objects, "Header").Value().IsNull());
    }

    REQUIRE(objects.Collect(access));
    CHECK(!objects.Resolve<Widget>(access, newId));
}

} // namespace rocks::ui
