#pragma once

#include <Core/Math/Quaternion.h>

// 키 커브(InterpCurve)가 쓰는 보간 식이다. 성분별 scalar 식은 UE 5.8 식과 같은 항 순서·같은 정밀도
// 조합(보간 비율은 float, 성분은 값 형식의 정밀도)으로 계산해 비트 단위로 같은 결과를 낸다(빌드가
// FMA 축약을 끄므로 -ffp-contract=off). 커브 밖에서도 쓸 수 있는 공개 함수이며 상태가 없고 스레드
// 제약이 없다.
namespace rocks::math {

// 3차 Hermite 값이다. p0·p1은 구간 끝 값, t0·t1은 구간 길이를 곱한 끝 접선, alpha ∈ [0, 1]이다.
// UE FMath::CubicInterp의 일반 템플릿과 같은 식·항 순서다:
//   (2a³ − 3a² + 1)·p0 + (a³ − 2a² + a)·t0 + (a³ − a²)·t1 + (−2a³ + 3a²)·p1 (왼쪽부터 합산)
// 기저 계수는 alpha의 형식 A로 계산한 뒤 S로 넓힌다(UE CubicInterp<T, U>에서 U = float, T =
// double인 조합과 같다). 기존 math::CubicInterp는 p1 항을 t1 항보다 먼저 더해 반올림이 다를 수
// 있다. alpha = 0이면 p0, alpha = 1이면 p1이다(유한한 입력에서 정확). NaN은 전파된다.
//
// 사용 예:
//   const float value = math::CurveHermite(0.0f, 1.0f, 2.0f, 1.0f, 0.5f);
template <typename S, typename A>
    requires(IsFloatingPointV<S> && IsFloatingPointV<A>)
constexpr S CurveHermite(S p0, S t0, S p1, S t1, A alpha) noexcept {
    const A a2 = alpha * alpha;
    const A a3 = a2 * alpha;
    const A h00 = static_cast<A>(2) * a3 - static_cast<A>(3) * a2 + static_cast<A>(1);
    const A h10 = a3 - static_cast<A>(2) * a2 + alpha;
    const A h11 = a3 - a2;
    const A h01 = static_cast<A>(-2) * a3 + static_cast<A>(3) * a2;
    return static_cast<S>(h00) * p0 + static_cast<S>(h10) * t0 + static_cast<S>(h11) * t1 +
           static_cast<S>(h01) * p1;
}

// CurveHermite의 alpha에 대한 1차 미분이다(UE FMath::CubicInterpDerivative와 같은 식):
//   a = 6p0 + 3t0 + 3t1 − 6p1, b = −6p0 − 4t0 − 2t1 + 6p1, 결과 a·α² + b·α + t0
// a·b는 S, α²는 A로 계산한다. 입력 단위 미분은 결과를 구간 길이로 나눈다.
template <typename S, typename A>
    requires(IsFloatingPointV<S> && IsFloatingPointV<A>)
constexpr S CurveHermiteDerivative(S p0, S t0, S p1, S t1, A alpha) noexcept {
    const S a = static_cast<S>(6) * p0 + static_cast<S>(3) * t0 + static_cast<S>(3) * t1 -
                static_cast<S>(6) * p1;
    const S b = static_cast<S>(-6) * p0 - static_cast<S>(4) * t0 - static_cast<S>(2) * t1 +
                static_cast<S>(6) * p1;
    const S c = t0;
    const A alpha2 = alpha * alpha;
    return a * static_cast<S>(alpha2) + b * static_cast<S>(alpha) + c;
}

// CurveHermite의 alpha에 대한 2차 미분이다(UE FMath::CubicInterpSecondDerivative와 같은 식):
//   a = 12p0 + 6t0 + 6t1 − 12p1, b = −6p0 − 4t0 − 2t1 + 6p1, 결과 a·α + b
// 입력 단위 미분은 결과를 구간 길이의 제곱으로 나눈다.
template <typename S, typename A>
    requires(IsFloatingPointV<S> && IsFloatingPointV<A>)
constexpr S CurveHermiteSecondDerivative(S p0, S t0, S p1, S t1, A alpha) noexcept {
    const S a = static_cast<S>(12) * p0 + static_cast<S>(6) * t0 + static_cast<S>(6) * t1 -
                static_cast<S>(12) * p1;
    const S b = static_cast<S>(-6) * p0 - static_cast<S>(4) * t0 - static_cast<S>(2) * t1 +
                static_cast<S>(6) * p1;
    return a * static_cast<S>(alpha) + b;
}

// 자동 접선 계산의 시간 차 하한이다. 같은 입력값 키(시간 차 0)에서도 나눗셈이 유한하도록 UE
// UE_KINDA_SMALL_NUMBER(1e-4)로 막는다. 따라서 키 간격이 1e-4보다 작으면 자동 접선이 작아진다.
template <typename K>
    requires IsFloatingPointV<K>
constexpr K CurveTangentTimeDifference(K earlierTime, K laterTime) noexcept {
    return Max(KindaSmallNumber<K>, laterTime - earlierTime);
}

namespace detail {

// rising이면 UE FMath::Min(current, limit), 아니면 FMath::Max(current, limit)다.
template <typename S>
constexpr S CurveSelect(bool rising, S current, S limit) noexcept {
    if (rising) {
        return (current <= limit) ? current : limit;
    }

    return (current >= limit) ? current : limit;
}

} // namespace detail

// CurveAutoClamped 키의 한 성분 접선이다(UE ClampFloatTangent와 같은 식). 값은 S, 시간은
// K(커브에서는 float)이며 S = K = float이면 UE와 비트가 같다. 이웃 두 키가 같은 쪽에
// 있으면(극값·평탄) 0이다. 단조 구간이면 이웃 키를 잇는 기울기에서 시작해, 현재 키 높이가 양
// 끝 33.3% 안쪽에 있으면 가까운 쪽 구간 기울기로 섞어 줄여 곡선이 키 값을 넘어가지 않게 한다.
// tension은 적용하지 않는다(호출자가 (1 − tension)을 곱한다).
template <typename S, typename K>
    requires(IsFloatingPointV<S> && IsFloatingPointV<K>)
constexpr S ClampCurveTangent(S previousValue, K previousTime, S currentValue, K currentTime,
                              S nextValue, K nextTime) noexcept {
    const K previousToNextTime = CurveTangentTimeDifference(previousTime, nextTime);
    const K previousToCurrentTime = CurveTangentTimeDifference(previousTime, currentTime);
    const K currentToNextTime = CurveTangentTimeDifference(currentTime, nextTime);

    const S previousToNextHeight = nextValue - previousValue;
    const S previousToCurrentHeight = currentValue - previousValue;
    const S currentToNextHeight = nextValue - currentValue;

    const S zero = static_cast<S>(0);
    const bool crest = (previousToCurrentHeight >= zero && currentToNextHeight <= zero) ||
                       (previousToCurrentHeight <= zero && currentToNextHeight >= zero);
    if (crest) {
        return zero;
    }

    const S threshold = static_cast<S>(0.333);
    const S currentToNextTangent = currentToNextHeight / static_cast<S>(currentToNextTime);
    const S previousToCurrentTangent =
        previousToCurrentHeight / static_cast<S>(previousToCurrentTime);
    const S previousToNextTangent = previousToNextHeight / static_cast<S>(previousToNextTime);
    const S lowerThreshold = threshold;
    const S upperThreshold = static_cast<S>(1) - threshold;
    const S heightAlpha = previousToCurrentHeight / previousToNextHeight;

    // UE FMath::Lerp(A, B, Alpha) = A + Alpha·(B − A)와 FMath::Min/Max((A <= B) ? A : B 등)를
    // 그대로 쓴다. math::Min/Max는 같은 값에서 두 번째 인자를 골라 ±0 부호가 UE와 달라질 수 있다.
    const bool rising = previousToNextHeight > zero;
    S clamped = previousToNextTangent;
    if (heightAlpha < lowerThreshold) {
        const S clampAlpha = static_cast<S>(1) - heightAlpha / threshold;
        const S lowerClamp =
            previousToNextTangent + clampAlpha * (previousToCurrentTangent - previousToNextTangent);
        clamped = detail::CurveSelect(rising, clamped, lowerClamp);
    }

    if (heightAlpha > upperThreshold) {
        const S clampAlpha = (heightAlpha - upperThreshold) / threshold;
        const S upperClamp =
            previousToNextTangent + clampAlpha * (currentToNextTangent - previousToNextTangent);
        clamped = detail::CurveSelect(rising, clamped, upperClamp);
    }

    return clamped;
}

} // namespace rocks::math

namespace rocks {

// 단위 quaternion의 로그다. q = (v·sinθ, cosθ)이면 (v·θ, 0)이다(θ ∈ [0, π]). θ는
// atan2(|v|, w)로 구해 작은 각에서도 정확하다. 회전이 없으면(|v| = 0) (0, 0, 0, 0)이다. q = −1(θ =
// π, 축이 정해지지 않음)도 (0, 0, 0, 0)이므로 SQUAD 계산은 같은 반구로 맞춘 입력(w ≥ 0)만 넘긴다.
// 단위 quaternion 전제이며 검사하지 않는다. UE TQuat::Log와 같은 결과를 더 안정적인 식으로 낸다.
template <typename S>
Quaternion<S> QuaternionLog(const Quaternion<S>& unit) noexcept {
    const S vectorLength = Vector3<S>(unit.x, unit.y, unit.z).Length();
    if (!(vectorLength > static_cast<S>(0))) {
        return Quaternion<S>(static_cast<S>(0), static_cast<S>(0), static_cast<S>(0),
                             static_cast<S>(0));
    }

    const S angle = math::Atan2(vectorLength, unit.w);
    const S scale = angle / vectorLength;
    return Quaternion<S>(unit.x * scale, unit.y * scale, unit.z * scale, static_cast<S>(0));
}

// 순허수 quaternion (u, 0)의 지수다: (u·sin|u|/|u|, cos|u|). w 성분은 무시한다. |u| = 0이면
// 항등이다. QuaternionLog의 역함수이며 결과는 단위 quaternion이다(UE TQuat::Exp 대응).
template <typename S>
Quaternion<S> QuaternionExp(const Quaternion<S>& pure) noexcept {
    const S angle = Vector3<S>(pure.x, pure.y, pure.z).Length();
    if (!(angle > static_cast<S>(0))) {
        return Quaternion<S>::Identity();
    }

    const S scale = math::Sin(angle) / angle;
    return Quaternion<S>(pure.x * scale, pure.y * scale, pure.z * scale, math::Cos(angle));
}

namespace detail {

// 반구를 맞추지 않는 구면 보간이다(SQUAD 안쪽용). 두 4차원 단위 벡터 사이 각 θ를
// 2·atan2(|a − b|, |a + b|)로 구해 작은 각에서도 정확하고, 가중치 sin((1 − t)θ)/sinθ·sin(tθ)/sinθ는
// 작은 θ에서도 sin이 정확하므로 그대로 쓴다. sinθ가 machine epsilon 이하(θ ≈ 0 또는 π)면 선형
// 가중치를 쓴다. 결과는 정규화하지 않는다.
template <typename S>
Quaternion<S> SlerpFullPathUnnormalized(const Quaternion<S>& a, const Quaternion<S>& b,
                                        S t) noexcept {
    const Quaternion<S> difference(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
    const Quaternion<S> sum(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
    const S angle = static_cast<S>(2) * math::Atan2(difference.Length(), sum.Length());
    const S sine = math::Sin(angle);
    S weightA = static_cast<S>(1) - t;
    S weightB = t;
    if (sine > math::MachineEpsilon<S>) {
        weightA = math::Sin(weightA * angle) / sine;
        weightB = math::Sin(weightB * angle) / sine;
    }

    return Quaternion<S>(a.x * weightA + b.x * weightB, a.y * weightA + b.y * weightB,
                         a.z * weightA + b.z * weightB, a.w * weightA + b.w * weightB);
}

// 정규화한다. 비유한 성분은 그대로 남기고, 유한한데 0이면 fallback을 반환한다.
template <typename S>
Quaternion<S> NormalizedOr(const Quaternion<S>& value, const Quaternion<S>& fallback) noexcept {
    auto normalized = value.TryNormalize();
    if (normalized) {
        return normalized.Value();
    }

    return value.IsFinite() ? fallback : value;
}

} // namespace detail

// 반구를 맞추지 않는 구면 보간이다. t = 0이면 a, t = 1이면 b(정확), 결과는 정규화한다. a와 b가
// 정확히 반대에 가까우면 경로가 정해지지 않아 선형 가중치의 결과를 정규화한다(0이 되면 a). 단위
// quaternion 전제다. UE TQuat::SlerpFullPath 대응. UE·Rocks Slerp는 내적이 0.9999·0.9995를 넘으면
// 정규화 선형 보간으로 바꾸지만 여기서는 SQUAD 정밀도를 위해 작은 각에서도 구면 보간을 유지한다.
template <typename S>
Quaternion<S> SlerpFullPath(const Quaternion<S>& a, const Quaternion<S>& b, S t) noexcept {
    if (t == static_cast<S>(0)) {
        return a;
    }

    if (t == static_cast<S>(1)) {
        return b;
    }

    return detail::NormalizedOr(detail::SlerpFullPathUnnormalized(a, b, t), a);
}

// 최단 경로 구면 보간이다(UE TQuat::Slerp = Slerp_NotNormalized + GetNormalized와 같은 식). 내적의
// 부호로 b를 같은 반구로 맞추고, |내적| < 0.9999이면 sin 가중치, 그 밖은 선형 가중치를 쓴 뒤
// 정규화한다. 커브의 Linear quaternion 구간이 쓴다. Rocks Slerp(전환 기준 0.9995, 끝점 특별 처리)와
// 반올림이 다를 수 있다. 정규화할 수 없으면(제곱 길이 < 1e-8 — UE GetNormalized의 기준) 항등을
// 반환한다(UE와 같다). 단위 quaternion 전제다.
template <typename S>
Quaternion<S> CurveSlerp(const Quaternion<S>& a, const Quaternion<S>& b, S t) noexcept {
    S rawCosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const S sign = rawCosine >= static_cast<S>(0) ? static_cast<S>(1) : static_cast<S>(-1);
    rawCosine *= sign;
    S scaleA = static_cast<S>(1) - t;
    S scaleB = t * sign;
    if (rawCosine < static_cast<S>(0.9999)) {
        const S omega = math::Acos(rawCosine);
        const S inverseSine = static_cast<S>(1) / math::Sin(omega);
        scaleA = math::Sin(scaleA * omega) * inverseSine;
        scaleB = math::Sin(scaleB * omega) * inverseSine;
    }

    const Quaternion<S> blended(scaleA * a.x + scaleB * b.x, scaleA * a.y + scaleB * b.y,
                                scaleA * a.z + scaleB * b.z, scaleA * a.w + scaleB * b.w);
    const S lengthSquared = blended.x * blended.x + blended.y * blended.y + blended.z * blended.z +
                            blended.w * blended.w;
    if (!(lengthSquared >= math::SmallNumber<S>)) {
        return blended.IsFinite() ? Quaternion<S>::Identity() : blended;
    }

    const S inverseLength = math::InvSqrt(lengthSquared);
    return Quaternion<S>(blended.x * inverseLength, blended.y * inverseLength,
                         blended.z * inverseLength, blended.w * inverseLength);
}

// 키 current의 SQUAD 제어점이다(Shoemake):
//   s = current · exp(−(1 − tension) · (log(current⁻¹·previous) + log(current⁻¹·next)) / 4)
// previous·next는 current와 같은 반구로 맞춘 뒤 쓴다. tension = 0은 표준 SQUAD, tension = 1은 s =
// current(키에서 각속도 0)다. 끝 키처럼 이웃이 자기 자신이면 그쪽 로그가 0이다. 키 간격(시간)은
// 반영하지 않는다. 단위 quaternion 전제이며 결과도 단위 quaternion이다.
// UE TQuat::CalcTangents는 계수 −1/2를 쓰고 tension을
// 무시한다(Docs/Implementation/InterpCurve.ko.md).
//
// 사용 예:
//   const Quat control = SquadControlPoint(keys[i - 1], keys[i], keys[i + 1], 0.0f);
template <typename S>
Quaternion<S> SquadControlPoint(const Quaternion<S>& previous, const Quaternion<S>& current,
                                const Quaternion<S>& next, S tension) noexcept {
    const Quaternion<S> alignedPrevious =
        Dot(current, previous) < static_cast<S>(0)
            ? Quaternion<S>(-previous.x, -previous.y, -previous.z, -previous.w)
            : previous;
    const Quaternion<S> alignedNext = Dot(current, next) < static_cast<S>(0)
                                          ? Quaternion<S>(-next.x, -next.y, -next.z, -next.w)
                                          : next;
    const Quaternion<S> inverse = current.Conjugate();
    const Quaternion<S> toPrevious = QuaternionLog(inverse * alignedPrevious);
    const Quaternion<S> toNext = QuaternionLog(inverse * alignedNext);
    const S scale = -(static_cast<S>(1) - tension) / static_cast<S>(4);
    const Quaternion<S> exponent((toPrevious.x + toNext.x) * scale,
                                 (toPrevious.y + toNext.y) * scale,
                                 (toPrevious.z + toNext.z) * scale, static_cast<S>(0));
    return detail::NormalizedOr(current * QuaternionExp(exponent), current);
}

// SQUAD 보간이다: Slerp(Slerp(q0, q1, t), Slerp(s0, s1, t), 2t(1 − t)).
// q1이 q0과 다른 반구면 q1과 s1을 함께 뒤집어 최단 경로를 택하고, 안쪽 세 slerp는 다시 뒤집지
// 않는다 (SlerpFullPath). t = 0이면 q0, t = 1이면 반구를 맞춘 q1이다. 결과는 단위 quaternion이다.
// 따라서 뒤집은 구간의 끝(t = 1, 커브에서는 루프 구간 끝에서 비율이 1로 잘린 경우)에서는 −q1을
// 반환한다. 같은 회전이지만 성분 부호가 키에서 평가한 q1과 반대이므로, 성분을 그대로 섞는 소비자는
// 반구를 다시 맞춰야 한다(UE TQuat::Squad도 같은 성질이다).
// s0은 q0에서 나가는 제어점, s1은 q1로 들어오는 제어점이다. 단위 quaternion 전제다.
// UE TQuat::Squad와 같은 구조(q0→q1만 최단 경로)이며 제어점 계산이 다르다(SquadControlPoint).
template <typename S>
Quaternion<S> Squad(const Quaternion<S>& q0, const Quaternion<S>& s0, const Quaternion<S>& q1,
                    const Quaternion<S>& s1, S t) noexcept {
    const bool flip = Dot(q0, q1) < static_cast<S>(0);
    const Quaternion<S> end = flip ? Quaternion<S>(-q1.x, -q1.y, -q1.z, -q1.w) : q1;
    const Quaternion<S> endControl = flip ? Quaternion<S>(-s1.x, -s1.y, -s1.z, -s1.w) : s1;
    if (t == static_cast<S>(0)) {
        return q0;
    }

    if (t == static_cast<S>(1)) {
        return end;
    }

    const Quaternion<S> along = SlerpFullPath(q0, end, t);
    const Quaternion<S> controls = SlerpFullPath(s0, endControl, t);
    const S blend = static_cast<S>(2) * t * (static_cast<S>(1) - t);
    return SlerpFullPath(along, controls, blend);
}

} // namespace rocks
