#include <Core/Memory/SystemAllocator.h>
#include <Curves/PlainCurve.h>
#include <Curves/TangentCurve.h>
#include <Platform/Memory.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>
#include "CurveUeReference.h"

// TangentCurve·PlainCurve를 UE 5.8 FRichCurve·FSimpleCurve 식을 그대로 옮긴 참조(CurveUeReference.h
// — 머리 주석에 참조 함수 ↔ UE 파일·줄 대조표)와 비트 단위로 대조한다(Docs/Design/CurveData.ko.md
// 16절). 수정을 모두 켠 참조 = Rocks 의미이고 모든 사례에서 비트가 같아야 한다. 커브 층 수정 하나만
// 끈 참조(curveLayerUe — 수학 층 수정(15절 1–3: Lerp 끝점·순환·근 2개)은 켠 채라 "UE 그대로"가
// 아니다)는 그 수정의 경우(NaN 시간 15절 4, 범위판 중복 제거의 비교 기준 15절 8)에서만 다르다. UE
// 그대로의 수학 층 차이는 CV1 golden(MathKeyCurveGoldenTests)이 본다. 접선을 다시 계산하는 UE 편집
// 연산(DeleteKey·SetKeyTime·RemoveRedundantKeys)은 Rocks 연산 뒤 같은 지점에서 AutoSetTangents를
// 부르면 같다(D10). 순환 사례는 |주기 몫| < 2^29로 둔다(double 재배치는 CV1 손 계산 벡터가 본다).
// 무작위 사례는 고정 seed다.
namespace rocks::test::curve_golden {

using ue58richcurve::CurveOptions;
using ue58richcurve::RichKey;
using ue58richcurve::SimpleKey;

namespace {

bool SameFloat(float a, float b) noexcept {
    if (platform::PlatformMath::IsNaN(a) && platform::PlatformMath::IsNaN(b)) {
        return true;
    }

    return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
}

bool SameKey(const TangentCurveKey& a, const RichKey& b) noexcept {
    return SameFloat(a.time, b.time) && SameFloat(a.value, b.value) &&
           static_cast<uint8>(a.interp) == static_cast<uint8>(b.interp) &&
           static_cast<uint8>(a.tangentMode) == static_cast<uint8>(b.tangentMode) &&
           static_cast<uint8>(a.tangentWeight) == static_cast<uint8>(b.tangentWeight) &&
           SameFloat(a.arriveTangent, b.arriveTangent) &&
           SameFloat(a.leaveTangent, b.leaveTangent) &&
           SameFloat(a.arriveTangentWeight, b.arriveTangentWeight) &&
           SameFloat(a.leaveTangentWeight, b.leaveTangentWeight);
}

bool SameKeys(const TangentCurve& curve, const Array<RichKey>& reference) noexcept {
    if (curve.KeyCount() != reference.Size()) {
        return false;
    }

    for (usize index = 0; index < reference.Size(); ++index) {
        if (!SameKey(curve.Keys()[index], reference[index])) {
            return false;
        }
    }

    return true;
}

bool SameKeys(const PlainCurve& curve, const Array<SimpleKey>& reference) noexcept {
    if (curve.KeyCount() != reference.Size()) {
        return false;
    }

    for (usize index = 0; index < reference.Size(); ++index) {
        if (!SameFloat(curve.Keys()[index].time, reference[index].time) ||
            !SameFloat(curve.Keys()[index].value, reference[index].value)) {
            return false;
        }
    }

    return true;
}

float RandomFloat(MathRandom& random, double minimum, double maximum) noexcept {
    return static_cast<float>(random.Range(minimum, maximum));
}

float RandomTangent(MathRandom& random) noexcept {
    const uint64 pick = random.Next() % 8;
    if (pick == 0) {
        return 0.0f;
    }

    return pick == 1 ? RandomFloat(random, -200.0, 200.0) : RandomFloat(random, -5.0, 5.0);
}

// 시간 오름차순 무작위 키다: 간격은 대개 [0.05, 3], 가끔 0(같은 시간)·아주 작은 값이다.
void RandomKeys(MathRandom& random, usize count, Array<TangentCurveKey>& keys) noexcept {
    keys.Clear();
    float time = RandomFloat(random, -10.0, 10.0);
    for (usize index = 0; index < count; ++index) {
        if (index > 0) {
            const uint64 gap = random.Next() % 10;
            time += gap == 0 ? 0.0f : (gap == 1 ? 1.0e-3f : RandomFloat(random, 0.05, 3.0));
        }

        TangentCurveKey key;
        key.time = time;
        key.value = random.Next() % 6 == 0 ? 0.0f : RandomFloat(random, -50.0, 50.0);
        key.interp = static_cast<CurveInterp>(random.Next() % 3);
        key.tangentMode = static_cast<CurveTangentMode>(random.Next() % 3);
        key.tangentWeight = static_cast<CurveTangentWeight>(random.Next() % 4);
        key.arriveTangent = RandomTangent(random);
        key.leaveTangent = RandomTangent(random);
        key.arriveTangentWeight = RandomFloat(random, 0.0, 4.0);
        key.leaveTangentWeight = RandomFloat(random, 0.0, 4.0);
        CHECK(keys.TryPushBack(key));
    }
}

void RandomBase(MathRandom& random, ScalarCurveBase& curve) noexcept {
    curve.preExtrapolation = static_cast<CurveExtrapolation>(random.Next() % 5);
    curve.postExtrapolation = static_cast<CurveExtrapolation>(random.Next() % 5);
    if (random.Next() % 2 == 0) {
        curve.defaultValue = RandomFloat(random, -5.0, 5.0);
    } else {
        curve.defaultValue = Optional<float>{};
    }
}

void ToReference(const TangentCurve& curve, Array<RichKey>& keys) noexcept {
    keys.Clear();
    for (const TangentCurveKey& key : curve.Keys()) {
        CHECK(keys.TryPushBack(ue58richcurve::ToReference(key)));
    }
}

void ToReference(const PlainCurve& curve, Array<SimpleKey>& keys) noexcept {
    keys.Clear();
    for (const PlainCurveKey& key : curve.Keys()) {
        CHECK(keys.TryPushBack(SimpleKey{key.time, key.value}));
    }
}

// 평가 시간이다: 키 시간 정확히, 범위 안, 범위 밖 몇 주기, 멀리(주기 몫 < 2^29), NaN·±Inf.
float RandomEvaluationTime(MathRandom& random, Span<const float> times) noexcept {
    const float first = times.Size() > 0 ? times[0] : 0.0f;
    const float last = times.Size() > 0 ? times[times.Size() - 1] : 1.0f;
    const double span = static_cast<double>(last) - first;
    const uint64 pick = random.Next() % 16;
    if (pick == 0 && times.Size() > 0) {
        return times[random.Next() % times.Size()];
    }

    if (pick == 1) {
        return platform::PlatformMath::QuietNaN<float>();
    }

    if (pick == 2) {
        return random.Next() % 2 == 0 ? platform::PlatformMath::Infinity<float>()
                                      : -platform::PlatformMath::Infinity<float>();
    }

    if (pick <= 9) {
        return RandomFloat(random, first - 0.1 * span - 0.1, last + 0.1 * span + 0.1);
    }

    if (pick <= 13) {
        return RandomFloat(random, first - 7.0 * span - 1.0, last + 7.0 * span + 1.0);
    }

    // 멀리: 주기 몫이 2^29 미만이 되게 한다(길이 0이면 아무 시간).
    const double reach = span > 0.0 ? span * 1.0e5 : 1.0e5;
    return RandomFloat(random, first - reach, last + reach);
}

} // namespace

// 무작위 TangentCurve(키 0–8개, 앞·뒤 외삽 5종, 기본값 있음·없음)의 Evaluate가 수정을 켠 UE
// FRichCurve::Eval 참조와 모든 시간에서 비트가 같다. NaN 수정(15절 4)만 끈 참조와는 NaN 시간(키
// 있음)에서만 다르다.
TEST(CurveGolden_TangentEvaluateMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0xC5A11);
    Array<TangentCurveKey> keys(allocator);
    Array<RichKey> reference(allocator);
    Array<float> times(allocator);
    CurveOptions curveLayerUe;
    curveLayerUe.nanTimeIsNaN = false;
    int32 mismatches = 0;
    int32 curveLayerUeOutsideNaN = 0;
    int32 curveLayerUeNaN = 0;
    for (int32 curveIndex = 0; curveIndex < 3000; ++curveIndex) {
        TangentCurve curve(allocator);
        RandomBase(random, curve);
        RandomKeys(random, random.Next() % 9, keys);
        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        const ue58richcurve::CurveState state = ue58richcurve::StateOf(curve);
        times.Clear();
        for (const TangentCurveKey& key : curve.Keys()) {
            REQUIRE(times.TryPushBack(key.time));
        }

        const float fallback = RandomFloat(random, -3.0, 3.0);
        for (int32 sample = 0; sample < 40; ++sample) {
            const float time = RandomEvaluationTime(random, times.View());
            const float actual = curve.Evaluate(time, fallback);
            const float expected = ue58richcurve::RichEval(state, reference, time, fallback, {});
            mismatches += SameFloat(actual, expected) ? 0 : 1;
            const float ue =
                ue58richcurve::RichEval(state, reference, time, fallback, curveLayerUe);
            if (!SameFloat(actual, ue)) {
                if (platform::PlatformMath::IsNaN(time) && curve.KeyCount() > 0) {
                    ++curveLayerUeNaN;
                } else {
                    ++curveLayerUeOutsideNaN;
                }
            }
        }
    }

    CHECK(mismatches == 0);
    CHECK(curveLayerUeOutsideNaN == 0);
    CHECK(curveLayerUeNaN > 0);
}

// 무작위 PlainCurve(Linear·Constant)의 Evaluate가 UE FSimpleCurve::Eval 참조와 비트가 같다.
TEST(CurveGolden_PlainEvaluateMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x51A9E);
    Array<TangentCurveKey> source(allocator);
    Array<PlainCurveKey> keys(allocator);
    Array<SimpleKey> reference(allocator);
    Array<float> times(allocator);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 3000; ++curveIndex) {
        PlainCurve curve(allocator);
        RandomBase(random, curve);
        curve.interp = static_cast<PlainCurveInterp>(random.Next() % 2);
        RandomKeys(random, random.Next() % 9, source);
        keys.Clear();
        times.Clear();
        for (const TangentCurveKey& key : source.View()) {
            REQUIRE(keys.TryPushBack(PlainCurveKey{key.time, key.value}));
            REQUIRE(times.TryPushBack(key.time));
        }

        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        const ue58richcurve::CurveState state = ue58richcurve::StateOf(curve);
        const auto mode = static_cast<key_curve_golden::Interp>(curve.interp);
        for (int32 sample = 0; sample < 40; ++sample) {
            const float time = RandomEvaluationTime(random, times.View());
            const float actual = curve.Evaluate(time, 1.5f);
            const float expected =
                ue58richcurve::SimpleEval(state, mode, reference, time, 1.5f, {});
            mismatches += SameFloat(actual, expected) ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
}

// AutoSetTangents가 UE FRichCurve::AutoSetTangents와 비트가 같다(모든 보간·접선 방식 조합, tension
// 0·0.5·1·무작위). 가중치는 그대로다.
TEST(CurveGolden_AutoSetTangentsMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0xA070);
    Array<TangentCurveKey> keys(allocator);
    Array<RichKey> reference(allocator);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 2000; ++curveIndex) {
        TangentCurve curve(allocator);
        RandomKeys(random, random.Next() % 9, keys);
        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        const uint64 pick = random.Next() % 4;
        const float tension =
            pick == 0 ? 0.0f : (pick == 1 ? 0.5f : (pick == 2 ? 1.0f : RandomFloat(random, -1, 2)));
        curve.AutoSetTangents(tension);
        ue58richcurve::AutoSetTangents(reference, tension);
        mismatches += SameKeys(curve, reference) ? 0 : 1;
    }

    CHECK(mismatches == 0);
}

// 무작위 편집 순서(키 더하기·회전 풀기·UpdateOrAdd·지우기·시간 옮기기)가 UE와 같은 키 배열을 낸다.
// UE DeleteKey·SetKeyTime은 안에서 AutoSetTangents를 부르므로 Rocks 쪽은 같은 지점에서 부른다(D10):
// 지우기 = TryRemoveKey + AutoSetTangents, 시간 옮기기 = TryRemoveKey + AutoSetTangents + TryAddKey
// + 옛 칸 복원. TrySetKeyTime 자체는 UE의 삭제 재계산이 없는 판과 같은 자리·칸이다.
TEST(CurveGolden_EditSequenceMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0xED17);
    Array<TangentCurveKey> keys(allocator);
    Array<RichKey> reference(allocator);
    Array<RichKey> plainMove(allocator);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 400; ++curveIndex) {
        TangentCurve curve(allocator);
        RandomKeys(random, 1 + random.Next() % 6, keys);
        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        for (int32 step = 0; step < 30; ++step) {
            const uint64 operation = random.Next() % 5;
            const usize count = curve.KeyCount();
            if (operation == 0 || count == 0) {
                // 더하기(가끔 있는 키 시간 — 같은 시간 키 앞), 가끔 회전 풀기.
                const float time = count > 0 && random.Next() % 4 == 0
                                       ? curve.Keys()[random.Next() % count].time
                                       : RandomFloat(random, -15.0, 25.0);
                const float value = RandomFloat(random, -720.0, 720.0);
                const bool unwind = random.Next() % 2 == 0;
                auto added = curve.TryAddKey(time, value, unwind);
                REQUIRE(added);
                const int32 index = ue58richcurve::AddKey(reference, time, value, unwind);
                mismatches += added.Value() == static_cast<usize>(index) ? 0 : 1;
            } else if (operation == 1) {
                const float time = random.Next() % 2 == 0
                                       ? curve.Keys()[random.Next() % count].time + 5.0e-5f
                                       : RandomFloat(random, -15.0, 25.0);
                const float value = RandomFloat(random, -50.0, 50.0);
                auto updated = curve.TryUpdateOrAddKey(time, value, false, 1.0e-4f);
                REQUIRE(updated);
                const int32 index =
                    ue58richcurve::UpdateOrAddKey(reference, time, value, false, 1.0e-4f);
                mismatches += updated.Value() == static_cast<usize>(index) ? 0 : 1;
            } else if (operation == 2) {
                const usize index = random.Next() % count;
                REQUIRE(curve.TryRemoveKey(index));
                curve.AutoSetTangents();
                ue58richcurve::DeleteKey(reference, static_cast<int32>(index));
            } else if (operation == 3) {
                // UE SetKeyTime과 같은 결과: 지우고 재계산한 뒤 옛 칸으로 다시 넣는다.
                const usize index = random.Next() % count;
                const float time = RandomFloat(random, -15.0, 25.0);
                const TangentCurveKey old = curve.Keys()[index];
                REQUIRE(curve.TryRemoveKey(index));
                curve.AutoSetTangents();
                auto added = curve.TryAddKey(time, old.value);
                REQUIRE(added);
                REQUIRE(curve.TrySetKeyInterp(added.Value(), old.interp));
                REQUIRE(curve.TrySetKeyTangentMode(added.Value(), old.tangentMode));
                REQUIRE(curve.TrySetKeyTangentWeight(added.Value(), old.tangentWeight));
                REQUIRE(
                    curve.TrySetKeyTangents(added.Value(), old.arriveTangent, old.leaveTangent));
                REQUIRE(curve.TrySetKeyTangentWeights(added.Value(), old.arriveTangentWeight,
                                                      old.leaveTangentWeight));
                const int32 moved =
                    ue58richcurve::SetKeyTime(reference, static_cast<int32>(index), time);
                mismatches += added.Value() == static_cast<usize>(moved) ? 0 : 1;
            } else {
                // TrySetKeyTime: 접선 재계산 없이 같은 자리·같은 칸(UE 식에서 재계산만 뺀 것).
                const usize index = random.Next() % count;
                const float time = RandomFloat(random, -15.0, 25.0);
                const RichKey old = reference[index];
                static_cast<void>(reference.TryRemoveAt(index));
                const int32 expected = ue58richcurve::AddKey(reference, time, old.value, false);
                reference[expected] = old;
                reference[expected].time = time;
                auto moved = curve.TrySetKeyTime(index, time);
                REQUIRE(moved);
                mismatches += moved.Value() == static_cast<usize>(expected) ? 0 : 1;
            }

            mismatches += SameKeys(curve, reference) ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
}

// 중복 키 제거(표본 없음·있음, 전체·범위판)에 이어 AutoSetTangents를 부른 결과가 UE
// RemoveRedundantKeysInternal(마지막 AutoSetTangents 포함)과 비트가 같다. 범위판은 비교 기준을 시작
// 보존 키로 고친 참조와 같고(15절 8), 그 수정만 끈 참조(UE의 index 0 기준)와 다른 사례가 있다.
TEST(CurveGolden_RemoveRedundantKeysMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x8ED0);
    Array<TangentCurveKey> keys(allocator);
    Array<RichKey> reference(allocator);
    Array<RichKey> curveLayerUe(allocator);
    const FrameRate rates[] = {FrameRate::TryCreate(30, 1).Value(),
                               FrameRate::TryCreate(24000, 1001).Value(),
                               FrameRate::TryCreate(60, 1).Value()};
    CurveOptions curveLayerUeOptions;
    curveLayerUeOptions.redundantStartFromKeep = false;
    int32 mismatches = 0;
    int32 removedTotal = 0;
    int32 curveLayerUeDiffers = 0;
    for (int32 curveIndex = 0; curveIndex < 1500; ++curveIndex) {
        TangentCurve curve(allocator);
        // 지울 키가 생기게 값을 자주 직선 위에 둔다.
        RandomKeys(random, 3 + random.Next() % 8, keys);
        for (usize index = 0; index < keys.Size(); ++index) {
            if (random.Next() % 2 == 0) {
                keys[index].value = keys[index].time * 2.0f;
                keys[index].tangentMode = CurveTangentMode::Auto;
            }
        }

        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        ToReference(curve, curveLayerUe);
        const float tolerance = random.Next() % 3 == 0 ? 0.0f : RandomFloat(random, 0.0, 0.5);
        Optional<FrameRate> rate;
        if (random.Next() % 2 == 0) {
            rate = rates[random.Next() % 3];
        }

        const usize before = curve.KeyCount();
        // UE는 실제로 고를 때만(키 3개 이상, 보존 키 사이가 2 이상) 끝에 AutoSetTangents를 부른다.
        bool ran = true;
        if (random.Next() % 2 == 0) {
            REQUIRE(curve.TryRemoveRedundantKeys(tolerance, rate));
            ue58richcurve::RemoveRedundantKeysInternal(
                reference, tolerance, 0, static_cast<int32>(reference.Size()) - 1, rate, true, {});
        } else {
            const float first = RandomFloat(random, -12.0, 20.0);
            const float last = first + RandomFloat(random, 0.0, 25.0);
            REQUIRE(curve.TryRemoveRedundantKeys(tolerance, first, last, rate));
            int32 start = 0;
            int32 end = 0;
            if (ue58richcurve::RemoveRedundantRange(reference, first, last, start, end)) {
                ue58richcurve::RemoveRedundantKeysInternal(reference, tolerance, start, end, rate,
                                                           true, {});
                ue58richcurve::RemoveRedundantKeysInternal(curveLayerUe, tolerance, start, end,
                                                           rate, true, curveLayerUeOptions);
                curveLayerUeDiffers += reference.Size() != curveLayerUe.Size() ? 1 : 0;
                ran = end - start >= 2;
            } else {
                ran = false;
            }
        }

        if (ran) {
            curve.AutoSetTangents();
        }

        removedTotal += static_cast<int32>(before - curve.KeyCount());
        mismatches += SameKeys(curve, reference) ? 0 : 1;
    }

    CHECK(mismatches == 0);
    CHECK(removedTotal > 0);
    CHECK(curveLayerUeDiffers > 0);
}

// PlainCurve 중복 키 제거(전체·범위판)가 UE FSimpleCurve::RemoveRedundantKeysInternal과 같다. 두 번
// 불러도 더 지우지 않는다(UE 쿡 결정성 주석).
TEST(CurveGolden_PlainRemoveRedundantKeysMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x51DE);
    Array<TangentCurveKey> source(allocator);
    Array<PlainCurveKey> keys(allocator);
    Array<SimpleKey> reference(allocator);
    int32 mismatches = 0;
    int32 repeated = 0;
    int32 removedTotal = 0;
    for (int32 curveIndex = 0; curveIndex < 1500; ++curveIndex) {
        PlainCurve curve(allocator);
        curve.interp = static_cast<PlainCurveInterp>(random.Next() % 2);
        RandomKeys(random, 3 + random.Next() % 10, source);
        keys.Clear();
        for (const TangentCurveKey& key : source.View()) {
            const float value = random.Next() % 2 == 0 ? key.time * 2.0f : key.value;
            REQUIRE(keys.TryPushBack(PlainCurveKey{key.time, value}));
        }

        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        const auto mode = static_cast<key_curve_golden::Interp>(curve.interp);
        const float tolerance = random.Next() % 3 == 0 ? 0.0f : RandomFloat(random, 0.0, 0.5);
        const usize before = curve.KeyCount();
        const bool full = random.Next() % 2 == 0;
        if (full) {
            REQUIRE(curve.TryRemoveRedundantKeys(tolerance));
            ue58richcurve::SimpleRemoveRedundantKeysInternal(
                reference, mode, tolerance, 0, static_cast<int32>(reference.Size()) - 1, {});
        } else {
            const float first = RandomFloat(random, -12.0, 20.0);
            const float last = first + RandomFloat(random, 0.0, 25.0);
            REQUIRE(curve.TryRemoveRedundantKeys(tolerance, first, last));
            int32 start = 0;
            int32 end = 0;
            if (ue58richcurve::RemoveRedundantRange(reference, first, last, start, end)) {
                ue58richcurve::SimpleRemoveRedundantKeysInternal(reference, mode, tolerance, start,
                                                                 end, {});
            }
        }

        removedTotal += static_cast<int32>(before - curve.KeyCount());
        mismatches += SameKeys(curve, reference) ? 0 : 1;
        if (full) {
            const usize after = curve.KeyCount();
            REQUIRE(curve.TryRemoveRedundantKeys(tolerance));
            repeated += curve.KeyCount() == after ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
    CHECK(repeated == 0);
    CHECK(removedTotal > 0);
}

// TryBake가 UE BakeCurve와 같다(누적 덧셈이 정확한 격자 — 간격 2^-k·작은 시간 — 에서 UE float
// 누적과 first + i·interval이 같다). IsConstant도 UE와 같다.
TEST(CurveGolden_BakeAndIsConstantMatchUe) {
    SystemAllocator allocator;
    MathRandom random(0xBA4E);
    Array<TangentCurveKey> keys(allocator);
    Array<RichKey> reference(allocator);
    const float intervals[] = {0.5f, 0.25f, 0.125f, 0.0625f};
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 500; ++curveIndex) {
        TangentCurve curve(allocator);
        RandomKeys(random, 2 + random.Next() % 5, keys);
        for (usize index = 0; index < keys.Size(); ++index) {
            keys[index].time =
                static_cast<float>(index) * 1.5f + static_cast<float>(random.Next() % 3) * 0.25f;
        }

        REQUIRE(curve.TrySetKeys(keys.View()));
        ToReference(curve, reference);
        const float tolerance = random.Next() % 2 == 0 ? 1.0e-8f : 100.0f;
        mismatches +=
            curve.IsConstant(tolerance) == ue58richcurve::IsConstant(reference, tolerance) ? 0 : 1;
        const float interval = intervals[random.Next() % 4];
        const ue58richcurve::CurveState state = ue58richcurve::StateOf(curve);
        Array<RichKey> original(allocator);
        ToReference(curve, original);
        REQUIRE(curve.TryBake(interval));
        ue58richcurve::BakeCurve(
            reference, interval, original[0].time, original[original.Size() - 1].time,
            [&](float time) noexcept {
                return ue58richcurve::RichEval(state, original, time, 0.0f, {});
            },
            allocator);
        mismatches += SameKeys(curve, reference) ? 0 : 1;
    }

    CHECK(mismatches == 0);
}

} // namespace rocks::test::curve_golden

namespace rocks::test::curve_golden {

// Bake 시간식의 UE와 다른 점(15절 7): 키 (0, 0)·(1, 10)을 간격 0.1로 구우면 Rocks 시간은
// 0.1·i(float 곱)이고 UE는 0.1을 float로 누적해 일부 시간이 다르다(값은 각 시간의 Evaluate). 키
// 수는 같다.
TEST(CurveGolden_BakeTimesDifferFromUeAccumulation) {
    SystemAllocator allocator;
    TangentCurve curve(allocator);
    REQUIRE(curve.TryAddKey(0.0f, 0.0f));
    REQUIRE(curve.TryAddKey(1.0f, 10.0f));
    Array<RichKey> reference(allocator);
    ToReference(curve, reference);
    Array<RichKey> original(allocator);
    ToReference(curve, original);
    const ue58richcurve::CurveState state = ue58richcurve::StateOf(curve);
    REQUIRE(curve.TryBake(0.1f));
    ue58richcurve::BakeCurve(
        reference, 0.1f, 0.0f, 1.0f,
        [&](float time) noexcept {
            return ue58richcurve::RichEval(state, original, time, 0.0f, {});
        },
        allocator);
    REQUIRE(curve.KeyCount() == 11);
    REQUIRE(reference.Size() == 11);
    int32 differentTimes = 0;
    for (usize index = 1; index + 1 < curve.KeyCount(); ++index) {
        CHECK(curve.Keys()[index].time == static_cast<float>(index) * 0.1f);
        differentTimes += curve.Keys()[index].time != reference[index].time ? 1 : 0;
    }

    CHECK(differentTimes > 0);
}

// PlainCurve 편집(더하기·같은 시간 더하기·UpdateOrAdd·시간 옮기기·지우기)이 UE FSimpleCurve와 같은
// 키 배열을 낸다(SimpleCurve.cpp 86–116·140–165·167–177 — SetKeyTime은 DeleteKey + AddKey(새 시간,
// 값)이고 접선이 없어 Rocks TrySetKeyTime과 같다).
TEST(CurveGolden_PlainEditSequenceMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x5EDC);
    Array<SimpleKey> reference(allocator);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 400; ++curveIndex) {
        PlainCurve curve(allocator);
        reference.Clear();
        for (int32 step = 0; step < 30; ++step) {
            const usize count = curve.KeyCount();
            const uint64 operation = count == 0 ? 0 : random.Next() % 4;
            const float value = RandomFloat(random, -720.0, 720.0);
            if (operation == 0) {
                const float time = count > 0 && random.Next() % 3 == 0
                                       ? curve.Keys()[random.Next() % count].time
                                       : RandomFloat(random, -10.0, 10.0);
                const bool unwind = random.Next() % 2 == 0;
                auto added = curve.TryAddKey(time, value, unwind);
                REQUIRE(added);
                const int32 index = ue58richcurve::AddKey(reference, time, value, unwind);
                mismatches += added.Value() == static_cast<usize>(index) ? 0 : 1;
            } else if (operation == 1) {
                const float time = random.Next() % 2 == 0
                                       ? curve.Keys()[random.Next() % count].time + 5.0e-5f
                                       : RandomFloat(random, -10.0, 10.0);
                auto updated = curve.TryUpdateOrAddKey(time, value);
                REQUIRE(updated);
                const int32 index =
                    ue58richcurve::UpdateOrAddKey(reference, time, value, false, 1.0e-4f);
                mismatches += updated.Value() == static_cast<usize>(index) ? 0 : 1;
            } else if (operation == 2) {
                const usize index = random.Next() % count;
                const float time = RandomFloat(random, -10.0, 10.0);
                const SimpleKey old = reference[index];
                static_cast<void>(reference.TryRemoveAt(index));
                const int32 expected = ue58richcurve::AddKey(reference, time, old.value, false);
                auto moved = curve.TrySetKeyTime(index, time);
                REQUIRE(moved);
                mismatches += moved.Value() == static_cast<usize>(expected) ? 0 : 1;
            } else {
                const usize index = random.Next() % count;
                REQUIRE(curve.TryRemoveKey(index));
                static_cast<void>(reference.TryRemoveAt(index));
            }

            Array<SimpleKey> actual(allocator);
            ToReference(curve, actual);
            bool same = actual.Size() == reference.Size();
            for (usize index = 0; same && index < actual.Size(); ++index) {
                same = SameFloat(actual[index].time, reference[index].time) &&
                       SameFloat(actual[index].value, reference[index].value);
            }

            mismatches += same ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
}

} // namespace rocks::test::curve_golden
