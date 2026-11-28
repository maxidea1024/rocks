#pragma once

#include <Core/Math/MathUtility.h>
#include <Core/Memory/SharedPtr.h>
#include <CurveAssets/FloatCurveSource.h>
#include <Curves/TangentCurve.h>
#include <Object/ObjectAccessContext.h>

// AudioAssets 값 검사의 공용 도움 함수다(스냅숏·분류·믹스 검사가 같이 쓴다).
namespace rocks::audio_assets_detail {

inline bool IsFiniteWithin(float value, float low, float high) noexcept {
    return math::IsFinite(value) && value >= low && value <= high;
}

inline bool IsNonNegativeFinite(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

// 쓰이는 곡선을 깊이 복사해 공유 사본으로 둔다(3.42.1절 — 제어 쪽이 Registry 없이 평가). 감쇠
// 곡선과 버스 send 곡선이 같이 쓴다.
inline Result<void> FlattenCurve(const FloatCurveSource& source, Allocator& allocator,
                                 ObjectRegistry& registry, const ObjectAccessContext& access,
                                 SharedPtr<const TangentCurve>& out) noexcept {
    auto made = SharedPtr<TangentCurve>::TryMake(allocator, allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (auto flattened = source.TryFlattenInto(registry, access, *made.Value().Get()); !flattened) {
        return flattened;
    }

    out = made.Value();
    return Result<void>::Success();
}

} // namespace rocks::audio_assets_detail
