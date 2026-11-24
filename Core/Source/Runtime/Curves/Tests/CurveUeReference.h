#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Tests/KeyCurveUeReference.h>
#include <Core/Time/FrameRate.h>
#include <Curves/PlainCurve.h>
#include <Curves/TangentCurve.h>

// UE 5.8 FRichCurve·FSimpleCurve의 평가·자동 접선·편집·중복 제거를 그대로 옮긴 시험 참조다
// (Docs/Design/CurveData.ko.md 16절 — CurveGoldenTests.cpp가 Curves 구현과 비트 대조한다). 두 키
// 사이 식·순환·SinCos 등 수학 층 참조는 Core KeyCurveUeReference.h(ue58curve)를 그대로 쓴다. UE의
// TArray는 Array, 키 핸들 표는 빼고 옮겼다(평가·키 배열에 영향이 없다). 설계 15절의 수정은
// CurveOptions로 하나씩 켜고 끈다(기본 = 모두 켬 = Rocks 의미).
//
// 참조 함수 ↔ UE 5.8.4 원본(Engine/Source/Runtime/Engine/Private/Curves, 리뷰어 줄 단위 대조용):
//   ue58richcurve::RemapTimeValue      ↔ RichCurve.cpp 1104–1161(SimpleCurve.cpp 645–702도 같은 식)
//   ue58richcurve::RichEval            ↔ RichCurve.cpp 1163–1253
//   ue58richcurve::SimpleEvalForTwoKeys ↔ SimpleCurve.cpp 493–509
//   ue58richcurve::SimpleEval          ↔ SimpleCurve.cpp 704–794
//   ue58richcurve::AutoSetTangents     ↔ RichCurve.cpp 680–755
//   ue58richcurve::AddKey              ↔ RichCurve.cpp 303–339(SimpleCurve.cpp 86–116은 같은 위치)
//   ue58richcurve::UpdateOrAddKey      ↔ RichCurve.cpp 376–400
//   ue58richcurve::DeleteKey           ↔ RichCurve.cpp 365–373
//   ue58richcurve::SetKeyTime          ↔ RichCurve.cpp 403–417
//   ue58richcurve::IsConstant          ↔ RichCurve.cpp 485–502
//   ue58richcurve::BakeCurve           ↔ RichCurve.cpp 890–914
//   ue58richcurve::RemoveRedundantKeysInternal ↔ RichCurve.cpp 966–1102
//   ue58richcurve::RemoveRedundantRange ↔ RichCurve.cpp 936–963
//   ue58richcurve::SimpleRemoveRedundantKeysInternal ↔ SimpleCurve.cpp 511–643
namespace rocks::test::ue58richcurve {

using key_curve_golden::Interp;
using key_curve_golden::Weight;

// UE ERichCurveTangentMode·ERichCurveExtrapolation 번호다.
enum class TangentMode : uint8 {
    Auto = 0,
    User = 1,
    Break = 2
};

enum class Extrap : uint8 {
    Cycle = 0,
    CycleWithOffset = 1,
    Oscillate = 2,
    Linear = 3,
    Constant = 4
};

// FRichCurveKey의 모든 칸이다(평가 칸은 ue58curve::Key).
struct RichKey : key_curve_golden::Key {
    TangentMode tangentMode = TangentMode::Auto;
};

struct SimpleKey {
    float time = 0.0f;
    float value = 0.0f;
};

// 설계 15절 수정 스위치다(수학 층 Options에 커브 층 수정을 더한다).
struct CurveOptions {
    ue58curve::Options math;
    bool nanTimeIsNaN = true;           // 15절 4
    bool redundantStartFromKeep = true; // 15절 8
};

constexpr float MAX_flt = 3.402823466e+38F;
constexpr float UE_SMALL_NUMBER = 1.e-8f;

inline bool IsNearlyZero(float Value) noexcept {
    return platform::PlatformMath::Abs(Value) <= UE_SMALL_NUMBER;
}

inline bool IsNearlyEqual(float A, float B, float Tolerance) noexcept {
    return platform::PlatformMath::Abs(A - B) <= Tolerance;
}

struct CurveState {
    float DefaultValue = MAX_flt;
    Extrap PreInfinityExtrap = Extrap::Constant;
    Extrap PostInfinityExtrap = Extrap::Constant;
};

// RichCurve.cpp 1104–1161(키 형식만 다른 SimpleCurve.cpp 645–702와 같은 식). CycleCount는 UE int다.
template <class K>
void RemapTimeValue(const CurveState& Curve, const Array<K>& Keys, float& InTime,
                    float& CycleValueOffset, const CurveOptions& options) noexcept {
    const int32 NumKeys = static_cast<int32>(Keys.Size());

    if (NumKeys < 2) {
        return;
    }

    if (InTime <= Keys[0].time) {
        if (Curve.PreInfinityExtrap != Extrap::Linear &&
            Curve.PreInfinityExtrap != Extrap::Constant) {
            float MinTime = Keys[0].time;
            float MaxTime = Keys[NumKeys - 1].time;

            const ue58curve::CycleResult cycled =
                ue58curve::CycleTime(MinTime, MaxTime, InTime, options.math);
            InTime = cycled.time;
            int CycleCount = static_cast<int>(cycled.cycleCount);

            if (Curve.PreInfinityExtrap == Extrap::CycleWithOffset) {
                float DV = Keys[0].value - Keys[NumKeys - 1].value;
                CycleValueOffset = DV * CycleCount;
            } else if (Curve.PreInfinityExtrap == Extrap::Oscillate) {
                if (CycleCount % 2 == 1) {
                    InTime = MinTime + (MaxTime - InTime);
                }
            }
        }
    } else if (InTime >= Keys[NumKeys - 1].time) {
        if (Curve.PostInfinityExtrap != Extrap::Linear &&
            Curve.PostInfinityExtrap != Extrap::Constant) {
            float MinTime = Keys[0].time;
            float MaxTime = Keys[NumKeys - 1].time;

            const ue58curve::CycleResult cycled =
                ue58curve::CycleTime(MinTime, MaxTime, InTime, options.math);
            InTime = cycled.time;
            int CycleCount = static_cast<int>(cycled.cycleCount);

            if (Curve.PostInfinityExtrap == Extrap::CycleWithOffset) {
                float DV = Keys[NumKeys - 1].value - Keys[0].value;
                CycleValueOffset = DV * CycleCount;
            } else if (Curve.PostInfinityExtrap == Extrap::Oscillate) {
                if (CycleCount % 2 == 1) {
                    InTime = MinTime + (MaxTime - InTime);
                }
            }
        }
    }
}

// RichCurve.cpp 1163–1253. nanTimeIsNaN이면 15절 4(키가 있으면 NaN)를 켠다.
inline float RichEval(const CurveState& Curve, const Array<RichKey>& Keys, float InTime,
                      float InDefaultValue, const CurveOptions& options) noexcept {
    if (options.nanTimeIsNaN && Keys.Size() > 0 && platform::PlatformMath::IsNaN(InTime)) {
        return platform::PlatformMath::QuietNaN<float>();
    }

    float CycleValueOffset = 0;
    RemapTimeValue(Curve, Keys, InTime, CycleValueOffset, options);

    const int32 NumKeys = static_cast<int32>(Keys.Size());

    float InterpVal = Curve.DefaultValue == MAX_flt ? InDefaultValue : Curve.DefaultValue;

    if (NumKeys == 0) {
    } else if (NumKeys < 2 || (InTime < Keys[0].time)) {
        if (Curve.PreInfinityExtrap == Extrap::Linear && NumKeys > 1) {
            float DT = Keys[1].time - Keys[0].time;

            if (IsNearlyZero(DT)) {
                InterpVal = Keys[0].value;
            } else {
                float DV = Keys[1].value - Keys[0].value;
                float Slope = DV / DT;

                InterpVal = Slope * (InTime - Keys[0].time) + Keys[0].value;
            }
        } else {
            InterpVal = Keys[0].value;
        }
    } else if (InTime <= Keys[NumKeys - 1].time) {
        int32 first = 1;
        int32 last = NumKeys - 1;
        int32 count = last - first;

        while (count > 0) {
            const int32 step = count / 2;
            const int32 middle = first + step;

            if (InTime >= Keys[middle].time) {
                first = middle + 1;
                count -= step + 1;
            } else {
                count = step;
            }
        }

        InterpVal = ue58curve::EvalForTwoKeys(Keys[first - 1], Keys[first], InTime, options.math);
    } else {
        if (Curve.PostInfinityExtrap == Extrap::Linear) {
            float DT = Keys[NumKeys - 2].time - Keys[NumKeys - 1].time;

            if (IsNearlyZero(DT)) {
                InterpVal = Keys[NumKeys - 1].value;
            } else {
                float DV = Keys[NumKeys - 2].value - Keys[NumKeys - 1].value;
                float Slope = DV / DT;

                InterpVal = Slope * (InTime - Keys[NumKeys - 1].time) + Keys[NumKeys - 1].value;
            }
        } else {
            InterpVal = Keys[NumKeys - 1].value;
        }
    }

    return InterpVal + CycleValueOffset;
}

// SimpleCurve.cpp 493–509. InterpMode는 UE RCIM 번호(0 Linear·1 Constant).
inline float SimpleEvalForTwoKeys(Interp InterpMode, const SimpleKey& Key1, const SimpleKey& Key2,
                                  const float InTime, const CurveOptions& options) noexcept {
    const float Diff = Key2.time - Key1.time;

    if (Diff > 0.f && InterpMode != Interp::Constant) {
        const float Alpha = (InTime - Key1.time) / Diff;
        const float P0 = Key1.value;
        const float P3 = Key2.value;

        return ue58curve::Lerp(P0, P3, Alpha, options.math);
    } else {
        return Key1.value;
    }
}

// SimpleCurve.cpp 704–794.
inline float SimpleEval(const CurveState& Curve, Interp InterpMode, const Array<SimpleKey>& Keys,
                        float InTime, float InDefaultValue, const CurveOptions& options) noexcept {
    if (options.nanTimeIsNaN && Keys.Size() > 0 && platform::PlatformMath::IsNaN(InTime)) {
        return platform::PlatformMath::QuietNaN<float>();
    }

    float CycleValueOffset = 0;
    RemapTimeValue(Curve, Keys, InTime, CycleValueOffset, options);

    const int32 NumKeys = static_cast<int32>(Keys.Size());

    float InterpVal = Curve.DefaultValue == MAX_flt ? InDefaultValue : Curve.DefaultValue;

    if (NumKeys == 0) {
    } else if (NumKeys < 2 || (InTime <= Keys[0].time)) {
        if (Curve.PreInfinityExtrap == Extrap::Linear && NumKeys > 1) {
            float DT = Keys[1].time - Keys[0].time;

            if (IsNearlyZero(DT)) {
                InterpVal = Keys[0].value;
            } else {
                float DV = Keys[1].value - Keys[0].value;
                float Slope = DV / DT;

                InterpVal = Slope * (InTime - Keys[0].time) + Keys[0].value;
            }
        } else {
            InterpVal = Keys[0].value;
        }
    } else if (InTime < Keys[NumKeys - 1].time) {
        int32 first = 1;
        int32 last = NumKeys - 1;
        int32 count = last - first;

        while (count > 0) {
            int32 step = count / 2;
            int32 middle = first + step;

            if (InTime >= Keys[middle].time) {
                first = middle + 1;
                count -= step + 1;
            } else {
                count = step;
            }
        }

        InterpVal = SimpleEvalForTwoKeys(InterpMode, Keys[first - 1], Keys[first], InTime, options);
    } else {
        if (Curve.PostInfinityExtrap == Extrap::Linear) {
            float DT = Keys[NumKeys - 2].time - Keys[NumKeys - 1].time;

            if (IsNearlyZero(DT)) {
                InterpVal = Keys[NumKeys - 1].value;
            } else {
                float DV = Keys[NumKeys - 2].value - Keys[NumKeys - 1].value;
                float Slope = DV / DT;

                InterpVal = Slope * (InTime - Keys[NumKeys - 1].time) + Keys[NumKeys - 1].value;
            }
        } else {
            InterpVal = Keys[NumKeys - 1].value;
        }
    }

    return InterpVal + CycleValueOffset;
}

// RichCurve.cpp 680–755.
inline void AutoSetTangents(Array<RichKey>& Keys, float Tension) noexcept {
    const int32 Num = static_cast<int32>(Keys.Size());
    for (int32 KeyIndex = 0; KeyIndex < Num; KeyIndex++) {
        RichKey& Key = Keys[KeyIndex];
        float ArriveTangent = Key.arriveTangent;
        float LeaveTangent = Key.leaveTangent;

        bool needsComputeTangent = false;
        const RichKey* PrevKey = &Key;
        const RichKey* CurrentKey = &Key;
        const RichKey* NextKey = &Key;

        if (KeyIndex == 0) {
            if (KeyIndex < (Num - 1) && Key.tangentMode == TangentMode::Auto) {
                LeaveTangent = 0.0f;
            }
        } else {
            if (KeyIndex < Num - 1) {
                PrevKey = &Keys[KeyIndex - 1];

                if (Key.interp == Interp::Cubic && (Key.tangentMode == TangentMode::Auto)) {
                    NextKey = &Keys[KeyIndex + 1];
                    needsComputeTangent = true;
                } else if ((PrevKey->interp == Interp::Constant) ||
                           (Key.interp == Interp::Constant)) {
                    LeaveTangent = 0.0f;

                    if (PrevKey->interp != Interp::Cubic) {
                        ArriveTangent = 0.0f;
                    }
                }
            } else {
                if (Key.interp == Interp::Cubic && Key.tangentMode == TangentMode::Auto) {
                    ArriveTangent = 0.0f;
                }
            }
        }

        if (needsComputeTangent) {
            ArriveTangent = ue58curve::ComputeCurveTangent(PrevKey->time, PrevKey->value,
                                                           CurrentKey->time, CurrentKey->value,
                                                           NextKey->time, NextKey->value, Tension);

            LeaveTangent = ArriveTangent;
        }

        Key.arriveTangent = ArriveTangent;
        Key.leaveTangent = LeaveTangent;
    }
}

// RichCurve.cpp 303–339(키 핸들 빼고). 새 키의 index를 돌려준다.
template <class K>
int32 AddKey(Array<K>& Keys, const float InTime, const float InValue,
             const bool unwindRotation) noexcept {
    int32 Index = 0;
    const int32 Num = static_cast<int32>(Keys.Size());
    if (Num == 0 || Keys[Num - 1].time < InTime) {
        Index = Num;
    } else {
        for (; Index < Num && Keys[Index].time < InTime; ++Index)
            ;
    }

    K NewKey;
    NewKey.time = InTime;
    NewKey.value = InValue;
    static_cast<void>(Keys.TryInsert(NewKey, static_cast<usize>(Index)));

    if (Index > 0 && unwindRotation) {
        const float OldValue = Keys[Index - 1].value;
        float NewValue = Keys[Index].value;

        while (NewValue - OldValue > 180.0f) {
            NewValue -= 360.0f;
        }
        while (NewValue - OldValue < -180.0f) {
            NewValue += 360.0f;
        }

        Keys[Index].value = NewValue;
    }

    return Index;
}

// RichCurve.cpp 376–400.
template <class K>
int32 UpdateOrAddKey(Array<K>& Keys, float InTime, float InValue, const bool unwindRotation,
                     float KeyTimeTolerance) noexcept {
    for (int32 KeyIndex = 0; KeyIndex < static_cast<int32>(Keys.Size()); ++KeyIndex) {
        float KeyTime = Keys[KeyIndex].time;

        if (IsNearlyEqual(KeyTime, InTime, KeyTimeTolerance)) {
            Keys[KeyIndex].value = InValue;

            return KeyIndex;
        }

        if (KeyTime > InTime) {
            break;
        }
    }

    return AddKey(Keys, InTime, InValue, unwindRotation);
}

// RichCurve.cpp 365–373.
inline void DeleteKey(Array<RichKey>& Keys, int32 Index) noexcept {
    static_cast<void>(Keys.TryRemoveAt(static_cast<usize>(Index)));
    AutoSetTangents(Keys, 0.0f);
}

// RichCurve.cpp 403–417. 새 index를 돌려준다.
inline int32 SetKeyTime(Array<RichKey>& Keys, int32 Index, float NewTime) noexcept {
    const RichKey OldKey = Keys[Index];

    DeleteKey(Keys, Index);
    const int32 NewIndex = AddKey(Keys, NewTime, OldKey.value, false);

    RichKey& NewKey = Keys[NewIndex];
    NewKey = OldKey;
    NewKey.time = NewTime;
    return NewIndex;
}

// RichCurve.cpp 485–502.
inline bool IsConstant(const Array<RichKey>& Keys, float Tolerance) noexcept {
    if (Keys.Size() <= 1) {
        return true;
    }

    const RichKey& RefKey = Keys[0];
    for (const RichKey& Key : Keys.View()) {
        if (!IsNearlyEqual(Key.value, RefKey.value, Tolerance) || Key.interp == Interp::Cubic) {
            return false;
        }
    }

    return true;
}

// RichCurve.cpp 890–914(Eval 대신 eval 함수 — 표본 시간은 UE float 누적).
template <class K, class Eval>
void BakeCurve(Array<K>& Keys, float SampleRate, float FirstKeyTime, float LastKeyTime, Eval&& eval,
               Allocator& allocator) noexcept {
    if (Keys.Size() == 0) {
        return;
    }

    Array<float> BakedTimes(allocator);
    Array<float> BakedValues(allocator);

    for (float Time = FirstKeyTime + SampleRate; Time < LastKeyTime;) {
        const float Value = eval(Time);
        static_cast<void>(BakedTimes.TryPushBack(Time));
        static_cast<void>(BakedValues.TryPushBack(Value));
        Time += SampleRate;
    }

    for (usize index = 0; index < BakedTimes.Size(); ++index) {
        UpdateOrAddKey(Keys, BakedTimes[index], BakedValues[index], false,
                       ue58curve::UE_KINDA_SMALL_NUMBER);
    }
}

// RichCurve.cpp 966–1102(키 핸들 빼고). SampleRate가 없으면 UE FFrameRate(0, 0)(무효)이다.
// 마지막 AutoSetTangents(RC 1094)는 callAutoSetTangents로 켠다(Rocks는 D10으로 부르지 않는다).
inline void RemoveRedundantKeysInternal(Array<RichKey>& Keys, float Tolerance, int32 InStartKeepKey,
                                        int32 InEndKeepKey, const Optional<FrameRate>& SampleRate,
                                        bool callAutoSetTangents,
                                        const CurveOptions& options) noexcept {
    const int32 Num = static_cast<int32>(Keys.Size());
    if (Num < 3) {
        return;
    }

    const int32 ActualStartKeepKey = InStartKeepKey > 0 ? InStartKeepKey : 0;       // FMath::Max
    const int32 ActualEndKeepKey = InEndKeepKey < Num - 1 ? InEndKeepKey : Num - 1; // FMath::Min

    if ((ActualEndKeepKey - ActualStartKeepKey) < 2) {
        return;
    }

    {
        Array<RichKey> NewKeys(Keys.GetAllocator());
        static_cast<void>(NewKeys.TryReserve(Keys.Size()));

        for (int32 StartKeepIndex = 0; StartKeepIndex <= ActualStartKeepKey; ++StartKeepIndex) {
            static_cast<void>(NewKeys.TryPushBack(Keys[StartKeepIndex]));
        }

        int32 MostRecentKeepKeyIndex = options.redundantStartFromKeep ? ActualStartKeepKey : 0;

        const bool tryRemoveUserTangentKeys = SampleRate.HasValue();
        for (int32 TestIndex = ActualStartKeepKey + 1; TestIndex < ActualEndKeepKey; ++TestIndex) {
            bool keepKey = false;

            if (tryRemoveUserTangentKeys) {
                const FrameRate& Rate = SampleRate.Value();
                const FrameTime KeyOneFrameTime =
                    Rate.AsFrameTime(Keys[MostRecentKeepKeyIndex].time);
                const FrameTime KeyTwoFrameTime = Rate.AsFrameTime(Keys[TestIndex + 1].time);

                FrameTime SampleFrameTime = KeyOneFrameTime;
                const float ToRemoveKeyTime = Keys[TestIndex].time;

                while (SampleFrameTime <= KeyTwoFrameTime) {
                    const float SampleSeconds = static_cast<float>(Rate.AsSeconds(SampleFrameTime));
                    const float ValueWithoutKey =
                        ue58curve::EvalForTwoKeys(Keys[MostRecentKeepKeyIndex], Keys[TestIndex + 1],
                                                  SampleSeconds, options.math);

                    const RichKey& KeyOne = SampleSeconds <= ToRemoveKeyTime
                                                ? Keys[MostRecentKeepKeyIndex]
                                                : Keys[TestIndex];
                    const RichKey& KeyTwo =
                        SampleSeconds <= ToRemoveKeyTime ? Keys[TestIndex] : Keys[TestIndex + 1];
                    const float ValueWithKey =
                        ue58curve::EvalForTwoKeys(KeyOne, KeyTwo, SampleSeconds, options.math);

                    if (platform::PlatformMath::Abs(ValueWithoutKey - ValueWithKey) > Tolerance) {
                        keepKey = true;
                        break;
                    }

                    SampleFrameTime.SetFrame(
                        FrameNumber(SampleFrameTime.Frame().value + 1)); // FrameNumber += 1
                }
            } else {
                auto IsNonUserTangentKey = [](const RichKey& Key) {
                    return Key.tangentMode != TangentMode::User &&
                           Key.tangentMode != TangentMode::Break;
                };

                keepKey = true;
                if (IsNonUserTangentKey(Keys[MostRecentKeepKeyIndex]) &&
                    IsNonUserTangentKey(Keys[TestIndex + 1]) &&
                    IsNonUserTangentKey(Keys[TestIndex])) {
                    const float KeyValue = Keys[TestIndex].value;
                    const float ValueWithoutKey =
                        ue58curve::EvalForTwoKeys(Keys[MostRecentKeepKeyIndex], Keys[TestIndex + 1],
                                                  Keys[TestIndex].time, options.math);

                    keepKey = platform::PlatformMath::Abs(ValueWithoutKey - KeyValue) > Tolerance;
                }
            }

            if (keepKey) {
                MostRecentKeepKeyIndex = TestIndex;
                static_cast<void>(NewKeys.TryPushBack(Keys[TestIndex]));
            }
        }

        for (int32 EndKeepIndex = ActualEndKeepKey; EndKeepIndex < Num; ++EndKeepIndex) {
            static_cast<void>(NewKeys.TryPushBack(Keys[EndKeepIndex]));
        }

        Keys = Move(NewKeys);
    }

    if (callAutoSetTangents) {
        AutoSetTangents(Keys, 0.0f);
    }
}

// RichCurve.cpp 936–963의 시작·끝 키 찾기. 찾으면 참이다.
template <class K>
bool RemoveRedundantRange(const Array<K>& Keys, float FirstKeyTime, float LastKeyTime,
                          int32& StartKey, int32& EndKey) noexcept {
    if (FirstKeyTime >= LastKeyTime) {
        return false;
    }

    StartKey = -1;
    EndKey = -1;
    for (int32 KeyIndex = 0; KeyIndex < static_cast<int32>(Keys.Size()); ++KeyIndex) {
        const float CurrentKeyTime = Keys[KeyIndex].time;

        if (CurrentKeyTime <= FirstKeyTime) {
            StartKey = KeyIndex;
        }
        if (CurrentKeyTime >= LastKeyTime) {
            EndKey = KeyIndex;
            break;
        }
    }

    return StartKey != -1 && EndKey != -1;
}

// SimpleCurve.cpp 511–643(키 핸들 빼고).
inline void SimpleRemoveRedundantKeysInternal(Array<SimpleKey>& Keys, Interp InterpMode,
                                              float Tolerance, int32 InStartKeepKey,
                                              int32 InEndKeepKey,
                                              const CurveOptions& options) noexcept {
    int32 NumKeys = static_cast<int32>(Keys.Size());
    if (NumKeys < 3) {
        return;
    }

    const int32 ActualStartKeepKey = InStartKeepKey > 0 ? InStartKeepKey : 0; // FMath::Max
    const int32 ActualEndKeepKey =
        InEndKeepKey < NumKeys - 1 ? InEndKeepKey : NumKeys - 1; // FMath::Min

    if ((ActualEndKeepKey - ActualStartKeepKey) < 2) {
        return;
    }

    Array<int32> KeepIndices(Keys.GetAllocator());
    static_cast<void>(KeepIndices.TryReserve(static_cast<usize>(NumKeys)));
    int32 NextEvalIndex = 0;
    for (; NextEvalIndex <= ActualStartKeepKey; ++NextEvalIndex) {
        static_cast<void>(KeepIndices.TryPushBack(NextEvalIndex));
    }

    auto Last = [&]() { return KeepIndices[KeepIndices.Size() - 1]; };
    auto ShouldKeepMiddleKey = [&](const SimpleKey* Start, const SimpleKey* Middle,
                                   const SimpleKey* End) {
        const float KeyValue = Middle->value;
        const float ValueWithoutKey =
            SimpleEvalForTwoKeys(InterpMode, *Start, *End, Middle->time, options);
        return platform::PlatformMath::Abs(ValueWithoutKey - KeyValue) > Tolerance;
    };

    for (;;) {
        bool skippedKeys = false;
        const SimpleKey* EvalStartKey = &Keys[Last()];
        while (NextEvalIndex < ActualEndKeepKey) {
            const SimpleKey* EvalMiddleKey = &Keys[NextEvalIndex];
            const SimpleKey* EvalEndKey = &Keys[NextEvalIndex + 1];
            if (ShouldKeepMiddleKey(EvalStartKey, EvalMiddleKey, EvalEndKey)) {
                break;
            }
            ++NextEvalIndex;
            skippedKeys = true;
        }

        if (skippedKeys) {
            bool removedKeys = false;
            while (Last() > ActualStartKeepKey) {
                const usize NumKeepIndices = KeepIndices.Size();
                EvalStartKey = &Keys[KeepIndices[NumKeepIndices - 2]];
                const SimpleKey* EvalMiddleKey = &Keys[KeepIndices[NumKeepIndices - 1]];
                const SimpleKey* EvalEndKey = &Keys[NextEvalIndex];
                if (ShouldKeepMiddleKey(EvalStartKey, EvalMiddleKey, EvalEndKey)) {
                    break;
                }
                static_cast<void>(KeepIndices.TryPopBack());
                removedKeys = true;
            }
            if (removedKeys) {
                continue;
            }
        }
        if (NextEvalIndex >= ActualEndKeepKey) {
            break;
        }

        static_cast<void>(KeepIndices.TryPushBack(NextEvalIndex));
        ++NextEvalIndex;
    }

    for (; NextEvalIndex < NumKeys; ++NextEvalIndex) {
        static_cast<void>(KeepIndices.TryPushBack(NextEvalIndex));
    }

    Array<SimpleKey> NewKeys(Keys.GetAllocator());
    static_cast<void>(NewKeys.TryReserve(KeepIndices.Size()));
    for (usize NewIndex = 0; NewIndex < KeepIndices.Size(); ++NewIndex) {
        static_cast<void>(NewKeys.TryPushBack(Keys[KeepIndices[NewIndex]]));
    }
    Keys = Move(NewKeys);
}

// ---- Rocks 커브 ↔ 참조 변환 ----

inline RichKey ToReference(const TangentCurveKey& key) noexcept {
    RichKey result;
    result.time = key.time;
    result.value = key.value;
    result.interp = static_cast<Interp>(key.interp);
    result.tangentWeight = static_cast<Weight>(key.tangentWeight);
    result.arriveTangent = key.arriveTangent;
    result.arriveTangentWeight = key.arriveTangentWeight;
    result.leaveTangent = key.leaveTangent;
    result.leaveTangentWeight = key.leaveTangentWeight;
    result.tangentMode = static_cast<TangentMode>(key.tangentMode);
    return result;
}

inline TangentCurveKey FromReference(const RichKey& key) noexcept {
    TangentCurveKey result;
    result.time = key.time;
    result.value = key.value;
    result.interp = static_cast<CurveInterp>(key.interp);
    result.tangentWeight = static_cast<CurveTangentWeight>(key.tangentWeight);
    result.arriveTangent = key.arriveTangent;
    result.arriveTangentWeight = key.arriveTangentWeight;
    result.leaveTangent = key.leaveTangent;
    result.leaveTangentWeight = key.leaveTangentWeight;
    result.tangentMode = static_cast<CurveTangentMode>(key.tangentMode);
    return result;
}

inline CurveState StateOf(const ScalarCurveBase& curve) noexcept {
    CurveState state;
    state.DefaultValue = curve.defaultValue.HasValue() ? curve.defaultValue.Value() : MAX_flt;
    state.PreInfinityExtrap = static_cast<Extrap>(curve.preExtrapolation);
    state.PostInfinityExtrap = static_cast<Extrap>(curve.postExtrapolation);
    return state;
}

} // namespace rocks::test::ue58richcurve
