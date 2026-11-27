#include "CurveAssetResolve.h"
#include <CurveAssets/CurveAssetSlot.h>
#include <CurveAssets/FloatCurveAsset.generated.h>

// 커브 에셋 slot의 평탄화다(Docs/Design/CurveAssets 6.1절).
namespace rocks {

Result<void> TryResolveCurveSlot(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 const ObjectRefArray<FloatCurveAsset>& assets, uint32& slot,
                                 TangentCurve& curve) noexcept {
    if (slot == NoCurveAssetSlot) {
        return Result<void>::Success();
    }

    const Span<const ObjectHandle> handles = assets.View();
    if (slot >= handles.Size() || handles[slot].IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto copied = curve_assets_detail::WithCurveAsset<FloatCurveAsset>(
        registry, access, handles[slot], [&curve](const FloatCurveAsset& source) noexcept {
            return curve.TryCopyFrom(source.curve);
        });
    if (!copied) {
        return copied;
    }

    slot = NoCurveAssetSlot;
    return Result<void>::Success();
}

} // namespace rocks
