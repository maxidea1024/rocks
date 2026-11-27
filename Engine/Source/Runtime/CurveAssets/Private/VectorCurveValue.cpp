#include "CurveChannelCopy.h"
#include <CurveAssets/VectorCurveValue.h>

// 벡터 커브 평탄 값이다(Docs/Design/CurveAssets 2.5절).
namespace rocks {

Vec3d VectorCurveValue::Evaluate(float time) const noexcept {
    return Vec3d(x.Evaluate(time), y.Evaluate(time), z.Evaluate(time));
}

Result<void> VectorCurveValue::TryCopyFrom(const VectorCurveValue& other) noexcept {
    if (this == &other) {
        return Result<void>::Success();
    }

    TangentCurve* const targets[] = {&x, &y, &z};
    const TangentCurve* const sources[] = {&other.x, &other.y, &other.z};
    return curve_assets_detail::TryCopyChannels(targets, sources);
}

} // namespace rocks
