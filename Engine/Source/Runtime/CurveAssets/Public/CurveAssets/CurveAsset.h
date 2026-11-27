#pragma once

#include <AssetRuntime/Package.h>
#include <Core/Math/Interval.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Curves/TangentCurve.h>

// 커브 에셋의 추상 기반이다(Docs/Design/CurveAssets 2.1절, UE UCurveBase 대응).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다. 읽기 함수는 할당하지 않는다.
namespace rocks {

// 채널(이름 붙은 TangentCurve) 목록을 가진 커브 에셋이다. 파생 클래스가 채널을 정한다.
//
// 사용 예(시간 범위 — 모든 채널의 키를 합친 범위):
//   auto range = curve.TryGetTimeRange();
//   if (range) { const float length = range.Value().max - range.Value().min; }
ROCKS_CLASS(Abstract)
class ROCKS_CURVEASSETS_API CurveAsset : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit CurveAsset(Allocator& allocator) noexcept {
        (void)allocator;
    }

    // 채널 수다(스칼라 1, 벡터 3, 색 4).
    virtual usize ChannelCount() const noexcept = 0;

    // index번 채널이다. index < ChannelCount()여야 한다(범위 밖은 assert).
    virtual const TangentCurve& Channel(usize index) const noexcept = 0;
    virtual TangentCurve& MutableChannel(usize index) noexcept = 0;

    // index번 채널의 이름이다(UE GetCurves의 이름 — 스칼라 "Value", 벡터 "X"·"Y"·"Z", 색
    // "R"·"G"·"B"·"A"). 범위 밖은 빈 문자열이다.
    virtual StringView ChannelName(usize index) const noexcept = 0;

    // 키가 있는 채널들의 시간 범위 합집합이다. 실패: 키가 있는 채널이 없으면 NotFound(UE는 키 없는
    // 채널의 (0, 0)도 합친다 — 설계 7.2절 1).
    Result<Interval<float>> TryGetTimeRange() const noexcept;

    // 키가 있는 채널들의 값 범위 합집합이다(TangentCurve::TryGetValueRange). 실패: 키가 있는 채널이
    // 없으면 NotFound, 채널의 범위가 유한하지 않으면 InvalidArgument.
    Result<Interval<float>> TryGetValueRange() const noexcept;

    // 모든 채널의 키를 비운다. 기본값·외삽은 그대로다(UE ResetCurve).
    void ResetCurve() noexcept;
};

} // namespace rocks
