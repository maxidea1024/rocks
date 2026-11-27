#pragma once

#include <CurveAssets/CurveAsset.h>

// 스칼라 커브 에셋이다(Docs/Design/CurveAssets 2.2절, UE UCurveFloat 대응).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
namespace rocks {

// TangentCurve 하나를 담는다. 타임라인·후처리·게임 코드가 시간 → 값으로 평가한다.
//
// 사용 예(소유 Registry에서 읽기):
//   auto borrowed = registry.Resolve<FloatCurveAsset>(access, handle);
//   if (!borrowed) { return Result<float>::Failure(borrowed.Error()); }
//   auto curve = borrowed.Value().Get();
//   if (!curve) { return Result<float>::Failure(curve.Error()); }
//   const float value = curve.Value()->Evaluate(0.5f);
ROCKS_CLASS()
class ROCKS_CURVEASSETS_API FloatCurveAsset final : public CurveAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit FloatCurveAsset(Allocator& allocator) noexcept
        : CurveAsset(allocator), curve(allocator) {
    }

    // curve.Evaluate(time)다(fallback 0 — UE GetFloatValue: 키가 없으면 defaultValue, 없으면 0).
    float Evaluate(float time) const noexcept {
        return curve.Evaluate(time);
    }

    usize ChannelCount() const noexcept override;
    const TangentCurve& Channel(usize index) const noexcept override;
    TangentCurve& MutableChannel(usize index) noexcept override;
    StringView ChannelName(usize index) const noexcept override;

    ROCKS_PROPERTY() TangentCurve curve;
    // 타임라인 이벤트 트랙용 커브다(UE bIsEventCurve — 이벤트 트랙은 키 시간만 쓴다).
    ROCKS_PROPERTY() bool eventCurve = false;
};

} // namespace rocks
