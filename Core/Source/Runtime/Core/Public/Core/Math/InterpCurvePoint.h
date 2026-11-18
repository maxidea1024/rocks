#pragma once

#include <Base/TypeTraits.h>
#include <Core/Math/Color.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector4.h>
#include <Platform/PlatformMath.h>

namespace rocks {

// 키에서 다음 키까지 구간의 보간 방식이다(UE 5.8 EInterpCurveMode 대응, 번호 순서도 같다).
// 모드는 구간을 시작하는 키에 붙는다. 마지막 키(루프 아님)의 모드는 구간을 만들지 않지만
// AutoSetTangents가 그 키의 접선을 정하는 데 쓴다.
enum class InterpCurveMode : uint8 {
    // 두 키 값 사이의 직선이다. 접선은 평가에 쓰이지 않는다.
    Linear = 0,
    // 3차 Hermite 곡선이다. AutoSetTangents가 이웃 키로 접선을 다시 계산한다(clamp 없음).
    CurveAuto = 1,
    // 다음 키까지 이 키 값을 유지하고 다음 키에서 바로 바뀐다.
    Constant = 2,
    // 3차 Hermite 곡선이다. 접선은 호출자가 정하며 AutoSetTangents가 바꾸지 않는다.
    CurveUser = 3,
    // CurveUser와 같고 도착·출발 접선이 달라도 된다(키에서 꺾임을 허용한다).
    CurveBreak = 4,
    // CurveAuto와 같고 자동 접선을 clamp해 단조 구간에서 키 값을 넘어가지 않게 한다.
    CurveAutoClamped = 5,
};

// 곡선(Hermite·SQUAD) 모드인지다. UE FInterpCurvePoint::IsCurveKey의 모드 판정과 같다.
constexpr bool IsCurveMode(InterpCurveMode mode) noexcept {
    return mode == InterpCurveMode::CurveAuto || mode == InterpCurveMode::CurveAutoClamped ||
           mode == InterpCurveMode::CurveUser || mode == InterpCurveMode::CurveBreak;
}

// 커브 값 형식별 성질이다. 지원 형식: float, double, Vector2/3/4<float|double>,
// Quaternion<float|double>, LinearColor. 그 밖의 형식은 특수화가 없어 InterpCurve가 컴파일되지
// 않는다.
//   Scalar          성분(값·접선)의 정밀도(float 계열 float, double 계열 double). 입력 키·구간
//   길이·
//                   보간 비율은 형식과 무관하게 float다(UE와 같다 — InterpCurvePoint::inValue).
//   ComponentCount  성분 수. 회전이 아닌 형식의 보간·접선 계산은 모두 성분별 scalar 식이다.
//   IsRotation      true면 quaternion 회전 보간(SQUAD)을 쓰고 접선 칸에 제어점을 담는다.
//   HasDistance     true면 성분 차의 제곱합을 거리 제곱으로 쓰는 최근접 탐색을 제공한다.
//   DividesByReciprocal  true면 float 입력 길이로 나눌 때 역수를 한 번 구해 곱한다(UE TVector·
//                   TVector2·TVector4·FLinearColor의 operator/ 와 /=). false(scalar)는 정확한
//                   나눗셈.
//   ClampsAutoTangents   true면 CurveAutoClamped 키의 자동 접선을 성분별로 clamp한다. false면
//                   CurveAuto와 같게 계산한다(UE 일반 ComputeCurveTangent — FLinearColor·FVector4).
//   HasBounds       true면 InterpCurve::TryCalcBounds(성분별 최소·최대 — UE CalcBounds)를 제공한다.
//                   UE가 구간 범위 함수를 둔 형식(float·FVector2D·FVector·FLinearColor)과 그 정밀도
//                   짝이다. quaternion·Vector4는 false다(UE 일반 형식은 아무것도 하지 않는다).
//   DefaultValue()  빈 커브 평가의 기본 fallback(0, quaternion은 항등)이다.
//   DefaultTangent  새 키의 접선 기본값(0). quaternion은 키 자신(제어점 = 키, 정지)이다.
//   NaNValue()      모든 성분이 quiet NaN인 값(NaN 입력 평가 결과)이다.
//   Store·Load      성분 배열과 값 사이의 변환이다(메모리 배치에 기대지 않는다).
template <typename T>
struct CurveValueTraits;

template <typename S>
    requires math::IsFloatingPointV<S>
struct CurveValueTraits<S> {
    using Scalar = S;
    static constexpr usize ComponentCount = 1;
    static constexpr bool IsRotation = false;
    static constexpr bool HasDistance = true;
    static constexpr bool DividesByReciprocal = false;
    static constexpr bool ClampsAutoTangents = true;
    static constexpr bool HasBounds = true;

    static constexpr S DefaultValue() noexcept {
        return static_cast<S>(0);
    }

    static constexpr S DefaultTangent(const S&) noexcept {
        return static_cast<S>(0);
    }

    static constexpr S NaNValue() noexcept {
        return platform::PlatformMath::QuietNaN<S>();
    }

    static constexpr void Store(const S& value, S (&components)[ComponentCount]) noexcept {
        components[0] = value;
    }

    static constexpr S Load(const S (&components)[ComponentCount]) noexcept {
        return components[0];
    }
};

template <typename S>
struct CurveValueTraits<Vector2<S>> {
    using Scalar = S;
    static constexpr usize ComponentCount = 2;
    static constexpr bool IsRotation = false;
    static constexpr bool HasDistance = true;
    static constexpr bool DividesByReciprocal = true;
    static constexpr bool ClampsAutoTangents = true;
    static constexpr bool HasBounds = true;

    static constexpr Vector2<S> DefaultValue() noexcept {
        return Vector2<S>();
    }

    static constexpr Vector2<S> DefaultTangent(const Vector2<S>&) noexcept {
        return Vector2<S>();
    }

    static constexpr Vector2<S> NaNValue() noexcept {
        return Vector2<S>::Splat(platform::PlatformMath::QuietNaN<S>());
    }

    static constexpr void Store(const Vector2<S>& value, S (&components)[ComponentCount]) noexcept {
        components[0] = value.x;
        components[1] = value.y;
    }

    static constexpr Vector2<S> Load(const S (&components)[ComponentCount]) noexcept {
        return Vector2<S>(components[0], components[1]);
    }
};

template <typename S>
struct CurveValueTraits<Vector3<S>> {
    using Scalar = S;
    static constexpr usize ComponentCount = 3;
    static constexpr bool IsRotation = false;
    static constexpr bool HasDistance = true;
    static constexpr bool DividesByReciprocal = true;
    static constexpr bool ClampsAutoTangents = true;
    static constexpr bool HasBounds = true;

    static constexpr Vector3<S> DefaultValue() noexcept {
        return Vector3<S>();
    }

    static constexpr Vector3<S> DefaultTangent(const Vector3<S>&) noexcept {
        return Vector3<S>();
    }

    static constexpr Vector3<S> NaNValue() noexcept {
        return Vector3<S>::Splat(platform::PlatformMath::QuietNaN<S>());
    }

    static constexpr void Store(const Vector3<S>& value, S (&components)[ComponentCount]) noexcept {
        components[0] = value.x;
        components[1] = value.y;
        components[2] = value.z;
    }

    static constexpr Vector3<S> Load(const S (&components)[ComponentCount]) noexcept {
        return Vector3<S>(components[0], components[1], components[2]);
    }
};

template <typename S>
struct CurveValueTraits<Vector4<S>> {
    using Scalar = S;
    static constexpr usize ComponentCount = 4;
    static constexpr bool IsRotation = false;
    static constexpr bool HasDistance = true;
    static constexpr bool DividesByReciprocal = true;
    static constexpr bool ClampsAutoTangents = false;
    static constexpr bool HasBounds = false;

    static constexpr Vector4<S> DefaultValue() noexcept {
        return Vector4<S>();
    }

    static constexpr Vector4<S> DefaultTangent(const Vector4<S>&) noexcept {
        return Vector4<S>();
    }

    static constexpr Vector4<S> NaNValue() noexcept {
        return Vector4<S>::Splat(platform::PlatformMath::QuietNaN<S>());
    }

    static constexpr void Store(const Vector4<S>& value, S (&components)[ComponentCount]) noexcept {
        components[0] = value.x;
        components[1] = value.y;
        components[2] = value.z;
        components[3] = value.w;
    }

    static constexpr Vector4<S> Load(const S (&components)[ComponentCount]) noexcept {
        return Vector4<S>(components[0], components[1], components[2], components[3]);
    }
};

// 색은 RGBA 네 성분을 독립적으로 보간한다(선형 RGB·straight alpha 그대로). 거리는 정의하지 않는다
// (UE FLinearColor 커브에도 최근접 탐색이 없다).
template <>
struct CurveValueTraits<LinearColor> {
    using Scalar = float;
    static constexpr usize ComponentCount = 4;
    static constexpr bool IsRotation = false;
    static constexpr bool HasDistance = false;
    static constexpr bool DividesByReciprocal = true;
    static constexpr bool ClampsAutoTangents = false;
    static constexpr bool HasBounds = true;

    static constexpr LinearColor DefaultValue() noexcept {
        return LinearColor();
    }

    static constexpr LinearColor DefaultTangent(const LinearColor&) noexcept {
        return LinearColor();
    }

    static constexpr LinearColor NaNValue() noexcept {
        const float nan = platform::PlatformMath::QuietNaN<float>();
        return LinearColor(nan, nan, nan, nan);
    }

    static constexpr void Store(const LinearColor& value,
                                float (&components)[ComponentCount]) noexcept {
        components[0] = value.r;
        components[1] = value.g;
        components[2] = value.b;
        components[3] = value.a;
    }

    static constexpr LinearColor Load(const float (&components)[ComponentCount]) noexcept {
        return LinearColor(components[0], components[1], components[2], components[3]);
    }
};

// 회전 커브다. 키 값은 단위 quaternion이어야 하며 검사하지 않는다. 접선 칸에는 SQUAD 제어점을
// 담고(UE와 같은 저장 규약) 제어점도 단위 quaternion이어야 한다. 미분·최근접 탐색은 없다
// (Docs/Implementation/InterpCurve.ko.md 4절).
template <typename S>
struct CurveValueTraits<Quaternion<S>> {
    using Scalar = S;
    static constexpr usize ComponentCount = 4;
    static constexpr bool IsRotation = true;
    static constexpr bool HasDistance = false;
    static constexpr bool DividesByReciprocal = false;
    static constexpr bool ClampsAutoTangents = false;
    static constexpr bool HasBounds = false;

    static constexpr Quaternion<S> DefaultValue() noexcept {
        return Quaternion<S>::Identity();
    }

    static constexpr Quaternion<S> DefaultTangent(const Quaternion<S>& value) noexcept {
        return value;
    }

    static constexpr Quaternion<S> NaNValue() noexcept {
        const S nan = platform::PlatformMath::QuietNaN<S>();
        return Quaternion<S>(nan, nan, nan, nan);
    }

    static constexpr void Store(const Quaternion<S>& value,
                                S (&components)[ComponentCount]) noexcept {
        components[0] = value.x;
        components[1] = value.y;
        components[2] = value.z;
        components[3] = value.w;
    }

    static constexpr Quaternion<S> Load(const S (&components)[ComponentCount]) noexcept {
        return Quaternion<S>(components[0], components[1], components[2], components[3]);
    }
};

// CurveValueTraits 특수화가 있는 형식인지다.
template <typename T>
concept InterpCurveValue = requires { typename CurveValueTraits<T>::Scalar; };

// 커브 키 하나다(UE 5.8 FInterpCurvePoint 대응). 내부 상태가 없는 일반 값이다.
//   inValue        이 키의 입력값(보통 시간). 모든 값 형식에서 float다(UE와 같다). 커브 안에서는
//                  InterpCurve가 정렬을 관리한다.
//   outValue       inValue에서의 출력값.
//   arriveTangent  이 키로 들어오는 구간 끝의 접선(입력 1단위당 값 변화).
//   leaveTangent   이 키에서 나가는 구간 시작의 접선.
//   mode           이 키에서 다음 키까지 구간의 보간 방식.
// quaternion 커브의 두 접선 칸은 접선이 아니라 SQUAD 제어점(단위 quaternion)이다.
//
// 사용 예:
//   InterpCurvePoint<float> key(1.0f, 5.0f);            // Linear, 접선 0
//   InterpCurvePoint<Vec3> curved(0.5f, Vec3(1, 0, 0), Vec3(), Vec3(), InterpCurveMode::CurveUser);
template <typename T>
struct InterpCurvePoint {
    static_assert(InterpCurveValue<T>, "InterpCurvePoint supports float, double, Vector2/3/4, "
                                       "Quaternion and LinearColor values");

    using Traits = CurveValueTraits<T>;
    using Scalar = typename Traits::Scalar;

    float inValue = 0.0f;
    T outValue = Traits::DefaultValue();
    T arriveTangent = Traits::DefaultTangent(Traits::DefaultValue());
    T leaveTangent = Traits::DefaultTangent(Traits::DefaultValue());
    InterpCurveMode mode = InterpCurveMode::Linear;

    constexpr InterpCurvePoint() noexcept = default;

    // Linear 키다. 접선은 0(quaternion은 키 자신)이다(UE 생성자의 memset 0과 같다. quaternion만
    // 다름).
    constexpr InterpCurvePoint(float input, const T& output) noexcept
        : inValue(input), outValue(output), arriveTangent(Traits::DefaultTangent(output)),
          leaveTangent(Traits::DefaultTangent(output)) {
    }

    constexpr InterpCurvePoint(float input, const T& output, const T& arrive, const T& leave,
                               InterpCurveMode interpMode) noexcept
        : inValue(input), outValue(output), arriveTangent(arrive), leaveTangent(leave),
          mode(interpMode) {
    }

    // 정확한 필드 비교다. NaN 성분이 있으면 자기 자신과도 같지 않다.
    friend constexpr bool operator==(const InterpCurvePoint&,
                                     const InterpCurvePoint&) noexcept = default;

    constexpr bool IsCurveKey() const noexcept {
        return IsCurveMode(mode);
    }
};

static_assert(IsTriviallyCopyableV<InterpCurvePoint<float>> &&
              IsTriviallyCopyableV<InterpCurvePoint<Quatd>>);

} // namespace rocks
