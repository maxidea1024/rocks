#include "CurveAssetResolve.h"
#include <CurveAssets/CurveTableAsset.generated.h>
#include <CurveAssets/CurveTableRowRef.h>

// 커브 표 행 참조의 평가다(Docs/Design/CurveAssets 3.3절).
namespace rocks {

Result<float> CurveTableRowRef::TryEvaluate(ObjectRegistry& registry,
                                            const ObjectAccessContext& access,
                                            float time) const noexcept {
    if (table.Handle().IsNull()) {
        return Result<float>::Failure(ErrorCode::NotFound);
    }

    const Name target = row;
    return curve_assets_detail::WithCurveAsset<CurveTableAsset>(
        registry, access, table.Handle(), [target, time](const CurveTableAsset& source) noexcept {
            return source.TryEvaluateRow(target, time);
        });
}

} // namespace rocks
