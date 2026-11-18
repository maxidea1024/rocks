#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Interval.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Time/FrameRate.h>
#include <Curves/CurveTypes.h>
#include <Meta/Annotations.h>

namespace rocks {

// 키마다 보간·접선·가중 접선을 갖는 스칼라 커브다(UE 5.8 FRichCurve — Docs/Design/CurveData.ko.md
// 4–7절). 시간 오름차순으로 정렬된 TangentCurveKey 배열과 공통 칸(ScalarCurveBase: 기본값·앞·뒤
// 외삽)으로 이뤄진다.
//
// 불변식: 키 시간은 유한하고 오름차순이다(같은 시간 키는 허용한다). 키 배열은 private이라 C++
// API가 이 불변식을 지키고, 리플렉션 경로(graph decode·JSON·명령행 import·대입 묶음·경로 쓰기
// `keys[3].time=…`)는 검증 hook Validate가 지킨다(설계 9.1절 — 어기는 값은 게시하지 않는다).
// 키 값·접선·가중치는 비유한이어도 받아서 결과로 전파한다.
//
// 평가는 UE FRichCurve::Eval과 같은 식·같은 연산 순서라 Windows x64에서 UE와 비트가 같다(설계
// 15절의 수정 — NaN 시간은 NaN, 순환 외삽의 길이 0·큰 주기 몫, Lerp 끝점 — 만 다르다).
//
// 편집 연산은 모두 키 index와 Result로 동작하고 실패하면 커브를 바꾸지 않는다. 어느 연산도 접선을
// 몰래 다시 계산하지 않는다(D10 — UE는 DeleteKey·SetKeys·SetKeyTime·RemoveRedundantKeys 뒤에
// AutoSetTangents를 부른다). 자동 접선이 필요한 편집 도구는 편집 묶음 끝에 AutoSetTangents를 한 번
// 부른다.
//
// 복사는 TryCopyFrom(할당 실패를 Result로), 이동은 noexcept다. 동기화는 없다. 바뀌지 않는 동안의
// const 함수는 여러 스레드가 함께 불러도 된다(내부 상태를 고치지 않는다). 키 배열의 Allocator는
// 커브보다 오래 살아야 한다.
//
// 사용 예:
//   TangentCurve curve(allocator);
//   curve.postExtrapolation = CurveExtrapolation::Cycle;
//   if (auto added = curve.TryAddKey(0.0f, 0.0f); !added) { return added.Error(); }
//   if (auto added = curve.TryAddKey(1.0f, 10.0f); !added) { return added.Error(); }
//   curve.TrySetKeyInterp(0, CurveInterp::Cubic);
//   curve.AutoSetTangents();
//   const float value = curve.Evaluate(2.5f);   // 순환: 1.5초 자리의 값
ROCKS_STRUCT()
struct TangentCurve : ScalarCurveBase {
    ROCKS_GENERATED_BODY()
public:
    // 키 없는 커브다(기본값 없음·앞·뒤 외삽 Constant). allocator는 키 배열이 쓴다.
    explicit TangentCurve(Allocator& allocator) noexcept : keys_(allocator) {
    }

    // other의 공통 칸과 키를 복사한다. 키 배열은 이 커브의 allocator로 새로 만든다.
    // 실패(OutOfMemory)면 이 커브는 그대로다.
    Result<void> TryCopyFrom(const TangentCurve& other) noexcept;

    // ---- 읽기 ----

    // 시간 오름차순 키다. 다음 편집 연산까지 유효하다.
    Span<const TangentCurveKey> Keys() const noexcept {
        return keys_.View();
    }

    usize KeyCount() const noexcept {
        return keys_.Size();
    }

    // time의 값이다(UE FRichCurve::Eval — 설계 5절).
    // - 키가 없으면 defaultValue, 그것도 없으면 fallback.
    // - 키가 하나면 외삽과 무관하게 그 키 값.
    // - 첫 키 이전·마지막 키 이후는 앞·뒤 외삽(CurveExtrapolation). 순환 계열은 시간을 키 범위로
    //   옮기고(math::CycleCurveTime), Linear는 끝의 두 키 값의 기울기(두 키 시간 차가 1e-8 이하면
    //   끝 키 값), Constant는 끝 키 값이다.
    // - 키 범위 안은 time 이하인 마지막 키에서 시작하는 구간을 그 키의 보간 방식으로 계산한다
    //   (math::EvalCurveSegment). 키 시간에 정확히 맞으면 그 키 값이다.
    // - NaN 시간은 키가 있으면 NaN이다(UE는 마지막 키 값 — 설계 15절 4). ±Inf는 순환 외삽이면 끝 키
    //   값, Linear 외삽이면 산술 결과(±Inf 또는 NaN)다.
    // 할당하지 않고 Fatal로 가는 경로가 없다.
    float Evaluate(float time, float fallback = 0.0f) const noexcept;

    // 키 ≤ 1이거나, 모든 키 값이 첫 키 값과 tolerance 안이고 Cubic 키가 없으면 참이다(UE
    // FRichCurve::IsConstant). tolerance가 NaN이면 키가 둘 이상일 때 거짓이다.
    bool IsConstant(float tolerance = 1.0e-8f) const noexcept;

    // |키 시간 − time| ≤ tolerance인 가장 앞 키의 index다. 없거나 time·tolerance가 NaN·음수면
    // IndexNone이다(UE GetKeyIndex는 이진 탐색 경로에 따라 여러 키 중 하나 — 설계 15절 6). O(log
    // N).
    usize FindKey(float time, float tolerance = 1.0e-4f) const noexcept;

    // [첫 키 시간, 마지막 키 시간]이다. 실패: 키가 없으면 NotFound(UE는 (0, 0)).
    Result<Interval<float>> TryGetTimeRange() const noexcept;

    // 키 범위 [첫 키, 마지막 키] 안에서 Evaluate가 낼 수 있는 값의 닫힌 범위 [최소,
    // 최대]다(설계 7.3절, D11). 구간마다 그 구간이 실제로 쓰는 값을 해석적으로 구한다: Linear는 두
    // 끝, Constant는 앞 키 값(마지막 구간은 끝 키 값도), Cubic은 두 끝과 극값(가중 구간은
    // Evaluate가 실제로 쓰는 매개변수 집합 위에서). 구간 끝은 극한으로 넣는다. 쓰이지 않는 키
    // 값(길이 0 구간의 앞 키, 가중 구간이 건너뛰는 시작 값)은 넣지 않는다. 외삽은 보지 않는다(UE와
    // 같다). 결과는 수학적 극값이라 Evaluate의 float 결과가 반올림 오차만큼 넘을 수 있고, 가중
    // 구간의 접힘(시간 Bezier가 되돌아가는 곳) 근처에서는 Evaluate의 3차 방정식 풀이(UE 식 —
    // 판별식이 1e-8 이하면 겹근으로 본다)가 정확한 곡선에서 조금 벗어나 그만큼 더 넘을 수
    // 있다(무작위 시험에서 폭의 약 1.3e-4). UE는 표본 재귀(FeaturePointMethod)라 결과가 다를 수
    // 있다(설계 15절 18). 실패: 키가 없으면 NotFound, 키 값·접선이 비유한이라 범위가 유한하지
    // 않으면 InvalidArgument.
    Result<Interval<float>> TryGetValueRange() const noexcept;

    // ---- 자동 접선 ----

    // Auto 키의 접선을 이웃 키로 다시 계산한다(UE FRichCurve::AutoSetTangents — 설계 6절). 가중치는
    // 건드리지 않는다.
    // - 첫 키: 키가 2개 이상이고 Auto면 출발 접선 0.
    // - 안쪽 키: Cubic이고 Auto면 math::ComputeCurveTangent(앞, 이 키, 뒤, tension)을 도착·출발
    //   접선 둘 다에 넣는다. 아니고 앞 키나 이 키가 Constant면(접선 방식과 무관) 출발 접선 0, 앞
    //   키가 Cubic이 아니면 도착 접선도 0.
    // - 마지막 키: Cubic이고 Auto면 도착 접선 0.
    // tension은 0(Catmull-Rom 꼴)–1(접선 0)이다. 할당하지 않는다.
    void AutoSetTangents(float tension = 0.0f) noexcept;

    // ---- 편집 ----

    // (time, value) 키를 더하고 그 index를 돌려준다(UE AddKey). 같은 시간 키가 있으면 그 키들 앞에
    // 넣는다. 새 키는 Linear·Auto·가중 없음·접선 0이다. unwindRotation이면 값을 도 단위 회전으로
    // 보고 바로 앞 키 값과의 차가 [−180, 180]에 들도록 360씩 옮긴다(UE와 같은 반복 뺄셈 — 2^20번
    // 안에 끝나지 않거나 360을 더하고 빼도 값이 그대로인 큰 값이면 double 나머지로 한 번에 옮기고,
    // 어느 한 값이 비유한이면 옮기지 않는다. UE 반복은 그런 값에서 끝나지 않는다). 실패: time이
    // 비유한이면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<usize> TryAddKey(float time, float value, bool unwindRotation = false) noexcept;

    // |키 시간 − time| ≤ tolerance인 가장 앞 키가 있으면 그 키 값만 바꾸고(unwind 없음 — UE와
    // 같다), 없으면 TryAddKey(time, value, unwindRotation)다. 바꾸거나 더한 키의 index를 돌려준다.
    // 실패: time이 비유한이거나 tolerance가 NaN·음수면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<usize> TryUpdateOrAddKey(float time, float value, bool unwindRotation = false,
                                    float tolerance = 1.0e-4f) noexcept;

    // index 키를 지운다. 다른 키의 접선은 다시 계산하지 않는다(D10).
    // 실패: index가 범위 밖이면 InvalidArgument.
    Result<void> TryRemoveKey(usize index) noexcept;

    // index 키의 시간을 바꾸고 새 index를 돌려준다. 키의 다른 칸은 그대로이고 같은 시간 키들 앞
    // 자리로 옮긴다. 다른 키의 접선은 바꾸지 않는다(UE SetKeyTime은 안의 DeleteKey로 재계산한다).
    // 할당하지 않는다. 실패: index가 범위 밖이거나 time이 비유한이면 InvalidArgument.
    Result<usize> TrySetKeyTime(usize index, float time) noexcept;

    // index 키의 칸 하나를 바꾼다(UE SetKeyValue·SetKeyInterpMode·SetKeyTangentMode·
    // SetKeyTangentWeightMode — 접선 재계산 없음). 실패: index가 범위 밖이면 InvalidArgument.
    Result<void> TrySetKeyValue(usize index, float value) noexcept;
    Result<void> TrySetKeyInterp(usize index, CurveInterp interp) noexcept;
    Result<void> TrySetKeyTangentMode(usize index, CurveTangentMode mode) noexcept;
    Result<void> TrySetKeyTangentWeight(usize index, CurveTangentWeight weight) noexcept;

    // index 키의 도착·출발 접선(기울기)을 바꾼다. 실패: index가 범위 밖이면 InvalidArgument.
    Result<void> TrySetKeyTangents(usize index, float arriveTangent, float leaveTangent) noexcept;

    // index 키의 도착·출발 가중치를 바꾼다(tangentWeight가 그 쪽을 켤 때만 평가에 쓰인다).
    // 실패: index가 범위 밖이면 InvalidArgument.
    Result<void> TrySetKeyTangentWeights(usize index, float arriveWeight,
                                         float leaveWeight) noexcept;

    // 키 전체를 keys로 바꾼다(UE SetKeys — 접선 재계산 없음). 공통 칸은 그대로다.
    // 실패: 시간이 비유한이거나 오름차순이 아니면 InvalidArgument(UE는 확인하지 않는다), 할당
    // 실패면 OutOfMemory.
    Result<void> TrySetKeys(Span<const TangentCurveKey> keys) noexcept;

    // 키만 비운다(기본값·외삽은 그대로 — UE Reset과 같다). 키 배열의 메모리는 남는다.
    void Reset() noexcept;

    // 모든 키 시간에 delta를 더한다(순서는 그대로다). 할당하지 않는다.
    // 실패: delta가 비유한이거나 결과 시간이 비유한이면 InvalidArgument.
    Result<void> TryShiftTime(float delta) noexcept;

    // 모든 키 시간을 origin 기준으로 factor배 한다: (time − origin)·factor + origin(UE ScaleCurve
    // 식). factor > 0만이라 순서는 그대로다. 할당하지 않는다.
    // 실패: origin·factor가 비유한이거나 factor ≤ 0, 결과 시간이 비유한이면 InvalidArgument.
    Result<void> TryScaleTime(float origin, float factor) noexcept;

    // 고른 키(indices)만 delta만큼 옮긴다(UE ShiftCurve(Delta, KeyHandles) — 편집기 선택 이동).
    // 고른 키를 빼고, 시간을 바꿔, 원래 index 오름차순으로 "같은 시간 키 앞" 규칙으로 다시 넣는다
    // (UE는 TSet 순회 순서라 결과 순서가 정해지지 않는다). 키의 다른 칸은 그대로다. indices가 비어
    // 있으면 아무것도 하지 않는다. 실패: index가 범위 밖이거나 중복이면, delta·결과 시간이
    // 비유한이면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<void> TryShiftKeyTimes(Span<const usize> indices, float delta) noexcept;

    // first + interval부터 last 미만까지 interval 간격(i번째 시간 = first + i·interval, i ≥ 1)으로
    // Evaluate한 값을 TryUpdateOrAddKey(허용오차 1e-4)로 넣는다(UE BakeCurve — 이름의 SampleRate는
    // 실제로 간격이다). 표본은 모두 원래 커브로 먼저 평가한다. 키가 없으면 아무것도 하지 않는다.
    // 실패: interval이 0 이하·비유한이거나 first·last가 비유한이면 InvalidArgument, 표본이 2^24개를
    // 넘으면 CapacityExceeded, 할당 실패면 OutOfMemory.
    Result<void> TryBake(float interval, float first, float last) noexcept;

    // 첫 키 시간부터 마지막 키 시간까지 TryBake(interval, 첫 키, 마지막 키)다.
    Result<void> TryBake(float interval) noexcept;

    // 첫 키와 마지막 키 사이에서 빼도 커브가 tolerance 안에서 같은 키를 지운다(UE
    // RemoveRedundantKeys — 설계 7.2절).
    // - sampleRate가 있으면 남기는 직전 키와 다음 키 사이를 그 프레임마다 평가해 "키를 뺀 값"과
    //   "있는 값"의 차가 tolerance를 넘으면 남긴다(User·Break 키도 지울 수 있다).
    // - 없으면 세 키(직전에 남긴 키·이 키·다음 키)가 모두 User·Break가 아닐 때만 이 키 시간에서
    //   비교한다.
    // 지운 뒤 AutoSetTangents를 부르지 않는다(D10 — UE와 결과를 맞추려면 호출자가 이어서 부른다).
    // 키가 3개 미만이면 아무것도 하지 않는다. 표본판의 표본 수는 키 간격 × 프레임 속도에 비례한다.
    // 실패: tolerance가 NaN·음수면 InvalidArgument, 할당 실패면 OutOfMemory.
    Result<void> TryRemoveRedundantKeys(float tolerance,
                                        Optional<FrameRate> sampleRate = {}) noexcept;

    // 범위판이다: 시간이 first 이하인 마지막 키부터 last 이상인 첫 키까지만 본다(그 둘과 바깥 키는
    // 남긴다). 비교 기준은 시작 키에서 시작한다(UE는 index 0 — 설계 15절 8). first ≥ last이거나
    // 그런 키가 없으면 아무것도 하지 않는다. 실패: first·last가 NaN이거나 tolerance가 NaN·음수면
    // InvalidArgument, 할당 실패면 OutOfMemory.
    Result<void> TryRemoveRedundantKeys(float tolerance, float first, float last,
                                        Optional<FrameRate> sampleRate = {}) noexcept;

    // 리플렉션 경로의 검증 hook이다(설계 9.1절): 키 시간이 모두 유한하고 오름차순이면 Success,
    // 아니면 InvalidArgument. 할당하지 않고 어느 스레드에서나 불린다.
    static Result<void> Validate(const TangentCurve& curve) noexcept;

private:
    // 시간 오름차순·유한(Validate). 도구·JSON 이름은 keys다.
    ROCKS_PROPERTY(Name = "keys") Array<TangentCurveKey> keys_;
};

} // namespace rocks
