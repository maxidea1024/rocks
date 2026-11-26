#pragma once

#include <Base/Checked.h>
#include <Base/Types.h>
#include <Core/Math/MathUtility.h>
#include <Core/Result.h>
#include <Platform/Fatal.h>

// 프레임 단위 시간이다(UE 5.8 Core/Public/Misc/FrameRate.h·FrameNumber.h·FrameTime.h의 최소 대응 —
// 설계 Docs/Design/CurveData.ko.md D20). 초 단위 실수 시간을 정해진 프레임 속도의 프레임 번호와
// 프레임 안 비율(subframe)로 나타낸다. 커브 중복 키 제거의 표본 시간이 쓰고, 시퀀서·애니메이션
// 시간도 같은 형식을 쓴다.
//
// 사용 예:
//   const FrameRate rate = FrameRate::TryCreate(30, 1).Value();     // 30 fps
//   const FrameTime frame = rate.AsFrameTime(1.25);                // 37번 프레임 + 0.5
//   const double seconds = rate.AsSeconds(frame);                   // 1.25
//
// 변환 식은 UE와 같은 식·같은 정밀도라 같은 입력에서 같은 비트를 낸다. 다른 점은 UE에서 정의되지
// 않는 경우뿐이다: 0·음수 분자·분모는 만들 수 없고(UE는 IsValid로 따로 확인), FrameNumber 연산이
// int32를 넘으면 Fatal 또는 Overflow이며(UE는 감싸 돈다), AsFrameTime의 NaN은 거부하고 ±Inf는 int32
// 끝으로 포화한다.
namespace rocks {

// 리플렉션 생성 코드의 접근점이다(정의는 MetaSchema Meta/GeneratedType.h — Core는 앞 선언만 한다).
// FrameRate의 private 멤버를 CoreTypeBindings 미러(Binds)의 생성 코드가 읽고 쓴다.
template <class>
struct GeneratedType;

// 프레임 번호다(UE FFrameNumber). 정수 하나이고 프레임 속도와 함께 써야 시간이 된다.
// 연산자(+, -, +=, -=, ++)는 int32를 넘으면 Fatal로 끝낸다. 외부 데이터에서 온 값은 TryAdd·
// TrySubtract로 다루고 Overflow를 처리한다.
struct FrameNumber {
    int32 value = 0;

    constexpr FrameNumber() noexcept = default;

    constexpr explicit FrameNumber(int32 frame) noexcept : value(frame) {
    }

    // 더하거나 뺀다. 결과가 int32를 넘으면 Overflow다(값은 바뀌지 않는다).
    Result<FrameNumber> TryAdd(FrameNumber other) const noexcept {
        int32 sum = 0;
        if (!::rocks::TryAdd(value, other.value, sum)) {
            return Result<FrameNumber>::Failure(ErrorCode::Overflow);
        }

        return Result<FrameNumber>::Success(FrameNumber(sum));
    }

    Result<FrameNumber> TrySubtract(FrameNumber other) const noexcept {
        int32 difference = 0;
        if (!::rocks::TrySubtract(value, other.value, difference)) {
            return Result<FrameNumber>::Failure(ErrorCode::Overflow);
        }

        return Result<FrameNumber>::Success(FrameNumber(difference));
    }

    friend FrameNumber operator+(FrameNumber left, FrameNumber right) noexcept {
        return Checked(left.TryAdd(right));
    }

    friend FrameNumber operator-(FrameNumber left, FrameNumber right) noexcept {
        return Checked(left.TrySubtract(right));
    }

    FrameNumber& operator+=(FrameNumber other) noexcept {
        *this = *this + other;
        return *this;
    }

    FrameNumber& operator-=(FrameNumber other) noexcept {
        *this = *this - other;
        return *this;
    }

    FrameNumber& operator++() noexcept {
        *this = *this + FrameNumber(1);
        return *this;
    }

    friend constexpr bool operator==(FrameNumber left, FrameNumber right) noexcept {
        return left.value == right.value;
    }

    friend constexpr bool operator<(FrameNumber left, FrameNumber right) noexcept {
        return left.value < right.value;
    }

    friend constexpr bool operator<=(FrameNumber left, FrameNumber right) noexcept {
        return left.value <= right.value;
    }

    friend constexpr bool operator>(FrameNumber left, FrameNumber right) noexcept {
        return left.value > right.value;
    }

    friend constexpr bool operator>=(FrameNumber left, FrameNumber right) noexcept {
        return left.value >= right.value;
    }

private:
    static FrameNumber Checked(const Result<FrameNumber>& result) noexcept {
        if (!result) {
            platform::Fatal("FrameNumber arithmetic overflow");
        }

        return result.Value();
    }
};

// 프레임 번호와 프레임 안 비율(subframe)이다(UE FFrameTime). subframe은 늘 [0, MaxSubframe]이고
// 값은 프레임 번호 + subframe 프레임이다. 음수 시간도 프레임은 내림이고 subframe은 0
// 이상이다(−0.25프레임은 프레임 −1, subframe 0.75).
class FrameTime {
public:
    // 1보다 작은 가장 큰 float다(UE FFrameTime::MaxSubframe = 0.99999994f).
    static constexpr float MaxSubframe = 0.99999994f;

    // 프레임 0, subframe 0이다.
    constexpr FrameTime() noexcept = default;

    // 정수 프레임이다(subframe 0).
    constexpr explicit FrameTime(FrameNumber frame) noexcept : frame_(frame) {
    }

    // 프레임과 subframe으로 만든다. subframe은 UE와 같은 식으로 다듬는다: (subframe + 0.5) − 0.5로
    // 아주 작은 값의 아래 비트를 버린 뒤 [0, MaxSubframe]로 자른다. 0보다 작으면 0, MaxSubframe
    // 이상과 NaN은 MaxSubframe이다(UE FMath::Clamp와 같다). 프레임을 올려 주지 않으므로 1 이상의
    // subframe은 다음 프레임이 아니라 MaxSubframe이 된다.
    FrameTime(FrameNumber frame, float subFrame) noexcept : frame_(frame) {
        const float rounded = (subFrame + 0.5f) - 0.5f;
        subFrame_ = rounded < 0.0f ? 0.0f : (rounded < MaxSubframe ? rounded : MaxSubframe);
    }

    FrameNumber Frame() const noexcept {
        return frame_;
    }

    float SubFrame() const noexcept {
        return subFrame_;
    }

    // 프레임 번호만 바꾼다(subframe 유지). 표본 프레임을 하나씩 옮길 때 쓴다.
    void SetFrame(FrameNumber frame) noexcept {
        frame_ = frame;
    }

    // 프레임 단위 실수 값이다: 프레임 + subframe(double).
    double AsDecimal() const noexcept {
        return static_cast<double>(frame_.value) + subFrame_;
    }

    friend bool operator==(FrameTime left, FrameTime right) noexcept {
        return left.frame_ == right.frame_ && left.subFrame_ == right.subFrame_;
    }

    friend bool operator<(FrameTime left, FrameTime right) noexcept {
        return left.frame_ < right.frame_ ||
               (left.frame_ == right.frame_ && left.subFrame_ < right.subFrame_);
    }

    friend bool operator<=(FrameTime left, FrameTime right) noexcept {
        return left.frame_ < right.frame_ ||
               (left.frame_ == right.frame_ && left.subFrame_ <= right.subFrame_);
    }

    friend bool operator>(FrameTime left, FrameTime right) noexcept {
        return right < left;
    }

    friend bool operator>=(FrameTime left, FrameTime right) noexcept {
        return right <= left;
    }

private:
    FrameNumber frame_;
    float subFrame_ = 0.0f;
};

// 프레임 속도다(UE FFrameRate): 초당 numerator / denominator 프레임. 예: 30 fps는 30/1, NTSC 29.97
// fps는 30000/1001. 분자·분모는 둘 다 양수다(TryCreate가 확인한다). 기본값은 UE와 같은
// 60000/1이다(프레임 속도를 고르지 않은 시간 표현의 정밀도용 — 표시용 속도가 아니다).
class FrameRate {
    // 리플렉션 미러(CoreTypeBindings FrameRate 미러 — Docs/Design/MeshAnimationAssets 7.5절
    // 3)의 생성 코드가 private 멤버(numerator_·denominator_)에 닿는다. 생성 코드는 decode한 값을
    // 게시하기 전에 미러의 Validate(두 값이 양수)를 부르므로 불변식이 깨진 값을 게시하지 않는다.
    template <class>
    friend struct ::rocks::GeneratedType;

public:
    constexpr FrameRate() noexcept = default;

    // numerator / denominator fps로 만든다. 둘 중 하나라도 0 이하면 InvalidArgument다.
    static Result<FrameRate> TryCreate(int32 numerator, int32 denominator) noexcept {
        if (numerator <= 0 || denominator <= 0) {
            return Result<FrameRate>::Failure(ErrorCode::InvalidArgument);
        }

        FrameRate rate;
        rate.numerator_ = numerator;
        rate.denominator_ = denominator;
        return Result<FrameRate>::Success(rate);
    }

    int32 Numerator() const noexcept {
        return numerator_;
    }

    int32 Denominator() const noexcept {
        return denominator_;
    }

    // 한 프레임의 초다: denominator / numerator.
    double AsInterval() const noexcept {
        return static_cast<double>(denominator_) / static_cast<double>(numerator_);
    }

    // 초당 프레임 수다: numerator / denominator.
    double AsDecimal() const noexcept {
        return static_cast<double>(numerator_) / static_cast<double>(denominator_);
    }

    // 프레임 시간을 초로 바꾼다(UE FFrameRate::AsSeconds, FrameRate.h 232–238):
    //   (frame·denominator(int64) + subframe·denominator(double)) / numerator
    double AsSeconds(FrameTime time) const noexcept {
        const int64 integerPart =
            static_cast<int64>(time.Frame().value) * static_cast<int64>(denominator_);
        const double floatPart = time.SubFrame() * static_cast<double>(denominator_);
        return (static_cast<double>(integerPart) + floatPart) / numerator_;
    }

    // 초를 프레임 시간으로 바꾼다(UE FFrameRate::AsFrameTime, FrameRate.h 240–255). 프레임은
    // (seconds·numerator)/denominator의 내림이고 int32 범위로 자른다. subframe은 내림하고 남은
    // 비율을 float로 바꾼 값이다. float 반올림으로 1이 되면 다음 프레임으로 올리고 subframe은
    // 0이다. 그 밖의 subframe은 MaxSubframe을 넘지 않는다. int32 범위 밖의 시간도 subframe은 원래
    // 값의 비율이다(UE와 같다). 실패: seconds가 NaN이면 InvalidArgument다. ±Inf(또는 곱이 넘쳐
    // Inf)는 프레임 int32 끝 + subframe 0이다(UE는 Inf − Inf = NaN subframe을 정수로 바꿔 의미 없는
    // 값을 낸다).
    Result<FrameTime> TryAsFrameTime(double seconds) const noexcept {
        if (math::IsNaN(seconds)) {
            return Result<FrameTime>::Failure(ErrorCode::InvalidArgument);
        }

        constexpr double MinimumFrame = static_cast<double>(NumericLimits<int32>::Min());
        constexpr double MaximumFrame = static_cast<double>(NumericLimits<int32>::Max());
        const double timeAsFrame = (seconds * numerator_) / denominator_;
        const double floored = math::Floor(timeAsFrame);
        if (!math::IsFinite(floored)) {
            const int32 edge =
                floored > 0.0 ? NumericLimits<int32>::Max() : NumericLimits<int32>::Min();
            return Result<FrameTime>::Success(FrameTime(FrameNumber(edge)));
        }

        const double clamped = floored < MinimumFrame
                                   ? MinimumFrame
                                   : (floored < MaximumFrame ? floored : MaximumFrame);
        FrameNumber frame(static_cast<int32>(clamped));
        float subFrame = static_cast<float>(timeAsFrame - floored);
        const int32 truncated = static_cast<int32>(subFrame); // subFrame ∈ [0, 1]이라 0 또는 1이다
        subFrame -= static_cast<float>(truncated);
        // 올림(비율이 float 반올림으로 1)은 비율이 1 − 2^-25보다 클 때뿐이고, 그러려면 double
        // 간격이 그보다 촘촘해야 해서 |프레임| < 2^28이다. 따라서 int32 최댓값 프레임에서는
        // 일어나지 않고 이 덧셈은 넘치지 않는다.
        frame = frame + FrameNumber(truncated);

        if (subFrame > 0.0f) {
            subFrame = math::Min(subFrame, FrameTime::MaxSubframe);
        }

        return Result<FrameTime>::Success(FrameTime(frame, subFrame));
    }

    // TryAsFrameTime과 같고 NaN이면 Fatal이다. 유한한 시간(검증된 키 시간 등)에 쓴다.
    FrameTime AsFrameTime(double seconds) const noexcept {
        const Result<FrameTime> result = TryAsFrameTime(seconds);
        if (!result) {
            platform::Fatal("FrameRate::AsFrameTime received NaN seconds");
        }

        return result.Value();
    }

    friend constexpr bool operator==(FrameRate left, FrameRate right) noexcept {
        return left.numerator_ == right.numerator_ && left.denominator_ == right.denominator_;
    }

private:
    int32 numerator_ = 60000;
    int32 denominator_ = 1;
};

} // namespace rocks
