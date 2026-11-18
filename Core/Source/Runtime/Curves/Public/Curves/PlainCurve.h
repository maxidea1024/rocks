#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Interval.h>
#include <Core/Result.h>
#include <Curves/CurveTypes.h>
#include <Meta/Annotations.h>

namespace rocks {

// 접선 없이 커브 전체에 보간 방식 하나(Linear·Constant)를 쓰는 스칼라 커브다(UE 5.8 FSimpleCurve —
// Docs/Design/CurveData.ko.md 4·5.7·7절). 시간 오름차순 PlainCurveKey 배열과 공통 칸
// (ScalarCurveBase), 보간 방식 interp로 이뤄진다. 키당 8 byte라 TangentCurve보다 작고 평가가 싸다.
//
// 불변식·검증 hook·편집 연산의 실패 규약·복사·스레드 규칙은 TangentCurve와 같다(TangentCurve.h).
// 평가는 UE FSimpleCurve::Eval과 같은 식이다. TangentCurve의 Linear 커브와는 구간 경계만 다르다:
// 첫 키 시간 이하는 앞 외삽 갈래, 마지막 키 시간 이상은 뒤 외삽 갈래다(설계 5.7절).
//
// 사용 예:
//   PlainCurve curve(allocator);
//   curve.interp = PlainCurveInterp::Constant;
//   if (auto added = curve.TryAddKey(0.0f, 1.0f); !added) { return added.Error(); }
//   if (auto added = curve.TryAddKey(2.0f, 5.0f); !added) { return added.Error(); }
//   const float value = curve.Evaluate(1.0f);   // 1(Constant)
ROCKS_STRUCT()
struct PlainCurve : ScalarCurveBase {
    ROCKS_GENERATED_BODY()
public:
    // 키 없는 Linear 커브다. allocator는 키 배열이 쓴다.
    explicit PlainCurve(Allocator& allocator) noexcept : keys_(allocator) {
    }

    // 커브 전체의 보간 방식이다. Cubic은 표현할 수 없다(PlainCurveInterp — D7).
    ROCKS_PROPERTY() PlainCurveInterp interp = PlainCurveInterp::Linear;

    // other의 공통 칸·보간 방식·키를 복사한다. 실패(OutOfMemory)면 이 커브는 그대로다.
    Result<void> TryCopyFrom(const PlainCurve& other) noexcept;

    // ---- 읽기 ----

    // 시간 오름차순 키다. 다음 편집 연산까지 유효하다.
    Span<const PlainCurveKey> Keys() const noexcept {
        return keys_.View();
    }

    usize KeyCount() const noexcept {
        return keys_.Size();
    }

    // time의 값이다(UE FSimpleCurve::Eval — 설계 5.7절). 키 없음·외삽·NaN 규칙은
    // TangentCurve::Evaluate와 같다. 구간 선택만 다르다: time ≤ 첫 키 시간이면 앞 외삽 갈래, time ≥
    // 마지막 키 시간이면 뒤 외삽 갈래(Linear 외삽이어도 끝 키 시간에서는 끝 키 값), 그 사이는 time
    // 이하인 마지막 키에서 시작하는 구간이다. 구간 값은 Linear면 Lerp, Constant면 앞 키 값이다(길이
    // 0이면 앞 키 값). 할당하지 않는다.
    float Evaluate(float time, float fallback = 0.0f) const noexcept;

    // 키 ≤ 1이거나 모든 키 값이 첫 키 값과 tolerance 안이면 참이다(Cubic이 없으므로 값만 본다).
    bool IsConstant(float tolerance = 1.0e-8f) const noexcept;

    // |키 시간 − time| ≤ tolerance인 가장 앞 키의 index다(없거나 NaN·음수 tolerance면 IndexNone).
    usize FindKey(float time, float tolerance = 1.0e-4f) const noexcept;

    // [첫 키 시간, 마지막 키 시간]이다. 실패: 키가 없으면 NotFound.
    Result<Interval<float>> TryGetTimeRange() const noexcept;

    // 키 값의 [최소, 최대]다(Linear·Constant 구간은 끝 값 사이라 키 값이 곧 범위다 — UE
    // FSimpleCurve::GetValueRange와 같다). 외삽은 보지 않는다. 실패: 키가 없으면 NotFound, 키 값이
    // 비유한이면 InvalidArgument.
    Result<Interval<float>> TryGetValueRange() const noexcept;

    // ---- 편집 — 규칙은 TangentCurve의 같은 이름 연산과 같다(새 키는 시간·값뿐) ----

    Result<usize> TryAddKey(float time, float value, bool unwindRotation = false) noexcept;
    Result<usize> TryUpdateOrAddKey(float time, float value, bool unwindRotation = false,
                                    float tolerance = 1.0e-4f) noexcept;
    Result<void> TryRemoveKey(usize index) noexcept;
    Result<usize> TrySetKeyTime(usize index, float time) noexcept;
    Result<void> TrySetKeyValue(usize index, float value) noexcept;
    Result<void> TrySetKeys(Span<const PlainCurveKey> keys) noexcept;
    void Reset() noexcept;
    Result<void> TryShiftTime(float delta) noexcept;
    Result<void> TryScaleTime(float origin, float factor) noexcept;
    Result<void> TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept;
    Result<void> TryBake(float interval, float first, float last) noexcept;
    Result<void> TryBake(float interval) noexcept;

    // 첫 키와 마지막 키 사이에서 빼도 커브가 tolerance 안에서 같은 키를 지운다(UE
    // FSimpleCurve::RemoveRedundantKeysInternal — 남긴 키를 되짚어 다시 지우는 알고리즘이라 두 번
    // 불러도 더 지우지 않는다). 키 시간에서 "앞뒤 남긴 키로 계산한 값"과 키 값을 비교한다. UE는
    // 표본 프레임 속도 인자를 받지만 쓰지 않으므로 두지 않는다. 키가 3개 미만이면 아무것도 하지
    // 않는다. 실패: tolerance가 NaN·음수면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<void> TryRemoveRedundantKeys(float tolerance) noexcept;

    // 범위판이다: 시간이 first 이하인 마지막 키부터 last 이상인 첫 키까지만 본다. first ≥
    // last이거나 그런 키가 없으면 아무것도 하지 않는다. 실패: first·last가 NaN이거나 tolerance가
    // NaN·음수면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<void> TryRemoveRedundantKeys(float tolerance, float first, float last) noexcept;

    // 리플렉션 경로의 검증 hook이다: 키 시간이 모두 유한하고 오름차순이면 Success, 아니면
    // InvalidArgument.
    static Result<void> Validate(const PlainCurve& curve) noexcept;

private:
    ROCKS_PROPERTY(Name = "keys") Array<PlainCurveKey> keys_;
};

} // namespace rocks
