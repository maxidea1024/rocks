#pragma once

#include <Core/Assert.h>
#include <Core/Traits.h>
#include <Platform/PlatformMath.h>

namespace rocks::math {

// Core 수학의 scalar 진입점이다. 계산은 CPU별로 선택된 platform::PlatformMath에 위임한다.
// 좌표·단위 규약: 오른손 좌표계, +X 전방, +Y 왼쪽, +Z 위, 길이는 미터, 각도는 라디안, 시간은 초.
// 허용 오차가 필요한 함수는 오차를 호출자가 명시한다. 모든 검사에 쓰는 전역 epsilon은 없다.
// float/double 외 타입은 지원하지 않는다.
template <typename T>
inline constexpr bool IsFloatingPointV = IsSameV<T, float> || IsSameV<T, double>;

template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T Pi = static_cast<T>(3.141592653589793238462643383279502884L);

template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T TwoPi = static_cast<T>(6.283185307179586476925286766559005768L);

template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T HalfPi = static_cast<T>(1.570796326794896619231321691639751442L);

template <typename T>
    requires IsFloatingPointV<T>
constexpr T DegreesToRadians(T degrees) noexcept {
    return degrees * (Pi<T> / static_cast<T>(180));
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr T RadiansToDegrees(T radians) noexcept {
    return radians * (static_cast<T>(180) / Pi<T>);
}

// ---- 분류·부호 ----
template <typename T>
    requires IsFloatingPointV<T>
constexpr bool IsNaN(T value) noexcept {
    return platform::PlatformMath::IsNaN(value);
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr bool IsFinite(T value) noexcept {
    return platform::PlatformMath::IsFinite(value);
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr T Abs(T value) noexcept {
    return platform::PlatformMath::Abs(value);
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr T CopySign(T magnitude, T sign) noexcept {
    return platform::PlatformMath::CopySign(magnitude, sign);
}

// (a < b) ? a : b. NaN이 섞이면 b를 반환하므로 NaN 검사를 대신하지 않는다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr T Min(T a, T b) noexcept {
    return platform::PlatformMath::Min(a, b);
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr T Max(T a, T b) noexcept {
    return platform::PlatformMath::Max(a, b);
}

// minimum <= maximum이 전제다(위반은 Fatal). value가 NaN이면 NaN을 그대로 반환한다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr T Clamp(T value, T minimum, T maximum) noexcept {
    ROCKS_ASSERT(minimum <= maximum);
    if (value < minimum) {
        return minimum;
    }
    if (maximum < value) {
        return maximum;
    }
    return value;
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr T Square(T value) noexcept {
    return value * value;
}

// -1, 0, +1을 반환한다. ±0은 +0, NaN은 NaN이다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr T Sign(T value) noexcept {
    if (value > static_cast<T>(0)) {
        return static_cast<T>(1);
    }
    if (value < static_cast<T>(0)) {
        return static_cast<T>(-1);
    }
    return IsNaN(value) ? value : static_cast<T>(0);
}

// t=0이면 정확히 a, t=1이면 정확히 b를 반환한다. 그 밖에는 a + (b - a) * t이며 [0,1] 밖은
// 외삽한다. 유한한 a, b의 차이가 overflow하면(예: -FLT_MAX와 FLT_MAX) a(1 - t) + bt로 계산해
// [0,1] 구간에서 유한한 결과를 유지한다. t가 0 또는 1이면 해당 끝점만 반환하므로 반대쪽 끝점의
// NaN은 결과에 나타나지 않는다. 그 밖의 NaN 입력은 NaN이다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr T Lerp(T a, T b, T t) noexcept {
    if (t == static_cast<T>(0)) {
        return a;
    }
    if (t == static_cast<T>(1)) {
        return b;
    }
    const T difference = b - a;
    if (!IsFinite(difference) && IsFinite(a) && IsFinite(b)) {
        const T fromA = a * (static_cast<T>(1) - t);
        const T fromB = b * t;
        return fromA + fromB;
    }
    const T scaled = difference * t;
    return a + scaled;
}

// NaN을 전파하는 최소·최대다. 어느 쪽이든 NaN이면 NaN을 반환한다. 경계 누적처럼 NaN 입력이
// 이후 단계에서 사라지면 안 되는 곳에 쓴다(Min/Max는 NaN이면 두 번째 인자를 반환한다).
template <typename T>
    requires IsFloatingPointV<T>
constexpr T MinPropagateNaN(T a, T b) noexcept {
    if (IsNaN(a)) {
        return a;
    }
    if (IsNaN(b)) {
        return b;
    }
    return a < b ? a : b;
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr T MaxPropagateNaN(T a, T b) noexcept {
    if (IsNaN(a)) {
        return a;
    }
    if (IsNaN(b)) {
        return b;
    }
    return a > b ? a : b;
}

// |a - b| <= tolerance. tolerance는 0 이상의 절대 오차다. NaN이 섞이면 false다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr bool IsNearlyEqual(T a, T b, T tolerance) noexcept {
    return Abs(a - b) <= tolerance;
}

template <typename T>
    requires IsFloatingPointV<T>
constexpr bool IsNearlyZero(T value, T tolerance) noexcept {
    return Abs(value) <= tolerance;
}

// ---- 이름 있는 허용 오차 상수(UE UE_SMALL_NUMBER·UE_KINDA_SMALL_NUMBER 대응, 값도 같다) ----
// 어떤 함수에도 묵시적으로 적용되지 않는다. 비교 함수에 명시적으로 넘겨 의도를 드러낸다:
//   math::IsNearlyEqual(a, b, math::KindaSmallNumber<float>)
// 절대 오차이므로 값의 크기에 맞는지 호출자가 판단한다. 예: float 월드 좌표 1e6 m 근처의 간격은
// 약 0.06 m라 1e-4는 사실상 정확 비교가 되고, 정규화된 값이나 작은 각도에는 너무 클 수 있다.
// 크기에 비례하는 비교가 필요하면 IsNearlyEqualRelative 또는 IsNearlyEqualByUlps를 쓴다.
template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T SmallNumber = static_cast<T>(1.0e-8);

template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T KindaSmallNumber = static_cast<T>(1.0e-4);

// 1과 그다음 표현 가능한 값의 차이(FLT_EPSILON·DBL_EPSILON)다.
template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T MachineEpsilon = IsSameV<T, float> ? static_cast<T>(1.1920928955078125e-7)
                                                      : static_cast<T>(2.220446049250313080847e-16);

// |a - b| <= max(absoluteTolerance, relativeTolerance * max(|a|, |b|)).
// 0 근처에서는 절대 오차, 큰 값에서는 상대 오차가 지배한다. NaN이 섞이면 false다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr bool IsNearlyEqualRelative(T a, T b, T relativeTolerance, T absoluteTolerance) noexcept {
    const T difference = Abs(a - b);
    if (difference <= absoluteTolerance) {
        return true;
    }
    return difference <= relativeTolerance * Max(Abs(a), Abs(b));
}

// 표현 가능한 값의 간격(ULP) 단위 비교다(UE IsNearlyEqualByULP와 같은 규칙). NaN은 false, 무한대는
// 같은 부호의 무한대와만 같고, +0과 -0은 0 ULP 차이다. maxUlps는 0 이상이어야 한다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr bool IsNearlyEqualByUlps(T a, T b, uint64 maxUlps) noexcept {
    if (IsNaN(a) || IsNaN(b)) {
        return false;
    }
    if (!IsFinite(a) || !IsFinite(b)) {
        return a == b;
    }
    if constexpr (IsSameV<T, float>) {
        // 부호-크기 표현을 0이 중앙인 단조 정수로 바꿔 0을 넘는 차이도 연속으로 센다.
        auto ordered = [](float value) constexpr {
            const uint32 raw = __builtin_bit_cast(uint32, value);
            return (raw & 0x80000000u) != 0
                       ? static_cast<int64>(0x80000000u) - static_cast<int64>(raw & 0x7fffffffu)
                       : static_cast<int64>(0x80000000u) + raw;
        };
        const int64 distance = ordered(a) - ordered(b);
        return static_cast<uint64>(distance < 0 ? -distance : distance) <= maxUlps;
    } else {
        auto ordered = [](double value) constexpr {
            const uint64 raw = __builtin_bit_cast(uint64, value);
            return (raw & 0x8000000000000000ull) != 0
                       ? 0x8000000000000000ull - (raw & 0x7fffffffffffffffull)
                       : 0x8000000000000000ull + raw;
        };
        const uint64 left = ordered(a);
        const uint64 right = ordered(b);
        return (left > right ? left - right : right - left) <= maxUlps;
    }
}

// ---- CPU별 구현으로 위임하는 함수. 특수값 규약은 GenericPlatformMath.h를 따른다. ----
template <typename T>
    requires IsFloatingPointV<T>
T Sqrt(T value) noexcept {
    return platform::PlatformMath::Sqrt(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T InvSqrt(T value) noexcept {
    return platform::PlatformMath::InvSqrt(value);
}

// 빠른 근사다. 정밀도 요구가 있는 계산과 결정성이 필요한 계산에는 InvSqrt를 쓴다.
template <typename T>
    requires IsFloatingPointV<T>
T InvSqrtEstimate(T value) noexcept {
    return platform::PlatformMath::InvSqrtEstimate(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Sin(T radians) noexcept {
    return platform::PlatformMath::Sin(radians);
}

template <typename T>
    requires IsFloatingPointV<T>
T Cos(T radians) noexcept {
    return platform::PlatformMath::Cos(radians);
}

template <typename T>
    requires IsFloatingPointV<T>
T Tan(T radians) noexcept {
    return platform::PlatformMath::Tan(radians);
}

template <typename T>
    requires IsFloatingPointV<T>
T Asin(T value) noexcept {
    return platform::PlatformMath::Asin(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Acos(T value) noexcept {
    return platform::PlatformMath::Acos(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Atan(T value) noexcept {
    return platform::PlatformMath::Atan(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Atan2(T y, T x) noexcept {
    return platform::PlatformMath::Atan2(y, x);
}

template <typename T>
    requires IsFloatingPointV<T>
T Floor(T value) noexcept {
    return platform::PlatformMath::Floor(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Ceil(T value) noexcept {
    return platform::PlatformMath::Ceil(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Trunc(T value) noexcept {
    return platform::PlatformMath::Trunc(value);
}

// 0.5 경계는 0에서 멀어지는 방향이다.
template <typename T>
    requires IsFloatingPointV<T>
T Round(T value) noexcept {
    return platform::PlatformMath::Round(value);
}

template <typename T>
    requires IsFloatingPointV<T>
T Fmod(T dividend, T divisor) noexcept {
    return platform::PlatformMath::Fmod(dividend, divisor);
}

// 각도를 (-pi, pi] 구간으로 감는다. 유한하지 않은 입력은 NaN이다.
// 큰 각도는 float의 2pi 표현 오차만큼 결과가 달라질 수 있으므로 누적 각도는 double로 유지한다.
template <typename T>
    requires IsFloatingPointV<T>
T UnwindRadians(T radians) noexcept {
    T wrapped = Fmod(radians, TwoPi<T>);
    if (wrapped > Pi<T>) {
        wrapped -= TwoPi<T>;
    } else if (wrapped <= -Pi<T>) {
        wrapped += TwoPi<T>;
    }
    return wrapped;
}

// sin과 cos을 함께 계산한다. 결과는 Sin/Cos를 각각 호출한 것과 같다.
template <typename T>
    requires IsFloatingPointV<T>
void SinCos(T radians, T& sine, T& cosine) noexcept {
    sine = Sin(radians);
    cosine = Cos(radians);
}

// a에서 b로 가는 최소 각 차이(라디안, (-pi, pi])다. UE FindDeltaAngleRadians 대응.
template <typename T>
    requires IsFloatingPointV<T>
T FindDeltaAngleRadians(T fromRadians, T toRadians) noexcept {
    return UnwindRadians(toRadians - fromRadians);
}

// ---- 보간 ----

// x가 [edge0, edge1]에서 0→1로 매끄럽게 변하는 3차 Hermite 값 t²(3 - 2t)다.
// x <= edge0이면 0, x >= edge1이면 1이다. edge0 < edge1이 전제다(위반은 Fatal). NaN은 NaN이다.
// UE SmoothStep과 같은 결과이며 UE와 달리 edge 순서를 검사한다.
template <typename T>
    requires IsFloatingPointV<T>
constexpr T SmoothStep(T edge0, T edge1, T x) noexcept {
    ROCKS_ASSERT(edge0 < edge1);
    if (x <= edge0) {
        return static_cast<T>(0);
    }
    if (x >= edge1) {
        return static_cast<T>(1);
    }
    if (IsNaN(x)) {
        return x;
    }
    const T t = (x - edge0) / (edge1 - edge0);
    return t * t * (static_cast<T>(3) - static_cast<T>(2) * t);
}

// 3차 Hermite 보간이다. p0·p1은 끝점, t0·t1은 접선, alpha ∈ [0,1]. UE CubicInterp 대응.
template <typename T>
    requires IsFloatingPointV<T>
constexpr T CubicInterp(T p0, T t0, T p1, T t1, T alpha) noexcept {
    const T a2 = alpha * alpha;
    const T a3 = a2 * alpha;
    const T h00 = static_cast<T>(2) * a3 - static_cast<T>(3) * a2 + static_cast<T>(1);
    const T h10 = a3 - static_cast<T>(2) * a2 + alpha;
    const T h01 = static_cast<T>(-2) * a3 + static_cast<T>(3) * a2;
    const T h11 = a3 - a2;
    return h00 * p0 + h10 * t0 + h01 * p1 + h11 * t1;
}

// 매 프레임 호출하는 거리 비례 접근이다: current + (target - current) * clamp(dt * speed, 0, 1).
// speed <= 0이거나 dt * speed >= 1이면 target을 정확히 반환한다(UE FInterpTo와 같은 의미).
// UE와 달리 "충분히 가까우면 target"으로 바꾸는 숨은 거리 기준이 없으므로 점근적으로 접근한다.
// 도달 판정 거리가 필요하면 호출자가 IsNearlyEqual로 명시한다. dt는 초 단위이며 음수·NaN이면
// Fatal이다. current·target·speed 중 NaN이 있으면 NaN을 반환한다(유효해 보이는 값으로 바꾸지 않음).
template <typename T>
    requires IsFloatingPointV<T>
T InterpTo(T current, T target, T deltaSeconds, T speed) noexcept {
    ROCKS_ASSERT(deltaSeconds >= static_cast<T>(0));
    if (IsNaN(current) || IsNaN(target) || IsNaN(speed)) {
        return current + target + speed;
    }
    if (!(speed > static_cast<T>(0))) {
        return target;
    }
    const T alpha = deltaSeconds * speed;
    if (alpha >= static_cast<T>(1)) {
        return target;
    }
    return current + (target - current) * alpha;
}

// 매 프레임 최대 speed * dt만큼 일정 속도로 접근한다. 남은 거리가 그 이하이면 target을 정확히
// 반환한다(UE FInterpConstantTo 대응, 숨은 거리 기준 없음). dt·speed가 음수·NaN이면 Fatal이다.
// current나 target이 NaN이면(남은 거리가 NaN) NaN을 반환한다.
template <typename T>
    requires IsFloatingPointV<T>
T InterpConstantTo(T current, T target, T deltaSeconds, T speed) noexcept {
    ROCKS_ASSERT(deltaSeconds >= static_cast<T>(0) && speed >= static_cast<T>(0));
    const T step = deltaSeconds * speed;
    const T distance = target - current;
    if (IsNaN(distance)) {
        return distance;
    }
    if (Abs(distance) <= step) {
        return target;
    }
    return current + CopySign(step, distance);
}

// 가장 가까운 grid 배수로 맞춘다. 0.5 경계는 grid 부호와 무관하게 +무한대 방향이다(|grid|를
// 쓴다). UE GridSnap은 음수 grid에서 경계 방향이 -무한대로 바뀐다. grid가 0이면 location을 그대로
// 반환한다.
template <typename T>
    requires IsFloatingPointV<T>
T GridSnap(T location, T grid) noexcept {
    if (grid == static_cast<T>(0)) {
        return location;
    }
    const T size = Abs(grid);
    return Floor((location + size / static_cast<T>(2)) / size) * size;
}

// value를 반열린 구간 [minimum, maximum)으로 주기적으로 감는다. minimum < maximum이 전제다
// (위반은 Fatal). 비유한 입력은 NaN이다. UE Wrap은 닫힌 구간에서 경계 선택 규칙이 달라 결과가
// max와 min 중 다른 쪽이 될 수 있다.
template <typename T>
    requires IsFloatingPointV<T>
T Wrap(T value, T minimum, T maximum) noexcept {
    ROCKS_ASSERT(minimum < maximum);
    const T size = maximum - minimum;
    T offset = Fmod(value - minimum, size);
    if (offset < static_cast<T>(0)) {
        offset += size;
    }
    const T wrapped = minimum + offset;
    // 음수 offset 보정의 반올림으로 maximum이 나오면 구간 시작으로 돌린다.
    return wrapped >= maximum ? minimum : wrapped;
}

// ---- 정수 변환: 결과가 범위 밖이거나 NaN이면 Fatal이다. 실패를 값으로 받으려면 Try*를 쓴다. ----
inline int32 FloorToInt32(float value) noexcept {
    return platform::PlatformMath::FloorToInt32(value);
}

inline int32 FloorToInt32(double value) noexcept {
    return platform::PlatformMath::FloorToInt32(value);
}

inline int32 CeilToInt32(float value) noexcept {
    return platform::PlatformMath::CeilToInt32(value);
}

inline int32 CeilToInt32(double value) noexcept {
    return platform::PlatformMath::CeilToInt32(value);
}

inline int32 RoundToInt32(float value) noexcept {
    return platform::PlatformMath::RoundToInt32(value);
}

inline int32 RoundToInt32(double value) noexcept {
    return platform::PlatformMath::RoundToInt32(value);
}

inline int32 TruncToInt32(float value) noexcept {
    return platform::PlatformMath::TruncToInt32(value);
}

inline int32 TruncToInt32(double value) noexcept {
    return platform::PlatformMath::TruncToInt32(value);
}

inline int64 FloorToInt64(double value) noexcept {
    return platform::PlatformMath::FloorToInt64(value);
}

inline int64 RoundToInt64(double value) noexcept {
    return platform::PlatformMath::RoundToInt64(value);
}

// 실패하면 false를 반환하고 output을 바꾸지 않는다.
inline bool TryFloorToInt32(double value, int32& output) noexcept {
    return platform::PlatformMath::TryFloorToInt32(value, output);
}

inline bool TryRoundToInt32(double value, int32& output) noexcept {
    return platform::PlatformMath::TryRoundToInt32(value, output);
}

inline bool TryTruncToInt32(double value, int32& output) noexcept {
    return platform::PlatformMath::TryTruncToInt32(value, output);
}

inline bool TryCeilToInt32(double value, int32& output) noexcept {
    return platform::PlatformMath::TryCeilToInt32(value, output);
}

} // namespace rocks::math
