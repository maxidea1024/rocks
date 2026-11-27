#pragma once

#include <Core/Math/Vector3.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>

// 벡터 커브의 평탄 값이다(Docs/Design/CurveAssets 2.5절). 참조가 없어 복사·배열 원소·Map 값에 쓸 수
// 있다. VectorCurveAsset·VectorCurveSource의 사본(평탄화) 대상이다.
namespace rocks {

// 성분 X·Y·Z의 TangentCurve다(단위·좌표는 UE와 같다 — CurveData 13절).
//
// 사용 예:
//   VectorCurveValue offset(allocator);
//   offset.z.TryAddKey(0.0f, 0.0f); offset.z.TryAddKey(1.0f, 100.0f);
//   const Vec3d at = offset.Evaluate(0.5f);
ROCKS_STRUCT()
struct ROCKS_CURVEASSETS_API VectorCurveValue {
    ROCKS_GENERATED_BODY()

    explicit VectorCurveValue(Allocator& allocator) noexcept
        : x(allocator), y(allocator), z(allocator) {
    }

    // 성분마다 Evaluate(time)다(fallback 0 — UE UCurveVector::GetVectorValue).
    Vec3d Evaluate(float time) const noexcept;

    // other를 깊이 복사한다. 실패(할당)면 이 값은 그대로다.
    Result<void> TryCopyFrom(const VectorCurveValue& other) noexcept;

    ROCKS_PROPERTY() TangentCurve x;
    ROCKS_PROPERTY() TangentCurve y;
    ROCKS_PROPERTY() TangentCurve z;
};

} // namespace rocks
