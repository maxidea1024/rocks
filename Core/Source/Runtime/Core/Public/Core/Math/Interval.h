#pragma once

#include <Core/Math/MathUtility.h>
#include <Core/Result.h>

namespace rocks {

// 1차원 닫힌 구간 [min, max](UE TInterval 대응)다. 길이 0인 구간도 유효하다.
// UE는 기본값을 무효 구간으로 두고 Include 첫 호출에서 채우지만, Rocks는 무효 상태를 값으로 두지
// 않고 TryMake로 검증한다. 유효 조건: 두 끝이 유한하고 min <= max.
template <typename T>
struct Interval {
    static_assert(math::IsFloatingPointV<T>, "Interval supports float and double");

    T min = static_cast<T>(0);
    T max = static_cast<T>(0);

    constexpr Interval() noexcept = default;

    constexpr Interval(T minimum, T maximum) noexcept : min(minimum), max(maximum) {
    }

    // 실패(InvalidArgument): 비유한 끝 또는 minimum > maximum.
    static Result<Interval> TryMake(T minimum, T maximum) noexcept {
        const Interval interval(minimum, maximum);
        if (!interval.IsValid()) {
            return Result<Interval>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Interval>::Success(interval);
    }

    friend constexpr bool operator==(const Interval&, const Interval&) noexcept = default;

    constexpr bool IsValid() const noexcept {
        return math::IsFinite(min) && math::IsFinite(max) && min <= max;
    }

    // 길이다. 매우 넓은 유한 구간(예: -FLT_MAX..FLT_MAX)은 +Inf가 될 수 있다.
    constexpr T Size() const noexcept {
        return max - min;
    }

    // 경계 포함이다. NaN은 false다.
    constexpr bool Contains(T value) const noexcept {
        return value >= min && value <= max;
    }

    // 구간 안으로 제한한다. min <= max가 아니면 Fatal이다(비유한 끝은 검사하지 않음). NaN은
    // NaN이다.
    constexpr T Clamp(T value) const noexcept {
        return math::Clamp(value, min, max);
    }

    // math::Lerp(min, max, alpha). alpha=0이면 min, 1이면 max를 정확히 반환하고 [0,1] 밖은
    // 외삽한다. 차이가 overflow하는 넓은 구간에서도 [0,1]의 결과는 유한하다.
    constexpr T Interpolate(T alpha) const noexcept {
        return math::Lerp(min, max, alpha);
    }

    // value를 포함하도록 넓힌다. NaN을 넣으면 결과 끝이 NaN이 되어 이후 누적에서도 무효(IsValid
    // false)로 남는다.
    constexpr Interval ExpandedToInclude(T value) const noexcept {
        return Interval(math::MinPropagateNaN(min, value), math::MaxPropagateNaN(max, value));
    }

    // 교집합이다. 끝점만 닿으면 길이 0 구간이고, 겹치지 않으면 NotFound, 어느 쪽이든 유효하지
    // 않으면 InvalidArgument다.
    Result<Interval> TryIntersection(const Interval& other) const noexcept {
        if (!IsValid() || !other.IsValid()) {
            return Result<Interval>::Failure(ErrorCode::InvalidArgument);
        }
        const T lower = math::Max(min, other.min);
        const T upper = math::Min(max, other.max);
        if (!(lower <= upper)) {
            return Result<Interval>::Failure(ErrorCode::NotFound);
        }
        return Result<Interval>::Success(Interval(lower, upper));
    }
};

} // namespace rocks
