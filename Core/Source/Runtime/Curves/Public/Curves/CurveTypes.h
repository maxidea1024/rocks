#pragma once

#include <Base/Types.h>
#include <Core/Math/KeyCurveMath.h>
#include <Core/Optional.h>
#include <Meta/Annotations.h>

// 스칼라 키 커브의 공통 칸과 키 형식이다(Docs/Design/CurveData/README.ko.md 2·4절 — CV5). UE 5.8
// ERichCurveInterpMode·ERichCurveTangentMode·ERichCurveTangentWeightMode·ERichCurveExtrapolation·
// FRichCurveKey·FSimpleCurveKey·FRealCurve 대응이다. 커브 자체는 TangentCurve.h·PlainCurve.h다.
//
// 시간과 값은 float다(UE와 같다 — 비트 대조의 전제, D1). 시간 단위는 커브를 쓰는 쪽이 정한다(대개
// 초). 값에도 단위가 없다 — 회전 성분을 담는 커브는 UE와 같이 도 단위다(13절).
namespace rocks {

// 키 사이 구간의 보간 방식이다(UE ERichCurveInterpMode와 같은 번호 — 구간은 앞 키의 방식을 쓴다).
// - Linear: 두 키 값을 직선으로 잇는다.
// - Constant: 다음 키 시간 직전까지 앞 키 값을 유지한다(계단).
// - Cubic: 앞 키의 출발 접선과 뒤 키의 도착 접선으로 3차 Bezier를 만든다(가중 접선이면 시간 축도
//   Bezier다 — TangentCurveKey의 tangentWeight).
// UE의 숨김 열거자 RCIM_None(3)은 두지 않는다(D4).
ROCKS_ENUM()
enum class CurveInterp : uint8 {
    Linear = 0,
    Constant = 1,
    Cubic = 2,
};

// Cubic 키의 접선을 누가 정하는지다(UE ERichCurveTangentMode와 같은 번호).
// - Auto: TangentCurve::AutoSetTangents가 이웃 키로 계산한다(도착·출발 접선이 같다).
// - User: 사용자가 정한 접선이고 도착·출발 접선이 같게 편집된다(값 타입은 같음을 강제하지 않는다).
// - Break: 도착·출발 접선을 따로 정한다.
// UE의 RCTM_None(3, 숨김)과 RCTM_SmartAuto는 두지 않는다(D4·D5).
ROCKS_ENUM()
enum class CurveTangentMode : uint8 {
    Auto = 0,
    User = 1,
    Break = 2,
};

// 가중 접선을 쓰는 쪽이다(UE ERichCurveTangentWeightMode와 같은 번호). 가중이면 그 쪽 가중치
// (arriveTangentWeight·leaveTangentWeight — 시간·값 평면에서 키부터 Bezier 제어점까지의 거리)를
// 쓰고, 아니면 가중치는 무시되고 시간 차의 1/3에서 계산된다.
ROCKS_ENUM()
enum class CurveTangentWeight : uint8 {
    None = 0,
    Arrive = 1,
    Leave = 2,
    Both = 3,
};

// 키 범위 밖의 값이다(UE ERichCurveExtrapolation과 같은 번호). 앞(첫 키 이전)·뒤(마지막 키 이후)를
// 따로 고른다.
// - Cycle: 키 범위를 반복한다.
// - CycleWithOffset: 반복하면서 주기마다 (마지막 키 값 − 첫 키 값)을 더한다(앞쪽은 뺀다).
// - Oscillate: 홀수 번째 주기는 시간을 뒤집어 왕복한다.
// - Linear: 끝의 두 키 값으로 정한 기울기로 잇는다(키 접선은 쓰지 않는다 — UE와 같다).
// - Constant: 끝 키 값을 유지한다.
// UE의 RCCE_None(5 — 동작은 Cycle)은 두지 않는다(D4).
ROCKS_ENUM()
enum class CurveExtrapolation : uint8 {
    Cycle = 0,
    CycleWithOffset = 1,
    Oscillate = 2,
    Linear = 3,
    Constant = 4,
};

// PlainCurve 전체의 보간 방식이다(UE FSimpleCurve::InterpMode의 허용 값). Cubic을 표현할 수 없게
// CurveInterp와 따로 둔다(D7 — 리플렉션 decode가 미선언 값 2를 거부한다).
ROCKS_ENUM()
enum class PlainCurveInterp : uint8 {
    Linear = 0,
    Constant = 1,
};

// TangentCurve의 키다(UE FRichCurveKey). 키 하나가 시간·값과 그 키에서 시작하는 구간의 보간 방식,
// 접선과 가중치를 갖는다. 칸이 서로 맞물려 있어(접선은 보간 방식, 가중치는 가중 방식에 따라 뜻이
// 바뀐다) 통째 단위(Atomic)다.
// - arriveTangent·leaveTangent: 도착·출발 접선(시간당 값 변화 — 기울기).
// - arriveTangentWeight·leaveTangentWeight: 가중 접선의 가중치(tangentWeight가 그 쪽을 켤 때만
//   쓴다).
// 기본값은 UE FRichCurveKey 기본 생성(Linear·Auto·가중 없음·접선 0)과 같다. 키 형식은
// math::CurveSegmentKey(KeyCurveMath.h)이므로 평가 핵심 식에 변환 없이 들어간다.
ROCKS_STRUCT(Atomic)
struct TangentCurveKey {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() float value = 0.0f;
    ROCKS_PROPERTY() CurveInterp interp = CurveInterp::Linear;
    ROCKS_PROPERTY() CurveTangentMode tangentMode = CurveTangentMode::Auto;
    ROCKS_PROPERTY() CurveTangentWeight tangentWeight = CurveTangentWeight::None;
    ROCKS_PROPERTY() float arriveTangent = 0.0f;
    ROCKS_PROPERTY() float arriveTangentWeight = 0.0f;
    ROCKS_PROPERTY() float leaveTangent = 0.0f;
    ROCKS_PROPERTY() float leaveTangentWeight = 0.0f;
};

// PlainCurve의 키다(UE FSimpleCurveKey): 시간과 값.
ROCKS_STRUCT(Atomic)
struct PlainCurveKey {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float time = 0.0f;
    ROCKS_PROPERTY() float value = 0.0f;
};

// 스칼라 커브 공통 칸이다(UE FRealCurve의 DefaultValue·PreInfinityExtrap·PostInfinityExtrap).
// TangentCurve·PlainCurve의 리플렉션 base다(필드 ID는 이 struct가 소유한다).
// - defaultValue: 키가 없을 때 Evaluate가 내는 값이다. 없으면 Evaluate의 fallback 인자다(UE
//   DefaultValue = MAX_flt "없음" 표지 대신 Optional — D6). 키가 하나라도 있으면 쓰지 않는다.
// - preExtrapolation·postExtrapolation: 첫 키 이전·마지막 키 이후의 값(CurveExtrapolation).
// 이 struct만으로는 평가하지 않는다(키가 없다). 다형 저장의 평가 규칙은 설계 14절이다.
ROCKS_STRUCT()
struct ScalarCurveBase {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Optional<float> defaultValue;
    ROCKS_PROPERTY() CurveExtrapolation preExtrapolation = CurveExtrapolation::Constant;
    ROCKS_PROPERTY() CurveExtrapolation postExtrapolation = CurveExtrapolation::Constant;
};

static_assert(math::CurveSegmentKey<TangentCurveKey>,
              "TangentCurveKey must satisfy the key curve math key concept");
static_assert(static_cast<uint8>(CurveInterp::Linear) == math::CurveInterpLinear &&
                  static_cast<uint8>(CurveInterp::Constant) == math::CurveInterpConstant &&
                  static_cast<uint8>(CurveInterp::Cubic) == math::CurveInterpCubic,
              "CurveInterp values must match the key curve math interpolation codes");
static_assert(static_cast<uint8>(CurveTangentWeight::None) == math::CurveWeightNone &&
                  static_cast<uint8>(CurveTangentWeight::Arrive) == math::CurveWeightArrive &&
                  static_cast<uint8>(CurveTangentWeight::Leave) == math::CurveWeightLeave &&
                  static_cast<uint8>(CurveTangentWeight::Both) == math::CurveWeightBoth,
              "CurveTangentWeight values must match the key curve math weight codes");

} // namespace rocks
