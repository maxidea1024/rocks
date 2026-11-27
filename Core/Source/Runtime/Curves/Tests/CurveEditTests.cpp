#include <Core/Memory/SystemAllocator.h>
#include <Curves/PlainCurve.h>
#include <Curves/TangentCurve.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 커브 키 편집 연산 시험이다(Docs/Design/CurveData/README.ko.md 7절): 삽입 위치·회전
// 풀기·UpdateOrAdd 허용오차·지우기·시간 옮기기·SetKeys 검증·FindKey·IsConstant·시간
// 범위·Shift·Scale·선택 키 이동· Bake·중복 키 제거·Reset·복사, 그리고 모든 실패(잘못된 인자·할당
// 실패)에서 커브가 그대로인지.
using namespace rocks;
namespace curve_edit_tests_file {

namespace {

TangentCurveKey Key(float time, float value) noexcept {
    TangentCurveKey key;
    key.time = time;
    key.value = value;
    return key;
}

// 키 시간 목록이 expected와 같은지다.
bool Times(const TangentCurve& curve, Span<const float> expected) noexcept {
    if (curve.KeyCount() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (curve.Keys()[index].time != expected[index]) {
            return false;
        }
    }

    return true;
}

bool Values(const TangentCurve& curve, Span<const float> expected) noexcept {
    if (curve.KeyCount() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (curve.Keys()[index].value != expected[index]) {
            return false;
        }
    }

    return true;
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

} // namespace

// 같은 시간 키가 있으면 그 키들 앞에 넣는다. 새 키는 Linear·Auto·가중 없음·접선 0이다. 비유한
// 시간은 InvalidArgument이고 커브는 그대로다.
TEST(CurveEdit_AddKeyPositionAndDefaults) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(1.0f, 10.0f));
    REQUIRE(curve.TryAddKey(3.0f, 30.0f));
    auto middle = curve.TryAddKey(1.0f, 11.0f);
    REQUIRE(middle);
    CHECK(middle.Value() == 0);
    auto last = curve.TryAddKey(5.0f, 50.0f);
    REQUIRE(last);
    CHECK(last.Value() == 3);
    const float values[] = {11.0f, 10.0f, 30.0f, 50.0f};
    CHECK(Values(curve, values));
    const TangentCurveKey& key = curve.Keys()[0];
    CHECK(key.interp == CurveInterp::Linear && key.tangentMode == CurveTangentMode::Auto &&
          key.tangentWeight == CurveTangentWeight::None && key.arriveTangent == 0.0f &&
          key.leaveTangent == 0.0f);

    CHECK(ErrorOf(curve.TryAddKey(platform::PlatformMath::QuietNaN<float>(), 1.0f)) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryAddKey(platform::PlatformMath::Infinity<float>(), 1.0f)) ==
          ErrorCode::InvalidArgument);
    CHECK(Values(curve, values));
}

// 회전 풀기: 앞 키 값과의 차가 [−180, 180] 쪽에 들도록 360씩 옮긴다(경계 180은 그대로). 아주 큰
// 값은 double 나머지로 한 번에 옮기고(UE 반복은 끝나지 않는다), 비유한 값은 옮기지 않는다. 첫
// 키는 풀지 않는다.
TEST(CurveEdit_UnwindRotation) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(0.0f, 170.0f, true));
    REQUIRE(curve.TryAddKey(1.0f, 190.0f + 360.0f, true));
    CHECK(curve.Keys()[1].value == 190.0f);
    REQUIRE(curve.TryAddKey(2.0f, -170.0f, true));
    CHECK(curve.Keys()[2].value == 190.0f);
    REQUIRE(curve.TryAddKey(3.0f, 370.0f, true));
    CHECK(curve.Keys()[3].value == 370.0f); // 차 180 — 옮기지 않는다
    REQUIRE(curve.TryAddKey(4.0f, 1.0e12f, true));
    const float huge = curve.Keys()[4].value;
    CHECK(huge - 370.0f >= -180.0f && huge - 370.0f <= 180.0f);
    REQUIRE(curve.TryAddKey(5.0f, platform::PlatformMath::Infinity<float>(), true));
    CHECK(curve.Keys()[5].value == platform::PlatformMath::Infinity<float>());

    // UE 반복이 끝나는 큰 차(약 112만 번)는 UE와 비트가 같다: 앞 키 0, 4e8 → 104(float 반복 뺄셈의
    // 결과 — 2^20번 상한이던 때는 double 나머지 40이었다, CV5 리뷰 2).
    TangentCurve large(allocator);
    REQUIRE(large.TryAddKey(0.0f, 0.0f));
    REQUIRE(large.TryAddKey(1.0f, 4.0e8f, true));
    CHECK(large.Keys()[1].value == 104.0f);
}

// UpdateOrAdd는 허용오차 안의 가장 앞 키 값만 바꾸고(다른 칸 유지), 없으면 더한다. 허용오차가
// NaN·음수면 InvalidArgument다. FindKey도 가장 앞 키이고 없거나 NaN이면 IndexNone이다.
TEST(CurveEdit_UpdateOrAddAndFindKey) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(1.0f, 1.0f));
    REQUIRE(curve.TryAddKey(1.00005f, 2.0f));
    REQUIRE(curve.TrySetKeyInterp(0, CurveInterp::Cubic));
    auto updated = curve.TryUpdateOrAddKey(1.00003f, 9.0f);
    REQUIRE(updated);
    CHECK(updated.Value() == 0 && curve.Keys()[0].value == 9.0f &&
          curve.Keys()[0].interp == CurveInterp::Cubic && curve.KeyCount() == 2);
    auto added = curve.TryUpdateOrAddKey(1.5f, 4.0f);
    REQUIRE(added);
    CHECK(added.Value() == 2 && curve.KeyCount() == 3);
    CHECK(ErrorOf(curve.TryUpdateOrAddKey(1.0f, 0.0f, false,
                                          platform::PlatformMath::QuietNaN<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryUpdateOrAddKey(1.0f, 0.0f, false, -1.0f)) == ErrorCode::InvalidArgument);

    CHECK(curve.FindKey(1.00004f) == 0 && curve.FindKey(1.5f) == 2);
    CHECK(curve.FindKey(1.2f) == IndexNone);
    CHECK(curve.FindKey(platform::PlatformMath::QuietNaN<float>()) == IndexNone);
    CHECK(curve.FindKey(1.2f, 0.3f) == 0);
}

// 지우기는 다른 키의 접선을 다시 계산하지 않는다(D10). 시간 옮기기는 모든 칸을 유지하고 같은 시간
// 키들 앞 자리로 가며 새 index를 돌려준다. 범위 밖 index·비유한 시간은 InvalidArgument이고
// 그대로다.
TEST(CurveEdit_RemoveAndSetKeyTime) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(1.0f, 1.0f), Key(2.0f, 2.0f),
                                    Key(3.0f, 3.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    REQUIRE(curve.TrySetKeyTangents(2, 5.0f, 6.0f));
    REQUIRE(curve.TrySetKeyTangentWeights(2, 0.5f, 0.25f));
    REQUIRE(curve.TrySetKeyTangentWeight(2, CurveTangentWeight::Both));
    REQUIRE(curve.TryRemoveKey(1));
    CHECK(curve.Keys()[1].arriveTangent == 5.0f && curve.Keys()[1].leaveTangent == 6.0f);
    CHECK(ErrorOf(curve.TryRemoveKey(3)) == ErrorCode::InvalidArgument);

    // [0, 2*, 3] → 키 2를 3으로: 같은 시간 키 3 앞 → index 1.
    auto moved = curve.TrySetKeyTime(1, 3.0f);
    REQUIRE(moved);
    CHECK(moved.Value() == 1);
    const TangentCurveKey& key = curve.Keys()[1];
    CHECK(key.time == 3.0f && key.value == 2.0f && key.leaveTangent == 6.0f &&
          key.arriveTangentWeight == 0.5f && key.tangentWeight == CurveTangentWeight::Both);
    // 뒤에서 앞으로.
    auto back = curve.TrySetKeyTime(2, -1.0f);
    REQUIRE(back);
    CHECK(back.Value() == 0);
    const float times[] = {-1.0f, 0.0f, 3.0f};
    const float values[] = {3.0f, 0.0f, 2.0f};
    CHECK(Times(curve, times) && Values(curve, values));
    // 자기 자리.
    auto same = curve.TrySetKeyTime(1, 0.5f);
    REQUIRE(same);
    CHECK(same.Value() == 1);
    CHECK(ErrorOf(curve.TrySetKeyTime(3, 1.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TrySetKeyTime(0, platform::PlatformMath::QuietNaN<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TrySetKeyValue(3, 1.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TrySetKeyInterp(3, CurveInterp::Cubic)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TrySetKeyTangentMode(3, CurveTangentMode::User)) ==
          ErrorCode::InvalidArgument);
}

// SetKeys는 정렬·유한 시간만 받는다(UE는 확인하지 않는다). 시간 범위는 키가 없으면 NotFound다.
// IsConstant는 값이 tolerance 안이고 Cubic 키가 없을 때다. Reset은 키만 비운다.
TEST(CurveEdit_SetKeysRangesAndReset) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    CHECK(ErrorOf(curve.TryGetTimeRange()) == ErrorCode::NotFound);
    CHECK(ErrorOf(curve.TryGetValueRange()) == ErrorCode::NotFound);
    CHECK(curve.IsConstant());
    const TangentCurveKey unsorted[] = {Key(1.0f, 0.0f), Key(0.0f, 0.0f)};
    CHECK(ErrorOf(curve.TrySetKeys(unsorted)) == ErrorCode::InvalidArgument);
    const TangentCurveKey notFinite[] = {Key(0.0f, 0.0f),
                                         Key(platform::PlatformMath::Infinity<float>(), 0.0f)};
    CHECK(ErrorOf(curve.TrySetKeys(notFinite)) == ErrorCode::InvalidArgument);
    CHECK(curve.KeyCount() == 0);

    const TangentCurveKey keys[] = {Key(-1.0f, 2.0f), Key(-1.0f, 2.0f), Key(4.0f, 2.000001f)};
    REQUIRE(curve.TrySetKeys(keys));
    auto range = curve.TryGetTimeRange();
    REQUIRE(range);
    CHECK(range.Value().min == -1.0f && range.Value().max == 4.0f);
    CHECK(!curve.IsConstant() && curve.IsConstant(1.0e-5f));
    REQUIRE(curve.TrySetKeyInterp(1, CurveInterp::Cubic));
    CHECK(!curve.IsConstant(1.0f));

    curve.defaultValue = 3.0f;
    curve.postExtrapolation = CurveExtrapolation::Cycle;
    curve.Reset();
    CHECK(curve.KeyCount() == 0 && curve.defaultValue.HasValue() &&
          curve.postExtrapolation == CurveExtrapolation::Cycle);
}

// Shift·Scale은 순서를 유지한다. Scale은 (t − origin)·factor + origin이다. factor 0·음수·NaN,
// 결과가 넘치는 시간·NaN 이동은 InvalidArgument이고 그대로다.
TEST(CurveEdit_ShiftAndScale) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(1.0f, 1.0f), Key(3.0f, 2.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    REQUIRE(curve.TryShiftTime(2.0f));
    const float shifted[] = {2.0f, 3.0f, 5.0f};
    CHECK(Times(curve, shifted));
    REQUIRE(curve.TryScaleTime(3.0f, 2.0f));
    const float scaled[] = {1.0f, 3.0f, 7.0f};
    CHECK(Times(curve, scaled));
    CHECK(ErrorOf(curve.TryScaleTime(0.0f, 0.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryScaleTime(0.0f, -1.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryScaleTime(0.0f, platform::PlatformMath::QuietNaN<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryScaleTime(0.0f, 1.0e38f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryShiftTime(platform::PlatformMath::QuietNaN<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryShiftTime(platform::PlatformMath::Infinity<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(Times(curve, scaled));
}

// 선택 키 이동: 고른 키를 빼고 원래 index 오름차순으로 "같은 시간 키 앞"에 다시 넣는다. 두 키가
// 같은 시간으로 가면 나중에 넣은 키(큰 원래 index)가 앞이다. 중복·범위 밖 index는
// InvalidArgument다.
TEST(CurveEdit_ShiftSelectedKeys) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 10.0f), Key(1.0f, 11.0f), Key(2.0f, 12.0f),
                                    Key(3.0f, 13.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    const usize selection[] = {2, 0};
    REQUIRE(curve.TryShiftKeyTimes(selection, 2.5f));
    const float times[] = {1.0f, 2.5f, 3.0f, 4.5f};
    const float values[] = {11.0f, 10.0f, 13.0f, 12.0f};
    CHECK(Times(curve, times) && Values(curve, values));

    TangentCurve stacked(allocator);
    const TangentCurveKey pair[] = {Key(0.0f, 1.0f), Key(0.0f, 2.0f), Key(5.0f, 3.0f)};
    REQUIRE(stacked.TrySetKeys(pair));
    const usize both[] = {0, 1};
    REQUIRE(stacked.TryShiftKeyTimes(both, 1.0f));
    const float stackedValues[] = {2.0f, 1.0f, 3.0f};
    CHECK(Values(stacked, stackedValues));

    const usize duplicate[] = {1, 1};
    const usize outside[] = {3};
    CHECK(ErrorOf(stacked.TryShiftKeyTimes(duplicate, 1.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(stacked.TryShiftKeyTimes(outside, 1.0f)) == ErrorCode::InvalidArgument);
    const usize first[] = {0};
    CHECK(ErrorOf(stacked.TryShiftKeyTimes(first, platform::PlatformMath::QuietNaN<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(Values(stacked, stackedValues));
    CHECK(stacked.TryShiftKeyTimes(Span<const usize>{}, 1.0f));
}

// Bake: (0, 0)·(2, 4) Linear를 간격 0.5로 → 0.5·1·1.5에 1·2·3. first == last는 표본 없음(UE는
// Reserve(−1) fatal), 간격 0은 InvalidArgument, 표본이 2^24개를 넘으면 CapacityExceeded이고
// 그대로다.
TEST(CurveEdit_Bake) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(2.0f, 4.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    REQUIRE(curve.TryBake(0.5f, 1.0f, 1.0f));
    CHECK(curve.KeyCount() == 2);
    REQUIRE(curve.TryBake(0.5f));
    const float times[] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
    const float values[] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
    CHECK(Times(curve, times) && Values(curve, values));
    CHECK(ErrorOf(curve.TryBake(0.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(curve.TryBake(1.0e-7f, 0.0f, 10.0f)) == ErrorCode::CapacityExceeded);
    CHECK(Times(curve, times));

    PlainCurve plain(allocator);
    REQUIRE(plain.TryBake(0.5f));
    CHECK(plain.KeyCount() == 0);
}

// 중복 키 제거(표본 없음): (0,0)·(1,1)·(2,2)·(3,5) Linear에서 키 1은 (0,0)–(2,2) 직선 위라 지우고,
// 키 2는 (0,0)–(3,5) 직선 값 3.33과 달라 남긴다. User 키는 남긴다. 표본판은 User 키도 지울 수 있다.
// 지운 뒤 접선을 다시 계산하지 않는다(D10). 범위판은 시작 보존 키부터 비교한다.
TEST(CurveEdit_RemoveRedundantKeys) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(1.0f, 1.0f), Key(2.0f, 2.0f),
                                    Key(3.0f, 5.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    REQUIRE(curve.TrySetKeyTangents(2, 7.0f, 7.0f));
    REQUIRE(curve.TryRemoveRedundantKeys(1.0e-4f));
    const float times[] = {0.0f, 2.0f, 3.0f};
    CHECK(Times(curve, times));
    CHECK(curve.Keys()[1].arriveTangent == 7.0f);

    TangentCurve user(allocator);
    REQUIRE(user.TrySetKeys(keys));
    REQUIRE(user.TrySetKeyTangentMode(1, CurveTangentMode::User));
    REQUIRE(user.TryRemoveRedundantKeys(1.0e-4f));
    CHECK(user.KeyCount() == 4);
    REQUIRE(user.TryRemoveRedundantKeys(1.0e-4f, FrameRate::TryCreate(30, 1).Value()));
    CHECK(Times(user, times));

    // 범위판: [1, 3] — 시작 보존 키 1(시간 1 이하 마지막 키), 끝 키 3. 키 2는 키 1과 3 사이 직선
    // (1,1)–(3,5)의 값 3과 2가 달라 남는다(UE는 키 0부터 비교한다 — 15절 8).
    TangentCurve range(allocator);
    REQUIRE(range.TrySetKeys(keys));
    REQUIRE(range.TryRemoveRedundantKeys(1.0e-4f, 1.0f, 3.0f));
    CHECK(range.KeyCount() == 4);
    REQUIRE(range.TryRemoveRedundantKeys(1.0e-4f, 3.0f, 3.0f));
    CHECK(range.KeyCount() == 4);

    CHECK(ErrorOf(range.TryRemoveRedundantKeys(platform::PlatformMath::QuietNaN<float>())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(range.TryRemoveRedundantKeys(-1.0f)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(range.TryRemoveRedundantKeys(0.1f, platform::PlatformMath::QuietNaN<float>(),
                                               1.0f)) == ErrorCode::InvalidArgument);
}

// 복사는 공통 칸·키를 모두 옮긴다(PlainCurve는 보간 방식도).
TEST(CurveEdit_CopyFrom) {
    SystemAllocator allocator;
    TangentCurve source(allocator);
    source.defaultValue = 1.5f;
    source.preExtrapolation = CurveExtrapolation::Oscillate;
    const TangentCurveKey keys[] = {Key(0.0f, 1.0f), Key(2.0f, 3.0f)};
    REQUIRE(source.TrySetKeys(keys));
    TangentCurve copy(allocator);
    REQUIRE(copy.TryCopyFrom(source));
    CHECK(copy.KeyCount() == 2 && copy.Keys()[1].value == 3.0f &&
          copy.defaultValue.Value() == 1.5f &&
          copy.preExtrapolation == CurveExtrapolation::Oscillate);

    PlainCurve plainSource(allocator);
    plainSource.interp = PlainCurveInterp::Constant;
    REQUIRE(plainSource.TryAddKey(1.0f, 2.0f));
    PlainCurve plainCopy(allocator);
    REQUIRE(plainCopy.TryCopyFrom(plainSource));
    CHECK(plainCopy.interp == PlainCurveInterp::Constant && plainCopy.KeyCount() == 1);
}

// 할당하는 모든 연산은 할당이 실패하면(시도마다 실패를 주입) OutOfMemory이고 커브가 그대로다.
TEST(CurveEdit_AllocationFailureLeavesCurveUnchanged) {
    SystemAllocator system;
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(1.0f, 1.0f), Key(2.0f, 2.0f),
                                    Key(3.0f, 5.0f)};
    using Operation = Result<void> (*)(TangentCurve&, TangentCurve&);
    const Operation operations[] = {
        [](TangentCurve& curve, TangentCurve&) noexcept {
            auto added = curve.TryAddKey(1.5f, 9.0f);
            return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
        },
        [](TangentCurve& curve, TangentCurve&) noexcept {
            const TangentCurveKey more[] = {Key(0.0f, 0.0f), Key(1.0f, 1.0f), Key(2.0f, 2.0f),
                                            Key(3.0f, 5.0f), Key(4.0f, 6.0f)};
            return curve.TrySetKeys(more);
        },
        [](TangentCurve& curve, TangentCurve&) noexcept {
            const usize picked[] = {0, 2};
            return curve.TryShiftKeyTimes(picked, 0.25f);
        },
        [](TangentCurve& curve, TangentCurve&) noexcept { return curve.TryBake(0.25f); },
        [](TangentCurve& curve, TangentCurve&) noexcept {
            return curve.TryRemoveRedundantKeys(1.0e-4f);
        },
        [](TangentCurve& curve, TangentCurve& other) noexcept { return curve.TryCopyFrom(other); },
        [](TangentCurve& curve, TangentCurve&) noexcept {
            auto updated = curve.TryUpdateOrAddKey(1.5f, 9.0f);
            return updated ? Result<void>::Success() : Result<void>::Failure(updated.Error());
        },
        [](TangentCurve& curve, TangentCurve&) noexcept {
            return curve.TryRemoveRedundantKeys(1.0e-4f, 0.5f, 2.5f);
        },
        [](TangentCurve& curve, TangentCurve&) noexcept {
            return curve.TryRemoveRedundantKeys(1.0e-4f, FrameRate::TryCreate(30, 1).Value());
        },
    };
    for (const Operation operation : operations) {
        bool injected = false;
        for (uint64 attempt = 1; attempt <= 8; ++attempt) {
            test::NFailAllocator failing(system);
            TangentCurve curve(failing);
            REQUIRE(curve.TrySetKeys(keys));
            TangentCurve other(system);
            REQUIRE(other.TryAddKey(7.0f, 7.0f));
            failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
            const Result<void> result = operation(curve, other);
            failing.DisableFailure();
            if (!result) {
                injected = true;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                const float times[] = {0.0f, 1.0f, 2.0f, 3.0f};
                const float values[] = {0.0f, 1.0f, 2.0f, 5.0f};
                CHECK(Times(curve, times) && Values(curve, values));
            }
        }

        CHECK(injected); // 실패가 실제로 주입됐다(할당하는 연산이다)
    }
}

// PlainCurve의 할당하는 연산도 시도마다 실패를 주입하면 OutOfMemory이고 커브가 그대로다(중복 키
// 제거는 남길 index 목록과 새 키 배열 두 번 할당한다 — 둘 다 본다).
TEST(CurveEdit_PlainAllocationFailureLeavesCurveUnchanged) {
    SystemAllocator system;
    const PlainCurveKey keys[] = {{0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 2.0f}, {3.0f, 5.0f}};
    using Operation = Result<void> (*)(PlainCurve&, PlainCurve&);
    const Operation operations[] = {
        [](PlainCurve& curve, PlainCurve&) noexcept {
            auto added = curve.TryAddKey(1.5f, 9.0f);
            return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
        },
        [](PlainCurve& curve, PlainCurve&) noexcept {
            auto updated = curve.TryUpdateOrAddKey(1.5f, 9.0f);
            return updated ? Result<void>::Success() : Result<void>::Failure(updated.Error());
        },
        [](PlainCurve& curve, PlainCurve&) noexcept {
            const PlainCurveKey more[] = {{0.0f, 0.0f}, {1.0f, 1.0f}, {4.0f, 6.0f}};
            return curve.TrySetKeys(more);
        },
        [](PlainCurve& curve, PlainCurve&) noexcept {
            const usize picked[] = {0, 2};
            return curve.TryShiftKeyTimes(picked, 0.25f);
        },
        [](PlainCurve& curve, PlainCurve&) noexcept { return curve.TryBake(0.25f); },
        [](PlainCurve& curve, PlainCurve&) noexcept {
            return curve.TryRemoveRedundantKeys(1.0e-4f);
        },
        [](PlainCurve& curve, PlainCurve&) noexcept {
            return curve.TryRemoveRedundantKeys(1.0e-4f, 0.5f, 2.5f);
        },
        [](PlainCurve& curve, PlainCurve& other) noexcept { return curve.TryCopyFrom(other); },
    };
    for (const Operation operation : operations) {
        uint32 injected = 0;
        for (uint64 attempt = 1; attempt <= 8; ++attempt) {
            test::NFailAllocator failing(system);
            PlainCurve curve(failing);
            REQUIRE(curve.TrySetKeys(keys));
            PlainCurve other(system);
            REQUIRE(other.TryAddKey(7.0f, 7.0f));
            failing.FailOnAttempt(attempt); // 시도 수는 여기서 0부터 센다
            const Result<void> result = operation(curve, other);
            failing.DisableFailure();
            if (!result) {
                ++injected;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                REQUIRE(curve.KeyCount() == 4);
                for (usize index = 0; index < 4; ++index) {
                    CHECK(curve.Keys()[index].time == keys[index].time &&
                          curve.Keys()[index].value == keys[index].value);
                }
            }
        }

        CHECK(injected >= 1);
    }
}

// 중복 키 제거는 차가 NaN인 키(NaN 값)를 남긴다(UE `>` 비교는 지운다 — CV5 리뷰 4). 표본판은
// 프레임 번호가 int32 끝에 닿으면 표본을 멈추고(UE는 감싸 돌아 끝나지 않는다), 호출 하나의 표본이
// 2^24개를 넘으면 CapacityExceeded이고 그대로다.
TEST(CurveEdit_RedundantKeyEdgeCases) {
    SystemAllocator allocator;
    const float nan = platform::PlatformMath::QuietNaN<float>();
    TangentCurve curve(allocator);
    const TangentCurveKey keys[] = {Key(0.0f, 0.0f), Key(1.0f, nan), Key(2.0f, 2.0f)};
    REQUIRE(curve.TrySetKeys(keys));
    REQUIRE(curve.TryRemoveRedundantKeys(1.0e-4f));
    CHECK(curve.KeyCount() == 3);
    REQUIRE(curve.TryRemoveRedundantKeys(1.0e-4f, FrameRate::TryCreate(30, 1).Value()));
    CHECK(curve.KeyCount() == 3);
    PlainCurve plain(allocator);
    const PlainCurveKey plainKeys[] = {{0.0f, 0.0f}, {1.0f, nan}, {2.0f, 2.0f}};
    REQUIRE(plain.TrySetKeys(plainKeys));
    REQUIRE(plain.TryRemoveRedundantKeys(1.0e-4f));
    CHECK(plain.KeyCount() == 3);

    // 30fps에서 7.158e7초는 프레임 2,147,400,000, 그 뒤 키는 int32 끝으로 잘린다.
    TangentCurve far(allocator);
    const TangentCurveKey farKeys[] = {Key(7.158e7f, 0.0f), Key(8.0e7f, 0.0f), Key(1.0e8f, 0.0f)};
    REQUIRE(far.TrySetKeys(farKeys));
    REQUIRE(far.TryRemoveRedundantKeys(1.0e-4f, FrameRate::TryCreate(30, 1).Value()));
    CHECK(far.KeyCount() == 2);

    // 1000fps × 2만 초 = 2천만 표본(> 2^24).
    TangentCurve wide(allocator);
    const TangentCurveKey wideKeys[] = {Key(0.0f, 0.0f), Key(1.0e4f, 1.0f), Key(2.0e4f, 2.0f)};
    REQUIRE(wide.TrySetKeys(wideKeys));
    CHECK(ErrorOf(wide.TryRemoveRedundantKeys(1.0f, FrameRate::TryCreate(1000, 1).Value())) ==
          ErrorCode::CapacityExceeded);
    CHECK(wide.KeyCount() == 3);
}

// PlainCurve 편집은 TangentCurve와 같은 규칙이다(대표 연산). 중복 키 제거는 UE FSimpleCurve
// 알고리즘(되짚어 다시 지움)이다: (0,0)·(1,1)·(2,2)·(3,3)·(4,8)에서 1·2를 지우고 3은 남긴다.
TEST(CurveEdit_PlainCurveOperations) {
    SystemAllocator allocator;
    PlainCurve curve(allocator);
    REQUIRE(curve.TryAddKey(1.0f, 1.0f));
    REQUIRE(curve.TryAddKey(0.0f, 0.0f));
    REQUIRE(curve.TryAddKey(2.0f, 2.0f));
    REQUIRE(curve.TryAddKey(3.0f, 3.0f));
    REQUIRE(curve.TryAddKey(4.0f, 8.0f));
    CHECK(curve.Keys()[0].time == 0.0f && curve.FindKey(2.0f) == 2);
    REQUIRE(curve.TryRemoveRedundantKeys(1.0e-4f));
    REQUIRE(curve.KeyCount() == 3);
    CHECK(curve.Keys()[1].time == 3.0f);
    auto moved = curve.TrySetKeyTime(1, -1.0f);
    REQUIRE(moved);
    CHECK(moved.Value() == 0);
    auto range = curve.TryGetValueRange();
    REQUIRE(range);
    CHECK(range.Value().min == 0.0f && range.Value().max == 8.0f);
    REQUIRE(curve.TrySetKeyValue(0, platform::PlatformMath::Infinity<float>()));
    CHECK(ErrorOf(curve.TryGetValueRange()) == ErrorCode::InvalidArgument);
    const PlainCurveKey unsorted[] = {{1.0f, 0.0f}, {0.0f, 0.0f}};
    CHECK(ErrorOf(curve.TrySetKeys(unsorted)) == ErrorCode::InvalidArgument);
    CHECK(curve.KeyCount() == 3);
}
} // namespace curve_edit_tests_file
