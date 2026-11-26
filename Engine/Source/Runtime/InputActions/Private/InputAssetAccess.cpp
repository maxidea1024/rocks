#include <AssetRuntime/AssetManager.h>
#include <InputActions/Haptics/HapticEffectAsset.generated.h>
#include <InputActions/Haptics/HapticEffectAsset.h>
#include <InputActions/InputAssetAccess.h>
#include <InputActions/InputContextCollect.h>
#include <Object/ObjectRegistry.h>

namespace rocks::input {

Result<void> CollectInputContext(AssetManager& assets, const AssetLease& context,
                                 InputContextBundle& bundle) noexcept {
    Result<void> collected = Result<void>::Success();
    auto opened = assets.With(context, [&](ObjectHandle object) noexcept {
        ObjectRegistry& registry = assets.Registry();
        collected = CollectInputContext(registry, registry.OwnerAccess(), object, bundle);
    });
    if (!opened) {
        return opened;
    }

    return collected;
}

Result<void> CopyHapticEffect(AssetManager& assets, const AssetLease& effect,
                              HapticEffect& out) noexcept {
    Result<void> copied = Result<void>::Success();
    auto opened = assets.With(effect, [&](ObjectHandle object) noexcept {
        ObjectRegistry& registry = assets.Registry();
        auto borrow = registry.Resolve<HapticEffectAsset>(registry.OwnerAccess(), object);
        if (!borrow) {
            copied = Result<void>::Failure(borrow.Error());
            return;
        }

        auto pointer = borrow.Value().Get();
        if (!pointer) {
            copied = Result<void>::Failure(pointer.Error());
            return;
        }

        const HapticEffectAsset& asset = *pointer.Value();
        copied = out.TryCopyFrom(asset.effect);
        if (copied) {
            out.id = asset.EffectiveId();
        }
    });
    if (!opened) {
        return opened;
    }

    return copied;
}

} // namespace rocks::input
