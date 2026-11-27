#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <Curves/StepCurves.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>

// 계단 커브를 UE 5.8 FIntegralCurve·FStringCurve 식을 그대로 옮긴 참조와 대조한다(Docs/Design/
// CurveData/README.ko.md 5.8·16절 — CV6). 참조는 UE 원본을 줄 단위로 옮겼고(TArray는 Array, 키 핸들
// 표는 뺐다) 설계 15절·17.5절의 수정을 Options로 켜고 끈다. 수정을 켠 참조 = Rocks 의미라 모든
// 사례에서 같아야 하고, 끈 참조(UE 그대로)는 문서화한 경우(NaN 시간, 중복 제거 뒤 남은 키 하나의
// 기본값 변환, FString 대소문자 무시 같음)에서만 다르다. FNameCurve에는 평가·중복 제거가 없으므로
// NameStepCurve는 String 참조 규칙(D9)과 대조한다. 무작위 사례는 고정 seed다.
//
// 참조 함수 ↔ UE 5.8.4 원본(Engine/Source/Runtime/Engine/Private/Curves, 리뷰어 줄 단위 대조용):
//   ue58step::IntegralEvaluate        ↔ IntegralCurve.cpp 7–40
//   ue58step::AddKey                  ↔ IntegralCurve.cpp 49–58(StringCurve.cpp 33–44·NameCurve.cpp
//                                       33–44는 같은 위치)
//   ue58step::UpdateOrAddKey          ↔ IntegralCurve.cpp 71–93(StringCurve.cpp 183–207)
//   ue58step::SetKeyTime              ↔ IntegralCurve.cpp 96–110(DeleteKey 61–68 + AddKey)
//   ue58step::FindKeyBeforeOrAt       ↔ IntegralCurve.cpp 186–226
//   ue58step::IntegralRemoveRedundant ↔ IntegralCurve.cpp 228–257
//   ue58step::StringEval              ↔ StringCurve.cpp 57–90
//   ue58step::StringRemoveRedundant   ↔ StringCurve.cpp 209–238
namespace rocks::test::step_golden {

namespace ue58step {

// 15절·17.5절 수정 스위치다(기본 = 모두 켬 = Rocks 의미).
struct Options {
    bool nanTimeIsDefault = true;     // 17.5절: NaN 시간은 기본값(UE는 마지막 키)
    bool keepLastRedundantKey = true; // 15절 9: 남은 키 하나를 기본값으로 바꾸지 않는다
    bool caseSensitiveStringEqual =
        true; // 15절 10: String 같음은 byte(UE FString ==는 대소문자 무시)
};

constexpr int32 MAX_int32 = 2147483647;

struct IntegralKey {
    float time = 0.0f;
    int32 value = 0;
};

struct IntegralCurve {
    explicit IntegralCurve(Allocator& allocator) noexcept : Keys(allocator) {
    }

    Array<IntegralKey> Keys;
    int32 DefaultValue = MAX_int32;
    bool useDefaultValueBeforeFirstKey = false;
};

// 표시 철자를 비교 대상으로 쓰는 String 키다(값은 StringView 하나 — 시험 문자열은 정적 상수다).
struct StringKey {
    float time = 0.0f;
    StringView value;
};

struct StringCurve {
    explicit StringCurve(Allocator& allocator) noexcept : Keys(allocator) {
    }

    Array<StringKey> Keys;
    // UE FString DefaultValue — 빈 문자열이 "없음"이다.
    StringView DefaultValue;
};

inline bool IsNearlyEqual(float A, float B, float Tolerance) noexcept {
    return platform::PlatformMath::Abs(A - B) <= Tolerance;
}

// UE FString ==(대소문자 무시, ASCII)다.
inline bool FStringEquals(StringView left, StringView right) noexcept {
    if (left.ByteCount() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.ByteCount(); ++index) {
        char a = left.Data()[index];
        char b = right.Data()[index];
        a = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
        b = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
        if (a != b) {
            return false;
        }
    }

    return true;
}

// IntegralCurve.cpp 7–40
inline int32 IntegralEvaluate(const IntegralCurve& Curve, float Time, int32 InDefaultValue,
                              const Options& options) noexcept {
    const Array<IntegralKey>& Keys = Curve.Keys;
    int32 ReturnVal = Curve.DefaultValue == MAX_int32 ? InDefaultValue : Curve.DefaultValue;
    const int32 Num = static_cast<int32>(Keys.Size());

    if (options.nanTimeIsDefault && platform::PlatformMath::IsNaN(Time)) {
        return ReturnVal;
    }

    if (Num == 0 || (Curve.useDefaultValueBeforeFirstKey && Time < Keys[0].time)) {
    } else if (Num < 2 || Time < Keys[0].time) {
        ReturnVal = Keys[0].value;
    } else if (Time < Keys[Num - 1].time) {
        for (int32 i = 0; i < Num; ++i) {
            if (Time < Keys[i].time) {
                ReturnVal = Keys[(i - 1) > 0 ? (i - 1) : 0].value;
                break;
            }
        }
    } else {
        ReturnVal = Keys[Num - 1].value;
    }

    return ReturnVal;
}

// StringCurve.cpp 57–90
inline StringView StringEval(const StringCurve& Curve, float Time, StringView InDefaultValue,
                             const Options& options) noexcept {
    const Array<StringKey>& Keys = Curve.Keys;
    StringView ReturnVal =
        Curve.DefaultValue.ByteCount() == 0 ? InDefaultValue : Curve.DefaultValue;
    const int32 Num = static_cast<int32>(Keys.Size());

    if (options.nanTimeIsDefault && platform::PlatformMath::IsNaN(Time)) {
        return ReturnVal;
    }

    if (Num == 0 || Time < Keys[0].time) {
    } else if (Num < 2 || Time < Keys[0].time) {
        ReturnVal = Keys[0].value;
    } else if (Time < Keys[Num - 1].time) {
        for (int32 i = 0; i < Num; ++i) {
            if (Time < Keys[i].time) {
                ReturnVal = Keys[(i - 1) > 0 ? (i - 1) : 0].value;
                break;
            }
        }
    } else {
        ReturnVal = Keys[Num - 1].value;
    }

    return ReturnVal;
}

// IntegralCurve.cpp 49–58(키 핸들 빼고). 새 index다.
template <class K, class V>
int32 AddKey(Array<K>& Keys, float InTime, V InValue) noexcept {
    int32 Index = 0;
    for (; Index < static_cast<int32>(Keys.Size()) && Keys[Index].time < InTime; ++Index)
        ;
    K key;
    key.time = InTime;
    key.value = InValue;
    static_cast<void>(Keys.TryInsert(key, static_cast<usize>(Index)));
    return Index;
}

// IntegralCurve.cpp 71–93
template <class K, class V>
int32 UpdateOrAddKey(Array<K>& Keys, float InTime, V Value, float KeyTimeTolerance) noexcept {
    for (int32 KeyIndex = 0; KeyIndex < static_cast<int32>(Keys.Size()); ++KeyIndex) {
        float KeyTime = Keys[KeyIndex].time;

        if (IsNearlyEqual(KeyTime, InTime, KeyTimeTolerance)) {
            Keys[KeyIndex].value = Value;
            return KeyIndex;
        }

        if (KeyTime > InTime) {
            break;
        }
    }

    return AddKey(Keys, InTime, Value);
}

// IntegralCurve.cpp 96–110(DeleteKey 61–68 + AddKey, 옛 칸 복원). 새 index다.
template <class K>
int32 SetKeyTime(Array<K>& Keys, int32 Index, float NewTime) noexcept {
    const K OldKey = Keys[Index];
    static_cast<void>(Keys.TryRemoveAt(static_cast<usize>(Index)));
    const int32 NewIndex = AddKey(Keys, NewTime, OldKey.value);
    K& NewKey = Keys[NewIndex];
    NewKey = OldKey;
    NewKey.time = NewTime;
    return NewIndex;
}

// IntegralCurve.cpp 186–226(핸들 대신 index, 없으면 -1).
template <class K>
int32 FindKeyBeforeOrAt(const Array<K>& Keys, float KeyTime) noexcept {
    const int32 Num = static_cast<int32>(Keys.Size());
    if (Num == 0 || KeyTime < Keys[0].time) {
        return -1;
    }

    if (KeyTime >= Keys[Num - 1].time) {
        return Num - 1;
    }

    int32 Start = 0;
    int32 End = Num - 1;
    int32 FoundIndex = -1;
    while (FoundIndex < 0) {
        int32 TestPos = (Start + End) / 2;
        float TestKeyTime = Keys[TestPos].time;
        float NextTestKeyTime = Keys[TestPos + 1].time;
        if (TestKeyTime <= KeyTime) {
            if (NextTestKeyTime > KeyTime) {
                FoundIndex = TestPos;
            } else {
                Start = TestPos + 1;
            }
        } else {
            End = TestPos;
        }
    }

    return FoundIndex;
}

// IntegralCurve.cpp 228–257(핸들 대신 index — 지울 index를 모은 뒤 뒤에서부터 지운다: 원래 배열
// 기준의 판정은 같다).
inline void IntegralRemoveRedundant(IntegralCurve& Curve, Allocator& allocator,
                                    const Options& options) noexcept {
    Array<IntegralKey>& Keys = Curve.Keys;
    Array<int32> ToRemove(allocator);
    for (int32 KeyIndex = 0; KeyIndex < static_cast<int32>(Keys.Size()); ++KeyIndex) {
        if (KeyIndex + 2 < static_cast<int32>(Keys.Size())) {
            const IntegralKey& CurrentKey = Keys[KeyIndex];
            const IntegralKey& NextKeyOne = Keys[KeyIndex + 1];
            const IntegralKey& NextKeyTwo = Keys[KeyIndex + 2];

            if (CurrentKey.value == NextKeyOne.value && NextKeyOne.value == NextKeyTwo.value) {
                static_cast<void>(ToRemove.TryPushBack(KeyIndex + 1));
            }
        }
    }

    for (usize index = ToRemove.Size(); index > 0; --index) {
        static_cast<void>(Keys.TryRemoveAt(static_cast<usize>(ToRemove[index - 1])));
    }

    if (!options.keepLastRedundantKey && Keys.Size() == 1) {
        Curve.DefaultValue = Keys[0].value;
        static_cast<void>(Keys.TryRemoveAt(0));
    }
}

// StringCurve.cpp 209–238
inline void StringRemoveRedundant(StringCurve& Curve, Allocator& allocator,
                                  const Options& options) noexcept {
    Array<StringKey>& Keys = Curve.Keys;
    auto Same = [&](StringView left, StringView right) noexcept {
        return options.caseSensitiveStringEqual ? left == right : FStringEquals(left, right);
    };
    Array<int32> ToRemove(allocator);
    for (int32 KeyIndex = 0; KeyIndex < static_cast<int32>(Keys.Size()); ++KeyIndex) {
        if (KeyIndex + 2 < static_cast<int32>(Keys.Size())) {
            const StringKey& CurrentKey = Keys[KeyIndex];
            const StringKey& NextKeyOne = Keys[KeyIndex + 1];
            const StringKey& NextKeyTwo = Keys[KeyIndex + 2];

            if (Same(CurrentKey.value, NextKeyOne.value) &&
                Same(NextKeyOne.value, NextKeyTwo.value)) {
                static_cast<void>(ToRemove.TryPushBack(KeyIndex + 1));
            }
        }
    }

    for (usize index = ToRemove.Size(); index > 0; --index) {
        static_cast<void>(Keys.TryRemoveAt(static_cast<usize>(ToRemove[index - 1])));
    }

    if (!options.keepLastRedundantKey && Keys.Size() == 1) {
        Curve.DefaultValue = Keys[0].value;
        static_cast<void>(Keys.TryRemoveAt(0));
    }
}

} // namespace ue58step

namespace {

constexpr const char* Words[] = {"Idle", "Walk", "Run", "walk", "Jump"};

float RandomFloat(MathRandom& random, double minimum, double maximum) noexcept {
    return static_cast<float>(random.Range(minimum, maximum));
}

// 시간 오름차순 무작위 시간이다(간격은 대개 [0.05, 2], 가끔 0·아주 작은 값).
void RandomTimes(MathRandom& random, usize count, Array<float>& times) noexcept {
    times.Clear();
    float time = RandomFloat(random, -5.0, 5.0);
    for (usize index = 0; index < count; ++index) {
        if (index > 0) {
            const uint64 gap = random.Next() % 8;
            time += gap == 0 ? 0.0f : (gap == 1 ? 1.0e-3f : RandomFloat(random, 0.05, 2.0));
        }

        CHECK(times.TryPushBack(time));
    }
}

float RandomEvaluationTime(MathRandom& random, const Array<float>& times) noexcept {
    const uint64 pick = random.Next() % 10;
    if (pick <= 2 && times.Size() > 0) {
        return times[random.Next() % times.Size()];
    }

    const float first = times.Size() > 0 ? times[0] : 0.0f;
    const float last = times.Size() > 0 ? times[times.Size() - 1] : 1.0f;
    return RandomFloat(random, first - 2.0, last + 2.0);
}

bool SameIntegerKeys(const IntegerStepCurve& curve,
                     const ue58step::IntegralCurve& reference) noexcept {
    if (curve.KeyCount() != reference.Keys.Size()) {
        return false;
    }

    for (usize index = 0; index < curve.KeyCount(); ++index) {
        if (curve.Keys()[index].time != reference.Keys[index].time ||
            curve.Keys()[index].value != reference.Keys[index].value) {
            return false;
        }
    }

    return true;
}

} // namespace

// 무작위 IntegerStepCurve(키 0–8개, 같은 시간 키, 기본값 있음·없음, useDefaultBeforeFirstKey)의
// Evaluate가 UE FIntegralCurve::Evaluate 참조와 모든 시간에서 같다. UE 그대로와는 NaN 시간에서만
// 다르다(키가 있으면 UE는 마지막 키, Rocks는 기본값).
TEST(StepCurveGolden_IntegerEvaluateMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x51E9);
    Array<float> times(allocator);
    ue58step::Options literal;
    literal.nanTimeIsDefault = false;
    int32 mismatches = 0;
    int32 literalOther = 0;
    int32 literalNaN = 0;
    for (int32 curveIndex = 0; curveIndex < 3000; ++curveIndex) {
        IntegerStepCurve curve(allocator);
        ue58step::IntegralCurve reference(allocator);
        RandomTimes(random, random.Next() % 9, times);
        for (const float time : times.View()) {
            const int32 value = static_cast<int32>(random.Next() % 5) - 2;
            REQUIRE(curve.TryAddKey(time, value));
            ue58step::AddKey(reference.Keys, time, value);
        }

        if (random.Next() % 2 == 0) {
            curve.defaultValue = 40;
            reference.DefaultValue = 40;
        }

        curve.useDefaultBeforeFirstKey = random.Next() % 2 == 0;
        reference.useDefaultValueBeforeFirstKey = curve.useDefaultBeforeFirstKey;
        REQUIRE(SameIntegerKeys(curve, reference));
        for (int32 sample = 0; sample < 30; ++sample) {
            const float time = sample == 0 ? platform::PlatformMath::QuietNaN<float>()
                                           : RandomEvaluationTime(random, times);
            const int32 actual = curve.Evaluate(time, -7);
            mismatches += actual == ue58step::IntegralEvaluate(reference, time, -7, {}) ? 0 : 1;
            if (actual != ue58step::IntegralEvaluate(reference, time, -7, literal)) {
                ++(platform::PlatformMath::IsNaN(time) ? literalNaN : literalOther);
            }

            const int32 before = curve.FindKeyAtOrBefore(time) == IndexNone
                                     ? -1
                                     : static_cast<int32>(curve.FindKeyAtOrBefore(time));
            mismatches += platform::PlatformMath::IsNaN(time) ||
                                  before == ue58step::FindKeyBeforeOrAt(reference.Keys, time)
                              ? 0
                              : 1;
        }
    }

    CHECK(mismatches == 0);
    CHECK(literalOther == 0);
    CHECK(literalNaN > 0);
}

// 무작위 StringStepCurve·NameStepCurve의 Evaluate가 UE FStringCurve::Eval 참조와 같다(Name은 D9 —
// String 규칙). 값 비교는 String byte, Name은 표시 철자다. 기본값·fallback은 키 단어 표 밖의
// 이름이라 어느 갈래가 냈는지 구별된다.
TEST(StepCurveGolden_StringAndNameEvaluateMatchUe) {
    SystemAllocator allocator;
    NamePool pool(allocator);
    REQUIRE(pool.Initialize());
    Name names[5];
    for (usize index = 0; index < 5; ++index) {
        auto interned = pool.TryIntern(StringView(Words[index]));
        REQUIRE(interned);
        names[index] = interned.Value();
    }

    auto defaultName = pool.TryIntern(StringView("Default"));
    auto fallbackName = pool.TryIntern(StringView("Fallback"));
    REQUIRE(defaultName && fallbackName);

    MathRandom random(0x57E1);
    Array<float> times(allocator);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 3000; ++curveIndex) {
        StringStepCurve strings(allocator);
        NameStepCurve namesCurve(allocator);
        ue58step::StringCurve reference(allocator);
        RandomTimes(random, random.Next() % 9, times);
        for (const float time : times.View()) {
            const usize word = random.Next() % 5;
            REQUIRE(strings.TryAddKey(time, StringView(Words[word])));
            REQUIRE(namesCurve.TryAddKey(time, names[word]));
            ue58step::AddKey(reference.Keys, time, StringView(Words[word]));
        }

        if (random.Next() % 2 == 0) {
            String fallbackDefault(allocator);
            REQUIRE(fallbackDefault.TryAssign("Default"));
            strings.defaultValue.Emplace(Move(fallbackDefault));
            namesCurve.defaultValue = defaultName.Value();
            reference.DefaultValue = StringView("Default");
        }

        for (int32 sample = 0; sample < 30; ++sample) {
            const float time = sample == 0 ? platform::PlatformMath::QuietNaN<float>()
                                           : RandomEvaluationTime(random, times);
            const StringView expected =
                ue58step::StringEval(reference, time, StringView("Fallback"), {});
            mismatches += strings.Evaluate(time, StringView("Fallback")) == expected ? 0 : 1;
            const Name name = namesCurve.Evaluate(time, fallbackName.Value());
            auto lease = pool.Acquire(name);
            mismatches += lease && lease.Value().View() == expected ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
    CHECK(pool.Shutdown());
}

// 무작위 편집 순서(더하기·같은 시간 더하기·UpdateOrAdd·시간 옮기기·지우기)가 UE와 같은 키 배열을
// 낸다(계단 커브의 UE SetKeyTime은 접선이 없어 Rocks TrySetKeyTime과 같다).
TEST(StepCurveGolden_EditSequenceMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0xED16);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 400; ++curveIndex) {
        IntegerStepCurve curve(allocator);
        ue58step::IntegralCurve reference(allocator);
        for (int32 step = 0; step < 30; ++step) {
            const usize count = curve.KeyCount();
            const uint64 operation = count == 0 ? 0 : random.Next() % 4;
            const int32 value = static_cast<int32>(random.Next() % 100);
            if (operation == 0) {
                const float time = count > 0 && random.Next() % 3 == 0
                                       ? curve.Keys()[random.Next() % count].time
                                       : RandomFloat(random, -10.0, 10.0);
                auto added = curve.TryAddKey(time, value);
                REQUIRE(added);
                mismatches += added.Value() == static_cast<usize>(
                                                   ue58step::AddKey(reference.Keys, time, value))
                                  ? 0
                                  : 1;
            } else if (operation == 1) {
                const float time = random.Next() % 2 == 0
                                       ? curve.Keys()[random.Next() % count].time + 5.0e-5f
                                       : RandomFloat(random, -10.0, 10.0);
                auto updated = curve.TryUpdateOrAddKey(time, value);
                REQUIRE(updated);
                mismatches += updated.Value() == static_cast<usize>(ue58step::UpdateOrAddKey(
                                                     reference.Keys, time, value, 1.0e-4f))
                                  ? 0
                                  : 1;
            } else if (operation == 2) {
                const usize index = random.Next() % count;
                const float time = RandomFloat(random, -10.0, 10.0);
                auto moved = curve.TrySetKeyTime(index, time);
                REQUIRE(moved);
                mismatches += moved.Value() == static_cast<usize>(ue58step::SetKeyTime(
                                                   reference.Keys, static_cast<int32>(index), time))
                                  ? 0
                                  : 1;
            } else {
                const usize index = random.Next() % count;
                REQUIRE(curve.TryRemoveKey(index));
                static_cast<void>(reference.Keys.TryRemoveAt(index));
            }

            mismatches += SameIntegerKeys(curve, reference) ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
}

// String 키 편집 순서(더하기·같은 시간 더하기·UpdateOrAdd·SetKeyTime·지우기)가 UE FStringCurve
// 참조(같은 index 규칙 — AddKey 33–44, UpdateOrAddKey 183–207, SetKeyTime 159–173)와 같은 키 배열을
// 만든다. 값은 단어 표에서 고른다(대소문자만 다른 "Walk"·"walk" 포함 — UpdateOrAdd는 시간으로만
// 고른다).
TEST(StepCurveGolden_StringEditSequenceMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x57ED);
    int32 mismatches = 0;
    for (int32 curveIndex = 0; curveIndex < 300; ++curveIndex) {
        StringStepCurve curve(allocator);
        ue58step::StringCurve reference(allocator);
        for (int32 step = 0; step < 30; ++step) {
            const usize count = curve.KeyCount();
            const uint64 operation = count == 0 ? 0 : random.Next() % 4;
            const StringView value(Words[random.Next() % 5]);
            if (operation == 0) {
                const float time = count > 0 && random.Next() % 3 == 0
                                       ? curve.Keys()[random.Next() % count].time
                                       : RandomFloat(random, -10.0, 10.0);
                auto added = curve.TryAddKey(time, value);
                REQUIRE(added);
                mismatches += added.Value() == static_cast<usize>(
                                                   ue58step::AddKey(reference.Keys, time, value))
                                  ? 0
                                  : 1;
            } else if (operation == 1) {
                const float time = random.Next() % 2 == 0
                                       ? curve.Keys()[random.Next() % count].time + 5.0e-5f
                                       : RandomFloat(random, -10.0, 10.0);
                auto updated = curve.TryUpdateOrAddKey(time, value);
                REQUIRE(updated);
                mismatches += updated.Value() == static_cast<usize>(ue58step::UpdateOrAddKey(
                                                     reference.Keys, time, value, 1.0e-4f))
                                  ? 0
                                  : 1;
            } else if (operation == 2) {
                const usize index = random.Next() % count;
                const float time = RandomFloat(random, -10.0, 10.0);
                auto moved = curve.TrySetKeyTime(index, time);
                REQUIRE(moved);
                mismatches += moved.Value() == static_cast<usize>(ue58step::SetKeyTime(
                                                   reference.Keys, static_cast<int32>(index), time))
                                  ? 0
                                  : 1;
            } else {
                const usize index = random.Next() % count;
                REQUIRE(curve.TryRemoveKey(index));
                static_cast<void>(reference.Keys.TryRemoveAt(index));
            }

            bool same = curve.KeyCount() == reference.Keys.Size();
            for (usize index = 0; same && index < curve.KeyCount(); ++index) {
                same = curve.Keys()[index].time == reference.Keys[index].time &&
                       curve.Keys()[index].value.View() == reference.Keys[index].value;
            }

            mismatches += same ? 0 : 1;
        }
    }

    CHECK(mismatches == 0);
}

// 중복 키 제거가 UE와 같은 키를 남긴다(남은 키를 기본값으로 바꾸지 않는 수정을 켠 참조). UE
// 그대로는 남은 키가 하나일 때 키를 지우고 기본값으로 옮겨, useDefaultBeforeFirstKey일 때 첫 키
// 이전 평가가 달라진다(Integer). String은 UE FString 대소문자 무시 같음이라 "walk"·"Walk"가
// 이어지면 UE만 지운다.
TEST(StepCurveGolden_RemoveRedundantKeysMatchesUe) {
    SystemAllocator allocator;
    MathRandom random(0x8E06);
    Array<float> times(allocator);
    ue58step::Options literal;
    literal.keepLastRedundantKey = false;
    literal.caseSensitiveStringEqual = false;
    int32 mismatches = 0;
    int32 removed = 0;
    int32 literalCollapsed = 0;
    int32 literalCaseDiffers = 0;
    for (int32 curveIndex = 0; curveIndex < 2000; ++curveIndex) {
        IntegerStepCurve curve(allocator);
        ue58step::IntegralCurve reference(allocator);
        ue58step::IntegralCurve ue(allocator);
        StringStepCurve strings(allocator);
        ue58step::StringCurve stringReference(allocator);
        ue58step::StringCurve stringUe(allocator);
        RandomTimes(random, random.Next() % 10, times);
        for (const float time : times.View()) {
            const int32 value = static_cast<int32>(random.Next() % 2);
            REQUIRE(curve.TryAddKey(time, value));
            ue58step::AddKey(reference.Keys, time, value);
            ue58step::AddKey(ue.Keys, time, value);
            const StringView word(Words[1 + random.Next() % 2 + (random.Next() % 4 == 0 ? 2 : 0)]);
            REQUIRE(strings.TryAddKey(time, word));
            ue58step::AddKey(stringReference.Keys, time, word);
            ue58step::AddKey(stringUe.Keys, time, word);
        }

        curve.useDefaultBeforeFirstKey = true;
        reference.useDefaultValueBeforeFirstKey = true;
        ue.useDefaultValueBeforeFirstKey = true;
        const usize before = curve.KeyCount();
        REQUIRE(curve.TryRemoveRedundantKeys());
        ue58step::IntegralRemoveRedundant(reference, allocator, {});
        ue58step::IntegralRemoveRedundant(ue, allocator, literal);
        removed += static_cast<int32>(before - curve.KeyCount());
        mismatches += SameIntegerKeys(curve, reference) ? 0 : 1;
        if (curve.KeyCount() >= 1) {
            const float early = curve.Keys()[0].time - 1.0f;
            literalCollapsed +=
                curve.Evaluate(early, 9) != ue58step::IntegralEvaluate(ue, early, 9, literal) ? 1
                                                                                              : 0;
        }

        REQUIRE(strings.TryRemoveRedundantKeys());
        ue58step::StringRemoveRedundant(stringReference, allocator, {});
        ue58step::StringRemoveRedundant(stringUe, allocator, literal);
        mismatches += strings.KeyCount() == stringReference.Keys.Size() ? 0 : 1;
        for (usize index = 0; index < strings.KeyCount() && index < stringReference.Keys.Size();
             ++index) {
            mismatches += strings.Keys()[index].value.View() == stringReference.Keys[index].value &&
                                  strings.Keys()[index].time == stringReference.Keys[index].time
                              ? 0
                              : 1;
        }

        literalCaseDiffers += strings.KeyCount() != stringUe.Keys.Size() ? 1 : 0;
    }

    CHECK(mismatches == 0);
    CHECK(removed > 0);
    CHECK(literalCollapsed > 0);
    CHECK(literalCaseDiffers > 0);
}

} // namespace rocks::test::step_golden
