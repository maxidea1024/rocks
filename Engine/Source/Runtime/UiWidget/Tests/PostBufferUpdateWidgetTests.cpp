#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/PostBufferUpdateWidget.generated.h>

namespace rocks::ui {

TEST(UiWidget_PostBufferEntriesRebuildAndRejectInvalidChannelsAtomically) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x425546, 1});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto object = objects.NewObject<PostBufferUpdateWidget>(
        access, GeneratedType<PostBufferUpdateWidget>::Id());
    REQUIRE(object);
    auto borrowed =
        objects.ResolveForWrite<PostBufferUpdateWidget>(access, object.Value().Handle());
    REQUIRE(borrowed);
    auto* widget = borrowed.Value().Get().Value();
    const PostBufferEntry entries[] = {{0, 1}, {4, 0}};
    REQUIRE(widget->SetBufferUpdates(entries));
    auto element = widget->TakeElement(objects);
    REQUIRE(element);
    auto* native = element.Value()->GetWidgetSurface();
    REQUIRE(native);
    CHECK(native->RequestedPostBuffers() == 17u);
    const PostBufferEntry invalid[] = {{0, 2}, {5, 0}};
    CHECK(!widget->SetBufferUpdates(invalid));
    CHECK(native->RequestedPostBuffers() == 17u);
    widget->ReleaseElements();
    auto rebuilt = widget->TakeElement(objects);
    REQUIRE(rebuilt);
    CHECK(rebuilt.Value()->GetWidgetSurface()->RequestedPostBuffers() == 17u);
}

} // namespace rocks::ui
