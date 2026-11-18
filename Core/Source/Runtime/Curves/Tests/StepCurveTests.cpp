#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Curves/StepCurves.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 계단 커브의 독립 고정 벡터·편집 시험이다(Docs/Design/CurveData.ko.md 5.8·7절 — CV6): 평가 규칙(키
// 없음·첫 키 이전·같은 시간 키·마지막 키 이후·NaN), useDefaultBeforeFirstKey, String·Name의 첫 키
// 이전 기본값과 빈 문자열 기본값, Name 대소문자 같음, FindKeyAtOrBefore, 편집 연산의 거부와 할당
// 실패 뒤 불변, 중복 키 제거(남은 키를 기본값으로 바꾸지 않는다).
using namespace rocks;

namespace {

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

Name Intern(NamePool& pool, const char* text) noexcept {
    auto interned = pool.TryIntern(StringView(text));
    CHECK(interned);
    return interned ? interned.Value() : Name{};
}

} // namespace

// 키 (1, 10)·(2, 20)·(2, 21)·(4, 40): 키 없음은 기본값(없으면 fallback), 첫 키 앞은 첫 키
// 값(useDefaultBeforeFirstKey면 기본값), 같은 시간은 그 시간의 마지막 키(21), 마지막 키 시간부터는
// 마지막 키 값, NaN은 기본값이다.
TEST(StepCurve_IntegerEvaluateVectors) {
    SystemAllocator allocator;
    IntegerStepCurve curve(allocator);
    CHECK(curve.Evaluate(1.0f, 5) == 5);
    curve.defaultValue = 7;
    CHECK(curve.Evaluate(1.0f, 5) == 7);
    REQUIRE(curve.TryAddKey(1.0f, 10));
    REQUIRE(curve.TryAddKey(2.0f, 20));
    REQUIRE(curve.TryAddKey(4.0f, 40));
    REQUIRE(curve.TryAddKey(2.0f, 21)); // 같은 시간 키 앞
    REQUIRE(curve.TrySetKeyTime(1, 2.0f));
    CHECK(curve.Keys()[1].value == 21 && curve.Keys()[2].value == 20);
    CHECK(curve.Evaluate(0.0f) == 10 && curve.Evaluate(1.0f) == 10 && curve.Evaluate(1.5f) == 10);
    CHECK(curve.Evaluate(2.0f) == 20 && curve.Evaluate(3.9f) == 20);
    CHECK(curve.Evaluate(4.0f) == 40 && curve.Evaluate(100.0f) == 40);
    CHECK(curve.Evaluate(platform::PlatformMath::QuietNaN<float>(), 5) == 7);
    CHECK(curve.Evaluate(-platform::PlatformMath::Infinity<float>()) == 10);
    CHECK(curve.Evaluate(platform::PlatformMath::Infinity<float>()) == 40);
    curve.useDefaultBeforeFirstKey = true;
    CHECK(curve.Evaluate(0.0f) == 7 && curve.Evaluate(1.0f) == 10);
    curve.defaultValue = Optional<int32>{};
    CHECK(curve.Evaluate(0.0f, -3) == -3);

    CHECK(curve.FindKeyAtOrBefore(0.5f) == IndexNone && curve.FindKeyAtOrBefore(2.0f) == 2);
    CHECK(curve.FindKeyAtOrBefore(9.0f) == 3);
    CHECK(curve.FindKeyAtOrBefore(platform::PlatformMath::QuietNaN<float>()) == IndexNone);
    CHECK(curve.FindKey(2.00005f) == 1 && curve.FindKey(3.0f) == IndexNone);
    auto range = curve.TryGetTimeRange();
    REQUIRE(range);
    CHECK(range.Value().min == 1.0f && range.Value().max == 4.0f);
}

// String·Name은 첫 키 이전이 늘 기본값이다(UE FStringCurve::Eval, Name은 D9). String은 빈 문자열도
// 기본값이다(UE는 빈 문자열이 "없음"). Name은 대소문자만 다른 이름이 같다.
TEST(StepCurve_StringAndNameEvaluateVectors) {
    SystemAllocator allocator;
    StringStepCurve strings(allocator);
    CHECK(strings.Evaluate(1.0f, "fallback") == StringView("fallback"));
    String empty(allocator);
    strings.defaultValue.Emplace(Move(empty));
    CHECK(strings.Evaluate(1.0f, "fallback").ByteCount() == 0);
    REQUIRE(strings.TryAddKey(1.0f, "Intro"));
    REQUIRE(strings.TryAddKey(3.0f, "Outro"));
    CHECK(strings.Evaluate(0.5f, "fallback").ByteCount() == 0);
    CHECK(strings.Evaluate(1.0f) == StringView("Intro") &&
          strings.Evaluate(2.9f) == StringView("Intro"));
    CHECK(strings.Evaluate(3.0f) == StringView("Outro") &&
          strings.Evaluate(9.0f) == StringView("Outro"));
    CHECK(strings.Evaluate(platform::PlatformMath::QuietNaN<float>()).ByteCount() == 0);
    REQUIRE(strings.TrySetKeyValue(0, "Start"));
    CHECK(strings.Evaluate(1.5f) == StringView("Start"));

    NamePool pool(allocator);
    REQUIRE(pool.Initialize());
    {
        NameStepCurve names(allocator);
        const Name idle = Intern(pool, "Idle");
        const Name walk = Intern(pool, "Walk");
        const Name lowerWalk = Intern(pool, "walk");
        CHECK(names.Evaluate(1.0f, idle) == idle);
        REQUIRE(names.TryAddKey(1.0f, walk));
        CHECK(names.Evaluate(0.0f, idle) == idle && names.Evaluate(1.0f, idle) == walk);
        names.defaultValue = lowerWalk;
        CHECK(names.Evaluate(0.0f, idle) == walk); // 대소문자만 다른 이름은 같다
        REQUIRE(names.TryAddKey(2.0f, lowerWalk));
        REQUIRE(names.TryAddKey(3.0f, walk));
        REQUIRE(names.TryRemoveRedundantKeys());
        // 가운데 "walk"는 앞뒤와 같은 Name이지만 표시 철자가 달라 남긴다(지우면 평가 결과의 철자가
        // 바뀐다 — CV6·CV7 리뷰 6).
        CHECK(names.KeyCount() == 3);
    }

    CHECK(pool.Shutdown());
}

// 중복 키 제거: 앞·뒤 키와 값이 같은 안쪽 키만 지운다(평가 결과는 그대로). 남은 키 하나를
// 기본값으로 바꾸지 않는다(UE는 바꾼다 — 15절 9). String 같음은 byte라 대소문자만 다른 값은 남긴다.
TEST(StepCurve_RemoveRedundantKeys) {
    SystemAllocator allocator;
    IntegerStepCurve curve(allocator);
    const IntegerStepKey keys[] = {{0.0f, 1}, {1.0f, 1}, {2.0f, 1}, {3.0f, 2},
                                   {4.0f, 2}, {5.0f, 2}, {6.0f, 2}, {7.0f, 3}};
    REQUIRE(curve.TrySetKeys(keys));
    REQUIRE(curve.TryRemoveRedundantKeys());
    REQUIRE(curve.KeyCount() == 5);
    const float times[] = {0.0f, 2.0f, 3.0f, 6.0f, 7.0f};
    for (usize index = 0; index < 5; ++index) {
        CHECK(curve.Keys()[index].time == times[index]);
    }

    IntegerStepCurve single(allocator);
    const IntegerStepKey same[] = {{0.0f, 4}, {1.0f, 4}, {2.0f, 4}};
    REQUIRE(single.TrySetKeys(same));
    single.useDefaultBeforeFirstKey = true;
    REQUIRE(single.TryRemoveRedundantKeys());
    CHECK(single.KeyCount() == 2 && !single.defaultValue.HasValue());
    CHECK(single.Evaluate(-1.0f, 9) == 9 && single.Evaluate(5.0f, 9) == 4);

    StringStepCurve strings(allocator);
    REQUIRE(strings.TryAddKey(0.0f, "walk"));
    REQUIRE(strings.TryAddKey(1.0f, "Walk"));
    REQUIRE(strings.TryAddKey(2.0f, "walk"));
    REQUIRE(strings.TryRemoveRedundantKeys());
    CHECK(strings.KeyCount() == 3);
}

// 편집 거부(비유한 시간·범위 밖 index·비정렬 SetKeys·음수 허용오차)는 InvalidArgument이고 그대로다.
// 선택 키 이동·Shift·Scale은 TangentCurve와 같은 규칙이다. 복사는 기본값·키를 복제한다.
TEST(StepCurve_EditRules) {
    SystemAllocator allocator;
    StringStepCurve curve(allocator);
    REQUIRE(curve.TryAddKey(0.0f, "a"));
    REQUIRE(curve.TryAddKey(1.0f, "b"));
    REQUIRE(curve.TryAddKey(2.0f, "c"));
    CHECK(ErrorOf(curve.TryAddKey(platform::PlatformMath::QuietNaN<float>(), "x")) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryUpdateOrAddKey(1.0f, "x", -1.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryRemoveKey(3)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TrySetKeyValue(3, "x")) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TrySetKeyTime(0, platform::PlatformMath::Infinity<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(curve.KeyCount() == 3);

    auto updated = curve.TryUpdateOrAddKey(1.00005f, "B");
    REQUIRE(updated);
    CHECK(updated.Value() == 1 && curve.Keys()[1].value.View() == StringView("B"));
    const usize selection[] = {0};
    REQUIRE(curve.TryShiftKeyTimes(selection, 1.5f));
    CHECK(curve.Keys()[0].value.View() == StringView("B") &&
          curve.Keys()[1].value.View() == StringView("a") && curve.Keys()[1].time == 1.5f);
    REQUIRE(curve.TryScaleTime(0.0f, 2.0f));
    CHECK(curve.Keys()[2].time == 4.0f);
    REQUIRE(curve.TryShiftTime(-1.0f));
    CHECK(curve.Keys()[0].time == 1.0f);

    StringStepCurve copy(allocator);
    String label(allocator);
    REQUIRE(label.TryAssign("none"));
    curve.defaultValue.Emplace(Move(label));
    REQUIRE(copy.TryCopyFrom(curve));
    REQUIRE(copy.KeyCount() == 3 && copy.defaultValue.HasValue());
    CHECK(copy.defaultValue.Value().View() == StringView("none") &&
          copy.Keys()[2].value.View() == StringView("c"));

    StringStepCurve unsorted(allocator);
    StringStepKey late(allocator);
    late.time = 2.0f;
    StringStepKey early(allocator);
    early.time = 1.0f;
    StringStepKey both[] = {Move(late), Move(early)};
    CHECK(ErrorOf(unsorted.TrySetKeys(Span<const StringStepKey>(both, 2))) ==
          ErrorCode::InvalidArgument);
    CHECK(unsorted.KeyCount() == 0);
    StringStepKey swapped = Move(both[0]);
    both[0] = Move(both[1]);
    both[1] = Move(swapped);
    REQUIRE(unsorted.TrySetKeys(Span<const StringStepKey>(both, 2)));
    CHECK(unsorted.KeyCount() == 2 && unsorted.Keys()[0].time == 1.0f);
}

// String 키 편집이 할당에 실패하면(시도마다 실패 주입) OutOfMemory이고 커브가 그대로다.
TEST(StepCurve_StringAllocationFailureLeavesCurveUnchanged) {
    SystemAllocator system;
    using Operation = Result<void> (*)(StringStepCurve&, const StringStepCurve&);
    const Operation operations[] = {
        [](StringStepCurve& curve, const StringStepCurve&) noexcept {
            auto added = curve.TryAddKey(0.5f, "a fairly long value that needs its own buffer");
            return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
        },
        [](StringStepCurve& curve, const StringStepCurve&) noexcept {
            auto updated = curve.TryUpdateOrAddKey(1.0f, "another fairly long replacement value");
            return updated ? Result<void>::Success() : Result<void>::Failure(updated.Error());
        },
        [](StringStepCurve& curve, const StringStepCurve&) noexcept {
            return curve.TrySetKeyValue(0, "yet another fairly long replacement value");
        },
        [](StringStepCurve& curve, const StringStepCurve& other) noexcept {
            return curve.TryCopyFrom(other);
        },
        [](StringStepCurve& curve, const StringStepCurve& other) noexcept {
            return curve.TrySetKeys(other.Keys());
        },
        [](StringStepCurve& curve, const StringStepCurve&) noexcept {
            const usize picked[] = {0};
            return curve.TryShiftKeyTimes(picked, 3.0f);
        },
    };
    for (const Operation operation : operations) {
        bool injected = false;
        for (uint64 attempt = 1; attempt <= 6; ++attempt) {
            test::NFailAllocator failing(system);
            StringStepCurve curve(failing);
            REQUIRE(curve.TryAddKey(0.0f, "first value with enough bytes"));
            REQUIRE(curve.TryAddKey(1.0f, "second value with enough bytes"));
            StringStepCurve other(system);
            REQUIRE(other.TryAddKey(5.0f, "other curve value with enough bytes"));
            REQUIRE(other.TryAddKey(6.0f, "other curve second value"));
            failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
            const Result<void> result = operation(curve, other);
            failing.DisableFailure();
            if (!result) {
                injected = true;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                REQUIRE(curve.KeyCount() == 2);
                CHECK(curve.Keys()[0].time == 0.0f &&
                      curve.Keys()[0].value.View() == StringView("first value with enough bytes"));
                CHECK(curve.Keys()[1].value.View() == StringView("second value with enough bytes"));
            }
        }

        CHECK(injected); // 실패가 실제로 주입됐다
    }
}

// 리뷰 6 재현: Name 중복 제거는 표시 철자가 다른 키를 남긴다(평가 결과의 철자가 바뀌지 않는다 —
// 키 핸들 표의 Name 비교와 같은 판단).
TEST(StepCurve_NameRedundantKeepsDisplaySpelling) {
    SystemAllocator allocator;
    NamePool pool(allocator);
    REQUIRE(pool.Initialize());
    {
        NameStepCurve curve(allocator);
        REQUIRE(curve.TryAddKey(0.0f, Intern(pool, "Walk")));
        REQUIRE(curve.TryAddKey(1.0f, Intern(pool, "walk")));
        REQUIRE(curve.TryAddKey(2.0f, Intern(pool, "Walk")));
        REQUIRE(curve.TryAddKey(3.0f, Intern(pool, "Walk")));
        REQUIRE(curve.TryAddKey(4.0f, Intern(pool, "Walk")));
        REQUIRE(curve.TryRemoveRedundantKeys());
        REQUIRE(curve.KeyCount() == 4);
        auto lease = pool.Acquire(curve.Evaluate(1.5f));
        CHECK(lease && lease.Value().View() == StringView("walk"));
        CHECK(curve.Keys()[3].time == 4.0f);
    }

    CHECK(pool.Shutdown());
}

// String 중복 제거의 할당 실패: 지울 키가 있어야 할당하므로(지울 표·새 키 배열·남은 index 목록)
// 키를 늘려 실제로 할당하게 한다. 실패하면 OutOfMemory이고 커브(와 남은 index 목록)가 그대로다
// (주입 단언).
TEST(StepCurve_StringRemoveRedundantAllocationFailure) {
    SystemAllocator system;
    const char* const values[] = {"repeated value long enough to allocate",
                                  "repeated value long enough to allocate",
                                  "repeated value long enough to allocate", "the last value"};
    for (int32 withIndices = 0; withIndices < 2; ++withIndices) {
        bool injected = false;
        for (uint64 attempt = 1; attempt <= 6; ++attempt) {
            test::NFailAllocator failing(system);
            StringStepCurve curve(failing);
            for (usize index = 0; index < 4; ++index) {
                REQUIRE(curve.TryAddKey(static_cast<float>(index), values[index]));
            }

            Array<usize> kept(failing);
            REQUIRE(kept.TryPushBack(usize{77}));
            failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
            const Result<void> result = withIndices != 0 ? curve.TryRemoveRedundantKeys(kept)
                                                         : curve.TryRemoveRedundantKeys();
            failing.DisableFailure();
            if (!result) {
                injected = true;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                REQUIRE(curve.KeyCount() == 4);
                for (usize index = 0; index < 4; ++index) {
                    CHECK(curve.Keys()[index].time == static_cast<float>(index) &&
                          curve.Keys()[index].value.View() == StringView(values[index]));
                }

                CHECK(kept.Size() == 1 && kept[0] == 77);
            } else {
                REQUIRE(curve.KeyCount() == 3);
                if (withIndices != 0) {
                    REQUIRE(kept.Size() == 3);
                    CHECK(kept[0] == 0 && kept[1] == 2 && kept[2] == 3);
                }
            }
        }

        CHECK(injected); // 실패가 실제로 주입됐다
    }
}
