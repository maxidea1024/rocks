#include <UiWidget/RetainerEffectMaterial.generated.h>
#include "MakeWidgetElement.h"
#include <UiWidget/RetainerBoxWidget.h>
#include <UiFramework/RetainerBox.h>

namespace rocks::ui {

RetainerBoxWidget::RetainerBoxWidget(Allocator& allocator) noexcept
    : ContentWidget(allocator), textureParameter_(allocator) {
}

namespace {
Result<SharedPtr<const RetainerEffect>>
ResolveEffect(ObjectRegistry& objects, ObjectHandle material, StringView parameter) noexcept {
    if (material.IsNull()) {
        return Result<SharedPtr<const RetainerEffect>>::Success({});
    }
    auto found = objects.Resolve<RetainerEffectMaterial>(objects.OwnerAccess(), material);
    if (!found) {
        return Result<SharedPtr<const RetainerEffect>>::Failure(found.Error());
    }
    const auto* asset = found.Value().Get().Value();
    if (asset->TextureParameter() != parameter) {
        return Result<SharedPtr<const RetainerEffect>>::Failure(ErrorCode::NotFound);
    }
    return asset->Snapshot(objects.ObjectAllocator());
}
} // namespace

Result<SharedPtr<Element>> RetainerBoxWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto snapshot = ResolveEffect(objects, effectMaterial_.Handle(),
                                  textureParameter_.ByteCount() ? textureParameter_.View()
                                                                : StringView("Texture"));
    if (!snapshot) {
        return Result<SharedPtr<Element>>::Failure(snapshot.Error());
    }
    effect_ = Move(snapshot.Value());
    return ContentWidget::BuildElement(objects);
}

ObjectHandle RetainerBoxWidget::GetEffectMaterial() const noexcept {
    return effectMaterial_.Handle();
}

Result<void> RetainerBoxWidget::SetEffectMaterial(ObjectRegistry& objects, ObjectHandle material,
                                                  StringView textureParameter) noexcept {
    if (textureParameter.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    auto snapshot = ResolveEffect(objects, material, textureParameter);
    if (!snapshot) {
        return Result<void>::Failure(snapshot.Error());
    }
    String next(objects.ObjectAllocator());
    auto result = next.TryAssign(textureParameter);
    if (result) {
        result = objects.SetReference(objects.OwnerAccess(), Handle(), effectMaterial_, material);
    }
    if (result) {
        textureParameter_.Swap(next);
        effect_ = Move(snapshot.Value());
        if (auto live = CachedElement()) {
            ApplyProperties(*live);
        }
    }
    return result;
}

Result<void> RetainerBoxWidget::SetEffectTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    effectTint_ = tint;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
    return Result<void>::Success();
}

Result<SharedPtr<Element>> RetainerBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<RetainerBox>(allocator);
}

Result<void> RetainerBoxWidget::SetPhase(uint32 phase, uint32 count) noexcept {
    if (count == 0 || phase >= count) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    phase_ = phase;
    phaseCount_ = count;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
    return Result<void>::Success();
}

void RetainerBoxWidget::RequestRender() noexcept {
    if (auto live = CachedElement()) {
        static_cast<RetainerBox&>(*live).RequestRender();
    }
}

void RetainerBoxWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<RetainerBox&>(element);
    native.SetEffectMaterial(effect_);
    (void)native.SetEffectTint(effectTint_);
    (void)native.SetPhase(phase_, phaseCount_);
}

} // namespace rocks::ui
