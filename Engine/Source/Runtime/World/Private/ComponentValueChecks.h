#pragma once

#include <Core/Math/MathUtility.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>

// 데이터 컴포넌트 setter가 같이 쓰는 값 검사다(Docs/Design/EngineRuntime 24.2.3절 — 비유한·범위
// 밖은 InvalidArgument). 이름 있는 namespace라 유니티 빌드에서 파일마다 겹치지 않는다.
namespace rocks::component_checks {

inline Result<void> Invalid() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

inline bool InRange(float value, float minimum, float maximum) noexcept {
    return math::IsFinite(value) && value >= minimum && value <= maximum;
}

inline bool NonNegative(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

// 유한이고 minimum 이상이다(UE ClampMin만 있는 속성).
inline bool AtLeast(float value, float minimum) noexcept {
    return math::IsFinite(value) && value >= minimum;
}

inline bool NonNegative(double value) noexcept {
    return math::IsFinite(value) && value >= 0.0;
}

inline bool FiniteVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

// 정규화할 수 없으면 0 벡터다(UE GetSafeNormal).
inline Vec3d SafeNormal(const Vec3d& value) noexcept {
    auto normal = value.TryNormalize();
    return normal ? normal.Value() : Vec3d();
}

} // namespace rocks::component_checks
