#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/RetainerBox.h>
#include <UiWidget/RetainerBoxWidget.generated.h>
#include <UiWidget/RetainerEffectMaterial.generated.h>

namespace rocks::ui {

TEST(UiWidget_RetainerResolvesNamedTextureAndRebuildsEffectFromAsset) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x524554, 1});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto asset = objects.NewObject<RetainerEffectMaterial>(
        access, GeneratedType<RetainerEffectMaterial>::Id());
    auto widget =
        objects.NewObject<RetainerBoxWidget>(access, GeneratedType<RetainerBoxWidget>::Id());
    REQUIRE(asset && widget);
    const auto handle = asset.Value().Handle();
    auto material = objects.ResolveForWrite<RetainerEffectMaterial>(access, handle);
    auto box = objects.ResolveForWrite<RetainerBoxWidget>(access, widget.Value().Handle());
    REQUIRE(material && box);
    auto* effect = material.Value().Get().Value();
    auto* owner = box.Value().Get().Value();
    REQUIRE(effect->SetShaders("pixel one", "fragment one", "SceneColor"));
    REQUIRE(owner->SetEffectMaterial(objects, handle, "SceneColor"));
    CHECK(!owner->SetEffectMaterial(objects, handle, "WrongName"));
    CHECK(owner->GetEffectMaterial() == handle);
    auto element = owner->TakeElement(objects);
    REQUIRE(element);
    auto cached = static_cast<RetainerBox&>(*element.Value()).EffectMaterial();
    REQUIRE(cached);
    CHECK(cached->Hlsl() == "pixel one");
    REQUIRE(effect->SetShaders("pixel two", "fragment two", "SceneColor"));
    CHECK(cached->Hlsl() == "pixel one");
    owner->ReleaseElements();
    auto rebuilt = owner->TakeElement(objects);
    REQUIRE(rebuilt);
    CHECK(static_cast<RetainerBox&>(*rebuilt.Value()).EffectMaterial()->Glsl() == "fragment two");
    REQUIRE(owner->SetEffectMaterial(objects, {}));
    CHECK(!static_cast<RetainerBox&>(*rebuilt.Value()).EffectMaterial());
}

} // namespace rocks::ui
