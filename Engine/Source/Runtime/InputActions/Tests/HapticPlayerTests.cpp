#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <InputActions/Haptics/HapticPlayer.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 게임패드 진동 재생·합성 시험이다(Docs/Design/InputActions 6.1–6.3절 — IA5): 커브 평가·길이·반복·
// tag 교체·멈춤 규칙·pause·최댓값 합성·배율·끔, 동적 진동(시작·갱신·멈춤·길이·pause), 오류·할당
// 실패.
using namespace rocks;
using namespace rocks::input;
namespace haptic_player_tests_file {

namespace {

constexpr uint64 IdHigh = 0x48415054;
constexpr uint64 MaxAllocationAttempts = 256;
const float NotANumber = static_cast<float>(nan(""));
const float Infinity = static_cast<float>(HUGE_VAL);

Guid Id(uint64 low) noexcept {
    return Guid{IdHigh, low};
}

struct Key {
    float time;
    float value;
};

// 직선 키 커브 채널 하나짜리 효과다(affects는 네 모터 모두 — 기본값).
bool AddChannel(HapticEffect& effect, Allocator& allocator, std::initializer_list<Key> keys,
                bool leftLarge = true, bool leftSmall = true, bool rightLarge = true,
                bool rightSmall = true) noexcept {
    HapticChannel channel(allocator);
    channel.affectsLeftLarge = leftLarge;
    channel.affectsLeftSmall = leftSmall;
    channel.affectsRightLarge = rightLarge;
    channel.affectsRightSmall = rightSmall;
    Array<TangentCurveKey> list(allocator);
    for (const Key& key : keys) {
        TangentCurveKey curveKey;
        curveKey.time = key.time;
        curveKey.value = key.value;
        curveKey.interp = CurveInterp::Linear;
        if (!list.TryPushBack(curveKey)) {
            return false;
        }
    }

    return channel.curve.TrySetKeys(list.View()).IsOk() &&
           effect.channels.TryPushBack(rocks::Move(channel)).IsOk();
}

HapticUpdateContext Step(double seconds, bool paused = false) noexcept {
    HapticUpdateContext context;
    context.deltaSeconds = seconds;
    context.unscaledDeltaSeconds = seconds;
    context.paused = paused;
    return context;
}

bool MotorsAre(const MotorValues& values, float leftLarge, float leftSmall, float rightLarge,
               float rightSmall) noexcept {
    const bool same = values.leftLarge == leftLarge && values.leftSmall == leftSmall &&
                      values.rightLarge == rightLarge && values.rightSmall == rightSmall;
    if (!same) {
        printf("  motors: %g %g %g %g\n", static_cast<double>(values.leftLarge),
               static_cast<double>(values.leftSmall), static_cast<double>(values.rightLarge),
               static_cast<double>(values.rightSmall));
    }

    return same;
}

} // namespace

// 6.1절: 길이는 채널 마지막 키 중 최댓값(키 없는 채널 0, 채널 없음 0), 값은 모터마다 최댓값이다.
TEST(HapticEffect_DurationAndAccumulation) {
    SystemAllocator allocator;
    HapticEffect effect(allocator);
    CHECK(effect.Duration() == 0.0f);
    REQUIRE(AddChannel(effect, allocator, {{0.0f, 1.0f}, {1.0f, 0.0f}}, true, false, false, false));
    REQUIRE(
        AddChannel(effect, allocator, {{0.0f, 0.25f}, {2.0f, 0.25f}}, true, true, false, false));
    REQUIRE(AddChannel(effect, allocator, {}));
    CHECK(effect.Duration() == 2.0f);

    MotorValues values;
    effect.AccumulateValues(0.25f, values);
    // 왼쪽 큰: max(0.75, 0.25), 왼쪽 작은: 0.25, 오른쪽: 키 없는 채널(0)뿐.
    CHECK(MotorsAre(values, 0.75f, 0.25f, 0.0f, 0.0f));
    // 이미 큰 값은 그대로, 1 넘는 값은 1, 음수는 진다.
    HapticEffect loud(allocator);
    REQUIRE(AddChannel(loud, allocator, {{0.0f, 3.0f}}, false, true, false, false));
    REQUIRE(AddChannel(loud, allocator, {{0.0f, -1.0f}}, false, false, true, false));
    loud.AccumulateValues(0.0f, values);
    CHECK(MotorsAre(values, 0.75f, 1.0f, 0.0f, 0.0f));
}

// 6.2절: 재생 시각 = 흐른 시간, 길이를 넘으면 끝(반복이 아니면). 반복은 나머지 시각, 길이 0은 바로
// 끝난다. ignoreTimeDilation은 unscaled dt다.
TEST(HapticPlayer_PlaybackTimeLoopAndDilation) {
    SystemAllocator allocator;
    HapticPlayer player(allocator);
    HapticEffect effect(allocator);
    effect.id = Id(1);
    REQUIRE(AddChannel(effect, allocator, {{0.0f, 1.0f}, {1.0f, 0.0f}}));
    REQUIRE(player.PlayHapticEffect(effect, {}));
    CHECK(MotorsAre(player.Update(Step(0.25)), 0.75f, 0.75f, 0.75f, 0.75f));
    CHECK(MotorsAre(player.Update(Step(0.25)), 0.5f, 0.5f, 0.5f, 0.5f));
    // 정확히 길이: 평가 시각 0(UE 식) → 1.
    CHECK(MotorsAre(player.Update(Step(0.5)), 1.0f, 1.0f, 1.0f, 1.0f));
    CHECK(player.ActiveEffectCount() == 1);
    CHECK(MotorsAre(player.Update(Step(0.25)), 0.0f, 0.0f, 0.0f, 0.0f));
    CHECK(player.ActiveEffectCount() == 0);

    // 반복: 1.25초 → 0.25초 자리.
    HapticParams looping;
    looping.looping = true;
    REQUIRE(player.PlayHapticEffect(effect, looping));
    (void)player.Update(Step(1.0));
    CHECK(MotorsAre(player.Update(Step(0.25)), 0.75f, 0.75f, 0.75f, 0.75f));
    CHECK(player.ActiveEffectCount() == 1);
    player.StopHapticEffect();

    // 길이 0 효과는 반복이어도 첫 갱신에서 끝난다(0초 갱신은 값을 낸다).
    HapticEffect instant(allocator);
    instant.id = Id(2);
    REQUIRE(AddChannel(instant, allocator, {{0.0f, 0.5f}}));
    REQUIRE(player.PlayHapticEffect(instant, looping));
    CHECK(MotorsAre(player.Update(Step(0.0)), 0.5f, 0.5f, 0.5f, 0.5f));
    (void)player.Update(Step(0.125));
    CHECK(player.ActiveEffectCount() == 0);

    // 시간 배율: 게임 dt는 반, ignoreTimeDilation은 실제 dt.
    HapticParams real;
    real.ignoreTimeDilation = true;
    REQUIRE(player.PlayHapticEffect(effect, {}));
    HapticEffect other(allocator);
    other.id = Id(3);
    REQUIRE(AddChannel(other, allocator, {{0.0f, 1.0f}, {1.0f, 0.0f}}, false, false, true, true));
    REQUIRE(player.PlayHapticEffect(other, real));
    HapticUpdateContext dilated;
    dilated.deltaSeconds = 0.25;
    dilated.unscaledDeltaSeconds = 0.5;
    // 왼쪽(게임 dt 0.25 → 0.75), 오른쪽(실제 0.5 → 0.5). 효과 하나는 네 모터 모두이므로 오른쪽은
    // max(0.75, 0.5).
    CHECK(MotorsAre(player.Update(dilated), 0.75f, 0.75f, 0.75f, 0.75f));
    // effect를 멈추면 other만: 실제 1.0초(정확히 길이 → 평가 시각 0)라 오른쪽 1, 다음에 끝난다.
    player.StopHapticEffect(Id(1));
    CHECK(MotorsAre(player.Update(dilated), 0.0f, 0.0f, 1.0f, 1.0f));
    CHECK(player.ActiveEffectCount() == 1);
    (void)player.Update(dilated);
    CHECK(player.ActiveEffectCount() == 0);
}

// 6.2절: 같은 tag 재생은 교체, 멈춤은 effect·tag 조합 규칙, 무효 effect는 InvalidArgument.
TEST(HapticPlayer_TagsAndStopRules) {
    SystemAllocator allocator;
    HapticPlayer player(allocator);
    HapticEffect first(allocator);
    first.id = Id(10);
    REQUIRE(AddChannel(first, allocator, {{0.0f, 0.5f}, {10.0f, 0.5f}}));
    HapticEffect second(allocator);
    second.id = Id(11);
    REQUIRE(AddChannel(second, allocator, {{0.0f, 0.25f}, {10.0f, 0.25f}}));

    HapticEffect invalid(allocator);
    CHECK(player.PlayHapticEffect(invalid, {}).Error() == ErrorCode::InvalidArgument);

    HapticParams hit;
    hit.tag = "Hit";
    REQUIRE(player.PlayHapticEffect(first, hit));
    REQUIRE(player.PlayHapticEffect(second, hit));
    CHECK(player.ActiveEffectCount() == 1);
    CHECK(MotorsAre(player.Update(Step(0.0)), 0.25f, 0.25f, 0.25f, 0.25f));
    // 빈 tag는 교체하지 않는다.
    REQUIRE(player.PlayHapticEffect(first, {}));
    REQUIRE(player.PlayHapticEffect(first, {}));
    CHECK(player.ActiveEffectCount() == 3);

    // tag만: Hit 하나가 빠진다.
    player.StopHapticEffect({}, "Hit");
    CHECK(player.ActiveEffectCount() == 2);
    // effect와 tag 둘 다 맞아야 한다(Hit 아닌 first는 남는다).
    REQUIRE(player.PlayHapticEffect(second, hit));
    player.StopHapticEffect(Id(10), "Hit");
    CHECK(player.ActiveEffectCount() == 3);
    // effect만: first 둘이 빠진다.
    player.StopHapticEffect(Id(10));
    CHECK(player.ActiveEffectCount() == 1);
    // 둘 다 비면 모두.
    player.StopHapticEffect();
    CHECK(player.ActiveEffectCount() == 0);
}

// 6.3절: pause 중에는 playWhilePaused 효과만 갱신·기여하고 동적 진동은 멈춘다. 배율은 곱한 뒤
// [0, 1], 끄면 0이다(시간은 흐른다).
TEST(HapticPlayer_PauseScaleAndEnable) {
    SystemAllocator allocator;
    HapticPlayer player(allocator);
    HapticEffect ramp(allocator);
    ramp.id = Id(20);
    REQUIRE(AddChannel(ramp, allocator, {{0.0f, 1.0f}, {1.0f, 0.0f}}, true, false, false, false));
    HapticEffect menu(allocator);
    menu.id = Id(21);
    REQUIRE(AddChannel(menu, allocator, {{0.0f, 1.0f}, {1.0f, 0.0f}}, false, true, false, false));
    HapticParams whilePaused;
    whilePaused.playWhilePaused = true;
    REQUIRE(player.PlayHapticEffect(ramp, {}));
    REQUIRE(player.PlayHapticEffect(menu, whilePaused));
    DynamicHaptic rumble;
    rumble.intensity = 0.5f;
    rumble.duration = 1.0f;
    rumble.affectsLeftLarge = rumble.affectsLeftSmall = rumble.affectsRightSmall = false;
    REQUIRE(player.StartDynamicHaptic(rumble));

    CHECK(MotorsAre(player.Update(Step(0.25, true)), 0.0f, 0.75f, 0.0f, 0.0f));
    // pause가 풀리면 ramp는 멈췄던 자리(0.25초)부터, 동적 진동도 경과가 멈춰 있었다.
    CHECK(MotorsAre(player.Update(Step(0.25)), 0.75f, 0.5f, 0.5f, 0.0f));
    (void)player.Update(Step(0.5));
    // 동적: 경과 0.75 → 다음 0.25로 1.0 ≥ 1 → 끝.
    (void)player.Update(Step(0.25));
    CHECK(player.ActiveDynamicCount() == 0);
    player.StopAll();

    // 배율·끔.
    DynamicHaptic steady;
    steady.intensity = 0.5f;
    REQUIRE(player.StartDynamicHaptic(steady));
    HapticUpdateContext scaled = Step(0.1);
    scaled.scale = 4.0f;
    CHECK(MotorsAre(player.Update(scaled), 1.0f, 1.0f, 1.0f, 1.0f));
    scaled.scale = 0.5f;
    CHECK(MotorsAre(player.Update(scaled), 0.25f, 0.25f, 0.25f, 0.25f));
    scaled.scale = -1.0f;
    CHECK(MotorsAre(player.Update(scaled), 0.0f, 0.0f, 0.0f, 0.0f));
    scaled.scale = NotANumber;
    CHECK(MotorsAre(player.Update(scaled), 0.0f, 0.0f, 0.0f, 0.0f));
    HapticUpdateContext off = Step(0.1);
    off.enabled = false;
    CHECK(MotorsAre(player.Update(off), 0.0f, 0.0f, 0.0f, 0.0f));
    CHECK(player.ActiveDynamicCount() == 1);
}

// 6.2절 동적 진동: handle은 1부터, 갱신은 세기·모터만(경과·길이 그대로), 없는 handle은 NotFound,
// 비유한 값은 InvalidArgument, 음수 길이는 멈출 때까지.
TEST(HapticPlayer_DynamicHaptics) {
    SystemAllocator allocator;
    HapticPlayer player(allocator);
    DynamicHaptic details;
    details.intensity = 0.25f;
    details.duration = 0.5f;
    auto first = player.StartDynamicHaptic(details);
    REQUIRE(first);
    CHECK(first.Value() == 1);
    DynamicHaptic forever;
    forever.intensity = 0.125f;
    auto second = player.StartDynamicHaptic(forever);
    REQUIRE(second && second.Value() == 2);
    CHECK(MotorsAre(player.Update(Step(0.25)), 0.25f, 0.25f, 0.25f, 0.25f));

    // 갱신: 세기·모터만, 길이를 늘려도 무시된다.
    DynamicHaptic changed;
    changed.intensity = 0.75f;
    changed.duration = 100.0f;
    changed.affectsRightLarge = changed.affectsRightSmall = false;
    REQUIRE(player.UpdateDynamicHaptic(first.Value(), changed));
    CHECK(MotorsAre(player.Update(Step(0.125)), 0.75f, 0.75f, 0.125f, 0.125f));
    (void)player.Update(Step(0.125));
    CHECK(player.ActiveDynamicCount() == 1);
    CHECK(player.UpdateDynamicHaptic(first.Value(), changed).Error() == ErrorCode::NotFound);

    changed.intensity = Infinity;
    CHECK(player.UpdateDynamicHaptic(second.Value(), changed).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(player.StartDynamicHaptic(changed).Error() == ErrorCode::InvalidArgument);
    DynamicHaptic badDuration;
    badDuration.duration = NotANumber;
    CHECK(player.StartDynamicHaptic(badDuration).Error() == ErrorCode::InvalidArgument);

    // 음수 길이: 오래 흘러도 남는다. 멈춤(없는 handle은 아무 일 없음).
    (void)player.Update(Step(1000.0));
    CHECK(player.ActiveDynamicCount() == 1);
    player.StopDynamicHaptic(99);
    player.StopDynamicHaptic(InvalidDynamicHapticHandle);
    CHECK(player.ActiveDynamicCount() == 1);
    player.StopDynamicHaptic(second.Value());
    CHECK(player.ActiveDynamicCount() == 0);
    // handle은 다시 쓰지 않는다.
    auto third = player.StartDynamicHaptic(forever);
    REQUIRE(third && third.Value() == 3);
}

// 할당 실패: 재생(사본·tag·칸)·동적 시작은 OutOfMemory이고 그대로다(같은 tag 효과도 남는다).
TEST(HapticPlayer_AllocationFailuresKeepState) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    HapticPlayer player(failing);
    HapticEffect effect(allocator);
    effect.id = Id(30);
    REQUIRE(AddChannel(effect, allocator, {{0.0f, 0.5f}, {1.0f, 0.5f}}));
    HapticParams params;
    params.tag = "A Tag Long Enough To Need Heap Storage";
    REQUIRE(player.PlayHapticEffect(effect, params));
    uint32 failures = 0;
    bool played = false;
    for (uint64 attempt = 1; attempt < MaxAllocationAttempts && !played; ++attempt) {
        failing.FailOnAttempt(attempt);
        auto result = player.PlayHapticEffect(effect, params);
        failing.DisableFailure();
        if (result) {
            played = true;
        } else {
            ++failures;
            CHECK(result.Error() == ErrorCode::OutOfMemory);
            CHECK(player.ActiveEffectCount() == 1);
        }
    }

    CHECK(played && failures > 0 && player.ActiveEffectCount() == 1);

    failing.FailOnAttempt(1);
    auto started = player.StartDynamicHaptic({});
    failing.DisableFailure();
    CHECK(!started && started.Error() == ErrorCode::OutOfMemory);
    CHECK(player.ActiveDynamicCount() == 0);
    auto next = player.StartDynamicHaptic({});
    REQUIRE(next && next.Value() == 1);
}

} // namespace haptic_player_tests_file
