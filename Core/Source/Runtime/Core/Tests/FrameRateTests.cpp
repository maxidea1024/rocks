#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Time/FrameRate.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// FrameRate·FrameNumber·FrameTime(Docs/Design/CurveData.ko.md D20) 시험이다. UE 식 대조는 UE 5.8.4
// Runtime/Core/Public/Misc/FrameRate.h 232–238(AsSeconds)·240–255(AsFrameTime)와 FrameTime.h의
// 생성자 (FFrameTime(FFrameNumber, float) — Clamp(InSubFrame + 0.5f − 0.5f, 0, MaxSubframe))를 옮긴
// 참조로 한다.
namespace rocks::test {

namespace frame_rate_tests {

// UE FMath::Clamp: X < Min ? Min : X < Max ? X : Max
template <typename T>
T UeClamp(T value, T minimum, T maximum) noexcept {
    return value < minimum ? minimum : (value < maximum ? value : maximum);
}

struct UeFrameTime {
    int32 frame = 0;
    float subFrame = 0.0f;
};

constexpr float UeMaxSubframe = 0.99999994f;

// FrameTime.h FFrameTime(FFrameNumber, float)
UeFrameTime UeMakeFrameTime(int32 frame, float subFrame) noexcept {
    return {frame, UeClamp(subFrame + 0.5f - 0.5f, 0.f, UeMaxSubframe)};
}

// FrameRate.h 240–255(유한한 TimeAsFrame만 — int32 덧셈이 넘치지 않는 입력)
UeFrameTime UeAsFrameTime(int32 Numerator, int32 Denominator, double TimeInSeconds) noexcept {
    const double TimeAsFrame = (TimeInSeconds * Numerator) / Denominator;
    int32 FrameNumber =
        static_cast<int32>(UeClamp(platform::PlatformMath::Floor(TimeAsFrame),
                                   static_cast<double>(NumericLimits<int32>::Min()),
                                   static_cast<double>(NumericLimits<int32>::Max())));
    float SubFrame = static_cast<float>(TimeAsFrame - platform::PlatformMath::Floor(TimeAsFrame));
    const int32 TruncatedSubFrame = static_cast<int32>(SubFrame);
    SubFrame -= static_cast<float>(TruncatedSubFrame);
    FrameNumber += TruncatedSubFrame;
    if (SubFrame > 0.f) {
        SubFrame = SubFrame <= UeMaxSubframe ? SubFrame : UeMaxSubframe; // FMath::Min
    }

    return UeMakeFrameTime(FrameNumber, SubFrame);
}

// FrameRate.h 232–238
double UeAsSeconds(int32 Numerator, int32 Denominator, UeFrameTime FrameTime) noexcept {
    const int64 IntegerPart = FrameTime.frame * int64(Denominator);
    const double FloatPart = FrameTime.subFrame * double(Denominator);
    return (double(IntegerPart) + FloatPart) / Numerator;
}

bool SameFloat(float a, float b) noexcept {
    return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
}

bool SameDouble(double a, double b) noexcept {
    return platform::BitCast<uint64>(a) == platform::BitCast<uint64>(b);
}

} // namespace frame_rate_tests

namespace frame_rate_tests_file {

using namespace frame_rate_tests;

TEST(FrameRate_CreateRejectsNonPositive) {
    const FrameRate standard;
    CHECK(standard.Numerator() == 60000 && standard.Denominator() == 1);
    CHECK(FrameRate::TryCreate(30, 1));
    CHECK(FrameRate::TryCreate(0, 1).Error() == ErrorCode::InvalidArgument);
    CHECK(FrameRate::TryCreate(30, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(FrameRate::TryCreate(-30, 1).Error() == ErrorCode::InvalidArgument);
    CHECK(FrameRate::TryCreate(30, -1).Error() == ErrorCode::InvalidArgument);

    const FrameRate ntsc = FrameRate::TryCreate(30000, 1001).Value();
    CHECK(ntsc.AsDecimal() == 30000.0 / 1001.0);
    CHECK(ntsc.AsInterval() == 1001.0 / 30000.0);
    CHECK(ntsc == FrameRate::TryCreate(30000, 1001).Value());
    CHECK(!(ntsc == standard));
}

TEST(FrameRate_HandVectors) {
    const FrameRate rate = FrameRate::TryCreate(30, 1).Value();
    // 1.25초 × 30 = 37.5프레임 → 프레임 37, subframe 0.5. 되돌리면 (37 + 0.5)/30 = 1.25.
    const FrameTime time = rate.AsFrameTime(1.25);
    CHECK(time.Frame() == FrameNumber(37) && time.SubFrame() == 0.5f);
    CHECK(rate.AsSeconds(time) == 1.25);
    CHECK(time.AsDecimal() == 37.5);

    // 음수: −0.25프레임(1 fps에서 −0.25초)은 프레임 −1, subframe 0.75.
    const FrameRate one = FrameRate::TryCreate(1, 1).Value();
    const FrameTime negative = one.AsFrameTime(-0.25);
    CHECK(negative.Frame() == FrameNumber(-1) && negative.SubFrame() == 0.75f);

    // NTSC 1초: 30000/1001 = 29.97002997… → 프레임 29, subframe float(0.97002997…) = 0.97002995.
    const FrameRate ntsc = FrameRate::TryCreate(30000, 1001).Value();
    const FrameTime second = ntsc.AsFrameTime(1.0);
    CHECK(second.Frame() == FrameNumber(29) && second.SubFrame() == 0.9700299501419067f);

    // subframe 올림: 5 − 2^-30초(1 fps) → 비율 1 − 2^-30이 float 1이 되어 프레임 5, subframe 0.
    const FrameTime carried = one.AsFrameTime(5.0 - 1.0 / 1073741824.0);
    CHECK(carried.Frame() == FrameNumber(5) && carried.SubFrame() == 0.0f);

    // int32 범위 밖: 프레임은 int32 최댓값, subframe은 원래 비율(UE와 같다).
    const FrameTime large = one.AsFrameTime(3000000000.25);
    CHECK(large.Frame() == FrameNumber(NumericLimits<int32>::Max()) && large.SubFrame() == 0.25f);
    const FrameTime small = one.AsFrameTime(-3000000000.25);
    CHECK(small.Frame() == FrameNumber(NumericLimits<int32>::Min()) && small.SubFrame() == 0.75f);

    // NaN은 InvalidArgument, ±Inf는 int32 끝 + subframe 0.
    CHECK(one.TryAsFrameTime(platform::PlatformMath::QuietNaN<double>()).Error() ==
          ErrorCode::InvalidArgument);
    const FrameTime infinite = one.AsFrameTime(platform::PlatformMath::Infinity<double>());
    CHECK(infinite.Frame() == FrameNumber(NumericLimits<int32>::Max()) &&
          infinite.SubFrame() == 0.0f);
    const FrameTime negativeInfinite = one.AsFrameTime(-platform::PlatformMath::Infinity<double>());
    CHECK(negativeInfinite.Frame() == FrameNumber(NumericLimits<int32>::Min()) &&
          negativeInfinite.SubFrame() == 0.0f);

    // 가장 큰 프레임의 초: int32 최댓값 × 1001은 int64로 계산한다.
    const FrameTime last{FrameNumber(NumericLimits<int32>::Max())};
    CHECK(ntsc.AsSeconds(last) == static_cast<double>(2147483647ll * 1001ll) / 30000.0);
}

TEST(FrameRate_SubframeClampMatchesUe) {
    // (subframe + 0.5) − 0.5로 아주 작은 값은 0이 되고, [0, MaxSubframe]로 자른다. NaN은
    // MaxSubframe(UE Clamp).
    CHECK(FrameTime(FrameNumber(3), 1.0e-9f).SubFrame() == 0.0f);
    CHECK(FrameTime(FrameNumber(3), -0.5f).SubFrame() == 0.0f);
    CHECK(FrameTime(FrameNumber(3), 1.5f).SubFrame() == FrameTime::MaxSubframe);
    CHECK(FrameTime(FrameNumber(3), 1.0f).SubFrame() == FrameTime::MaxSubframe);
    CHECK(FrameTime(FrameNumber(3), platform::PlatformMath::QuietNaN<float>()).SubFrame() ==
          FrameTime::MaxSubframe);
    CHECK(FrameTime(FrameNumber(3), 0.25f).SubFrame() == 0.25f);
    CHECK(FrameTime::MaxSubframe < 1.0f);
    CHECK(platform::BitCast<uint32>(FrameTime::MaxSubframe) + 1u ==
          platform::BitCast<uint32>(1.0f));
}

TEST(FrameRate_ConversionsMatchUeFormulas) {
    const int32 rates[][2] = {{30, 1},       {24, 1},  {60000, 1}, {30000, 1001},
                              {24000, 1001}, {120, 1}, {1, 3}};
    MathRandom random(0x4001);
    int32 mismatches = 0;
    for (const auto& pair : rates) {
        const FrameRate rate = FrameRate::TryCreate(pair[0], pair[1]).Value();
        for (int32 index = 0; index < 20000; ++index) {
            double seconds;
            const uint64 pick = random.Next() % 4;
            if (pick == 0) {
                seconds = random.Range(-10.0, 10.0);
            } else if (pick == 1) {
                seconds = random.Range(-1.0e5, 1.0e5);
            } else if (pick == 2) {
                // 프레임 경계 근처.
                seconds =
                    (static_cast<double>(random.Next() % 10000) + random.Range(-1.0e-9, 1.0e-9)) *
                    rate.AsInterval();
            } else {
                seconds = static_cast<double>(static_cast<float>(random.Range(-100.0, 100.0)));
            }

            const FrameTime rocks = rate.AsFrameTime(seconds);
            const UeFrameTime ue = UeAsFrameTime(pair[0], pair[1], seconds);
            if (rocks.Frame().value != ue.frame || !SameFloat(rocks.SubFrame(), ue.subFrame)) {
                ++mismatches;
                continue;
            }

            if (!SameDouble(rate.AsSeconds(rocks), UeAsSeconds(pair[0], pair[1], ue))) {
                ++mismatches;
            }
        }
    }

    CHECK(mismatches == 0);
}

TEST(FrameRate_FrameNumberAndOrdering) {
    FrameNumber frame(5);
    ++frame;
    CHECK(frame == FrameNumber(6));
    frame += FrameNumber(4);
    frame -= FrameNumber(2);
    CHECK(frame.value == 8);
    CHECK(FrameNumber(1) + FrameNumber(2) == FrameNumber(3));
    CHECK(FrameNumber(1) - FrameNumber(2) == FrameNumber(-1));
    CHECK(FrameNumber(NumericLimits<int32>::Max()).TryAdd(FrameNumber(1)).Error() ==
          ErrorCode::Overflow);
    CHECK(FrameNumber(NumericLimits<int32>::Min()).TrySubtract(FrameNumber(1)).Error() ==
          ErrorCode::Overflow);
    CHECK(FrameNumber(1) < FrameNumber(2) && FrameNumber(2) <= FrameNumber(2) &&
          FrameNumber(3) > FrameNumber(2));

    // FrameTime은 프레임 다음 subframe으로 비교한다.
    const FrameTime a(FrameNumber(3), 0.25f);
    const FrameTime b(FrameNumber(3), 0.5f);
    const FrameTime c(FrameNumber(4));
    CHECK(a < b && b < c && a <= a && c > a && c >= b);
    CHECK(a == FrameTime(FrameNumber(3), 0.25f) && !(a == b));

    FrameTime moved = a;
    moved.SetFrame(FrameNumber(10));
    CHECK(moved.Frame() == FrameNumber(10) && moved.SubFrame() == 0.25f);
}
} // namespace frame_rate_tests_file

} // namespace rocks::test
