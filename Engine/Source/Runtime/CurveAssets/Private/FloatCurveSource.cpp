#include "CurveAssetResolve.h"
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <CurveAssets/FloatCurveSource.h>

// 스칼라 커브 원천의 평가·평탄화다(Docs/Design/CurveAssets 3.1절).
namespace rocks {

Result<float> FloatCurveSource::TryEvaluate(ObjectRegistry& registry,
                                            const ObjectAccessContext& access,
                                            float time) const noexcept {
    if (!HasAsset()) {
        return Result<float>::Success(curve.Evaluate(time));
    }

    return curve_assets_detail::WithCurveAsset<FloatCurveAsset>(
        registry, access, asset.Handle(), [time](const FloatCurveAsset& source) noexcept {
            return Result<float>::Success(source.Evaluate(time));
        });
}

Result<void> FloatCurveSource::TryFlattenInto(ObjectRegistry& registry,
                                              const ObjectAccessContext& access,
                                              TangentCurve& out) const noexcept {
    if (!HasAsset()) {
        return out.TryCopyFrom(curve);
    }

    return curve_assets_detail::WithCurveAsset<FloatCurveAsset>(
        registry, access, asset.Handle(),
        [&out](const FloatCurveAsset& source) noexcept { return out.TryCopyFrom(source.curve); });
}

} // namespace rocks
