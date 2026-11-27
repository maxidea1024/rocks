#pragma once

#include <Core/Math/Vector3.h>
#include <CurveAssets/CurveAsset.h>

// 벡터 커브 에셋이다(Docs/Design/CurveAssets 2.3절, UE UCurveVector 대응).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
namespace rocks {

// 성분 X·Y·Z의 TangentCurve 셋을 담는다(UE FloatCurves[3] — 이름 있는 필드 셋).
//
// 사용 예:
//   const Vec3d offset = curve->Evaluate(time);
ROCKS_CLASS()
class ROCKS_CURVEASSETS_API VectorCurveAsset final : public CurveAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit VectorCurveAsset(Allocator& allocator) noexcept
        : CurveAsset(allocator), x(allocator), y(allocator), z(allocator) {
    }

    // 성분마다 Evaluate(time)다(fallback 0 — UE GetVectorValue).
    Vec3d Evaluate(float time) const noexcept;

    usize ChannelCount() const noexcept override;
    const TangentCurve& Channel(usize index) const noexcept override;
    TangentCurve& MutableChannel(usize index) noexcept override;
    StringView ChannelName(usize index) const noexcept override;

    ROCKS_PROPERTY() TangentCurve x;
    ROCKS_PROPERTY() TangentCurve y;
    ROCKS_PROPERTY() TangentCurve z;
};

} // namespace rocks
