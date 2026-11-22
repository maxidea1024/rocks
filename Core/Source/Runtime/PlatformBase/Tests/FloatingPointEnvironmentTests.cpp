#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace floating_point_environment_tests_file {

using namespace rocks::platform;

// 결정적 계산 실행(RunWithDeterministicFloatingPoint)이 비정규 수를 0으로 만드는
// 설정(FTZ·DAZ·FZ)·반올림 모드·예외 mask를 바로잡고, 끝나면 제어 설정을 되돌리며 상태 플래그는
// 남기는지 본다.
namespace {

// 비정규 수끼리의 곱: FTZ가 꺼져 있으면 정확한 비정규 결과, 켜져 있으면 0이다.
// volatile 지역 변수로 컴파일 시점 계산을 막아 실행 중 환경에서 곱한다.
double SubnormalProduct(double left, double right) noexcept {
    volatile double a = left;
    volatile double b = right;
    return a * b;
}

// 반올림 모드에 따라 달라지는 나눗셈(1/3: nearest와 toward-zero의 결과 비트가 다르다).
double OneThird() noexcept {
    volatile double one = 1.0;
    volatile double three = 3.0;
    return one / three;
}

const double MinimumNormal = BitCast<double>(uint64{0x0010000000000000});

struct Observation {
    bool deterministic = false;
    double product = 0.0;
    double third = 0.0;
};

void Observe(void* context) noexcept {
    auto& observation = *static_cast<Observation*>(context);
    observation.deterministic = IsDeterministicFloatingPointEnvironment();
    observation.product = SubnormalProduct(MinimumNormal, 0.5);
    observation.third = OneThird();
}

// 현재 제어 설정을 control로 바꾼 채 실행하고, 실행 뒤 제어 값과 함께 돌려준다. 원래 설정은
// 복원한다.
struct RunResult {
    bool changed = false;
    Observation inside;
    uint64 after = 0;
};

RunResult RunFrom(uint64 control) noexcept {
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(control);
    RunResult result;
    result.changed = RunWithDeterministicFloatingPoint(&Observe, &result.inside);
    result.after = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original);
    return result;
}

uint64 WithoutStatus(uint64 control) noexcept {
    return control & ~platform::detail::StatusFlagBits();
}

} // namespace

TEST(FloatingPointEnvironment_DefaultProcessIsDeterministic) {
    CHECK(IsDeterministicFloatingPointEnvironment());
    Observation inside;
    CHECK(!RunWithDeterministicFloatingPoint(&Observe, &inside));
    CHECK(inside.deterministic && inside.product != 0.0);
    CHECK(IsDeterministicFloatingPointEnvironment());
}

TEST(FloatingPointEnvironment_RunCorrectsFlushDenormalsAndRounding) {
    const uint64 original = platform::detail::ReadFloatingPointControl();
    const double expectedProduct = SubnormalProduct(MinimumNormal, 0.5);
    const double expectedThird = OneThird();
    CHECK(expectedProduct != 0.0);

    // 다른 코드가 켜 놓았을 수 있는 설정마다: 실행 안에서는 기대 환경·기본 결과, 실행 뒤에는 원래
    // 설정.
    const uint64 disturbed[] = {original | platform::detail::FlushToZeroBit(),
                                original | platform::detail::DenormalsAreZeroBit(),
                                original | platform::detail::RoundTowardZeroBits(),
                                original | platform::detail::FlushToZeroBit() |
                                    platform::detail::RoundTowardZeroBits()};
    for (uint64 control : disturbed) {
        CHECK(WithoutStatus(control) != WithoutStatus(original));
        const RunResult result = RunFrom(control);
        CHECK(result.changed && result.inside.deterministic);
        CHECK(BitCast<uint64>(result.inside.product) == BitCast<uint64>(expectedProduct));
        CHECK(BitCast<uint64>(result.inside.third) == BitCast<uint64>(expectedThird));
        CHECK(WithoutStatus(result.after) == WithoutStatus(control));
    }

    CHECK(IsDeterministicFloatingPointEnvironment());
}

TEST(FloatingPointEnvironment_RunRestoresControlChangedInside) {
    // 안에서 누가 FTZ를 켜도 끝나면 호출 전 설정(여기서는 기대 환경)으로 돌아간다.
    const bool changed = RunWithDeterministicFloatingPoint(
        [](void*) noexcept {
            platform::detail::WriteFloatingPointControl(
                platform::detail::ReadFloatingPointControl() | platform::detail::FlushToZeroBit());
        },
        nullptr);
    CHECK(!changed);
    CHECK(IsDeterministicFloatingPointEnvironment());
}

TEST(FloatingPointEnvironment_RunNestsAndKeepsStatusFlags) {
    const uint64 original = platform::detail::ReadFloatingPointControl();
    // 중첩: 바깥이 환경을 바꾸면 안쪽은 바꿀 것이 없다.
    struct Nested {
        bool innerChanged = true;
        bool innerDeterministic = false;
    } nested;
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit());
    const bool outerChanged = RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            auto& state = *static_cast<Nested*>(context);
            Observation inner;
            state.innerChanged = RunWithDeterministicFloatingPoint(&Observe, &inner);
            state.innerDeterministic = inner.deterministic;
        },
        &nested);
    const uint64 afterNested = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original);
    CHECK(outerChanged && !nested.innerChanged && nested.innerDeterministic);
    CHECK((afterNested & platform::detail::FlushToZeroBit()) != 0);

    // 상태 플래그(x64 MXCSR bit 0–5): 실행 중 1/3이 켠 부정확(PE) 플래그가 실행 뒤에도 남는다.
    // ARM64는 상태 플래그가 FPSR에 있어 FPCR 복원과 무관하다.
    if (platform::detail::StatusFlagBits() != 0) {
        platform::detail::WriteFloatingPointControl(WithoutStatus(original) |
                                                    platform::detail::FlushToZeroBit());
        const RunResult result =
            RunFrom(WithoutStatus(original) | platform::detail::FlushToZeroBit());
        CHECK((result.after & platform::detail::StatusFlagBits()) != 0);
        CHECK(WithoutStatus(result.after) ==
              (WithoutStatus(original) | platform::detail::FlushToZeroBit()));
        platform::detail::WriteFloatingPointControl(original);
    }

    CHECK(IsDeterministicFloatingPointEnvironment());
}

TEST(FloatingPointEnvironment_RealtimeAudioFlushesAndRestores) {
    // 실시간 오디오 실행: 안에서는 기준 환경 + FTZ(비정규 결과 0, nearest 유지), 끝나면 원래
    // 설정이다.
    struct Inside {
        bool realtime = false;
        bool deterministic = true;
        double product = 1.0;
        double third = 0.0;
    };

    const uint64 original = platform::detail::ReadFloatingPointControl();
    const double expectedThird = OneThird();
    CHECK(!IsRealtimeAudioFloatingPointEnvironment());
    const uint64 disturbed[] = {original, original | platform::detail::RoundTowardZeroBits(),
                                original | platform::detail::DenormalsAreZeroBit()};
    for (uint64 control : disturbed) {
        platform::detail::WriteFloatingPointControl(control);
        Inside inside;
        RunWithRealtimeAudioFloatingPoint(
            [](void* context) noexcept {
                auto& state = *static_cast<Inside*>(context);
                state.realtime = IsRealtimeAudioFloatingPointEnvironment();
                state.deterministic = IsDeterministicFloatingPointEnvironment();
                state.product = SubnormalProduct(MinimumNormal, 0.5);
                state.third = OneThird();
            },
            &inside);
        const uint64 after = platform::detail::ReadFloatingPointControl();
        platform::detail::WriteFloatingPointControl(original);
        CHECK(inside.realtime && !inside.deterministic);
        CHECK(inside.product == 0.0);
        CHECK(BitCast<uint64>(inside.third) == BitCast<uint64>(expectedThird));
        CHECK(WithoutStatus(after) == WithoutStatus(control));
    }

    // 결정적 실행 안에서 부르면 FTZ가 켜지고, 끝나면 결정적 환경으로 돌아간다.
    struct Nested {
        bool inner = false;
        bool restored = false;
    } nested;
    RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            auto& state = *static_cast<Nested*>(context);
            RunWithRealtimeAudioFloatingPoint(
                [](void* innerContext) noexcept {
                    *static_cast<bool*>(innerContext) = IsRealtimeAudioFloatingPointEnvironment();
                },
                &state.inner);
            state.restored = IsDeterministicFloatingPointEnvironment();
        },
        &nested);
    CHECK(nested.inner && nested.restored);
    CHECK(IsDeterministicFloatingPointEnvironment());
}

} // namespace floating_point_environment_tests_file
