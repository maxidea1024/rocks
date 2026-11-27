#include "CurveAssetResolve.h"
#include "CurveChannelCopy.h"
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/ColorCurveSource.h>

// 색 커브 원천의 평가·평탄화다(Docs/Design/CurveAssets 3.1절).
namespace rocks {

Result<LinearColor> ColorCurveSource::TryEvaluate(ObjectRegistry& registry,
                                                  const ObjectAccessContext& access,
                                                  float time) const noexcept {
    if (!HasAsset()) {
        return Result<LinearColor>::Success(EvaluateInline(time));
    }

    return curve_assets_detail::WithCurveAsset<ColorCurveAsset>(
        registry, access, asset.Handle(), [time](const ColorCurveAsset& source) noexcept {
            return Result<LinearColor>::Success(source.Evaluate(time));
        });
}

Result<void> ColorCurveSource::TryFlattenInto(ObjectRegistry& registry,
                                              const ObjectAccessContext& access,
                                              ColorCurveValue& out) const noexcept {
    TangentCurve* const targets[] = {&out.r, &out.g, &out.b, &out.a};
    if (!HasAsset()) {
        const TangentCurve* const sources[] = {&r, &g, &b, &a};
        if (auto copied = curve_assets_detail::TryCopyChannels(targets, sources); !copied) {
            return copied;
        }

        out.SetAdjustments(ColorCurveAdjustments{}, false);
        return Result<void>::Success();
    }

    return curve_assets_detail::WithCurveAsset<ColorCurveAsset>(
        registry, access, asset.Handle(), [&](const ColorCurveAsset& source) noexcept {
            const TangentCurve* const sources[] = {&source.r, &source.g, &source.b, &source.a};
            if (auto copied = curve_assets_detail::TryCopyChannels(targets, sources); !copied) {
                return copied;
            }

            out.SetAdjustments(source.Adjustments(), true);
            return Result<void>::Success();
        });
}

} // namespace rocks
