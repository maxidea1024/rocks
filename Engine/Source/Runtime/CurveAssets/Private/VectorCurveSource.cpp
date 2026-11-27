#include "CurveAssetResolve.h"
#include "CurveChannelCopy.h"
#include <CurveAssets/VectorCurveAsset.generated.h>
#include <CurveAssets/VectorCurveSource.h>

// 벡터 커브 원천의 평가·평탄화다(Docs/Design/CurveAssets 3.1절).
namespace rocks {

Result<Vec3d> VectorCurveSource::TryEvaluate(ObjectRegistry& registry,
                                             const ObjectAccessContext& access,
                                             float time) const noexcept {
    if (!HasAsset()) {
        return Result<Vec3d>::Success(curves.Evaluate(time));
    }

    return curve_assets_detail::WithCurveAsset<VectorCurveAsset>(
        registry, access, asset.Handle(), [time](const VectorCurveAsset& source) noexcept {
            return Result<Vec3d>::Success(source.Evaluate(time));
        });
}

Result<void> VectorCurveSource::TryFlattenInto(ObjectRegistry& registry,
                                               const ObjectAccessContext& access,
                                               VectorCurveValue& out) const noexcept {
    if (!HasAsset()) {
        return out.TryCopyFrom(curves);
    }

    return curve_assets_detail::WithCurveAsset<VectorCurveAsset>(
        registry, access, asset.Handle(), [&out](const VectorCurveAsset& source) noexcept {
            TangentCurve* const targets[] = {&out.x, &out.y, &out.z};
            const TangentCurve* const sources[] = {&source.x, &source.y, &source.z};
            return curve_assets_detail::TryCopyChannels(targets, sources);
        });
}

} // namespace rocks
